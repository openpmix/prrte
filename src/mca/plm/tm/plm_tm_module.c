/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
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
#include "constants.h"
#include "types.h"

#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SCHED_H
#include <sched.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <errno.h>
#include <tm.h>

#include "src/mca/installdirs/installdirs.h"
#include "src/event/event-internal.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/show_help.h"
#include "src/util/prrte_environ.h"
#include "src/util/basename.h"
#include "src/util/printf.h"

#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"

#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "plm_tm.h"



/*
 * API functions
 */
static int plm_tm_init(void);
static int plm_tm_launch_job(prrte_job_t *jdata);
static int plm_tm_terminate_orteds(void);
static int plm_tm_signal_job(prrte_jobid_t jobid, int32_t signal);
static int plm_tm_finalize(void);

/*
 * Local "global" variables
 */
static prrte_std_cntr_t launched = 0;
static bool connected = false;

/*
 * Global variable
 */
prrte_plm_base_module_t prrte_plm_tm_module = {
    plm_tm_init,
    prrte_plm_base_set_hnp_name,
    plm_tm_launch_job,
    NULL,
    prrte_plm_base_prted_terminate_job,
    plm_tm_terminate_orteds,
    prrte_plm_base_prted_kill_local_procs,
    plm_tm_signal_job,
    plm_tm_finalize
};

/* Local functions */
static int plm_tm_connect(void);
static void launch_daemons(int fd, short args, void *cbdata);
static void poll_spawns(int fd, short args, void *cbdata);


/**
* Init the module
 */
static int plm_tm_init(void)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_plm_base_comm_start())) {
        PRRTE_ERROR_LOG(rc);
    }

    /* we assign daemon nodes at launch */
    prrte_plm_globals.daemon_nodes_assigned_at_launch = true;

    /* point to our launch command */
    if (PRRTE_SUCCESS != (rc = prrte_state.add_job_state(PRRTE_JOB_STATE_LAUNCH_DAEMONS,
                                                       launch_daemons, PRRTE_SYS_PRI))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* overwrite the daemons_launched state to point to
     * our own local function
     */
    if (PRRTE_SUCCESS != (rc = prrte_state.set_job_state_callback(PRRTE_JOB_STATE_DAEMONS_LAUNCHED,
                                                                poll_spawns))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return rc;
}


static int plm_tm_launch_job(prrte_job_t *jdata)
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

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that prun will be woken up and
 * the job can cleanly terminate
 */
static void launch_daemons(int fd, short args, void *cbdata)
{
    prrte_job_map_t *map = NULL;
    prrte_app_context_t *app;
    prrte_node_t *node;
    int proc_vpid_index;
    char *param;
    char **env = NULL;
    char *var;
    char **argv = NULL;
    int argc = 0;
    int rc;
    prrte_std_cntr_t i;
    char *bin_base = NULL, *lib_base = NULL;
    tm_event_t *tm_events = NULL;
    tm_task_id *tm_task_ids = NULL;
    bool failed_launch = true;
    mode_t current_umask;
    char* vpid_string;
    prrte_job_t *daemons, *jdata;
    prrte_state_caddy_t *state = (prrte_state_caddy_t*)cbdata;
    int32_t launchid, *ldptr;
    char *prefix_dir = NULL;

    PRRTE_ACQUIRE_OBJECT(state);

    jdata = state->jdata;

    /* if we are launching debugger daemons, then just go
     * do it - no new daemons will be launched
     */
    if (PRRTE_FLAG_TEST(state->jdata, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
        PRRTE_RELEASE(state);
        return;
    }

    /* setup the virtual machine */
    daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_setup_virtual_machine(jdata))) {
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
        jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
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
        jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_DAEMONS_REPORTED);
        PRRTE_RELEASE(state);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                         "%s plm:tm: launching vm",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* Allocate a bunch of TM events to use for tm_spawn()ing */
    tm_events = malloc(sizeof(tm_event_t) * map->num_new_daemons);
    if (NULL == tm_events) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    tm_task_ids = malloc(sizeof(tm_task_id) * map->num_new_daemons);
    if (NULL == tm_task_ids) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* add the daemon command (as specified by user) */
    prrte_plm_base_setup_prted_cmd(&argc, &argv);

    /* Add basic orted command line options */
    prrte_plm_base_prted_append_basic_args(&argc, &argv, "tm",
                                           &proc_vpid_index);

    if (0 < prrte_output_get_verbosity(prrte_plm_base_framework.framework_output)) {
        param = prrte_argv_join(argv, ' ');
        PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                             "%s plm:tm: final top-level argv:\n\t%s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             (NULL == param) ? "NULL" : param));
        if (NULL != param) free(param);
    }

    if (!connected) {
        if (PRRTE_SUCCESS != plm_tm_connect()) {
            goto cleanup;
        }
        connected = true;
    }

    /* Figure out the basenames for the libdir and bindir.  There is a
       lengthy comment about this in plm_rsh_module.c explaining all
       the rationale for how / why we're doing this. */
    lib_base = prrte_basename(prrte_install_dirs.libdir);
    bin_base = prrte_basename(prrte_install_dirs.bindir);

    /* setup environment */
    env = prrte_argv_copy(prrte_launch_environ);

    /* enable local launch by the orteds */
    (void) mca_base_var_env_name ("plm", &var);
    prrte_setenv(var, "rsh", true, &env);
    free(var);

    /* add our umask -- see big note in orted.c */
    current_umask = umask(0);
    umask(current_umask);
    prrte_asprintf(&var, "0%o", current_umask);
    prrte_setenv("PRRTE_DAEMON_UMASK_VALUE", var, true, &env);
    free(var);

    /* If we have a prefix, then modify the PATH and
       LD_LIBRARY_PATH environment variables. We only allow
       a single prefix to be specified. Since there will
       always be at least one app_context, we take it from
       there
    */
    app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0);
    prrte_get_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&prefix_dir, PRRTE_STRING);
    if (NULL != prefix_dir) {
        char *newenv;

        for (i = 0; NULL != env && NULL != env[i]; ++i) {
            /* Reset PATH */
            if (0 == strncmp("PATH=", env[i], 5)) {
                prrte_asprintf(&newenv, "%s/%s:%s",
                               prefix_dir, bin_base, env[i] + 5);
                PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                     "%s plm:tm: resetting PATH: %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     newenv));
                prrte_setenv("PATH", newenv, true, &env);
                free(newenv);
            }

            /* Reset LD_LIBRARY_PATH */
            else if (0 == strncmp("LD_LIBRARY_PATH=", env[i], 16)) {
                prrte_asprintf(&newenv, "%s/%s:%s",
                               prefix_dir, lib_base, env[i] + 16);
                PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                     "%s plm:tm: resetting LD_LIBRARY_PATH: %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     newenv));
                prrte_setenv("LD_LIBRARY_PATH", newenv, true, &env);
                free(newenv);
            }
        }
        free(prefix_dir);
    }

    /* Iterate through each of the nodes and spin
     * up a daemon.
     */
    ldptr = &launchid;
    for (i = 0; i < map->nodes->size; i++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }
        /* if this daemon already exists, don't launch it! */
        if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            continue;
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                             "%s plm:tm: launching on node %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             node->name));

        /* setup process name */
        rc = prrte_util_convert_vpid_to_string(&vpid_string, node->daemon->name.vpid);
        if (PRRTE_SUCCESS != rc) {
            prrte_output(0, "plm:tm: unable to get daemon vpid as string");
            exit(-1);
        }
        free(argv[proc_vpid_index]);
        argv[proc_vpid_index] = strdup(vpid_string);
        free(vpid_string);

        /* exec the daemon */
        if (0 < prrte_output_get_verbosity(prrte_plm_base_framework.framework_output)) {
            param = prrte_argv_join(argv, ' ');
            PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                                 "%s plm:tm: executing:\n\t%s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == param) ? "NULL" : param));
            if (NULL != param) free(param);
        }

        launchid = 0;
        if (!prrte_get_attribute(&node->attributes, PRRTE_NODE_LAUNCH_ID, (void**)&ldptr, PRRTE_INT32)) {
            prrte_show_help("help-plm-tm.txt", "tm-spawn-failed", true, argv[0], node->name, 0);
            rc = PRRTE_ERROR;
            goto cleanup;
        }
        rc = tm_spawn(argc, argv, env, launchid, tm_task_ids + launched, tm_events + launched);
        if (TM_SUCCESS != rc) {
            prrte_show_help("help-plm-tm.txt", "tm-spawn-failed", true, argv[0], node->name, launchid);
            rc = PRRTE_ERROR;
            goto cleanup;
        }

        launched++;
    }

    /* indicate that the daemons for this job were launched */
    state->jdata->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;
    daemons->state = PRRTE_JOB_STATE_DAEMONS_LAUNCHED;

    /* flag that launch was successful, so far as we currently know */
    failed_launch = false;

    PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                         "%s plm:tm:launch: finished spawning orteds",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

  cleanup:
    /* cleanup */
    PRRTE_RELEASE(state);

    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRRTE_ACTIVATE_JOB_STATE(daemons, PRRTE_JOB_STATE_FAILED_TO_START);
    }
}

static void poll_spawns(int fd, short args, void *cbdata)
{
    prrte_state_caddy_t *state = (prrte_state_caddy_t*)cbdata;
    int i, rc;
    bool failed_launch = true;
    int local_err;
    tm_event_t event;

    PRRTE_ACQUIRE_OBJECT(state);

    /* TM poll for all the spawns */
    for (i = 0; i < launched; ++i) {
        rc = tm_poll(TM_NULL_EVENT, &event, 1, &local_err);
        if (TM_SUCCESS != rc) {
            prrte_output(0, "plm:tm: failed to poll for a spawned daemon, return status = %d", rc);
            goto cleanup;
        }
        if (TM_SUCCESS != local_err) {
            prrte_output(0, "plm:tm: failed to spawn daemon, error code = %d", local_err );
            goto cleanup;
        }
    }
    failed_launch = false;

  cleanup:
    /* cleanup */
    PRRTE_RELEASE(state);

    /* check for failed launch - if so, force terminate */
    if (failed_launch) {
        PRRTE_ACTIVATE_JOB_STATE(state->jdata, PRRTE_JOB_STATE_FAILED_TO_START);
    }
}


/**
 * Terminate the orteds for a given job
 */
int plm_tm_terminate_orteds(void)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_plm_base_prted_exit(PRRTE_DAEMON_EXIT_CMD))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

static int plm_tm_signal_job(prrte_jobid_t jobid, int32_t signal)
{
    int rc;

    /* order them to pass this signal to their local procs */
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_prted_signal_local_procs(jobid, signal))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}


/*
 * Free stuff
 */
static int plm_tm_finalize(void)
{
    int rc;

    /* cleanup any pending recvs */
    if (PRRTE_SUCCESS != (rc = prrte_plm_base_comm_stop())) {
        PRRTE_ERROR_LOG(rc);
    }

    if (connected) {
        tm_finalize();
        connected = false;
    }

    return PRRTE_SUCCESS;
}


static int plm_tm_connect(void)
{
    int ret;
    struct tm_roots tm_root;
    int count;
    struct timespec tp = {0, 100};

    /* try a couple times to connect - might get busy signals every
       now and then */
    for (count = 0 ; count < 10; ++count) {
        ret = tm_init(NULL, &tm_root);
        if (TM_SUCCESS == ret) {
            return PRRTE_SUCCESS;
        }

        /* provide a very short quiet period so we
         * don't hammer the cpu while we wait
         */
        nanosleep(&tp, NULL);
#ifdef HAVE_SCHED_H
        sched_yield();
#endif
    }

    return PRRTE_ERR_RESOURCE_BUSY;
}
