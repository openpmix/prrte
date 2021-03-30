/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif

#include "src/mca/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/prte_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "plm_alps.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Local functions
 */
static int plm_alps_init(void);
static int plm_alps_launch_job(prte_job_t *jdata);
static int plm_alps_terminate_orteds(void);
static int plm_alps_signal_job(pmix_nspace_t jobid, int32_t signal);
static int plm_alps_finalize(void);

static int plm_alps_start_proc(int argc, char **argv, char **env, char *prefix);

/*
 * Global variable
 */
prte_plm_base_module_t prte_plm_alps_module = {plm_alps_init,
                                               prte_plm_base_set_hnp_name,
                                               plm_alps_launch_job,
                                               NULL,
                                               prte_plm_base_prted_terminate_job,
                                               plm_alps_terminate_orteds,
                                               prte_plm_base_prted_kill_local_procs,
                                               plm_alps_signal_job,
                                               plm_alps_finalize};

/*
 * Local variables
 */
static prte_proc_t *alpsrun = NULL;
static bool failed_launch;
static void launch_daemons(int fd, short args, void *cbdata);

/**
 * Init the module
 */
static int plm_alps_init(void)
{
    int rc;
    prte_job_t *daemons;

    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* must map daemons since we won't be launching them */
        prte_plm_globals.daemon_nodes_assigned_at_launch = true;
    } else {
        /* we do NOT assign daemons to nodes at launch - we will
         * determine that mapping when the daemon
         * calls back. This is required because alps does
         * its own mapping of proc-to-node, and we cannot know
         * in advance which daemon will wind up on which node
         */
        prte_plm_globals.daemon_nodes_assigned_at_launch = false;
    }

    /* point to our launch command */
    if (PRTE_SUCCESS
        != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons,
                                          PRTE_SYS_PRI))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return rc;
}

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that prun will be woken up and
 * the job can cleanly terminate
 */
static int plm_alps_launch_job(prte_job_t *jdata)
{

    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        /* this is a restart situation - skip to the mapping stage */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    } else {
        /* new job - set it up */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_INIT);
    }
    return PRTE_SUCCESS;
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prte_job_map_t *map;
    char *param;
    char **argv = NULL;
    int argc;
    int rc;
    char *tmp;
    char **env = NULL;
    char *nodelist_flat;
    char **nodelist_argv;
    int nodelist_argc;
    char *vpid_string;
    char **custom_strings;
    int num_args, i;
    char *cur_prefix;
    int proc_vpid_index;
    prte_app_context_t *app;
    prte_node_t *node;
    int32_t nnode;
    prte_job_t *daemons;
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(state);

    /* if we are launching debugger daemons, then just go
     * do it - no new daemons will be launched
     */
    if (PRTE_FLAG_TEST(state->jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    /* start by setting up the virtual machine */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (PRTE_SUCCESS != (rc = prte_plm_base_setup_virtual_machine(state->jdata))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if we don't want to launch, then don't attempt to
     * launch the daemons - the user really wants to just
     * look at the proposed process map
     */
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    /* Get the map for this job */
    if (NULL == (map = daemons->map)) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (0 == map->num_new_daemons) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:alps: no new daemons to launch",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    /*
     * start building argv array
     */
    argv = NULL;
    argc = 0;

    /*
     * ALPS aprun  OPTIONS
     */

    /* protect against launchers that forward the entire environment */
    if (NULL != getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL")) {
        unsetenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL");
    }
    if (NULL != getenv("PMIX_LAUNCHER_RENDEZVOUS_FILE")) {
        unsetenv("PMIX_LAUNCHER_RENDEZVOUS_FILE");
    }

    /* add the aprun command */
    prte_argv_append(&argc, &argv, prte_plm_alps_component.aprun_cmd);

    /* Append user defined arguments to aprun */
    if (NULL != prte_plm_alps_component.custom_args) {
        custom_strings = prte_argv_split(prte_plm_alps_component.custom_args, ' ');
        num_args = prte_argv_count(custom_strings);
        for (i = 0; i < num_args; ++i) {
            prte_argv_append(&argc, &argv, custom_strings[i]);
        }
        prte_argv_free(custom_strings);
    }

    /* number of processors needed */
    prte_argv_append(&argc, &argv, "-n");
    prte_asprintf(&tmp, "%lu", (unsigned long) map->num_new_daemons);
    prte_argv_append(&argc, &argv, tmp);
    free(tmp);
    prte_argv_append(&argc, &argv, "-N");
    prte_argv_append(&argc, &argv, "1");
    prte_argv_append(&argc, &argv, "-cc");
    prte_argv_append(&argc, &argv, "none");
    /*
     * stuff below is necessary in the event that we've sadly configured PRTE with --disable-dlopen,
     * which results in the orted's being linked against all kinds of unnecessary cray libraries,
     * including the cray pmi, which has a ctor that cause bad things if run when using mpirun/orted
     * based launch.
     *
     * Code below adds env. variables for aprun to forward which suppresses the action of the Cray
     * PMI ctor.
     */
    prte_argv_append(&argc, &argv, "-e");
    prte_argv_append(&argc, &argv, "PMI_NO_PREINITIALIZE=1");
    prte_argv_append(&argc, &argv, "-e");
    prte_argv_append(&argc, &argv, "PMI_NO_FORK=1");
    prte_argv_append(&argc, &argv, "-e");
    prte_argv_append(&argc, &argv, "OMPI_NO_USE_CRAY_PMI=1");

    /* if we are using all allocated nodes, then alps
     * doesn't need a nodelist, or if running without a batch scheduler
     */
    if ((map->num_new_daemons < prte_num_allocated_nodes) || (prte_num_allocated_nodes == 0)) {
        /* create nodelist */
        nodelist_argv = NULL;
        nodelist_argc = 0;

        for (nnode = 0; nnode < map->nodes->size; nnode++) {
            if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, nnode))) {
                continue;
            }

            /* if the daemon already exists on this node, then
             * don't include it
             */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
                continue;
            }

            /* otherwise, add it to the list of nodes upon which
             * we need to launch a daemon
             */
            prte_argv_append(&nodelist_argc, &nodelist_argv, node->name);
        }
        if (0 == prte_argv_count(nodelist_argv)) {
            prte_show_help("help-plm-alps.txt", "no-hosts-in-list", true);
            rc = PRTE_ERR_FAILED_TO_START;
            goto cleanup;
        }
        nodelist_flat = prte_argv_join(nodelist_argv, ',');
        prte_argv_free(nodelist_argv);

        prte_argv_append(&argc, &argv, "-L");
        prte_argv_append(&argc, &argv, nodelist_flat);
        free(nodelist_flat);
    }

    /*
     * PRTED OPTIONS
     */

    /* add the daemon command (as specified by user) */
    prte_plm_base_setup_prted_cmd(&argc, &argv);

    /* Add basic orted command line options, including debug flags */
    prte_plm_base_prted_append_basic_args(&argc, &argv, NULL, &proc_vpid_index);

    /* tell the new daemons the base of the name list so they can compute
     * their own name on the other end
     */
    rc = prte_util_convert_vpid_to_string(&vpid_string, map->daemon_vpid_start);
    if (PRTE_SUCCESS != rc) {
        prte_output(0, "plm_alps: unable to create process name");
        goto cleanup;
    }

    free(argv[proc_vpid_index]);
    argv[proc_vpid_index] = strdup(vpid_string);
    free(vpid_string);

    if (prte_plm_alps_component.debug) {
        param = prte_argv_join(argv, ' ');
        if (NULL != param) {
            prte_output(0, "plm:alps: final top-level argv:");
            prte_output(0, "plm:alps:     %s", param);
            free(param);
        }
    }

    /* Copy the prefix-directory specified in the
       corresponding app_context.  If there are multiple,
       different prefix's in the app context, complain (i.e., only
       allow one --prefix option for the entire alps run -- we
       don't support different --prefix'es for different nodes in
       the ALPS plm) */
    cur_prefix = NULL;
    for (i = 0; i < state->jdata->apps->size; i++) {
        char *app_prefix_dir = NULL;
        if (NULL
            == (app = (prte_app_context_t *) prte_pointer_array_get_item(state->jdata->apps, i))) {
            continue;
        }
        prte_get_attribute(&app->attributes, PRTE_APP_PREFIX_DIR, (void **) &app_prefix_dir,
                           PMIX_STRING);
        /* Check for already set cur_prefix_dir -- if different,
           complain */
        if (NULL != app_prefix_dir) {
            if (NULL != cur_prefix && 0 != strcmp(cur_prefix, app_prefix_dir)) {
                prte_show_help("help-plm-alps.txt", "multiple-prefixes", true, cur_prefix,
                               app_prefix_dir);
                goto cleanup;
            }

            /* If not yet set, copy it; iff set, then it's the
               same anyway */
            if (NULL == cur_prefix) {
                cur_prefix = strdup(app_prefix_dir);
                if (prte_plm_alps_component.debug) {
                    prte_output(0, "plm:alps: Set prefix:%s", cur_prefix);
                }
            }
            free(app_prefix_dir);
        }
    }

    /* protect the args in case someone has a script wrapper around aprun */
    prte_plm_base_wrap_args(argv);

    /* setup environment */
    env = prte_argv_copy(prte_launch_environ);

    if (0 < prte_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        param = prte_argv_join(argv, ' ');
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:alps: final top-level argv:\n\t%s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (NULL == param) ? "NULL" : param));
        if (NULL != param)
            free(param);
    }

    /* exec the daemon(s) */
    if (PRTE_SUCCESS != (rc = plm_alps_start_proc(argc, argv, env, cur_prefix))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* indicate that the daemons for this job were launched */
    state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
    daemons->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;

    /* flag that launch was successful, so far as we currently know */
    failed_launch = false;

cleanup:
    if (NULL != argv) {
        prte_argv_free(argv);
    }
    if (NULL != env) {
        prte_argv_free(env);
    }

    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_FAILED_TO_START);
    }

    /* cleanup the caddy */
    PRTE_RELEASE(state);
}

/**
 * Terminate the orteds for a given job
 */
static int plm_alps_terminate_orteds(void)
{
    int rc;
    prte_job_t *jdata;

    PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                         "%s plm:alps: terminating orteds", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* deregister the waitpid callback to ensure we don't make it look like
     * alps failed when it didn't. Since the alps may have already completed,
     * do NOT ERROR_LOG any return code to avoid confusing, duplicate error
     * messages
     */
    if (NULL != alpsrun) {
        prte_wait_cb_cancel(alpsrun);
    }

    /* now tell them to die */
    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
        PRTE_ERROR_LOG(rc);
    }

    jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);

    PRTE_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                         "%s plm:alps: terminated orteds", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    return rc;
}

/**
 * Signal all the processes in the child alps by sending the signal directly to it
 */
static int plm_alps_signal_job(pmix_nspace_t jobid, int32_t signal)
{
    if (NULL != alpsrun && 0 != alpsrun->pid) {
        kill(alpsrun->pid, (int) signal);
    }
    return PRTE_SUCCESS;
}

static int plm_alps_finalize(void)
{
    int rc;

    if (NULL != alpsrun) {
        PRTE_RELEASE(alpsrun);
    }

    /* cleanup any pending recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    return PRTE_SUCCESS;
}

static void alps_wait_cb(int sd, short args, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_proc_t *proc = t2->child;
    prte_job_t *jdata;

    /* According to the ALPS folks, alps always returns the highest exit
       code of our remote processes. Thus, a non-zero exit status doesn't
       necessarily mean that alps failed - it could be that an orted returned
       a non-zero exit status. Of course, that means the orted failed(!), so
       the end result is the same - the job didn't start.

       As a result, we really can't do much with the exit status itself - it
       could be something in errno (if alps itself failed), or it could be
       something returned by an orted, or it could be something returned by
       the OS (e.g., couldn't find the orted binary). Somebody is welcome
       to sort out all the options and pretty-print a better error message. For
       now, though, the only thing that really matters is that
       alps failed. Report the error and make sure that prun
       wakes up - otherwise, do nothing!
    */
    jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    if (0 != proc->exit_code) {
        if (failed_launch) {
            /* report that the daemon has failed so we break out of the daemon
             * callback receive and exit
             */
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_FAILED_TO_START);
        } else {
            /* an orted must have died unexpectedly after launch - report
             * that the daemon has failed so we exit
             */
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ABORTED);
        }
    }
    PRTE_RELEASE(t2);
}

static int plm_alps_start_proc(int argc, char **argv, char **env, char *prefix)
{
    int fd;
    pid_t alps_pid;
    char *exec_argv = prte_path_findv(argv[0], 0, env, NULL);

    if (NULL == exec_argv) {
        return PRTE_ERR_NOT_FOUND;
    }

    alps_pid = fork();
    if (-1 == alps_pid) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        return PRTE_ERR_SYS_LIMITS_CHILDREN;
    }

    alpsrun = PRTE_NEW(prte_proc_t);
    alpsrun->pid = alps_pid;
    /* be sure to mark it as alive so we don't instantly fire */
    PRTE_FLAG_SET(alpsrun, PRTE_PROC_FLAG_ALIVE);
    /* setup the waitpid so we can find out if alps succeeds! */
    prte_wait_cb(alpsrun, alps_wait_cb, prte_event_base, NULL);

    if (0 == alps_pid) { /* child */
        char *bin_base = NULL, *lib_base = NULL;

        /* Figure out the basenames for the libdir and bindir.  There
           is a lengthy comment about this in plm_rsh_module.c
           explaining all the rationale for how / why we're doing
           this. */

        lib_base = prte_basename(prte_install_dirs.libdir);
        bin_base = prte_basename(prte_install_dirs.bindir);

        /* If we have a prefix, then modify the PATH and
           LD_LIBRARY_PATH environment variables.  */
        if (NULL != prefix) {
            char *oldenv, *newenv;

            /* Reset PATH */
            oldenv = getenv("PATH");
            if (NULL != oldenv) {
                prte_asprintf(&newenv, "%s/%s:%s", prefix, bin_base, oldenv);
            } else {
                prte_asprintf(&newenv, "%s/%s", prefix, bin_base);
            }
            prte_setenv("PATH", newenv, true, &env);
            if (prte_plm_alps_component.debug) {
                prte_output(0, "plm:alps: reset PATH: %s", newenv);
            }
            free(newenv);

            /* Reset LD_LIBRARY_PATH */
            oldenv = getenv("LD_LIBRARY_PATH");
            if (NULL != oldenv) {
                prte_asprintf(&newenv, "%s/%s:%s", prefix, lib_base, oldenv);
            } else {
                prte_asprintf(&newenv, "%s/%s", prefix, lib_base);
            }
            prte_setenv("LD_LIBRARY_PATH", newenv, true, &env);
            if (prte_plm_alps_component.debug) {
                prte_output(0, "plm:alps: reset LD_LIBRARY_PATH: %s", newenv);
            }
            free(newenv);
        }

        fd = open("/dev/null", O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd > 0) {
            dup2(fd, 0);
        }

        /* When not in debug mode and --debug-daemons was not passed,
         * tie stdout/stderr to dev null so we don't see messages from orted */
        if (0 == prte_plm_alps_component.debug && !prte_debug_daemons_flag) {
            if (fd >= 0) {
                if (fd != 1) {
                    dup2(fd, 1);
                }
                if (fd != 2) {
                    dup2(fd, 2);
                }
            }
        }

        if (fd > 2) {
            close(fd);
        }

        /* get the alps process out of prun's process group so that
           signals sent from the shell (like those resulting from
           cntl-c) don't get sent to alps */
        setpgid(0, 0);

        execve(exec_argv, argv, env);

        prte_output(0, "plm:alps:start_proc: exec failed");
        /* don't return - need to exit - returning would be bad -
           we're not in the calling process anymore */
        exit(1);
    } else { /* parent */
        /* just in case, make sure that the alps process is not in our
        process group any more.  Stevens says always do this on both
        sides of the fork... */
        setpgid(alps_pid, alps_pid);

        free(exec_argv);
    }

    return PRTE_SUCCESS;
}
