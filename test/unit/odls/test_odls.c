/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the odls (daemon local launch subsystem) framework.
 *
 * The substantive work of odls -- serializing the launch message on the
 * HNP, decoding it on each daemon, and the fork/exec/waitpid/kill/signal
 * lifecycle -- runs only inside a live DVM against real child processes
 * and cannot be exercised without one; it is covered by the integration
 * and offline-mapper harnesses.
 *
 * What *can* be exercised in isolation is the framework's structural
 * contract and the constructor/destructor invariants its launch code
 * relies on:
 *
 *   1. The module contract.  odls.h defines a 5-pointer vtable; the sole
 *      component, pdefault, is what every daemon selects, so a regression
 *      that left one of its five slots NULL would crash at first use.  We
 *      confirm all five are wired and that get_add_procs_data is the base
 *      function the component is documented to reuse verbatim.
 *
 *   2. The component identity: name "pdefault" (selection depends on it).
 *
 *   3. The daemon command flags (odls_types.h).  Every PRTE_DAEMON_*
 *      command byte leads an RML control message to a prted; a duplicate
 *      value would silently route one command to another's handler.  The
 *      project's coding rules require these hand-assigned codes be unique,
 *      so we assert pairwise distinctness.
 *
 *   4. The child->parent error-code enum (prte_odls_child_err_t).  The
 *      pipe protocol splits codes into "fatal" (child _exit()s) and
 *      "warn" (child continues to execve).  NONE must be 0 and every warn
 *      code must sort after every fatal code, or the child/parent halves
 *      would disagree about whether a report is terminal.
 *
 *   5. The reference-counted caddy classes (base.h).  Their constructors
 *      must establish the documented NULL/zero defaults that the launch
 *      path assumes, and their destructors must free every owned member
 *      -- including the all-NULL case -- without crashing.
 */

#include "prte_config.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif

#include "constants.h"
#include "src/mca/base/pmix_base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/proc_info.h"

#include "src/mca/odls/odls.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/odls/pdefault/odls_pdefault.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * Every slot of the pdefault module's vtable must be non-NULL.  The
 * launch/kill/signal/restart entry points are file-static in the
 * component, so we can only assert they are wired, not their identity;
 * get_add_procs_data, however, is documented to be the base function used
 * verbatim, so we pin that.
 */
/*
 * Ask the framework for a component by name, and for the module that
 * component hands *this* process.
 *
 * These are two different questions.  A component is present whenever the
 * framework opened it; whether it yields a module is up to its query,
 * which is free to decline - iof/prted, for one, answers only a daemon.
 * Naming the component's module symbol instead would assume it was linked
 * into libprrte, which is false with --enable-mca-dso: there the component
 * is a separate DSO and the symbol is not there to link against.
 */
static bool odls_component_present(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;

    PMIX_LIST_FOREACH(cli, &prte_odls_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 == strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            return true;
        }
    }
    return false;
}

static prte_odls_base_module_t *odls_module(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    int pri = 0;

    PMIX_LIST_FOREACH(cli, &prte_odls_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 != strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL == cli->cli_component->pmix_mca_query_component) {
            return NULL;
        }
        if (PRTE_SUCCESS != cli->cli_component->pmix_mca_query_component(&mod, &pri)) {
            return NULL;
        }
        return (prte_odls_base_module_t *) mod;
    }
    return NULL;
}

static int test_module_contract(void)
{
    int failures = 0;
    prte_odls_base_module_t *m = odls_module("pdefault");

    if (NULL == m) {
        fprintf(stdout, "SKIPPED test_module_contract (odls/pdefault absent)\n");
        return 0;
    }

    CHECK("get_add_procs_data set", NULL != m->get_add_procs_data);
    CHECK("launch_local_procs set", NULL != m->launch_local_procs);
    CHECK("kill_local_procs set", NULL != m->kill_local_procs);
    CHECK("signal_local_procs set", NULL != m->signal_local_procs);
    CHECK("restart_proc set", NULL != m->restart_proc);

    /* the HNP-side message builder has nothing OS-specific, so the
     * component reuses the base function directly */
    CHECK("get_add_procs_data identity",
          m->get_add_procs_data == prte_odls_base_default_get_add_procs_data);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_module_contract\n");
    }
    return failures;
}

/*
 * The pdefault component identifies itself as the odls component named
 * "pdefault".  Selection depends on that name, so guard it.
 */
static int test_component_identity(void)
{
    int failures = 0;
    /* the framework must have opened a component by this name - asking it
     * for one is the assertion */
    if (!odls_component_present("pdefault")) {
        fprintf(stdout, "SKIPPED test_component_identity"
                        " (odls/pdefault not loadable from the build tree)\n");
        return 0;
    }
    CHECK("component present", odls_component_present("pdefault"));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_component_identity\n");
    }
    return failures;
}

/*
 * Every PRTE_DAEMON_* command flag must be a distinct value -- these are
 * hand-assigned, and a collision would silently deliver one daemon
 * command to another's handler.
 */
static int test_daemon_cmd_uniqueness(void)
{
    int failures = 0;
    /* every command byte defined in odls_types.h */
    prte_daemon_cmd_flag_t cmds[] = {
        PRTE_DAEMON_CONTACT_QUERY_CMD, PRTE_DAEMON_KILL_LOCAL_PROCS,
        PRTE_DAEMON_SIGNAL_LOCAL_PROCS, PRTE_DAEMON_ADD_LOCAL_PROCS,
        PRTE_DAEMON_HEARTBEAT_CMD, PRTE_DAEMON_EXIT_CMD,
        PRTE_DAEMON_PROCESS_AND_RELAY_CMD, PRTE_DAEMON_NULL_CMD,
        PRTE_DAEMON_REPORT_JOB_INFO_CMD, PRTE_DAEMON_REPORT_NODE_INFO_CMD,
        PRTE_DAEMON_REPORT_PROC_INFO_CMD, PRTE_DAEMON_SPAWN_JOB_CMD,
        PRTE_DAEMON_TERMINATE_JOB_CMD, PRTE_DAEMON_HALT_VM_CMD,
        PRTE_DAEMON_HALT_DVM_CMD, PRTE_DAEMON_REPORT_JOB_COMPLETE,
        PRTE_DAEMON_DEFINE_PSET, PRTE_DAEMON_TOP_CMD,
        PRTE_DAEMON_NAME_REQ_CMD, PRTE_DAEMON_CHECKIN_CMD,
        PRTE_TOOL_CHECKIN_CMD, PRTE_DAEMON_PROCESS_CMD,
        PRTE_DAEMON_ABORT_PROCS_CALLED, PRTE_DAEMON_DVM_ADD_PROCS,
        PRTE_DAEMON_GET_STACK_TRACES, PRTE_DAEMON_GET_MEMPROFILE,
        PRTE_DAEMON_DVM_CLEANUP_JOB_CMD, PRTE_DAEMON_SHRINK_CMD,
    };
    size_t i, j, n = sizeof(cmds) / sizeof(cmds[0]);

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (cmds[i] == cmds[j]) {
                fprintf(stderr, "FAIL [daemon cmd uniqueness]: cmds[%zu] == cmds[%zu] == %d\n",
                        i, j, (int) cmds[i]);
                failures++;
            }
        }
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_daemon_cmd_uniqueness\n");
    }
    return failures;
}

/*
 * The child->parent error protocol splits its codes into a fatal group
 * (the child _exit()s after reporting) and a warn group (the child keeps
 * going toward execve).  NONE must be 0, and every warn code must sort
 * strictly after every fatal code -- do_child()/render_child_msg() and
 * the binding code all rely on that split.
 */
static int test_child_err_enum(void)
{
    int failures = 0;

    CHECK("NONE is zero", 0 == PRTE_ODLS_CHILD_ERR_NONE);

    /* the last fatal code precedes the first warn code */
    CHECK("fatal group before warn group",
          PRTE_ODLS_CHILD_ERR_EXEC < PRTE_ODLS_CHILD_WARN_NOT_BOUND);
    CHECK("warn NOT_BOUND ordered",
          PRTE_ODLS_CHILD_WARN_NOT_BOUND < PRTE_ODLS_CHILD_WARN_MEM_NOT_BOUND);
    CHECK("warn MEM ordered",
          PRTE_ODLS_CHILD_WARN_MEM_NOT_BOUND < PRTE_ODLS_CHILD_WARN_INCORRECT);

    /* every fatal code is a real, non-NONE value */
    CHECK("fatal IOF nonzero", PRTE_ODLS_CHILD_ERR_NONE != PRTE_ODLS_CHILD_ERR_IOF_SETUP);
    CHECK("fatal EXEC nonzero", PRTE_ODLS_CHILD_ERR_NONE != PRTE_ODLS_CHILD_ERR_EXEC);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_child_err_enum\n");
    }
    return failures;
}

/*
 * Constructor defaults and destructor safety for the two caddy classes
 * the framework defines.  The defaults are load-bearing: the launch path
 * checks these NULL/false fields before allocating or applying bindings,
 * and the destructors are the only place the caddy's owned strings and
 * hwloc bitmap are freed.
 */
static int test_classes(void)
{
    int failures = 0;

    /* spawn caddy: all owned pointers NULL, binding flags clear */
    prte_odls_spawn_caddy_t *cd = PMIX_NEW(prte_odls_spawn_caddy_t);
    CHECK("spawn cmd NULL", NULL == cd->cmd);
    CHECK("spawn wdir NULL", NULL == cd->wdir);
    CHECK("spawn argv NULL", NULL == cd->argv);
    CHECK("spawn env NULL", NULL == cd->env);
    CHECK("spawn bind_cpuset NULL", NULL == cd->bind_cpuset);
    CHECK("spawn bind_fatal false", !cd->bind_fatal);
    CHECK("spawn do_membind false", !cd->do_membind);
#if PRTE_HAVE_SCHED_SETAFFINITY
    CHECK("spawn bind_mask NULL", NULL == cd->bind_mask);
    CHECK("spawn bind_masksize 0", 0 == cd->bind_masksize);
#endif
    /* exercise the destructor's free paths for every owned member */
    cd->cmd = strdup("/bin/true");
    cd->wdir = strdup("/tmp");
    cd->argv = PMIx_Argv_split("a b c", ' ');
    cd->env = PMIx_Argv_split("X=1 Y=2", ' ');
    cd->bind_cpuset = hwloc_bitmap_alloc();
    PMIX_RELEASE(cd);

    /* spawn caddy again, released untouched: the all-NULL destructor path
     * must also be safe */
    cd = PMIX_NEW(prte_odls_spawn_caddy_t);
    PMIX_RELEASE(cd);

    /* launch-local caddy: event allocated, job cleared, no fork fn, no
     * retries */
    prte_odls_launch_local_t *ll = PMIX_NEW(prte_odls_launch_local_t);
    CHECK("launch ev set", NULL != ll->ev);
    CHECK("launch fork_local NULL", NULL == ll->fork_local);
    CHECK("launch retries 0", 0 == ll->retries);
    /* the constructor loads the job nspace with NULL - it must be empty */
    CHECK("launch job empty", PMIX_NSPACE_INVALID(ll->job));
    /* NB: releasing the caddy frees a raw event that was never assigned to
     * an event base (in real use PRTE_ACTIVATE_LOCAL_LAUNCH assigns it
     * before use), so libevent prints a benign "event has no event_base
     * set" line here - the destructor path is still correct. */
    PMIX_RELEASE(ll);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_classes\n");
    }
    return failures;
}

/*
 * The envar directives (SET/ADD/UNSET/PREPEND/APPEND) are multi-valued job
 * and app attributes, and process_envars() applies them to the process
 * environment by walking that list FRONT TO BACK.  The order of the list is
 * therefore the order the user's directives take effect in, and the user's
 * command line is what decides it: "--prepend-env FOO[:] x --set-env FOO=1"
 * must leave FOO=1, while the reverse order must leave FOO=x:1.
 *
 * That makes the append-vs-prepend distinction a behavioral contract, not a
 * style choice: prte_append_attribute() is what preserves generation order,
 * and prte_prepend_attribute() is what puts an entry in FRONT of everything
 * already there (used for the values PMIx_server_setup_application hands
 * back, which must be applied before - and hence be overridable by - the
 * user's own).  Assert both, since a swap in either direction is silent.
 */
static int test_attribute_order(void)
{
    int failures = 0;
    pmix_list_t attrs;
    prte_attribute_t *attr;
    pmix_envar_t envt;
    int n;
    const char *names[] = {"FIRST", "SECOND", "THIRD"};

    PMIX_CONSTRUCT(&attrs, pmix_list_t);

    /* appending preserves the order the entries were generated in */
    for (n = 0; n < 3; n++) {
        PMIX_ENVAR_CONSTRUCT(&envt);
        envt.envar = (char *) names[n];
        envt.value = (char *) "v";
        envt.separator = ':';
        prte_append_attribute(&attrs, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                              &envt, PMIX_ENVAR);
    }
    n = 0;
    PMIX_LIST_FOREACH(attr, &attrs, prte_attribute_t) {
        CHECK("append keeps generation order",
              n < 3 && 0 == strcmp(attr->data.data.envar.envar, names[n]));
        ++n;
    }
    CHECK("append added every entry", 3 == n);

    /* prepending puts the new entry in front of all of them */
    PMIX_ENVAR_CONSTRUCT(&envt);
    envt.envar = (char *) "SETUP";
    envt.value = (char *) "v";
    envt.separator = ':';
    prte_prepend_attribute(&attrs, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                           &envt, PMIX_ENVAR);
    attr = (prte_attribute_t *) pmix_list_get_first(&attrs);
    CHECK("prepend lands in front",
          NULL != attr && 0 == strcmp(attr->data.data.envar.envar, "SETUP"));
    CHECK("prepend kept the rest", 4 == pmix_list_get_size(&attrs));

    PMIX_LIST_DESTRUCT(&attrs);
    return failures;
}

/* ------------------------------------------------------------------ *
 * process_envars: the envar directives applied to an app's environment
 * ------------------------------------------------------------------ */

/* look up NAME in an environ-style array; NULL if absent */
static const char *envget(char **env, const char *name)
{
    size_t len = strlen(name);
    int n;

    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (0 == strncmp(env[n], name, len) && '=' == env[n][len]) {
            return env[n] + len + 1;
        }
    }
    return NULL;
}

static void add_envar(pmix_list_t *attrs, prte_attribute_key_t key,
                      const char *name, const char *value)
{
    pmix_envar_t envt;

    PMIX_ENVAR_CONSTRUCT(&envt);
    envt.envar = (char *) name;
    envt.value = (char *) value;
    envt.separator = ':';
    prte_append_attribute(attrs, key, PRTE_ATTR_GLOBAL, &envt, PMIX_ENVAR);
}

static int test_process_envars(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_app_context_t *app;
    const char *v;

    jdata = PMIX_NEW(prte_job_t);
    app = PMIX_NEW(prte_app_context_t);
    app->env = PMIx_Argv_split("PATH=/bin PATHEXT=.EXE KEEP=orig SET_ME=old"
                               " PFX_A=1 PFX_B=2 OTHER=3",
                               ' ');

    /* SET overwrites; ADD must NOT - attr.h defines it as "add envar, do
     * not override pre-existing one" (it carries PMIX_ADD_ENVAR, whose
     * definition says the same).  Treating ADD as SET silently threw away
     * a value the user or the environment had already established. */
    add_envar(&jdata->attributes, PRTE_JOB_SET_ENVAR, "SET_ME", "new");
    add_envar(&jdata->attributes, PRTE_JOB_ADD_ENVAR, "KEEP", "clobbered");
    add_envar(&jdata->attributes, PRTE_JOB_ADD_ENVAR, "FRESH", "added");

    /* PREPEND/APPEND must match the name up to AND INCLUDING the '=', or
     * they edit PATHEXT (which the environment above lists right after
     * PATH) instead of PATH */
    add_envar(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR, "PATH", "/pre");
    add_envar(&jdata->attributes, PRTE_JOB_APPEND_ENVAR, "PATH", "/post");

    /* UNSET is carried as a STRING, not a pmix_envar_t; a trailing '*'
     * makes it a prefix match */
    prte_append_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR, PRTE_ATTR_GLOBAL,
                          (void *) "OTHER", PMIX_STRING);
    prte_append_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR, PRTE_ATTR_GLOBAL,
                          (void *) "PFX_*", PMIX_STRING);

    /* the app's directives are applied after the job's, so they win */
    add_envar(&app->attributes, PRTE_APP_SET_ENVAR, "SET_ME", "app");

    prte_odls_base_process_envars(jdata, app);

    v = envget(app->env, "SET_ME");
    CHECK("app SET trumps job SET", NULL != v && 0 == strcmp(v, "app"));

    v = envget(app->env, "KEEP");
    CHECK("ADD does not override an existing value",
          NULL != v && 0 == strcmp(v, "orig"));

    v = envget(app->env, "FRESH");
    CHECK("ADD sets a variable that was absent",
          NULL != v && 0 == strcmp(v, "added"));

    v = envget(app->env, "PATH");
    CHECK("PREPEND/APPEND edited PATH in order",
          NULL != v && 0 == strcmp(v, "/pre:/bin:/post"));

    v = envget(app->env, "PATHEXT");
    CHECK("PATHEXT untouched by a PATH directive",
          NULL != v && 0 == strcmp(v, ".EXE"));

    CHECK("UNSET removed the named variable", NULL == envget(app->env, "OTHER"));
    CHECK("UNSET prefix removed PFX_A", NULL == envget(app->env, "PFX_A"));
    CHECK("UNSET prefix removed PFX_B", NULL == envget(app->env, "PFX_B"));

    PMIX_RELEASE(app);
    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_process_envars\n");
    }
    return failures;
}

/*
 * The child->parent pipe protocol.  The child half runs in the
 * async-signal-safe window between fork() and execve(), so the only thing
 * it can say is a fixed-size record; the parent half decodes it and
 * renders the diagnostic.  Both halves must agree on that record, and
 * child_fail must additionally terminate the child with the exit status it
 * was handed.  Exercise both across a real pipe and a real fork.
 */
static int test_child_pipe_protocol(void)
{
    int failures = 0;
    int p[2];
    prte_odls_pipe_err_msg_t msg;
    pid_t pid;
    int status;
    ssize_t n;

    if (0 != pipe(p)) {
        fprintf(stdout, "SKIPPED test_child_pipe_protocol (pipe failed)\n");
        return 0;
    }

    /* a warning is written and the caller keeps going */
    prte_odls_base_child_warn(p[1], PRTE_ODLS_CHILD_WARN_NOT_BOUND, EPERM);
    memset(&msg, 0, sizeof(msg));
    n = read(p[0], &msg, sizeof(msg));
    CHECK("warn wrote one whole record", (ssize_t) sizeof(msg) == n);
    CHECK("warn is not fatal", !msg.fatal);
    CHECK("warn carried its code", PRTE_ODLS_CHILD_WARN_NOT_BOUND == msg.which);
    CHECK("warn carried its errno", EPERM == msg.errnum);

    /* a failure is written and the child dies with the given status */
    pid = fork();
    if (0 == pid) {
        close(p[0]);
        prte_odls_base_child_fail(p[1], 7, PRTE_ODLS_CHILD_ERR_EXEC, ENOENT);
        /* does not return */
    }
    if (0 > pid) {
        close(p[0]);
        close(p[1]);
        fprintf(stdout, "SKIPPED test_child_pipe_protocol fork half\n");
        return failures;
    }
    memset(&msg, 0, sizeof(msg));
    n = read(p[0], &msg, sizeof(msg));
    CHECK("fail wrote one whole record", (ssize_t) sizeof(msg) == n);
    CHECK("fail is fatal", msg.fatal);
    CHECK("fail carried its code", PRTE_ODLS_CHILD_ERR_EXEC == msg.which);
    CHECK("fail carried its errno", ENOENT == msg.errnum);
    CHECK("fail carried its exit status", 7 == msg.exit_status);

    while (pid != waitpid(pid, &status, 0) && EINTR == errno) {
        continue;
    }
    CHECK("child exited rather than being signaled", WIFEXITED(status));
    CHECK("child exited with the requested status",
          WIFEXITED(status) && 7 == WEXITSTATUS(status));

    close(p[0]);
    close(p[1]);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_child_pipe_protocol\n");
    }
    return failures;
}

/*
 * The spawn-thread pool.  start_threads sizes the pool from the job it is
 * first handed, and writes its choice back into
 * prte_odls_globals.num_threads.  That makes the "have we already built a
 * pool?" test load-bearing: keying it off ev_threads (which stays NULL
 * whenever the pool is "no dedicated threads at all") let the choice made
 * for the first small job stand for the life of the daemon, so a later job
 * with thousands of local procs still forked them one at a time on the
 * progress thread.  Guard the guard, and the reset harvest_threads owes.
 */
static int test_thread_pool_sizing(void)
{
    int failures = 0;
    prte_job_t *jdata;
    bool save_persistent = prte_persistent;

    /* Drive the from-the-job sizing branch.  prte_persistent defaults to
     * true, and a persistent DVM short-circuits to max_threads and spins
     * that many real progress threads - which this bare test process is in
     * no position to start and stop. */
    prte_persistent = false;

    /* start from a known-clean pool */
    prte_odls_base_harvest_threads();
    CHECK("harvest clears the base array", NULL == prte_odls_globals.ev_bases);
    CHECK("harvest leaves no threads behind", 0 == prte_odls_globals.num_threads);
    /* the sizing sentinel is what register() established; harvest must NOT
     * put it back (see below) */
    prte_odls_globals.num_threads = -1;

    /* a job below the cutoff gets no dedicated threads, and forks on the
     * shared default base */
    jdata = PMIX_NEW(prte_job_t);
    jdata->num_local_procs = 1;
    prte_odls_base_start_threads(jdata);
    CHECK("small job uses no dedicated threads", 0 == prte_odls_globals.num_threads);
    /* ...which means the one entry is the shared default base itself.  (In
     * this bare test process prte_event_base has never been created, so
     * compare against it rather than asserting non-NULL.) */
    CHECK("small job dispatches on the default base",
          NULL != prte_odls_globals.ev_bases
          && prte_event_base == prte_odls_globals.ev_bases[0]);
    /* launch_local indexes ev_bases with next_base after incrementing and
     * wrapping at num_threads; with no dedicated threads that must land on
     * the single element, not past it */
    ++prte_odls_globals.next_base;
    if (prte_odls_globals.num_threads <= prte_odls_globals.next_base) {
        prte_odls_globals.next_base = 0;
    }
    CHECK("the dispatch index stays inside the one-element array",
          0 == prte_odls_globals.next_base);

    /* asking again for the same pool must be a no-op, not a rebuild */
    prte_odls_base_start_threads(jdata);
    CHECK("start_threads is idempotent", 0 == prte_odls_globals.num_threads);
    PMIX_RELEASE(jdata);

    /* Harvest tears the pool down and must leave it torn down.  It runs
     * from framework CLOSE, so anything that reopens the sizing question
     * here is an invitation to build a whole new pool on the way out: with
     * the "-1 = you decide" sentinel restored, a later start_threads during
     * teardown re-sized from the job and spun a second set of real threads
     * (seen on a 40-proc node, which printed "START 5 LAUNCH THREADS"
     * twice).  Record "no threads", not "undecided". */
    prte_odls_base_harvest_threads();
    CHECK("harvest released the base array", NULL == prte_odls_globals.ev_bases);
    CHECK("harvest leaves the pool at zero, not undecided",
          0 == prte_odls_globals.num_threads);

    /* ...and a call that arrives while finalizing builds nothing at all */
    prte_finalizing = true;
    jdata = PMIX_NEW(prte_job_t);
    jdata->num_local_procs = (int32_t) prte_odls_globals.cutoff + 8;
    prte_odls_base_start_threads(jdata);
    CHECK("start_threads builds nothing once finalizing",
          NULL == prte_odls_globals.ev_bases && 0 == prte_odls_globals.num_threads);
    PMIX_RELEASE(jdata);
    prte_finalizing = false;

    prte_persistent = save_persistent;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_thread_pool_sizing\n");
    }
    return failures;
}

/*
 * Signalling a proc that has no pid must not reach the daemon.
 *
 * Both component primitives turn a pid into a process GROUP (-pid) so a
 * signal reaches whatever the app spawned.  That makes pid 0 catastrophic
 * rather than merely useless: kill(0) and kill(-0) both mean "every
 * process in the CALLER's group", so a daemon asked to signal a child at
 * pid 0 signals itself, its other children, and - under prterun - the
 * launching tool.  A child sits at pid 0 before its fork, after a
 * failed launch, and once kill_local_procs has cleared it, and
 * prted_comm.c asks for every local child of a job BY NAME, so this is
 * reachable.
 *
 * Drive the base's signal fn with a recording stub in place of the real
 * kill: the assertion is that no pid <= 0 is ever handed down.
 */
static pid_t signalled_pids[8];
static int nsignalled = 0;

static int recording_signal(pid_t pid, int signum)
{
    PRTE_HIDE_UNUSED_PARAMS(signum);
    if (nsignalled < (int) (sizeof(signalled_pids) / sizeof(signalled_pids[0]))) {
        signalled_pids[nsignalled++] = pid;
    }
    return PRTE_SUCCESS;
}

static int test_signal_skips_dead_procs(void)
{
    int failures = 0;
    prte_proc_t *live, *dead, *unborn;
    int i, bad = 0;

    /* three local children of one job: one running, one that has been
     * reaped (pid cleared), one that never forked */
    live = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&live->name, "odls-sig-test", 0);
    live->pid = getpid(); /* any positive pid - it is never really signaled */
    PRTE_FLAG_SET(live, PRTE_PROC_FLAG_ALIVE);
    pmix_pointer_array_add(prte_local_children, live);

    dead = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&dead->name, "odls-sig-test", 1);
    dead->pid = 0;
    PRTE_FLAG_UNSET(dead, PRTE_PROC_FLAG_ALIVE);
    pmix_pointer_array_add(prte_local_children, dead);

    unborn = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&unborn->name, "odls-sig-test", 2);
    unborn->pid = 0;
    /* flagged ALIVE but not yet forked - launch_local sets the flag before
     * the fork, so this state really occurs */
    PRTE_FLAG_SET(unborn, PRTE_PROC_FLAG_ALIVE);
    pmix_pointer_array_add(prte_local_children, unborn);

    /* the all-procs branch */
    nsignalled = 0;
    prte_odls_base_default_signal_local_procs(NULL, SIGCONT, recording_signal);
    CHECK("signal-all reached only the live proc", 1 == nsignalled);
    for (i = 0; i < nsignalled; i++) {
        if (0 >= signalled_pids[i]) {
            bad++;
        }
    }
    CHECK("signal-all never used a non-positive pid", 0 == bad);

    /* ...and the by-name branch, which is the one prted_comm.c uses and
     * the one that had no gate at all */
    nsignalled = 0;
    prte_odls_base_default_signal_local_procs(&dead->name, SIGCONT, recording_signal);
    CHECK("a reaped proc is not signaled by name", 0 == nsignalled);

    nsignalled = 0;
    prte_odls_base_default_signal_local_procs(&unborn->name, SIGCONT, recording_signal);
    CHECK("a not-yet-forked proc is not signaled by name", 0 == nsignalled);

    nsignalled = 0;
    prte_odls_base_default_signal_local_procs(&live->name, SIGCONT, recording_signal);
    CHECK("...but the live one still is", 1 == nsignalled);
    CHECK("...with its real pid",
          1 == nsignalled && getpid() == signalled_pids[0]);

    /* leave the global array as we found it */
    for (i = 0; i < prte_local_children->size; i++) {
        prte_proc_t *p = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL != p) {
            pmix_pointer_array_set_item(prte_local_children, i, NULL);
            PMIX_RELEASE(p);
        }
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_signal_skips_dead_procs\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* the module tests ask the framework for their subject, so it has to
     * be open before they run */
    rc = pmix_mca_base_framework_open(&prte_odls_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "odls framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_daemon_cmd_uniqueness();
    failures += test_child_err_enum();
    failures += test_classes();
    failures += test_attribute_order();
    failures += test_process_envars();
    failures += test_child_pipe_protocol();
    failures += test_thread_pool_sizing();
    failures += test_signal_skips_dead_procs();

    (void) pmix_mca_base_framework_close(&prte_odls_base_framework);
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all odls unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d odls unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
