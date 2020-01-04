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
 * Copyright (c) 2006-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#include "prrte_config.h"
#include "src/runtime/prrte_globals.h"

#include <string.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "src/mca/base/base.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/util/path.h"
#include "src/util/basename.h"

#include "constants.h"
#include "types.h"
#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_quit.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"

#include "src/prted/prted.h"

#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "plm_slurm.h"


/*
 * Local functions
 */
static int plm_slurm_init(void);
static int plm_slurm_launch_job(prrte_job_t *jdata);
static int plm_slurm_terminate_prteds(void);
static int plm_slurm_signal_job(prrte_jobid_t jobid, int32_t signal);
static int plm_slurm_finalize(void);

static int plm_slurm_start_proc(int argc, char **argv, char **env,
                                char *prefix);


/*
 * Global variable
 */
prrte_plm_base_module_1_0_0_t prrte_plm_slurm_module = {
    plm_slurm_init,
    prrte_plm_base_set_hnp_name,
    plm_slurm_launch_job,
    NULL,
    prrte_plm_base_prted_terminate_job,
    plm_slurm_terminate_prteds,
    prrte_plm_base_prted_kill_local_procs,
    plm_slurm_signal_job,
    plm_slurm_finalize
};

/*
 * Local variables
 */
static pid_t primary_srun_pid = 0;
static bool primary_pid_set = false;
static void launch_daemons(int fd, short args, void *cbdata);

/**
* Init the module
 */
static int plm_slurm_init(void)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_plm_base_comm_start())) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* if we don't want to launch (e.g., someone just wants
     * to test the mappers), then we assign vpids at "launch"
     * so the mapper has something to work with
     */
    if (prrte_do_not_launch) {
        prrte_plm_globals.daemon_nodes_assigned_at_launch = true;
    } else {
        /* we do NOT assign daemons to nodes at launch - we will
         * determine that mapping when the daemon
         * calls back. This is required because slurm does
         * its own mapping of proc-to-node, and we cannot know
         * in advance which daemon will wind up on which node
         */
        prrte_plm_globals.daemon_nodes_assigned_at_launch = false;
    }

    /* point to our launch command */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_LAUNCH_DAEMONS,
                                                       launch_daemons, PRRTE_SYS_PRI))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return rc;
}

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that prrterun will be woken up and
 * the job can cleanly terminate
 */
static int plm_slurm_launch_job(prrte_job_t *jdata)
{
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        /* this is a restart situation - skip to the mapping stage */
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_MAP);
    } else {
        /* new job - set it up */
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_INIT);
    }
    return PRRTE_SUCCESS;
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prrte_app_context_t *app;
    prrte_node_t *node;
    prrte_std_cntr_t n;
    prrte_job_map_t *map;
    char *jobid_string = NULL;
    char *param;
    char **argv = NULL;
    int argc;
    int rc;
    char *tmp;
    char** env = NULL;
    char *nodelist_flat;
    char **nodelist_argv;
    char *name_string;
    char **custom_strings;
    int num_args, i;
    char *cur_prefix;
    int proc_vpid_index;
    bool failed_launch=true;
    prrte_job_t *daemons;
    prrte_state_caddy_t *state = (prrte_state_caddy_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(state);

    PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                         "%s plm:slurm: LAUNCH DAEMONS CALLED",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* if we are launching debugger daemons, then just go
     * do it - no new daemons will be launched
     */
    if (PRRTE_FLAG_TEST(state->jdata, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        state->jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(state->jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
        PRRTE_RELEASE(state);
        return;
    }

    /* start by setting up the virtual machine */
    daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_setup_virtual_machine(state->jdata))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

   /* if we don't want to launch, then don't attempt to
     * launch the daemons - the user really wants to just
     * look at the proposed process map
     */
    if (prrte_do_not_launch) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        state->jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(state->jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
        PRRTE_RELEASE(state);
        return;
    }

    /* Get the map for this job */
    if (NULL == (map = daemons->map)) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        rc = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (0 == map->num_new_daemons) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                             "%s plm:slurm: no new daemons to launch",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        state->jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(state->jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
        PRRTE_RELEASE(state);
        return;
    }

    /* need integer value for command line parameter */
    prrte_asprintf(&jobid_string, "%lu", (unsigned long) daemons->jobid);

    /*
     * start building argv array
     */
    argv = NULL;
    argc = 0;

    /*
     * SLURM srun OPTIONS
     */

    /* add the srun command */
    prrte_argv_append(&argc, &argv, "srun");

    /* start one orted on each node */
    prrte_argv_append(&argc, &argv, "--ntasks-per-node=1");

    if (!prrte_enable_recovery) {
        /* kill the job if any orteds die */
        prrte_argv_append(&argc, &argv, "--kill-on-bad-exit");
    }

    /* ensure the orteds are not bound to a single processor,
     * just in case the TaskAffinity option is set by default.
     * This will *not* release the orteds from any cpu-set
     * constraint, but will ensure it doesn't get
     * bound to only one processor
     *
     * NOTE: We used to pass --cpu_bind=none on the command line.  But
     * SLURM 19 changed this to --cpu-bind.  There is no easy way to
     * test at run time which of these two parameters is used (see
     * https://github.com/open-mpi/ompi/pull/6654).  There was
     * discussion of using --test-only to see which one works, but
     * --test-only is only effective if you're not already inside a
     * SLURM allocation.  Instead, set the env var SLURM_CPU_BIND to
     * "none", which should do the same thing as --cpu*bind=none.
     */
    prrte_setenv("SLURM_CPU_BIND", "none", true, &env);

#if SLURM_CRAY_ENV
    /*
     * If in a SLURM/Cray env. make sure that Cray PMI is not pulled in,
     * neither as a constructor run when orteds start, nor selected
     * when pmix components are registered
     */

    prrte_setenv("PMI_NO_PREINITIALIZE", "1", false, &prrte_launch_environ);
    prrte_setenv("PMI_NO_FORK", "1", false, &prrte_launch_environ);
    prrte_setenv("OMPI_NO_USE_CRAY_PMI", "1", false, &prrte_launch_environ);
#endif

    /* Append user defined arguments to srun */
    if ( NULL != prrte_plm_slurm_component.custom_args ) {
        custom_strings = prrte_argv_split(prrte_plm_slurm_component.custom_args, ' ');
        num_args       = prrte_argv_count(custom_strings);
        for (i = 0; i < num_args; ++i) {
            prrte_argv_append(&argc, &argv, custom_strings[i]);
        }
        prrte_argv_free(custom_strings);
    }

    /* create nodelist */
    nodelist_argv = NULL;

    for (n=0; n < map->nodes->size; n++ ) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, n))) {
            continue;
        }
        /* if the daemon already exists on this node, then
         * don't include it
         */
        if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            continue;
        }

        /* otherwise, add it to the list of nodes upon which
         * we need to launch a daemon
         */
        prrte_argv_append_nosize(&nodelist_argv, node->name);
    }
    if (0 == prrte_argv_count(nodelist_argv)) {
        prrte_show_help("help-plm-slurm.txt", "no-hosts-in-list", true);
        rc = PRRTE_ERR_FAILED_TO_START;
        goto cleanup;
    }
    nodelist_flat = prrte_argv_join(nodelist_argv, ',');
    prrte_argv_free(nodelist_argv);

    /* if we are using all allocated nodes, then srun doesn't
     * require any further arguments
     */
    if (map->num_new_daemons < prrte_num_allocated_nodes) {
        prrte_asprintf(&tmp, "--nodes=%lu", (unsigned long)map->num_new_daemons);
        prrte_argv_append(&argc, &argv, tmp);
        free(tmp);

        prrte_asprintf(&tmp, "--nodelist=%s", nodelist_flat);
        prrte_argv_append(&argc, &argv, tmp);
        free(tmp);
    }

    /* tell srun how many tasks to run */
    prrte_asprintf(&tmp, "--ntasks=%lu", (unsigned long)map->num_new_daemons);
    prrte_argv_append(&argc, &argv, tmp);
    free(tmp);

    PRRTE_OUTPUT_VERBOSE((2, prrte_plm_base_framework.framework_output,
                         "%s plm:slurm: launching on nodes %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), nodelist_flat));
    free(nodelist_flat);

    /*
     * PRRTED OPTIONS
     */

    /* add the daemon command (as specified by user) */
    prrte_plm_base_setup_prted_cmd(&argc, &argv);

    /* Add basic orted command line options, including debug flags */
    prrte_plm_base_prted_append_basic_args(&argc, &argv,
                                           "slurm", &proc_vpid_index);

    /* tell the new daemons the base of the name list so they can compute
     * their own name on the other end
     */
    rc = prrte_util_convert_vpid_to_string(&name_string, map->daemon_vpid_start);
    if (PRRTE_SUCCESS != rc) {
        prrte_output(0, "plm_slurm: unable to get daemon vpid as string");
        goto cleanup;
    }

    free(argv[proc_vpid_index]);
    argv[proc_vpid_index] = strdup(name_string);
    free(name_string);

    /* Copy the prefix-directory specified in the
       corresponding app_context.  If there are multiple,
       different prefix's in the app context, complain (i.e., only
       allow one --prefix option for the entire slurm run -- we
       don't support different --prefix'es for different nodes in
       the SLURM plm) */
    cur_prefix = NULL;
    for (n=0; n < state->jdata->apps->size; n++) {
        char * app_prefix_dir;
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(state->jdata->apps, n))) {
            continue;
        }
        app_prefix_dir = NULL;
        prrte_get_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&app_prefix_dir, PRRTE_STRING);
        /* Check for already set cur_prefix_dir -- if different,
           complain */
        if (NULL != app_prefix_dir) {
            if (NULL != cur_prefix &&
                0 != strcmp (cur_prefix, app_prefix_dir)) {
                prrte_show_help("help-plm-slurm.txt", "multiple-prefixes",
                               true, cur_prefix, app_prefix_dir);
                goto cleanup;
            }

            /* If not yet set, copy it; iff set, then it's the
             * same anyway
             */
            if (NULL == cur_prefix) {
                cur_prefix = strdup(app_prefix_dir);
                PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                     "%s plm:slurm: Set prefix:%s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     cur_prefix));
            }
            free(app_prefix_dir);
        }
    }

    /* protect the args in case someone has a script wrapper around srun */
    prrte_schizo.wrap_args(argv);

    /* setup environment */
    env = prrte_argv_copy(prrte_launch_environ);

    if (0 < prrte_output_get_verbosity(prrte_plm_base_framework.framework_output)) {
        param = prrte_argv_join(argv, ' ');
        prrte_output(prrte_plm_base_framework.framework_output,
                    "%s plm:slurm: final top-level argv:\n\t%s",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    (NULL == param) ? "NULL" : param);
        if (NULL != param) free(param);
    }

    /* exec the daemon(s) */
    if (PRRTE_SUCCESS != (rc = plm_slurm_start_proc(argc, argv, env, cur_prefix))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* indicate that the daemons for this job were launched */
    state->jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
    daemons->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;

    /* flag that launch was successful, so far as we currently know */
    failed_launch = false;

 cleanup:
    if (NULL != argv) {
        prrte_argv_free(argv);
    }
    if (NULL != env) {
        prrte_argv_free(env);
    }

    if(NULL != jobid_string) {
        free(jobid_string);
    }

    /* cleanup the caddy */
    PRRTE_RELEASE(state);

    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
    }
}


/**
* Terminate the orteds for a given job
 */
static int plm_slurm_terminate_prteds(void)
{
    int rc=PRRTE_SUCCESS;
    prrte_job_t *jdata;

    /* check to see if the primary pid is set. If not, this indicates
     * that we never launched any additional daemons, so we cannot
     * not wait for a waitpid to fire and tell us it's okay to
     * exit. Instead, we simply trigger an exit for ourselves
     */
    if (primary_pid_set) {
        if (PRRTE_SUCCESS != (rc = prrte_plm_base_prted_exit(PRRTE_DAEMON_EXIT_CMD))) {
            PRRTE_ERROR_LOG(rc);
        }
    } else {
        PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                             "%s plm:slurm: primary daemons complete!",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
        /* need to set the #terminated value to avoid an incorrect error msg */
        jdata->num_terminated = jdata->num_procs;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
    }

    return rc;
}


/**
 * Signal all the processes in the child srun by sending the signal directly to it
 */
static int plm_slurm_signal_job(prrte_jobid_t jobid, int32_t signal)
{
    int rc = PRRTE_SUCCESS;

    /* order them to pass this signal to their local procs */
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_prted_signal_local_procs(jobid, signal))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}


static int plm_slurm_finalize(void)
{
    int rc;

    /* cleanup any pending recvs */
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_comm_stop())) {
        PRRTE_ERROR_LOG(rc);
    }

    return PRRTE_SUCCESS;
}


static void srun_wait_cb(int sd, short fd, void *cbdata){
    prrte_wait_tracker_t *t2 = (prrte_wait_tracker_t*)cbdata;
    prrte_proc_t *proc = t2->child;
    prrte_job_t *jdata;

    /* According to the SLURM folks, srun always returns the highest exit
     code of our remote processes. Thus, a non-zero exit status doesn't
     necessarily mean that srun failed - it could be that an orted returned
     a non-zero exit status. Of course, that means the orted failed(!), so
     the end result is the same - the job didn't start.

     As a result, we really can't do much with the exit status itself - it
     could be something in errno (if srun itself failed), or it could be
     something returned by an orted, or it could be something returned by
     the OS (e.g., couldn't find the orted binary). Somebody is welcome
     to sort out all the options and pretty-print a better error message. For
     now, though, the only thing that really matters is that
     srun failed. Report the error and make sure that prrterun
     wakes up - otherwise, do nothing!

     Unfortunately, the pid returned here is the srun pid, not the pid of
     the proc that actually died! So, to avoid confusion, just use -1 as the
     pid so nobody thinks this is real
     */

    jdata = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);

    /* abort only if the status returned is non-zero - i.e., if
    * the orteds exited with an error
     */
    if (0 != proc->exit_code) {
        /* an orted must have died unexpectedly - report
         * that the daemon has failed so we exit
         */
        PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                             "%s plm:slurm: srun returned non-zero exit status (%d) from launching the per-node daemon",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             proc->exit_code));
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ABORTED);
    } else {
        /* otherwise, check to see if this is the primary pid */
        if (primary_srun_pid == proc->pid) {
            /* in this case, we just want to fire the proper trigger so
             * mpirun can exit
             */
            PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                 "%s plm:slurm: primary daemons complete!",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            /* need to set the #terminated value to avoid an incorrect error msg */
            jdata->num_terminated = jdata->num_procs;
            PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
    }

    /* done with this dummy */
    PRRTE_RELEASE(t2);
}


static int plm_slurm_start_proc(int argc, char **argv, char **env,
                                char *prefix)
{
    int fd;
    int srun_pid;
    char *exec_argv = prrte_path_findv(argv[0], 0, env, NULL);
    prrte_proc_t *dummy;

    if (NULL == exec_argv) {
        prrte_show_help("help-plm-slurm.txt", "no-srun", true);
        return PRRTE_ERR_SILENT;
    }

    srun_pid = fork();
    if (-1 == srun_pid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_SYS_LIMITS_CHILDREN);
        free(exec_argv);
        return PRRTE_ERR_SYS_LIMITS_CHILDREN;
    }
    /* if this is the primary launch - i.e., not a comm_spawn of a
     * child job - then save the pid
     */
    if (0 < srun_pid && !primary_pid_set) {
        primary_srun_pid = srun_pid;
        primary_pid_set = true;
    }

    /* setup a dummy proc object to track the srun */
    dummy = PRRTE_NEW(prrte_proc_t);
    dummy->pid = srun_pid;
    /* be sure to mark it as alive so we don't instantly fire */
    PRRTE_FLAG_SET(dummy, PRRTE_PROC_FLAG_ALIVE);
    /* setup the waitpid so we can find out if srun succeeds! */
    prrte_wait_cb(dummy, srun_wait_cb, prrte_event_base, NULL);

    if (0 == srun_pid) {  /* child */
        char *bin_base = NULL, *lib_base = NULL;

        /* Figure out the basenames for the libdir and bindir.  There
           is a lengthy comment about this in plm_rsh_module.c
           explaining all the rationale for how / why we're doing
           this. */

        lib_base = prrte_basename(prrte_install_dirs.libdir);
        bin_base = prrte_basename(prrte_install_dirs.bindir);

        /* If we have a prefix, then modify the PATH and
           LD_LIBRARY_PATH environment variables.  */
        if (NULL != prefix) {
            char *oldenv, *newenv;

            /* Reset PATH */
            oldenv = getenv("PATH");
            if (NULL != oldenv) {
                prrte_asprintf(&newenv, "%s/%s:%s", prefix, bin_base, oldenv);
            } else {
                prrte_asprintf(&newenv, "%s/%s", prefix, bin_base);
            }
            prrte_setenv("PATH", newenv, true, &env);
            PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                 "%s plm:slurm: reset PATH: %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 newenv));
            free(newenv);

            /* Reset LD_LIBRARY_PATH */
            oldenv = getenv("LD_LIBRARY_PATH");
            if (NULL != oldenv) {
                prrte_asprintf(&newenv, "%s/%s:%s", prefix, lib_base, oldenv);
            } else {
                prrte_asprintf(&newenv, "%s/%s", prefix, lib_base);
            }
            prrte_setenv("LD_LIBRARY_PATH", newenv, true, &env);
            PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                 "%s plm:slurm: reset LD_LIBRARY_PATH: %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 newenv));
            free(newenv);
        }

        fd = open("/dev/null", O_CREAT|O_RDWR|O_TRUNC, 0666);
        if (fd >= 0) {
            dup2(fd, 0);
            /* When not in debug mode and --debug-daemons was not passed,
             * tie stdout/stderr to dev null so we don't see messages from orted
             * EXCEPT if the user has requested that we leave sessions attached
             */
            if (0 > prrte_output_get_verbosity(prrte_plm_base_framework.framework_output) &&
                !prrte_debug_daemons_flag && !prrte_leave_session_attached) {
                dup2(fd,1);
                dup2(fd,2);
            }

            /* Don't leave the extra fd to /dev/null open */
            if (fd > 2) {
                close(fd);
            }
        }

        /* get the srun process out of prrterun's process group so that
           signals sent from the shell (like those resulting from
           cntl-c) don't get sent to srun */
        setpgid(0, 0);

        execve(exec_argv, argv, env);

        prrte_output(0, "plm:slurm:start_proc: exec failed");
        /* don't return - need to exit - returning would be bad -
           we're not in the calling process anymore */
        exit(1);
    } else {  /* parent */
        /* just in case, make sure that the srun process is not in our
           process group any more.  Stevens says always do this on both
           sides of the fork... */
        setpgid(srun_pid, srun_pid);

        free(exec_argv);
    }

    return PRRTE_SUCCESS;
}
