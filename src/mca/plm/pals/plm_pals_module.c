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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023-2024 Triad National Security, LLC. All rights
 *                         reserved.
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

#include "src/mca/base/pmix_base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/pinstalldirs/pinstalldirs_types.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "plm_pals.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Local functions
 */
static int plm_pals_init(void);
static int plm_pals_launch_job(prte_job_t *jdata);
static int plm_pals_terminate_prteds(void);
static int plm_pals_signal_job(pmix_nspace_t jobid, int32_t signal);
static int plm_pals_finalize(void);

static int plm_pals_start_proc(int argc, char **argv, char **env,
                               char *prefix, char *pmix_prefix);

/*
 * Global variable
 */
prte_plm_base_module_t prte_plm_pals_module = {
    .init = plm_pals_init,
    .set_hnp_name = prte_plm_base_set_hnp_name,
    .spawn = plm_pals_launch_job,
    .terminate_job = prte_plm_base_prted_terminate_job,
    .terminate_orteds = plm_pals_terminate_prteds,
    .terminate_procs = prte_plm_base_prted_kill_local_procs,
    .signal_job = plm_pals_signal_job,
    .finalize = plm_pals_finalize
};

/*
 * Local variables
 */
static prte_proc_t *palsrun = NULL;
static bool failed_launch;
static void launch_daemons(int fd, short args, void *cbdata);

/**
 * Init the module
 */
static int plm_pals_init(void)
{
    int rc;
    prte_job_t *daemons;

    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
	fprintf(stderr, "OOPS prte_plm_base_comm_start returned error\n");
        return rc;
    }

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        /* must map daemons since we won't be launching them */
        prte_plm_globals.daemon_nodes_assigned_at_launch = true;
    } else {
        /* we do NOT assign daemons to nodes at launch - we will
         * determine that mapping when the daemon
         * calls back. This is required because pals does
         * its own mapping of proc-to-node, and we cannot know
         * in advance which daemon will wind up on which node
         */
        prte_plm_globals.daemon_nodes_assigned_at_launch = false;
    }

    /* point to our launch command */
    if (PRTE_SUCCESS
        != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return rc;
}

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that prun will be woken up and
 * the job can cleanly terminate
 */
static int plm_pals_launch_job(prte_job_t *jdata)
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
    char *vpid_string;
    char **custom_strings;
    int num_args, i;
    char *cur_prefix, *pmix_prefix;
    int proc_vpid_index;
    prte_job_t *daemons;
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(state);

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
        PMIX_RELEASE(state);
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
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:pals: no new daemons to launch",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
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
    pmix_argv_append(&argc, &argv, prte_mca_plm_pals_component.aprun_cmd);

    /* Append user defined arguments to aprun */
    if (NULL != prte_mca_plm_pals_component.custom_args) {
        custom_strings = PMIx_Argv_split(prte_mca_plm_pals_component.custom_args, ' ');
        num_args = PMIx_Argv_count(custom_strings);
        for (i = 0; i < num_args; ++i) {
            pmix_argv_append(&argc, &argv, custom_strings[i]);
        }
        PMIx_Argv_free(custom_strings);
    }

    /* number of processors needed */
    pmix_argv_append(&argc, &argv, "-n");
    pmix_asprintf(&tmp, "%lu", (unsigned long) map->num_new_daemons);
    pmix_argv_append(&argc, &argv, tmp);
    free(tmp);
    pmix_argv_append(&argc, &argv, "-N");
    pmix_argv_append(&argc, &argv, "1");
    pmix_argv_append(&argc, &argv, "--cc");
    pmix_argv_append(&argc, &argv, "none");

    /* if we are using all allocated nodes, then pals
     * doesn't need a nodelist, or if running without a batch scheduler
     */
#if 0
    if ((map->num_new_daemons < prte_num_allocated_nodes) || (prte_num_allocated_nodes == 0)) {
        /* create nodelist */
        nodelist_argv = NULL;
        nodelist_argc = 0;

        for (nnode = 0; nnode < map->nodes->size; nnode++) {
            if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, nnode))) {
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
            pmix_argv_append(&nodelist_argc, &nodelist_argv, node->name);
        }
        if (0 == PMIx_Argv_count(nodelist_argv)) {
            pmix_show_help("help-plm-pals.txt", "no-hosts-in-list", true);
            rc = PRTE_ERR_FAILED_TO_START;
            goto cleanup;
        }
        nodelist_flat = PMIx_Argv_join(nodelist_argv, ',');
        PMIx_Argv_free(nodelist_argv);

        pmix_argv_append(&argc, &argv, "-L");
        pmix_argv_append(&argc, &argv, nodelist_flat);
        free(nodelist_flat);
    }
#endif

    /*
     * PRTED OPTIONS
     */

    /* add the daemon command (as specified by user) */
    prte_plm_base_setup_prted_cmd(&argc, &argv);

    /* Add basic orted command line options, including debug flags */
    prte_plm_base_prted_append_basic_args(&argc, &argv, "pals", &proc_vpid_index);

    /* tell the new daemons the base of the name list so they can compute
     * their own name on the other end
     */
    rc = prte_util_convert_vpid_to_string(&vpid_string, map->daemon_vpid_start);
    if (PRTE_SUCCESS != rc) {
        pmix_output(0, "plm_pals: unable to create process name");
        goto cleanup;
    }

    free(argv[proc_vpid_index]);
    argv[proc_vpid_index] = strdup(vpid_string);
    free(vpid_string);

    if (prte_mca_plm_pals_component.debug) {
        param = PMIx_Argv_join(argv, ' ');
        if (NULL != param) {
            pmix_output(0, "plm:pals: final top-level argv:");
            pmix_output(0, "plm:pals:     %s", param);
            free(param);
        }
    }

    /*
     * Any prefix was installed in the DAEMON job object, so
     * we only need to look there to find it. This covers any
     * prefix by default, PRTE_PREFIX given in the environment,
     * and '--prefix' from the cmd line
     */
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PREFIX, (void **) &cur_prefix, PMIX_STRING)) {
        cur_prefix = NULL;
    }
    /* Similarly, we have to check for any PMIx prefix that was specified */
    if (!prte_get_attribute(&daemons->attributes, PRTE_JOB_PMIX_PREFIX, (void **) &pmix_prefix, PMIX_STRING)) {
        pmix_prefix = NULL;
    }

    /* protect the args in case someone has a script wrapper around aprun */
    prte_plm_base_wrap_args(argv);

    /* setup environment - this is the pristine version that PRRTE
     * has already stripped of all PRTE_ and PMIX_ prefixed values */
    env = PMIx_Argv_copy(prte_launch_environ);

    if (0 < pmix_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        param = PMIx_Argv_join(argv, ' ');
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:pals: final top-level argv:\n\t%s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (NULL == param) ? "NULL" : param));
        if (NULL != param)
            free(param);
    }

    /* exec the daemon(s) */
    if (PRTE_SUCCESS != (rc = plm_pals_start_proc(argc, argv, env, cur_prefix, pmix_prefix))) {
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
        PMIx_Argv_free(argv);
    }
    if (NULL != env) {
        PMIx_Argv_free(env);
    }

    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_FAILED_TO_START);
    }

    /* cleanup the caddy */
    PMIX_RELEASE(state);
}

/**
 * Terminate the prteds for a given job
 */
static int plm_pals_terminate_prteds(void)
{
    int rc;

    PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                         "%s plm:pals: terminating prteds", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* deregister the waitpid callback to ensure we don't make it look like
     * pals failed when it didn't. Since the pals may have already completed,
     * do NOT ERROR_LOG any return code to avoid confusing, duplicate error
     * messages
     */
    if (NULL != palsrun) {
        prte_wait_cb_cancel(palsrun);
    }

    /* now tell them to die */
    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
        PRTE_ERROR_LOG(rc);
    }

    PMIX_OUTPUT_VERBOSE((10, prte_plm_base_framework.framework_output,
                         "%s plm:pals: terminated orteds %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),rc));
    return rc;
}

/**
 * Signal all the processes in the child pals by sending the signal directly to it
 */
static int plm_pals_signal_job(pmix_nspace_t jobid, int32_t signal)
{
    PRTE_HIDE_UNUSED_PARAMS(jobid);

    if (NULL != palsrun && 0 != palsrun->pid) {
        kill(palsrun->pid, (int) signal);
    }
    return PRTE_SUCCESS;
}

static int plm_pals_finalize(void)
{
    int rc;

    if (NULL != palsrun) {
        PMIX_RELEASE(palsrun);
    }

    /* cleanup any pending recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    return PRTE_SUCCESS;
}

static void pals_wait_cb(int sd, short args, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_proc_t *proc = t2->child;
    prte_job_t *jdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* According to the ALPS folks, pals always returns the highest exit
       code of our remote processes. Thus, a non-zero exit status doesn't
       necessarily mean that pals failed - it could be that an orted returned
       a non-zero exit status. Of course, that means the orted failed(!), so
       the end result is the same - the job didn't start.

       As a result, we really can't do much with the exit status itself - it
       could be something in errno (if pals itself failed), or it could be
       something returned by an orted, or it could be something returned by
       the OS (e.g., couldn't find the orted binary). Somebody is welcome
       to sort out all the options and pretty-print a better error message. For
       now, though, the only thing that really matters is that
       pals failed. Report the error and make sure that prun
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
    PMIX_RELEASE(t2);
}

static int plm_pals_start_proc(int argc, char **argv, char **env,
                               char *prefix, char *pmix_prefix)
{
    int fd;
    pid_t pals_pid;
    char *exec_argv = pmix_path_findv(argv[0], 0, env, NULL);
    char *p;
    char *oldenv, *newenv;
    PRTE_HIDE_UNUSED_PARAMS(argc);

    if (NULL == exec_argv) {
        return PRTE_ERR_NOT_FOUND;
    }

    pals_pid = fork();
    if (-1 == pals_pid) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        return PRTE_ERR_SYS_LIMITS_CHILDREN;
    }

    palsrun = PMIX_NEW(prte_proc_t);
    palsrun->pid = pals_pid;
    /* be sure to mark it as alive so we don't instantly fire */
    PRTE_FLAG_SET(palsrun, PRTE_PROC_FLAG_ALIVE);
    /* setup the waitpid so we can find out if pals succeeds! */
    prte_wait_cb(palsrun, pals_wait_cb, NULL);

    if (0 == pals_pid) { /* child */
        char *bin_base = NULL, *lib_base = NULL;

        /* Figure out the basenames for the libdir and bindir.  There
           is a lengthy comment about this in plm_rsh_module.c
           explaining all the rationale for how / why we're doing
           this. */

        lib_base = pmix_basename(prte_install_dirs.libdir);
        bin_base = pmix_basename(prte_install_dirs.bindir);

        /* If we have a prefix, then modify the PATH and
           LD_LIBRARY_PATH environment variables.  */
        if (NULL != prefix) {
            /* Reset PATH */
            oldenv = getenv("PATH");
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", prefix, bin_base, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", prefix, bin_base);
            }
            PMIx_Setenv("PATH", newenv, true, &env);
            if (prte_mca_plm_pals_component.debug) {
                pmix_output(0, "plm:pals: reset PATH: %s", newenv);
            }
            free(newenv);

            /* Reset LD_LIBRARY_PATH */
            oldenv = getenv("LD_LIBRARY_PATH");
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", prefix, lib_base, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", prefix, lib_base);
            }
            PMIx_Setenv("LD_LIBRARY_PATH", newenv, true, &env);
            if (prte_mca_plm_pals_component.debug) {
                pmix_output(0, "plm:pals: reset LD_LIBRARY_PATH: %s", newenv);
            }
            free(newenv);
            // add the prefix itself to the environment
            PMIx_Setenv("PRTE_PREFIX", prefix, true, &env);
        }

        /* for pmix_prefix, we only have to modify the library path.
         * NOTE: obviously, we cannot know the lib_base used for the
         * PMIx library. All we can do is hope they used the same one
         * for PMIx as they did for the one they linked to PRRTE */
        if (NULL != pmix_prefix) {
            oldenv = getenv("LD_LIBRARY_PATH");
            p = pmix_basename(pmix_pinstall_dirs.libdir);
            if (NULL != oldenv) {
                pmix_asprintf(&newenv, "%s/%s:%s", pmix_prefix, p, oldenv);
            } else {
                pmix_asprintf(&newenv, "%s/%s", pmix_prefix, p);
            }
            free(p);
            PMIx_Setenv("LD_LIBRARY_PATH", newenv, true, &env);
            if (prte_mca_plm_pals_component.debug) {
                pmix_output(0, "plm:pals: reset LD_LIBRARY_PATH: %s", newenv);
            }
            free(newenv);
            // add the prefix itself to the environment
            PMIx_Setenv("PMIX_PREFIX", pmix_prefix, true, &env);
        }

        fd = open("/dev/null", O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd > 0) {
            dup2(fd, 0);
        }

        /* When not in debug mode and --debug-daemons was not passed,
         * tie stdout/stderr to dev null so we don't see messages from orted */
        if (0 == prte_mca_plm_pals_component.debug && !prte_debug_daemons_flag) {
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

        /* get the pals process out of prun's process group so that
           signals sent from the shell (like those resulting from
           cntl-c) don't get sent to pals */
        setpgid(0, 0);

        execve(exec_argv, argv, env);

        pmix_output(0, "plm:pals:start_proc: exec failed");
        /* don't return - need to exit - returning would be bad -
           we're not in the calling process anymore */
        exit(1);
    } else { /* parent */
        /* just in case, make sure that the pals process is not in our
        process group any more.  Stevens says always do this on both
        sides of the fork... */
        setpgid(pals_pid, pals_pid);

        free(exec_argv);
    }

    return PRTE_SUCCESS;
}
