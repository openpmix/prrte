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
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "constants.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
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
static int test_module_contract(void)
{
    int failures = 0;
    prte_odls_base_module_t *m = &prte_odls_pdefault_module;

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
    prte_odls_base_component_t *c = &prte_mca_odls_pdefault_component;

    CHECK("component name", 0 == strcmp(c->pmix_mca_component_name, "pdefault"));

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

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_daemon_cmd_uniqueness();
    failures += test_child_err_enum();
    failures += test_classes();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all odls unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d odls unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
