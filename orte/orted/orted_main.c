/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <string.h>

#include <stdio.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */

#include <pmix_server.h>

#include "opal/event/event-internal.h"
#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/util/cmd_line.h"
#include "opal/util/if.h"
#include "opal/util/net.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_path.h"
#include "opal/util/printf.h"
#include "opal/util/argv.h"
#include "opal/util/fd.h"
#include "opal/runtime/opal.h"
#include "opal/mca/base/mca_base_var.h"
#include "opal/util/daemon_init.h"
#include "opal/dss/dss.h"
#include "opal/hwloc/hwloc-internal.h"
#include "opal/pmix/pmix-internal.h"
#include "opal/mca/compress/compress.h"

#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/name_fns.h"
#include "orte/util/nidmap.h"
#include "orte/util/parse_options.h"
#include "orte/mca/rml/base/rml_contact.h"
#include "orte/util/threads.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/mca/grpcomm/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/odls_private.h"
#include "orte/mca/oob/base/base.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/state/state.h"

/* need access to the create_jobid fn used by plm components
* so we can set singleton name, if necessary
*/
#include "orte/mca/plm/base/plm_private.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_locks.h"
#include "orte/runtime/orte_quit.h"
#include "orte/runtime/orte_wait.h"

#include "orte/orted/orted.h"
#include "orte/orted/pmix/pmix_server.h"
#include "orte/orted/orted_submit.h"

orte_cmd_options_t orte_cmd_options = {0};
opal_cmd_line_t *orte_cmd_line = NULL;

/*
 * Globals
 */
static void shutdown_callback(int fd, short flags, void *arg);
static void rollup(int status, orte_process_name_t* sender,
                   opal_buffer_t *buffer,
                   orte_rml_tag_t tag, void *cbdata);
static void node_regex_report(int status, orte_process_name_t* sender,
                              opal_buffer_t *buffer,
                              orte_rml_tag_t tag, void *cbdata);
static void report_orted(void);

static opal_buffer_t *bucket, *mybucket = NULL;
static int ncollected = 0;
static bool node_regex_waiting = false;

static char *orte_parent_uri = NULL;

static struct {
    bool debug;
    bool help;
    bool set_sid;
    bool daemonize;
    char* name;
    char* vpid_start;
    char* num_procs;
    bool abort;
    bool tree_spawn;
    bool test_suicide;
} orted_globals;

/*
 * define the orted context table for obtaining parameters
 */
opal_cmd_line_init_t orte_cmd_line_opts[] = {
    /* Various "obvious" options */
    { NULL, 'h', NULL, "help", 0,
      &orted_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "This help message" },

    { "orte_daemon_spin", 's', NULL, "spin", 0,
      &orted_spin_flag, OPAL_CMD_LINE_TYPE_BOOL,
      "Have the orted spin until we can connect a debugger to it" },

    { NULL, '\0', NULL, "test-suicide", 1,
      &orted_globals.test_suicide, OPAL_CMD_LINE_TYPE_BOOL,
      "Suicide instead of clean abort after delay" },

    { "orte_debug", 'd', NULL, "debug", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Debug the OpenRTE" },

    { "orte_daemonize", '\0', NULL, "daemonize", 0,
      &orted_globals.daemonize, OPAL_CMD_LINE_TYPE_BOOL,
      "Daemonize the orted into the background" },

    { "orte_debug_daemons", '\0', NULL, "debug-daemons", 0,
      &orted_globals.debug, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE daemons" },

    { "orte_debug_daemons_file", '\0', NULL, "debug-daemons-file", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE daemons, storing output in files" },

    { "orte_hnp_uri", '\0', NULL, "hnp-uri", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "URI for the HNP"},

    { "orte_parent_uri", '\0', NULL, "parent-uri", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "URI for the parent if tree launch is enabled."},

    { NULL, '\0', NULL, "set-sid", 0,
      &orted_globals.set_sid, OPAL_CMD_LINE_TYPE_BOOL,
      "Direct the orted to separate from the current session"},

    { NULL, '\0', "tree-spawn", "tree-spawn", 0,
      &orted_globals.tree_spawn, OPAL_CMD_LINE_TYPE_BOOL,
      "Tree-based spawn in progress" },

    { "tmpdir_base", '\0', NULL, "tmpdir", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Set the root for the session directory tree" },

    { "orte_output_filename", '\0', "output-filename", "output-filename", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename.rank" },

    { "orte_xterm", '\0', "xterm", "xterm", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Create a new xterm window and display output from the specified ranks there" },

    { "orte_report_bindings", '\0', "report-bindings", "report-bindings", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Whether to report process bindings to stderr" },

    /* End of list */
    { NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};

typedef struct {
    opal_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} myxfer_t;

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t release_fn,
                       void *release_cbdata)
{
    myxfer_t *xfer = (myxfer_t*)cbdata;
    size_t n;

    if (NULL != info) {
        xfer->ninfo = ninfo;
        PMIX_INFO_CREATE(xfer->info, xfer->ninfo);
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&xfer->info[n], &info[n]);
        }
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    OPAL_PMIX_WAKEUP_THREAD(&xfer->lock);
}

int orte_daemon(int argc, char *argv[])
{
    int ret = 0;
    opal_cmd_line_t *cmd_line = NULL;
    int i;
    opal_buffer_t *buffer;
    char hostname[OPAL_MAXHOSTNAMELEN];
    pmix_value_t val;
    pmix_proc_t proc;
    pmix_status_t prc;
    myxfer_t xfer;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    opal_byte_object_t bo, *boptr;
    orte_process_name_t target;

    /* initialize the globals */
    memset(&orted_globals, 0, sizeof(orted_globals));
    bucket = OBJ_NEW(opal_buffer_t);

    /* setup to check common command line options that just report and die */
    cmd_line = OBJ_NEW(opal_cmd_line_t);
    if (OPAL_SUCCESS != opal_cmd_line_create(cmd_line, orte_cmd_line_opts)) {
        OBJ_RELEASE(cmd_line);
        exit(1);
    }
    mca_base_cmd_line_setup(cmd_line);
    if (ORTE_SUCCESS != (ret = opal_cmd_line_parse(cmd_line, false, false,
                                                   argc, argv))) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(cmd_line);
        fprintf(stderr, "Usage: %s [OPTION]...\n%s\n", argv[0], args);
        free(args);
        OBJ_RELEASE(cmd_line);
        return ret;
    }

    /*
     * Since this process can now handle MCA/GMCA parameters, make sure to
     * process them.
     */
    mca_base_cmd_line_process_args(cmd_line, &environ, &environ);

    /* Ensure that enough of OPAL is setup for us to be able to run */
    /*
     * NOTE: (JJH)
     *  We need to allow 'mca_base_cmd_line_process_args()' to process command
     *  line arguments *before* calling opal_init_util() since the command
     *  line could contain MCA parameters that affect the way opal_init_util()
     *  functions. AMCA parameters are one such option normally received on the
     *  command line that affect the way opal_init_util() behaves.
     *  It is "safe" to call mca_base_cmd_line_process_args() before
     *  opal_init_util() since mca_base_cmd_line_process_args() does *not*
     *  depend upon opal_init_util() functionality.
     */
    if (OPAL_SUCCESS != opal_init_util(&argc, &argv)) {
        fprintf(stderr, "OPAL failed to initialize -- orted aborting\n");
        exit(1);
    }

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars that were set in the enviro when the
     * orted was executed - e.g., by .csh
     */
    orte_launch_environ = opal_argv_copy(environ);

    /* purge any ess/pmix flags set in the environ when we were launched */
    opal_unsetenv(OPAL_MCA_PREFIX"ess", &orte_launch_environ);

    /* if orte_daemon_debug is set, let someone know we are alive right
     * away just in case we have a problem along the way
     */
    if (orted_globals.debug) {
        gethostname(hostname, sizeof(hostname));
        fprintf(stderr, "Daemon was launched on %s - beginning to initialize\n", hostname);
    }

    /* check for help request */
    if (orted_globals.help) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(cmd_line);
        orte_show_help("help-orted.txt", "orted:usage", false,
                       argv[0], args);
        free(args);
        return 1;
    }
#if defined(HAVE_SETSID)
    /* see if we were directed to separate from current session */
    if (orted_globals.set_sid) {
        setsid();
    }
#endif
    /* see if they want us to spin until they can connect a debugger to us */
    i=0;
    while (orted_spin_flag) {
        i++;
        if (1000 < i) i=0;
    }

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if(!orte_debug_flag &&
       !orte_debug_daemons_flag &&
       orted_globals.daemonize) {
        opal_daemon_init(NULL);
    }

    if (ORTE_SUCCESS != (ret = orte_init(&argc, &argv, ORTE_PROC_DAEMON))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* finalize the OPAL utils. As they are opened again from orte_init->opal_init
     * we continue to have a reference count on them. So we have to finalize them twice...
     */
    opal_finalize_util();

    /* bind ourselves if so directed */
    if (NULL != orte_daemon_cores) {
        char **cores=NULL, tmp[128];
        hwloc_obj_t pu;
        hwloc_cpuset_t ours, res;
        int core;

        /* could be a collection of comma-delimited ranges, so
         * use our handy utility to parse it
         */
        orte_util_parse_range_options(orte_daemon_cores, &cores);
        if (NULL != cores) {
            ours = hwloc_bitmap_alloc();
            hwloc_bitmap_zero(ours);
            res = hwloc_bitmap_alloc();
            for (i=0; NULL != cores[i]; i++) {
                core = strtoul(cores[i], NULL, 10);
                if (NULL == (pu = opal_hwloc_base_get_pu(opal_hwloc_topology, core, OPAL_HWLOC_LOGICAL))) {
                    /* turn off the show help forwarding as we won't
                     * be able to cycle the event library to send
                     */
                    orte_show_help_finalize();
                    /* the message will now come out locally */
                    orte_show_help("help-orted.txt", "orted:cannot-bind",
                                   true, orte_process_info.nodename,
                                   orte_daemon_cores);
                    ret = ORTE_ERR_NOT_SUPPORTED;
                    hwloc_bitmap_free(ours);
                    hwloc_bitmap_free(res);
                    goto DONE;
                }
                hwloc_bitmap_or(res, ours, pu->cpuset);
                hwloc_bitmap_copy(ours, res);
            }
            /* if the result is all zeros, then don't bind */
            if (!hwloc_bitmap_iszero(ours)) {
                (void)hwloc_set_cpubind(opal_hwloc_topology, ours, 0);
                if (opal_hwloc_report_bindings) {
                    opal_hwloc_base_cset2mapstr(tmp, sizeof(tmp), opal_hwloc_topology, ours);
                    opal_output(0, "Daemon %s is bound to cores %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), tmp);
                }
            }
            /* cleanup */
            hwloc_bitmap_free(ours);
            hwloc_bitmap_free(res);
            opal_argv_free(cores);
        }
    }

    if ((int)ORTE_VPID_INVALID != orted_debug_failure) {
        orted_globals.abort=false;
        /* some vpid was ordered to fail. The value can be positive
         * or negative, depending upon the desired method for failure,
         * so need to check both here
         */
        if (0 > orted_debug_failure) {
            orted_debug_failure = -1*orted_debug_failure;
            orted_globals.abort = true;
        }
        /* are we the specified vpid? */
        if ((int)ORTE_PROC_MY_NAME->vpid == orted_debug_failure) {
            /* if the user specified we delay, then setup a timer
             * and have it kill us
             */
            if (0 < orted_debug_failure_delay) {
                ORTE_TIMER_EVENT(orted_debug_failure_delay, 0, shutdown_callback, ORTE_SYS_PRI);

            } else {
                opal_output(0, "%s is executing clean %s", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            orted_globals.abort ? "abort" : "abnormal termination");

                /* do -not- call finalize as this will send a message to the HNP
                 * indicating clean termination! Instead, just forcibly cleanup
                 * the local session_dir tree and exit
                 */
                orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);

                /* if we were ordered to abort, do so */
                if (orted_globals.abort) {
                    abort();
                }

                /* otherwise, return with non-zero status */
                ret = ORTE_ERROR_DEFAULT_EXIT_CODE;
                goto DONE;
            }
        }
    }

    /* insert our contact info into our process_info struct so we
     * have it for later use and set the local daemon field to our name
     */
    orte_oob_base_get_addr(&orte_process_info.my_daemon_uri);
    if (NULL == orte_process_info.my_daemon_uri) {
        /* no way to communicate */
        ret = ORTE_ERROR;
        goto DONE;
    }
    ORTE_PROC_MY_DAEMON->jobid = ORTE_PROC_MY_NAME->jobid;
    ORTE_PROC_MY_DAEMON->vpid = ORTE_PROC_MY_NAME->vpid;
    PMIX_VALUE_LOAD(&val, orte_process_info.my_daemon_uri, OPAL_STRING);
    (void)opal_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, ORTE_PROC_MY_NAME->jobid);
    proc.rank = ORTE_PROC_MY_NAME->vpid;
    if (PMIX_SUCCESS != (prc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
        PMIX_ERROR_LOG(prc);
        PMIX_VALUE_DESTRUCT(&val);
        ret = ORTE_ERROR;
        goto DONE;
    }
    PMIX_VALUE_DESTRUCT(&val);

    /* setup the primary daemon command receive function */
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_DAEMON,
                            ORTE_RML_PERSISTENT, orte_daemon_recv, NULL);

    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (orte_debug_daemons_flag) {
        fprintf(stderr, "Daemon %s checking in as pid %ld on host %s\n",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (long)orte_process_info.pid,
                orte_process_info.nodename);
    }

    /* We actually do *not* want the orted to voluntarily yield() the
       processor more than necessary.  The orted already blocks when
       it is doing nothing, so it doesn't use any more CPU cycles than
       it should; but when it *is* doing something, we do not want it
       to be unnecessarily delayed because it voluntarily yielded the
       processor in the middle of its work.

       For example: when a message arrives at the orted, we want the
       OS to wake up the orted in a timely fashion (which most OS's
       seem good about doing) and then we want the orted to process
       the message as fast as possible.  If the orted yields and lets
       aggressive MPI applications get the processor back, it may be a
       long time before the OS schedules the orted to run again
       (particularly if there is no IO event to wake it up).  Hence,
       routed OOB messages (for example) may be significantly delayed
       before being delivered to MPI processes, which can be
       problematic in some scenarios (e.g., COMM_SPAWN, BTL's that
       require OOB messages for wireup, etc.). */
    opal_progress_set_yield_when_idle(false);

    /* Change the default behavior of libevent such that we want to
       continually block rather than blocking for the default timeout
       and then looping around the progress engine again.  There
       should be nothing in the orted that cannot block in libevent
       until "something" happens (i.e., there's no need to keep
       cycling through progress because the only things that should
       happen will happen in libevent).  This is a minor optimization,
       but what the heck... :-) */
    opal_progress_set_event_flag(OPAL_EVLOOP_ONCE);

    /* If I have a parent, then save his contact info so
     * any messages we send can flow thru him.
     */
    orte_parent_uri = NULL;
    (void) mca_base_var_register ("orte", "orte", NULL, "parent_uri",
                                  "URI for the parent if tree launch is enabled.",
                                  MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                  MCA_BASE_VAR_FLAG_INTERNAL,
                                  OPAL_INFO_LVL_9,
                                  MCA_BASE_VAR_SCOPE_CONSTANT,
                                  &orte_parent_uri);
    if (NULL != orte_parent_uri) {
        /* set the contact info into our local database */
        ret = orte_rml_base_parse_uris(orte_parent_uri, ORTE_PROC_MY_PARENT, NULL);
        if (ORTE_SUCCESS != ret) {
            ORTE_ERROR_LOG(ret);
            goto DONE;
        }
        PMIX_VALUE_LOAD(&val, orte_parent_uri, OPAL_STRING);
        OPAL_PMIX_CONVERT_NAME(&proc, ORTE_PROC_MY_PARENT);
        if (PMIX_SUCCESS != (prc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
            PMIX_ERROR_LOG(prc);
            PMIX_VALUE_DESTRUCT(&val);
            ret = ORTE_ERROR;
            goto DONE;
        }
        PMIX_VALUE_DESTRUCT(&val);

        /* tell the routed module that we have a path
         * back to the HNP
         */
        if (ORTE_SUCCESS != (ret = orte_routed.update_route(ORTE_PROC_MY_HNP, ORTE_PROC_MY_PARENT))) {
            ORTE_ERROR_LOG(ret);
            goto DONE;
        }
        /* and a path to our parent */
        if (ORTE_SUCCESS != (ret = orte_routed.update_route(ORTE_PROC_MY_PARENT, ORTE_PROC_MY_PARENT))) {
            ORTE_ERROR_LOG(ret);
            goto DONE;
        }
        /* set the lifeline to point to our parent so that we
         * can handle the situation if that lifeline goes away
         */
        if (ORTE_SUCCESS != (ret = orte_routed.set_lifeline(ORTE_PROC_MY_PARENT))) {
            ORTE_ERROR_LOG(ret);
            goto DONE;
        }
    }

    /* setup the rollup callback */
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_ORTED_CALLBACK,
                            ORTE_RML_PERSISTENT, rollup, NULL);

    /* define the target jobid */
    target.jobid = ORTE_PROC_MY_NAME->jobid;
    if (orte_fwd_mpirun_port || orte_static_ports || NULL != orte_parent_uri) {
        /* we start by sending to ourselves */
        target.vpid = ORTE_PROC_MY_NAME->vpid;
        /* since we will be waiting for any children to send us
         * their rollup info before sending to our parent, save
         * a little time in the launch phase by "warming up" the
         * connection to our parent while we wait for our children */
        buffer = OBJ_NEW(opal_buffer_t);  // zero-byte message
        orte_rml.recv_buffer_nb(ORTE_PROC_MY_PARENT, ORTE_RML_TAG_NODE_REGEX_REPORT,
                                ORTE_RML_PERSISTENT, node_regex_report, &node_regex_waiting);
        node_regex_waiting = true;
        if (0 > (ret = orte_rml.send_buffer_nb(ORTE_PROC_MY_PARENT, buffer,
                                               ORTE_RML_TAG_WARMUP_CONNECTION,
                                               orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(buffer);
            goto DONE;
        }
    } else {
        target.vpid = 0;
    }

    /* send the information to the orted report-back point - this function
     * will process the data, but also counts the number of
     * orteds that reported back so the launch procedure can continue.
     * We need to do this at the last possible second as the HNP
     * can turn right around and begin issuing orders to us
     */

    buffer = OBJ_NEW(opal_buffer_t);
    /* insert our name for rollup purposes */
    if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, ORTE_PROC_MY_NAME, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(buffer);
        goto DONE;
    }

    /* get any connection info we may have pushed */
    {
        pmix_info_t *info;
        size_t ninfo;
        pmix_value_t *vptr;

        OPAL_PMIX_CONVERT_NAME(&proc, ORTE_PROC_MY_NAME);
        boptr = &bo;
        bo.bytes = NULL;
        bo.size = 0;
        if (PMIX_SUCCESS == PMIx_Get(&proc, NULL, NULL, 0, &vptr) && NULL != vptr) {
            /* the data is returned as a pmix_data_array_t */
            if (PMIX_DATA_ARRAY != vptr->type || NULL == vptr->data.darray ||
                PMIX_INFO != vptr->data.darray->type || NULL == vptr->data.darray->array) {
                ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                OBJ_RELEASE(buffer);
                goto DONE;
            }
            /* use the PMIx data support to pack it */
            info = (pmix_info_t*)vptr->data.darray->array;
            ninfo = vptr->data.darray->size;
            PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&proc, &pbuf, &ninfo, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(prc);
                ret = ORTE_ERROR;
                OBJ_RELEASE(buffer);
                goto DONE;
            }
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&proc, &pbuf, info, ninfo, PMIX_INFO))) {
                PMIX_ERROR_LOG(prc);
                ret = ORTE_ERROR;
                OBJ_RELEASE(buffer);
                goto DONE;
            }
            PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
            bo.bytes = (uint8_t*)pbo.bytes;
            bo.size = pbo.size;
            PMIX_VALUE_RELEASE(vptr);
        }
        if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &boptr, 1, OPAL_BYTE_OBJECT))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(buffer);
            goto DONE;
        }
    }

    /* include our node name */
    opal_dss.pack(buffer, &orte_process_info.nodename, 1, OPAL_STRING);

    /* if requested, include any non-loopback aliases for this node */
    if (orte_retain_aliases) {
        char **aliases=NULL;
        uint8_t naliases, ni;
        char hostname[OPAL_MAXHOSTNAMELEN];

        /* if we stripped the prefix or removed the fqdn,
         * include full hostname as an alias
         */
        gethostname(hostname, sizeof(hostname));
        if (strlen(orte_process_info.nodename) < strlen(hostname)) {
            opal_argv_append_nosize(&aliases, hostname);
        }
        opal_ifgetaliases(&aliases);
        naliases = opal_argv_count(aliases);
        if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &naliases, 1, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(buffer);
            opal_argv_free(aliases);
            goto DONE;
        }
        for (ni=0; ni < naliases; ni++) {
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &aliases[ni], 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(ret);
                OBJ_RELEASE(buffer);
                opal_argv_free(aliases);
                goto DONE;
            }
        }
        opal_argv_free(aliases);
    }

    /* always send back our topology signature - this is a small string
     * and won't hurt anything */
    if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &orte_topo_signature, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
    }

    /* if we are rank=1, then send our topology back - otherwise, mpirun
     * will request it if necessary */
    if (1 == ORTE_PROC_MY_NAME->vpid) {
        opal_buffer_t data;
        int8_t flag;
        uint8_t *cmpdata;
        size_t cmplen;

        /* setup an intermediate buffer */
        OBJ_CONSTRUCT(&data, opal_buffer_t);

        if (ORTE_SUCCESS != (ret = opal_dss.pack(&data, &opal_hwloc_topology, 1, OPAL_HWLOC_TOPO))) {
            ORTE_ERROR_LOG(ret);
        }
        if (opal_compress.compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                                         &cmpdata, &cmplen)) {
            /* the data was compressed - mark that we compressed it */
            flag = 1;
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &flag, 1, OPAL_INT8))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
            }
            /* pack the compressed length */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &cmplen, 1, OPAL_SIZE))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
            }
            /* pack the uncompressed length */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &data.bytes_used, 1, OPAL_SIZE))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
            }
            /* pack the compressed info */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, cmpdata, cmplen, OPAL_UINT8))) {
                ORTE_ERROR_LOG(ret);
                free(cmpdata);
                OBJ_DESTRUCT(&data);
            }
            OBJ_DESTRUCT(&data);
            free(cmpdata);
        } else {
            /* mark that it was not compressed */
            flag = 0;
            if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &flag, 1, OPAL_INT8))) {
                ORTE_ERROR_LOG(ret);
                OBJ_DESTRUCT(&data);
                free(cmpdata);
            }
            /* transfer the payload across */
            opal_dss.copy_payload(buffer, &data);
            OBJ_DESTRUCT(&data);
        }
    }

    /* collect our network inventory */
    memset(&xfer, 0, sizeof(myxfer_t));
    OPAL_PMIX_CONSTRUCT_LOCK(&xfer.lock);
    if (PMIX_SUCCESS != (prc = PMIx_server_collect_inventory(NULL, 0, infocbfunc, &xfer))) {
        PMIX_ERROR_LOG(prc);
        ret = ORTE_ERR_NOT_SUPPORTED;
        goto DONE;
    }
    OPAL_PMIX_WAIT_THREAD(&xfer.lock);
    if (NULL != xfer.info) {
        PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, &pbuf, &xfer.ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(prc);
            ret = ORTE_ERROR;
            OBJ_RELEASE(buffer);
            goto DONE;
        }
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, &pbuf, xfer.info, xfer.ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(prc);
            ret = ORTE_ERROR;
            OBJ_RELEASE(buffer);
            goto DONE;
        }
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        if (ORTE_SUCCESS != (ret = opal_dss.pack(buffer, &boptr, 1, OPAL_BYTE_OBJECT))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(buffer);
            goto DONE;
        }
    }
    /* send it to the designated target */
    if (0 > (ret = orte_rml.send_buffer_nb(&target, buffer,
                                           ORTE_RML_TAG_ORTED_CALLBACK,
                                           orte_rml_send_callback, NULL))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(buffer);
        goto DONE;
    }

    /* if we are tree-spawning, then we need to capture the MCA params
     * from our cmd line so we can pass them along to the daemons we spawn -
     * otherwise, only the first layer of daemons will ever see them
     */
    if (orted_globals.tree_spawn) {
        int j, k;
        bool ignore;
        char *no_keep[] = {
            "orte_hnp_uri",
            "orte_ess_jobid",
            "orte_ess_vpid",
            "orte_ess_num_procs",
            "orte_parent_uri",
            "mca_base_env_list",
            NULL
        };
        for (i=0; i < argc; i++) {
            if (0 == strcmp("-"OPAL_MCA_CMD_LINE_ID,  argv[i]) ||
                0 == strcmp("--"OPAL_MCA_CMD_LINE_ID, argv[i]) ) {
                ignore = false;
                /* see if this is something we cannot pass along */
                for (k=0; NULL != no_keep[k]; k++) {
                    if (0 == strcmp(no_keep[k], argv[i+1])) {
                        ignore = true;
                        break;
                    }
                }
                if (!ignore) {
                    /* see if this is already present so we at least can
                     * avoid growing the cmd line with duplicates
                     */
                    if (NULL != orted_cmd_line) {
                        for (j=0; NULL != orted_cmd_line[j]; j++) {
                            if (0 == strcmp(argv[i+1], orted_cmd_line[j])) {
                                /* already here - ignore it */
                                ignore = true;
                                break;
                            }
                        }
                    }
                    if (!ignore) {
                        opal_argv_append_nosize(&orted_cmd_line, argv[i]);
                        opal_argv_append_nosize(&orted_cmd_line, argv[i+1]);
                        opal_argv_append_nosize(&orted_cmd_line, argv[i+2]);
                    }
                }
                i += 2;
            }
        }
    }

    if (orte_debug_daemons_flag) {
        opal_output(0, "%s orted: up and running - waiting for commands!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    ret = ORTE_SUCCESS;

    /* loop the event lib until an exit event is detected */
    while (orte_event_base_active) {
        opal_event_loop(orte_event_base, OPAL_EVLOOP_ONCE);
    }
    ORTE_ACQUIRE_OBJECT(orte_event_base_active);

    /* ensure all local procs are dead */
    orte_odls.kill_local_procs(NULL);

  DONE:
    /* update the exit status, in case it wasn't done */
    ORTE_UPDATE_EXIT_STATUS(ret);

    /* cleanup and leave */
    orte_finalize();
    opal_finalize_util();

    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    /* cleanup the process info */
    orte_proc_info_finalize();

    if (orte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", orte_exit_status);
    }
    exit(orte_exit_status);
}

static void shutdown_callback(int fd, short flags, void *arg)
{
    orte_timer_t *tm = (orte_timer_t*)arg;

    if (NULL != tm) {
        /* release the timer */
        OBJ_RELEASE(tm);
    }

    /* if we were ordered to abort, do so */
    if (orted_globals.abort) {
        opal_output(0, "%s is executing %s abort", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    (orted_globals.test_suicide) ? "suicide" : "clean");
        /* do -not- call finalize as this will send a message to the HNP
         * indicating clean termination! Instead, just kill our
         * local procs, forcibly cleanup the local session_dir tree, and abort
         */
        if (orted_globals.test_suicide) {
            exit(1);
        }
        orte_odls.kill_local_procs(NULL);
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
        abort();
    }
    opal_output(0, "%s is executing clean abnormal termination", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    /* do -not- call finalize as this will send a message to the HNP
     * indicating clean termination! Instead, just forcibly cleanup
     * the local session_dir tree and exit
     */
    orte_odls.kill_local_procs(NULL);
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    exit(ORTE_ERROR_DEFAULT_EXIT_CODE);
}

static void rollup(int status, orte_process_name_t* sender,
                   opal_buffer_t *buffer,
                   orte_rml_tag_t tag, void *cbdata)
{
    int ret;
    orte_process_name_t child;
    int32_t flag, cnt;
    opal_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_info_t *info;
    pmix_proc_t proc;
    pmix_status_t prc;

    ncollected++;

    /* if the sender is ourselves, then we save that buffer
     * so we can insert it at the beginning */
    if (sender->jobid == ORTE_PROC_MY_NAME->jobid &&
        sender->vpid == ORTE_PROC_MY_NAME->vpid) {
        mybucket = OBJ_NEW(opal_buffer_t);
        opal_dss.copy_payload(mybucket, buffer);
    } else {
        /* xfer the contents of the rollup to our bucket */
        opal_dss.copy_payload(bucket, buffer);
        /* the first entry in the bucket will be from our
         * direct child - harvest it for connection info */
        cnt = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &child, &cnt, ORTE_NAME))) {
            ORTE_ERROR_LOG(ret);
            goto report;
        }
        cnt = 1;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &flag, &cnt, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            goto report;
        }
        if (0 < flag) {
            (void)opal_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, sender->jobid);
            proc.rank = sender->vpid;
            /* we have connection info */
            cnt = 1;
            if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &boptr, &cnt, OPAL_BYTE_OBJECT))) {
                ORTE_ERROR_LOG(ret);
                goto report;
            }
            /* it was packed using PMIx, so unpack it the same way */
            PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
            PMIX_INFO_CREATE(info, (size_t)flag);
            if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&proc, &pbkt, (void*)info, &flag, PMIX_INFO))) {
                PMIX_ERROR_LOG(prc);
                goto report;
            }
            for (cnt=0; cnt < flag; cnt++) {
                prc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &info[cnt].value);
                if (PMIX_SUCCESS != prc) {
                    PMIX_ERROR_LOG(prc);
                    PMIX_INFO_FREE(info, (size_t)flag);
                    goto report;
                }
            }
            PMIX_INFO_FREE(info, (size_t)flag);
        }
    }

  report:
    report_orted();
}

static void report_orted() {
    int nreqd, ret;

    /* get the number of children */
    nreqd = orte_routed.num_routes() + 1;
    if (nreqd == ncollected && NULL != mybucket && !node_regex_waiting) {
        /* add the collection of our children's buckets to ours */
        opal_dss.copy_payload(mybucket, bucket);
        OBJ_RELEASE(bucket);
        /* relay this on to our parent */
        if (0 > (ret = orte_rml.send_buffer_nb(ORTE_PROC_MY_PARENT, mybucket,
                                               ORTE_RML_TAG_ORTED_CALLBACK,
                                               orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(mybucket);
        }
    }
}

static void node_regex_report(int status, orte_process_name_t* sender,
                              opal_buffer_t *buffer,
                              orte_rml_tag_t tag, void *cbdata) {
    int rc;
    bool * active = (bool *)cbdata;

    /* extract the node info if needed, and update the routing tree */
    if (ORTE_SUCCESS != (rc = orte_util_decode_nidmap(buffer))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* update the routing tree so any tree spawn operation
     * properly gets the number of children underneath us */
    orte_routed.update_routing_plan();

    *active = false;

    /* now launch any child daemons of ours */
    orte_plm.remote_spawn();

    report_orted();
}
