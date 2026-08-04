/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the plm (Process Launch Manager) framework.
 *
 * The substantive work of plm -- standing up prted daemons and driving the
 * job state machine as they report back -- needs a live DVM (and, for the
 * RM components, a live resource manager), so it is covered by the
 * integration and dockerswarm harnesses. What can be checked in isolation
 * is everything the launch path *builds* before it ever forks anything,
 * plus the invariants the rest of the tree depends on:
 *
 *   1. plm_types.h. The job/proc/node/app state codes and the PLM command
 *      codes are hand-assigned integers consumed tree-wide, and the
 *      top-level AGENTS.md rule is that every value must stay unique
 *      within its family -- a duplicate silently makes two distinct codes
 *      compare equal. We check uniqueness mechanically, plus the
 *      "boundary" relationships (UNTERMINATED / TERMINATED / ERROR) that
 *      code all over the tree tests with < and >.
 *
 *   2. The module contract. Every component fills in the same vtable and
 *      the selected one is copied wholesale into prte_plm, so a NULL slot
 *      is a crash at first use. The default (local-only) module and the
 *      ssh module -- the always-built fallback -- are checked here.
 *
 *   3. prte_plm_base_wrap_args: quotes the value of a "...mca" option so
 *      launchers/shells do not split multi-word values.
 *
 *   4. prte_plm_base_setup_prted_cmd: splits prte_launch_agent and reports
 *      where the "prted" word landed, which is what lets a wrapper
 *      ("valgrind ... prted") be handled.
 *
 *   5. prte_plm_base_prted_append_basic_args: the daemon command line.
 *      This is the one every launcher hands to ssh/srun/aprun/lsb_launch,
 *      so its content matters: the vpid template must be recorded, the
 *      frameworks the daemons must not open (rmaps/ras/plm) must be
 *      dropped, MCA values must survive intact (including values that
 *      themselves contain '='), and the pass_environ_mca_params opt-out
 *      must actually suppress the environment-derived params.
 *
 *   6. prte_plm_base_set_hnp_name / prte_plm_base_create_jobid: the DVM
 *      base nspace and the monotonic "base@N" job nspaces derived from it.
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/mca/base/pmix_base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/proc_info.h"

#include "src/mca/rmaps/rmaps_types.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/plm_types.h"
#include "src/mca/plm/ssh/plm_ssh.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* find "needle" in an argv array, returning its index or -1 */
static int idx_of(char **argv, const char *needle)
{
    int i;

    for (i = 0; NULL != argv && NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], needle)) {
            return i;
        }
    }
    return -1;
}

/* does argv contain the three-word sequence <flag> <name> <value>? */
static bool has_mca(char **argv, const char *flag, const char *name, const char *value)
{
    int i;

    for (i = 0; NULL != argv && NULL != argv[i] && NULL != argv[i + 1] && NULL != argv[i + 2];
         i++) {
        if (0 == strcmp(argv[i], flag) &&
            0 == strcmp(argv[i + 1], name) &&
            0 == strcmp(argv[i + 2], value)) {
            return true;
        }
    }
    return false;
}

/* is "name" present as the value of any --prtemca/--pmixmca option? */
static bool has_mca_name(char **argv, const char *name)
{
    int i;

    for (i = 0; NULL != argv && NULL != argv[i] && NULL != argv[i + 1]; i++) {
        if ((0 == strcmp(argv[i], "--prtemca") || 0 == strcmp(argv[i], "--pmixmca")) &&
            0 == strcmp(argv[i + 1], name)) {
            return true;
        }
    }
    return false;
}

/*
 * The state codes are hand-assigned offsets and MUST stay unique within
 * their family: a collision makes two distinct states compare equal, which
 * is a miserable bug to track down at runtime.
 */
static int test_state_code_uniqueness(void)
{
    int failures = 0;
    size_t i, j;

    int32_t jobstates[] = {
        PRTE_JOB_STATE_UNDEF, PRTE_JOB_STATE_INIT, PRTE_JOB_STATE_INIT_COMPLETE,
        PRTE_JOB_STATE_ALLOCATE, PRTE_JOB_STATE_ALLOCATION_COMPLETE, PRTE_JOB_STATE_MAP,
        PRTE_JOB_STATE_MAP_COMPLETE, PRTE_JOB_STATE_SYSTEM_PREP, PRTE_JOB_STATE_LAUNCH_DAEMONS,
        PRTE_JOB_STATE_DAEMONS_LAUNCHED, PRTE_JOB_STATE_DAEMONS_REPORTED, PRTE_JOB_STATE_VM_READY,
        PRTE_JOB_STATE_LAUNCH_APPS, PRTE_JOB_STATE_SEND_LAUNCH_MSG, PRTE_JOB_STATE_RUNNING,
        PRTE_JOB_STATE_SUSPENDED, PRTE_JOB_STATE_REGISTERED, PRTE_JOB_STATE_WAITING_FOR_DAEMONS,
        PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE, PRTE_JOB_STATE_READY_FOR_DEBUG,
        PRTE_JOB_STATE_STARTED, PRTE_JOB_STATE_UNTERMINATED, PRTE_JOB_STATE_TERMINATED,
        PRTE_JOB_STATE_ALL_JOBS_COMPLETE, PRTE_JOB_STATE_DAEMONS_TERMINATED,
        PRTE_JOB_STATE_NOTIFY_COMPLETED, PRTE_JOB_STATE_NOTIFIED, PRTE_JOB_STATE_ERROR,
        PRTE_JOB_STATE_KILLED_BY_CMD, PRTE_JOB_STATE_ABORTED, PRTE_JOB_STATE_FAILED_TO_START,
        PRTE_JOB_STATE_ABORTED_BY_SIG, PRTE_JOB_STATE_ABORTED_WO_SYNC, PRTE_JOB_STATE_COMM_FAILED,
        PRTE_JOB_STATE_SENSOR_BOUND_EXCEEDED, PRTE_JOB_STATE_CALLED_ABORT,
        PRTE_JOB_STATE_HEARTBEAT_FAILED, PRTE_JOB_STATE_NEVER_LAUNCHED,
        PRTE_JOB_STATE_ABORT_ORDERED, PRTE_JOB_STATE_NON_ZERO_TERM,
        PRTE_JOB_STATE_FAILED_TO_LAUNCH, PRTE_JOB_STATE_FORCED_EXIT,
        PRTE_JOB_STATE_SILENT_ABORT, PRTE_JOB_STATE_REPORT_PROGRESS,
        PRTE_JOB_STATE_ALLOC_FAILED, PRTE_JOB_STATE_MAP_FAILED, PRTE_JOB_STATE_CANNOT_LAUNCH,
        PRTE_JOB_STATE_FILES_POSN_FAILED, PRTE_JOB_STATE_FT_CHECKPOINT,
        PRTE_JOB_STATE_FT_CONTINUE, PRTE_JOB_STATE_FT_RESTART
    };
    int32_t procstates[] = {
        PRTE_PROC_STATE_UNDEF, PRTE_PROC_STATE_INIT, PRTE_PROC_STATE_RESTART,
        PRTE_PROC_STATE_TERMINATE, PRTE_PROC_STATE_RUNNING, PRTE_PROC_STATE_REGISTERED,
        PRTE_PROC_STATE_IOF_COMPLETE, PRTE_PROC_STATE_WAITPID_FIRED,
        PRTE_PROC_STATE_MODEX_READY, PRTE_PROC_STATE_READY_FOR_DEBUG,
        PRTE_PROC_STATE_UNTERMINATED, PRTE_PROC_STATE_TERMINATED, PRTE_PROC_STATE_ERROR,
        PRTE_PROC_STATE_KILLED_BY_CMD, PRTE_PROC_STATE_ABORTED, PRTE_PROC_STATE_FAILED_TO_START,
        PRTE_PROC_STATE_ABORTED_BY_SIG, PRTE_PROC_STATE_TERM_WO_SYNC,
        PRTE_PROC_STATE_COMM_FAILED, PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED,
        PRTE_PROC_STATE_CALLED_ABORT, PRTE_PROC_STATE_HEARTBEAT_FAILED,
        PRTE_PROC_STATE_MIGRATING, PRTE_PROC_STATE_CANNOT_RESTART,
        PRTE_PROC_STATE_TERM_NON_ZERO, PRTE_PROC_STATE_FAILED_TO_LAUNCH,
        PRTE_PROC_STATE_UNABLE_TO_SEND_MSG, PRTE_PROC_STATE_LIFELINE_LOST,
        PRTE_PROC_STATE_NO_PATH_TO_TARGET, PRTE_PROC_STATE_FAILED_TO_CONNECT,
        PRTE_PROC_STATE_PEER_UNKNOWN
    };
    int32_t nodestates[] = {
        PRTE_NODE_STATE_UNDEF, PRTE_NODE_STATE_UNKNOWN, PRTE_NODE_STATE_DOWN,
        PRTE_NODE_STATE_UP, PRTE_NODE_STATE_REBOOT, PRTE_NODE_STATE_DO_NOT_USE,
        PRTE_NODE_STATE_NOT_INCLUDED, PRTE_NODE_STATE_ADDED
    };
    int32_t appstates[] = {
        PRTE_APP_STATE_UNDEF, PRTE_APP_STATE_INIT, PRTE_APP_STATE_ALL_MAPPED,
        PRTE_APP_STATE_RUNNING, PRTE_APP_STATE_COMPLETED
    };
    int32_t cmds[] = {
        PRTE_PLM_LAUNCH_JOB_CMD, PRTE_PLM_UPDATE_PROC_STATE, PRTE_PLM_REGISTERED_CMD,
        PRTE_PLM_TOOL_ATTACHED_CMD, PRTE_PLM_READY_FOR_DEBUG_CMD, PRTE_PLM_LOCAL_LAUNCH_COMP_CMD
    };
    struct {
        const char *name;
        int32_t *vals;
        size_t n;
    } families[] = {
        {"job state", jobstates, sizeof(jobstates) / sizeof(jobstates[0])},
        {"proc state", procstates, sizeof(procstates) / sizeof(procstates[0])},
        {"node state", nodestates, sizeof(nodestates) / sizeof(nodestates[0])},
        {"app state", appstates, sizeof(appstates) / sizeof(appstates[0])},
        {"plm cmd", cmds, sizeof(cmds) / sizeof(cmds[0])}
    };
    size_t f;

    for (f = 0; f < sizeof(families) / sizeof(families[0]); f++) {
        for (i = 0; i < families[f].n; i++) {
            for (j = i + 1; j < families[f].n; j++) {
                if (families[f].vals[i] == families[f].vals[j]) {
                    fprintf(stderr, "FAIL [%s]: duplicate value %d at entries %d and %d\n",
                            families[f].name, (int) families[f].vals[i], (int) i, (int) j);
                    failures++;
                }
            }
        }
    }

    /* the boundaries are load-bearing: code all over the tree decides
     * "still running?" and "abnormal?" by comparing against them */
    CHECK("job running states below UNTERMINATED",
          PRTE_JOB_STATE_STARTED < PRTE_JOB_STATE_UNTERMINATED &&
          PRTE_JOB_STATE_RUNNING < PRTE_JOB_STATE_UNTERMINATED &&
          PRTE_JOB_STATE_READY_FOR_DEBUG < PRTE_JOB_STATE_UNTERMINATED);
    CHECK("job TERMINATED above UNTERMINATED",
          PRTE_JOB_STATE_UNTERMINATED < PRTE_JOB_STATE_TERMINATED);
    CHECK("job errors above ERROR",
          PRTE_JOB_STATE_TERMINATED < PRTE_JOB_STATE_ERROR &&
          PRTE_JOB_STATE_ERROR < PRTE_JOB_STATE_FAILED_TO_START &&
          PRTE_JOB_STATE_ERROR < PRTE_JOB_STATE_NEVER_LAUNCHED);
    CHECK("proc running states below UNTERMINATED",
          PRTE_PROC_STATE_RUNNING < PRTE_PROC_STATE_UNTERMINATED &&
          PRTE_PROC_STATE_REGISTERED < PRTE_PROC_STATE_UNTERMINATED);
    CHECK("proc TERMINATED above UNTERMINATED",
          PRTE_PROC_STATE_UNTERMINATED < PRTE_PROC_STATE_TERMINATED);
    CHECK("proc errors above ERROR",
          PRTE_PROC_STATE_TERMINATED < PRTE_PROC_STATE_ERROR &&
          PRTE_PROC_STATE_ERROR < PRTE_PROC_STATE_FAILED_TO_START &&
          PRTE_PROC_STATE_ERROR < PRTE_PROC_STATE_COMM_FAILED);
    /* external developers key off these starting points */
    CHECK("job DYNAMIC above every defined job state",
          PRTE_JOB_STATE_FT_RESTART < PRTE_JOB_STATE_DYNAMIC);
    CHECK("proc DYNAMIC above every defined proc state",
          PRTE_PROC_STATE_PEER_UNKNOWN < PRTE_PROC_STATE_DYNAMIC);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_state_code_uniqueness\n");
    }
    return failures;
}

/*
 * The selected module is copied wholesale into prte_plm and called without
 * NULL checks, so every slot a launcher relies on must be wired. Only
 * remote_spawn is genuinely optional (ssh alone implements the tree
 * fan-out) and finalize is optional for the default module.
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
static prte_plm_base_module_t *plm_module(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    int pri = 0;

    PMIX_LIST_FOREACH(cli, &prte_plm_base_framework.framework_components,
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
        return (prte_plm_base_module_t *) mod;
    }
    return NULL;
}

static int test_module_contract(void)
{
    int failures = 0;

    /* the default "local only" module installed by the framework */
    CHECK("default init set", NULL != prte_plm.init);
    CHECK("default set_hnp_name set", NULL != prte_plm.set_hnp_name);
    CHECK("default spawn set", NULL != prte_plm.spawn);
    CHECK("default terminate_job set", NULL != prte_plm.terminate_job);
    CHECK("default terminate_orteds set", NULL != prte_plm.terminate_orteds);
    CHECK("default terminate_procs set", NULL != prte_plm.terminate_procs);
    CHECK("default signal_job set", NULL != prte_plm.signal_job);
    CHECK("default set_hnp_name identity",
          prte_plm_base_set_hnp_name == prte_plm.set_hnp_name);
    CHECK("default terminate_job identity",
          prte_plm_base_prted_terminate_job == prte_plm.terminate_job);
    CHECK("default terminate_procs identity",
          prte_plm_base_prted_kill_local_procs == prte_plm.terminate_procs);
    CHECK("default signal_job identity",
          prte_plm_base_prted_signal_local_procs == prte_plm.signal_job);

    /* ssh: the always-available fallback and the only tree-spawner.  Its
     * query is environment-sensitive (it declines when an indicated launch
     * agent is missing), so a NULL module here is an answer, not a fault. */
    prte_plm_base_module_t *ssh = plm_module("ssh");
    if (NULL == ssh) {
        fprintf(stdout, "  (skipping ssh module checks: component declined)\n");
        goto ssh_done;
    }
    CHECK("ssh init set", NULL != ssh->init);
    CHECK("ssh set_hnp_name set", NULL != ssh->set_hnp_name);
    CHECK("ssh spawn set", NULL != ssh->spawn);
    CHECK("ssh remote_spawn set", NULL != ssh->remote_spawn);
    CHECK("ssh terminate_job set", NULL != ssh->terminate_job);
    CHECK("ssh terminate_orteds set", NULL != ssh->terminate_orteds);
    CHECK("ssh terminate_procs set", NULL != ssh->terminate_procs);
    CHECK("ssh signal_job set", NULL != ssh->signal_job);
    CHECK("ssh finalize set", NULL != ssh->finalize);
    CHECK("ssh set_hnp_name identity",
          prte_plm_base_set_hnp_name == ssh->set_hnp_name);
ssh_done:

    /* the base defaults every component inherits */
    CHECK("nodes assigned at launch defaults true",
          prte_plm_globals.daemon_nodes_assigned_at_launch);
    CHECK("environ mca params passed by default", prte_plm_globals.pass_environ_mca_params);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_module_contract\n");
    }
    return failures;
}

/*
 * wrap_args quotes the VALUE of any option whose name ends in "mca" so a
 * multi-word value survives the trip through a shell or a launcher.
 */
static int test_wrap_args(void)
{
    int failures = 0;
    char **argv = NULL;

    PMIx_Argv_append_nosize(&argv, "prted");
    PMIx_Argv_append_nosize(&argv, "--prtemca");
    PMIx_Argv_append_nosize(&argv, "prte_base_help_aggregate");
    PMIx_Argv_append_nosize(&argv, "one two");
    PMIx_Argv_append_nosize(&argv, "--pmixmca");
    PMIx_Argv_append_nosize(&argv, "pmix_foo");
    PMIx_Argv_append_nosize(&argv, "\"already quoted\"");
    PMIx_Argv_append_nosize(&argv, "--leave-session-attached");

    prte_plm_base_wrap_args(argv);

    CHECK("value after --prtemca is quoted", 0 == strcmp(argv[3], "\"one two\""));
    CHECK("mca option name untouched", 0 == strcmp(argv[2], "prte_base_help_aggregate"));
    CHECK("already-quoted value left alone", 0 == strcmp(argv[6], "\"already quoted\""));
    CHECK("non-mca args untouched", 0 == strcmp(argv[0], "prted"));
    CHECK("trailing non-mca arg untouched",
          0 == strcmp(argv[7], "--leave-session-attached"));
    PMIx_Argv_free(argv);

    /* a truncated tail must not run off the end of the array */
    argv = NULL;
    PMIx_Argv_append_nosize(&argv, "--prtemca");
    PMIx_Argv_append_nosize(&argv, "dangling");
    prte_plm_base_wrap_args(argv);
    CHECK("dangling mca option survives", 0 == strcmp(argv[1], "dangling"));
    PMIx_Argv_free(argv);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_wrap_args\n");
    }
    return failures;
}

/*
 * setup_prted_cmd splits the launch agent and returns the index of the
 * "prted" word, which is what lets ssh separate a wrapper prefix
 * ("valgrind --tool=memcheck prted") from the daemon command itself.
 */
static int test_setup_prted_cmd(void)
{
    int failures = 0;
    char *save = prte_launch_agent;
    char **argv;
    int argc, loc;

    prte_launch_agent = "prted";
    argv = NULL;
    argc = 0;
    loc = prte_plm_base_setup_prted_cmd(&argc, &argv);
    CHECK("plain prted -> index 0", 0 == loc);
    CHECK("plain prted -> one word", 1 == argc && 0 == strcmp(argv[0], "prted"));
    PMIx_Argv_free(argv);

    prte_launch_agent = "valgrind --tool=memcheck prted";
    argv = NULL;
    argc = 0;
    loc = prte_plm_base_setup_prted_cmd(&argc, &argv);
    CHECK("wrapped prted -> index of prted", 2 == loc);
    CHECK("wrapped prted -> all words kept", 3 == argc);
    CHECK("wrapped prted -> wrapper first", 0 == strcmp(argv[0], "valgrind"));
    CHECK("wrapped prted -> prted at loc", 0 == strcmp(argv[loc], "prted"));
    PMIx_Argv_free(argv);

    /* a custom daemon: no "prted" word at all, so nothing to prefix */
    prte_launch_agent = "my_daemon";
    argv = NULL;
    argc = 0;
    loc = prte_plm_base_setup_prted_cmd(&argc, &argv);
    CHECK("custom daemon -> index 0", 0 == loc);
    CHECK("custom daemon -> kept verbatim", 0 == strcmp(argv[0], "my_daemon"));
    PMIx_Argv_free(argv);

    prte_launch_agent = save;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_setup_prted_cmd\n");
    }
    return failures;
}

/*
 * The daemon command line every launcher hands to its launch mechanism.
 * We drive it as a non-master so it reads the daemon count from
 * prte_process_info rather than needing a daemon job object.
 */
static int test_append_basic_args(void)
{
    int failures = 0;
    char **argv = NULL;
    int argc = 0, vpid_index = -1;
    prte_proc_type_t save_type = prte_process_info.proc_type;
    char *save_uri = prte_process_info.my_hnp_uri;

    PRTE_PROC_MY_NAME->rank = 0;
    prte_process_info.proc_type = PRTE_PROC_DAEMON;
    prte_process_info.num_daemons = 7;
    prte_process_info.my_hnp_uri = strdup("prte-test@0.0;tcp://10.0.0.1:1234");

    /* a normal param, one whose VALUE contains '=', and three that must be
     * dropped because the daemons must not re-run mapping/allocation or
     * open a PLM they were not told to open */
    setenv("PRTE_MCA_plm_base_verbose", "5", 1);
    setenv("PRTE_MCA_prte_test_kv", "a=b", 1);
    setenv("PRTE_MCA_rmaps_base_mapping_policy", "byslot", 1);
    setenv("PRTE_MCA_ras_base_verbose", "5", 1);
    setenv("PRTE_MCA_plm", "ssh", 1);

    CHECK("append_basic_args succeeds",
          PRTE_SUCCESS == prte_plm_base_prted_append_basic_args(&argc, &argv, "env", &vpid_index));

    /* the vpid slot must be recorded so the launcher can substitute the
     * real vpid per node */
    CHECK("vpid index recorded", 0 <= vpid_index && vpid_index < argc);
    if (0 <= vpid_index && vpid_index < argc) {
        CHECK("vpid slot holds the template", 0 == strcmp(argv[vpid_index], "<template>"));
        CHECK("vpid slot follows ess_base_vpid",
              0 == strcmp(argv[vpid_index - 1], "ess_base_vpid"));
    }

    CHECK("ess module passed", has_mca(argv, "--prtemca", "ess", "env"));
    CHECK("daemon nspace passed",
          has_mca(argv, "--prtemca", "ess_base_nspace", prte_process_info.myproc.nspace));
    CHECK("daemon count passed", has_mca(argv, "--prtemca", "ess_base_num_procs", "7"));
    CHECK("hnp uri passed",
          has_mca(argv, "--prtemca", "prte_hnp_uri", prte_process_info.my_hnp_uri));

    /* environment-derived params: forwarded, with the value intact */
    CHECK("environ mca param forwarded",
          has_mca(argv, "--prtemca", "plm_base_verbose", "5"));
    CHECK("mca value containing '=' is not truncated",
          has_mca(argv, "--prtemca", "prte_test_kv", "a=b"));

    /* ... except the frameworks the daemons must never open */
    CHECK("rmaps param dropped", !has_mca_name(argv, "rmaps_base_mapping_policy"));
    CHECK("ras param dropped", !has_mca_name(argv, "ras_base_verbose"));
    CHECK("plm selection dropped", !has_mca_name(argv, "plm"));

    PMIx_Argv_free(argv);

    /* the documented remedy for an over-long command line: drop the
     * environment-derived params entirely, keeping everything else */
    prte_plm_globals.pass_environ_mca_params = false;
    argv = NULL;
    argc = 0;
    vpid_index = -1;
    CHECK("append_basic_args succeeds (no environ params)",
          PRTE_SUCCESS == prte_plm_base_prted_append_basic_args(&argc, &argv, "env", &vpid_index));
    CHECK("environ mca param suppressed", !has_mca_name(argv, "plm_base_verbose"));
    CHECK("required params still present", has_mca(argv, "--prtemca", "ess", "env"));
    CHECK("vpid slot still recorded", 0 <= vpid_index && 0 <= idx_of(argv, "<template>"));
    PMIx_Argv_free(argv);
    prte_plm_globals.pass_environ_mca_params = true;

    unsetenv("PRTE_MCA_plm_base_verbose");
    unsetenv("PRTE_MCA_prte_test_kv");
    unsetenv("PRTE_MCA_rmaps_base_mapping_policy");
    unsetenv("PRTE_MCA_ras_base_verbose");
    unsetenv("PRTE_MCA_plm");
    free(prte_process_info.my_hnp_uri);
    prte_process_info.my_hnp_uri = save_uri;
    prte_process_info.proc_type = save_type;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_append_basic_args\n");
    }
    return failures;
}

/*
 * The DVM's base nspace and the "base@N" job nspaces derived from it. Every
 * job in the DVM -- and the routing that finds it -- keys off these names.
 */
static int test_naming(void)
{
    int failures = 0;
    char *save_base = prte_plm_globals.base_nspace;
    uint32_t save_next = prte_plm_globals.next_jobid;
    pmix_proc_t save_me, save_hnp;
    prte_job_t *jdata;
    char expect[PMIX_MAX_NSLEN + 1];

    memcpy(&save_me, PRTE_PROC_MY_NAME, sizeof(pmix_proc_t));
    memcpy(&save_hnp, PRTE_PROC_MY_HNP, sizeof(pmix_proc_t));

    /* create_jobid registers the job, so we need the global job array that
     * a full prte_init() would have built for us */
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                PRTE_GLOBAL_ARRAY_MAX_SIZE, PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    }

    /* set_hnp_name honors an inherited PMIx server nspace */
    setenv("PMIX_SERVER_NSPACE", "inherited-nspace", 1);
    prte_plm_globals.base_nspace = NULL;
    CHECK("set_hnp_name (inherited) succeeds", PRTE_SUCCESS == prte_plm_base_set_hnp_name());
    CHECK("inherited nspace adopted",
          NULL != prte_plm_globals.base_nspace &&
          0 == strcmp(prte_plm_globals.base_nspace, "inherited-nspace"));
    CHECK("inherited nspace is my nspace",
          0 == strcmp(PRTE_PROC_MY_NAME->nspace, "inherited-nspace"));
    CHECK("HNP is me", 0 == strcmp(PRTE_PROC_MY_HNP->nspace, PRTE_PROC_MY_NAME->nspace) &&
                       PRTE_PROC_MY_HNP->rank == PRTE_PROC_MY_NAME->rank);
    free(prte_plm_globals.base_nspace);
    unsetenv("PMIX_SERVER_NSPACE");

    /* otherwise it derives one and hangs the HNP off it at "@0" */
    prte_plm_globals.base_nspace = strdup("prte-testhost-1234");
    CHECK("set_hnp_name succeeds", PRTE_SUCCESS == prte_plm_base_set_hnp_name());
    CHECK("HNP nspace is base@0",
          0 == strcmp(PRTE_PROC_MY_NAME->nspace, "prte-testhost-1234@0"));
    CHECK("HNP rank is 0", 0 == PRTE_PROC_MY_NAME->rank);
    CHECK("HNP field mirrors my name",
          0 == strcmp(PRTE_PROC_MY_HNP->nspace, PRTE_PROC_MY_NAME->nspace));

    /* jobids are handed out monotonically from the same base */
    prte_plm_globals.next_jobid = 1;
    jdata = PMIX_NEW(prte_job_t);
    CHECK("create_jobid succeeds", PRTE_SUCCESS == prte_plm_base_create_jobid(jdata));
    snprintf(expect, sizeof(expect), "%s@1", prte_plm_globals.base_nspace);
    CHECK("first jobid is base@1", 0 == strcmp(jdata->nspace, expect));
    CHECK("job is registered", jdata == prte_get_job_data_object(jdata->nspace));
    CHECK("next_jobid advanced", 2 == prte_plm_globals.next_jobid);

    /* a restarting job keeps the nspace it already has */
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_RESTART);
    CHECK("create_jobid on restart succeeds", PRTE_SUCCESS == prte_plm_base_create_jobid(jdata));
    CHECK("restart kept its nspace", 0 == strcmp(jdata->nspace, expect));
    CHECK("restart did not consume a jobid", 2 == prte_plm_globals.next_jobid);
    PRTE_FLAG_UNSET(jdata, PRTE_JOB_FLAG_RESTART);

    free(prte_plm_globals.base_nspace);
    prte_plm_globals.base_nspace = save_base;
    prte_plm_globals.next_jobid = save_next;
    memcpy(PRTE_PROC_MY_NAME, &save_me, sizeof(pmix_proc_t));
    memcpy(PRTE_PROC_MY_HNP, &save_hnp, sizeof(pmix_proc_t));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_naming\n");
    }
    return failures;
}

/*
 * Stand up a synthetic prte_local_children array.  The report packers read
 * that global to decide which procs belong in the message, so a test can
 * pose the question by populating it directly.
 */
static prte_proc_t *add_child(const char *nspace, pmix_rank_t rank, bool alive)
{
    prte_proc_t *p = PMIX_NEW(prte_proc_t);

    PMIX_LOAD_PROCID(&p->name, nspace, rank);
    p->pid = 1000 + rank;
    p->state = alive ? PRTE_PROC_STATE_RUNNING : PRTE_PROC_STATE_TERMINATED;
    p->exit_code = alive ? 0 : (int32_t) rank;
    if (alive) {
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_ALIVE);
    }
    pmix_pointer_array_add(prte_local_children, p);
    return p;
}

static void reset_children(void)
{
    int i;
    prte_proc_t *p;

    if (NULL == prte_local_children) {
        prte_local_children = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_local_children, 8, INT32_MAX, 8);
        return;
    }
    for (i = 0; i < prte_local_children->size; i++) {
        p = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL != p) {
            pmix_pointer_array_set_item(prte_local_children, i, NULL);
            PMIX_RELEASE(p);
        }
    }
}

/*
 * A daemon reports proc state to the HNP with a PRTE_PLM_UPDATE_PROC_STATE
 * message built by prte_plm_base_pack_state_{for_proc,update}.  Its only
 * reader is prte_plm_base_recv(), a few hundred lines from the writers; the
 * wire carries no format version, so the two must change together.  This
 * test *is* that reader - it repeats the receiver's unpack sequence
 * verbatim - so a field added, dropped or retyped on either side alone shows
 * up here rather than as a desynchronized buffer in a running DVM.
 *
 * Both callers are covered: errmgr/prted's failure reports, which pack every
 * local child of the job, and state/prted's normal-termination report, which
 * passes skip_reported so that a proc already flagged TERM_REPORTED is not
 * sent twice.
 */
static int unpack_and_check(pmix_data_buffer_t *bkt, const char *expect_nspace,
                            prte_proc_t **expect, int nexpect, bool terminated)
{
    int failures = 0, n = 0;
    pmix_nspace_t job;
    pmix_rank_t vpid;
    pid_t pid;
    uint32_t state;
    int32_t exit_code;
    int32_t count;
    pmix_status_t rc;

    count = 1;
    rc = PMIx_Data_unpack(NULL, bkt, &job, &count, PMIX_PROC_NSPACE);
    CHECK("wire: nspace unpacks", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        return failures;
    }
    CHECK("wire: nspace matches", PMIX_CHECK_NSPACE(job, expect_nspace));

    while (1) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &vpid, &count, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            /* running off the end is how a single-proc report ends */
            CHECK("wire: clean end of buffer", PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc);
            break;
        }
        if (PMIX_RANK_INVALID == vpid) {
            /* the terminator: the job is complete */
            CHECK("wire: terminator expected", terminated);
            terminated = false;
            break;
        }
        count = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &pid, &count, PMIX_PID);
        CHECK("wire: pid unpacks", PMIX_SUCCESS == rc);
        count = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &state, &count, PMIX_UINT32);
        CHECK("wire: state unpacks", PMIX_SUCCESS == rc);
        count = 1;
        rc = PMIx_Data_unpack(NULL, bkt, &exit_code, &count, PMIX_INT32);
        CHECK("wire: exit code unpacks", PMIX_SUCCESS == rc);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        if (n < nexpect) {
            CHECK("wire: rank round-trips", expect[n]->name.rank == vpid);
            CHECK("wire: pid round-trips", expect[n]->pid == pid);
            CHECK("wire: state round-trips", (uint32_t) expect[n]->state == state);
            CHECK("wire: exit code round-trips", expect[n]->exit_code == exit_code);
        }
        ++n;
    }
    CHECK("wire: proc count", n == nexpect);
    CHECK("wire: terminator consumed", !terminated);
    return failures;
}

static int test_state_update_wire(void)
{
    int failures = 0;
    pmix_data_buffer_t bkt;
    prte_proc_t *expect[3];
    prte_job_t *jdata;

    reset_children();

    /* jobA has two local children; jobB's child must not leak into jobA's
     * report */
    expect[0] = add_child("jobA", 0, true);
    add_child("jobB", 7, true);
    expect[1] = add_child("jobA", 1, false);
    expect[1]->state = PRTE_PROC_STATE_TERM_NON_ZERO;
    expect[1]->exit_code = 42;

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "jobA");

    /* a whole-job update: nspace, every local child of that job, terminator */
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    CHECK("pack state update", PRTE_SUCCESS == prte_plm_base_pack_state_update(&bkt, jdata, false));
    failures += unpack_and_check(&bkt, "jobA", expect, 2, true);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);

    /* a single-proc report: the nspace, then exactly the one proc that
     * failed - and no other.  This is the shape the CALLED_ABORT /
     * TERM_NON_ZERO / abnormal-termination paths send. */
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    CHECK("pack single nspace",
          PMIX_SUCCESS == PMIx_Data_pack(NULL, &bkt, &jdata->nspace, 1, PMIX_PROC_NSPACE));
    CHECK("pack single proc",
          PRTE_SUCCESS == prte_plm_base_pack_state_for_proc(&bkt, expect[1]));
    failures += unpack_and_check(&bkt, "jobA", &expect[1], 1, false);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);

    /* skip_reported is what the normal-termination path passes: a child
     * already flagged TERM_REPORTED must not be sent again, and the ones
     * that are sent must come back flagged so a second call sends nothing
     * but the nspace and the terminator */
    PRTE_FLAG_SET(expect[0], PRTE_PROC_FLAG_TERM_REPORTED);
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    CHECK("pack skip-reported",
          PRTE_SUCCESS == prte_plm_base_pack_state_update(&bkt, jdata, true));
    failures += unpack_and_check(&bkt, "jobA", &expect[1], 1, true);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);
    CHECK("packing flagged the proc it sent",
          PRTE_FLAG_TEST(expect[1], PRTE_PROC_FLAG_TERM_REPORTED));
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    CHECK("pack skip-reported again",
          PRTE_SUCCESS == prte_plm_base_pack_state_update(&bkt, jdata, true));
    failures += unpack_and_check(&bkt, "jobA", NULL, 0, true);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);

    /* a job with no local children still produces a well-formed, complete
     * report - nspace plus terminator */
    reset_children();
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    CHECK("pack empty update", PRTE_SUCCESS == prte_plm_base_pack_state_update(&bkt, jdata, false));
    failures += unpack_and_check(&bkt, "jobA", NULL, 0, true);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);

    PMIX_RELEASE(jdata);
    reset_children();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_state_update_wire\n");
    }
    return failures;
}

/*
 * prte_init_util() stops short of building the global job/node arrays --
 * that happens in prte_init(), which wants a live ESS, session directories
 * and a network stack.  Stand up just the pieces setup_virtual_machine
 * touches: the job pool with the daemon job in it, the node pool with the
 * HNP's own node at index 0, and the topology array.
 */
/* create the array if this process has none yet, else empty the one it has -
 * an earlier test in this binary may already have registered jobs, and they
 * must not be visible to (or leaked by) this one */
static void fresh_array(pmix_pointer_array_t **array)
{
    int i;
    void *item;

    if (NULL == *array) {
        *array = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(*array, 8, INT_MAX, 8);
        return;
    }
    for (i = 0; i < (*array)->size; i++) {
        item = pmix_pointer_array_get_item(*array, i);
        if (NULL != item) {
            pmix_pointer_array_set_item(*array, i, NULL);
            PMIX_RELEASE(item);
        }
    }
}

static int setup_vm_globals(const char *dvm_nspace)
{
    prte_job_t *djob;
    prte_node_t *hnp;

    fresh_array(&prte_job_data);
    fresh_array(&prte_node_pool);
    fresh_array(&prte_node_topologies);

    djob = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(djob->nspace, dvm_nspace);
    PMIX_LOAD_PROCID(PRTE_PROC_MY_NAME, dvm_nspace, 0);
    /* the HNP is daemon 0 and is already "launched" */
    djob->num_procs = 1;
    prte_set_job_data_object(djob);

    hnp = PMIX_NEW(prte_node_t);
    hnp->name = strdup("plm-test-hnp");
    hnp->state = PRTE_NODE_STATE_UP;
    hnp->slots = 1;
    hnp->index = pmix_pointer_array_add(prte_node_pool, hnp);
    PRTE_FLAG_SET(hnp, PRTE_NODE_FLAG_DAEMON_LAUNCHED);

    return (0 == hnp->index) ? PRTE_SUCCESS : PRTE_ERROR;
}

/* add a node to the pool in the given state */
static prte_node_t *pool_node(const char *name, prte_node_state_t state)
{
    prte_node_t *nd = PMIX_NEW(prte_node_t);

    nd->name = strdup(name);
    nd->state = state;
    nd->slots = 2;
    nd->index = pmix_pointer_array_add(prte_node_pool, nd);
    return nd;
}

/* a job that looks like a dynamic spawn: it has an originator, which is what
 * routes setup_virtual_machine down the "only absorb nodes this request
 * added" branch */
static prte_job_t *spawn_job(const char *nspace)
{
    prte_job_t *jdata = PMIX_NEW(prte_job_t);

    PMIX_LOAD_NSPACE(jdata->nspace, nspace);
    PMIX_LOAD_PROCID(&jdata->originator, "some-tool", 0);
    return jdata;
}

/*
 * prte_plm_base_setup_virtual_machine builds the DAEMON job's map, and that
 * map is persistent: it accumulates the DVM's nodes across every launch.
 * Two of its fields are not cumulative, though - they describe the launch
 * about to happen, and every component reads them the moment setup_vm
 * returns:
 *
 *   num_new_daemons    - whether to launch anything at all;
 *   daemon_vpid_start  - the base vpid slurm/lsf/pals substitute into the
 *                        prted command line, from which each daemon the RM
 *                        starts computes its own name.
 *
 * A stale daemon_vpid_start is invisible under ssh (which substitutes each
 * node's real vpid per node) and fatal under any RM launcher: the daemons of
 * a later add-host launch are told to claim vpids that already belong to
 * live daemons.  So check that each pass reports ITS OWN daemons.
 */
static int test_setup_vm(void)
{
    int failures = 0;
    prte_job_t *daemons, *jdata;
    prte_job_map_t *map;
    prte_node_t *n1, *n2, *n3;
    pmix_proc_t save_me;
    pmix_rank_t first_start;

    memcpy(&save_me, PRTE_PROC_MY_NAME, sizeof(pmix_proc_t));

    if (PRTE_SUCCESS != setup_vm_globals("plm-test-dvm")) {
        fprintf(stderr, "FAIL [setup_vm]: could not build globals\n");
        memcpy(PRTE_PROC_MY_NAME, &save_me, sizeof(pmix_proc_t));
        return 1;
    }
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* two more nodes in the allocation.  The first pass through a dynamic
     * spawn has no map yet, so it absorbs the whole pool - the same shape as
     * initial DVM formation */
    n1 = pool_node("plm-test-n1", PRTE_NODE_STATE_UP);
    n2 = pool_node("plm-test-n2", PRTE_NODE_STATE_UP);

    jdata = spawn_job("plm-test-job1");
    CHECK("setup_vm first pass succeeds", PRTE_SUCCESS == prte_plm_base_setup_virtual_machine(jdata));
    map = daemons->map;
    CHECK("first pass has a map", NULL != map);
    if (NULL == map) {
        goto done;
    }
    CHECK("first pass launches both nodes", 2 == map->num_new_daemons);
    CHECK("first pass daemons got vpids", NULL != n1->daemon && NULL != n2->daemon);
    first_start = map->daemon_vpid_start;
    CHECK("first pass start vpid is the first new daemon",
          NULL != n1->daemon && first_start == n1->daemon->name.rank);
    CHECK("first pass start vpid is not INVALID", PMIX_RANK_INVALID != first_start);
    CHECK("daemon job spans the new vpids", 3 == daemons->num_procs);
    PMIX_RELEASE(jdata);

    /* Now a SECOND launch that brings in one more node - the add-host /
     * elastic-grow shape.  Its daemon takes the next free vpid, and that is
     * what the launcher must be told.  Before this was fixed, the map still
     * carried the first pass's start vpid and an RM launcher would have
     * started the new daemon as vpid 1 - a rank a live daemon already owns. */
    n3 = pool_node("plm-test-n3", PRTE_NODE_STATE_ADDED);
    jdata = spawn_job("plm-test-job2");
    CHECK("setup_vm second pass succeeds",
          PRTE_SUCCESS == prte_plm_base_setup_virtual_machine(jdata));
    CHECK("second pass launches only the added node", 1 == map->num_new_daemons);
    CHECK("second pass gave the new node a daemon", NULL != n3->daemon);
    CHECK("second pass start vpid is the NEW daemon",
          NULL != n3->daemon && map->daemon_vpid_start == n3->daemon->name.rank);
    CHECK("second pass start vpid is not the first pass's",
          map->daemon_vpid_start != first_start);
    CHECK("daemon job spans the new vpid", 4 == daemons->num_procs);
    PMIX_RELEASE(jdata);

    /* A third pass with nothing new: the fast path every component keys off.
     * num_new_daemons must be zero AND the start vpid must not be left
     * pointing at the previous launch's daemons. */
    jdata = spawn_job("plm-test-job3");
    CHECK("setup_vm third pass succeeds",
          PRTE_SUCCESS == prte_plm_base_setup_virtual_machine(jdata));
    CHECK("third pass launches nothing", 0 == map->num_new_daemons);
    CHECK("third pass start vpid is cleared", PMIX_RANK_INVALID == map->daemon_vpid_start);
    CHECK("third pass marks the daemons reported",
          PRTE_JOB_STATE_DAEMONS_REPORTED == daemons->state);
    PMIX_RELEASE(jdata);

    /* a job pinned to a fixed DVM never launches a daemon, whatever the pool
     * looks like */
    pool_node("plm-test-n4", PRTE_NODE_STATE_ADDED);
    jdata = spawn_job("plm-test-job4");
    prte_set_attribute(&jdata->attributes, PRTE_JOB_FIXED_DVM, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
    CHECK("setup_vm fixed-dvm succeeds",
          PRTE_SUCCESS == prte_plm_base_setup_virtual_machine(jdata));
    CHECK("fixed dvm launches nothing", 0 == map->num_new_daemons);
    CHECK("fixed dvm start vpid is cleared", PMIX_RANK_INVALID == map->daemon_vpid_start);
    PMIX_RELEASE(jdata);

done:
    memcpy(PRTE_PROC_MY_NAME, &save_me, sizeof(pmix_proc_t));
    if (0 == failures) {
        fprintf(stdout, "PASSED test_setup_vm\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;
    pmix_status_t prc;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* the module tests ask the framework for their subject, so it has to
     * be open before they run */
    /* the state-update report packs a buffer, and PMIx_Data_pack refuses to
     * run until PMIx itself is up.  A daemon reaches that state through
     * PMIx_server_init, so do the same */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        prte_finalize();
        return 1;
    }

    rc = pmix_mca_base_framework_open(&prte_plm_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "plm framework open failed: %d\n", rc);
        PMIx_server_finalize();
        prte_finalize();
        return 1;
    }

    failures += test_state_code_uniqueness();
    failures += test_module_contract();
    failures += test_wrap_args();
    failures += test_setup_prted_cmd();
    failures += test_append_basic_args();
    failures += test_naming();
    failures += test_state_update_wire();
    /* leaves the global job/node pools populated, so run it last */
    failures += test_setup_vm();

    (void) pmix_mca_base_framework_close(&prte_plm_base_framework);
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all plm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d plm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
