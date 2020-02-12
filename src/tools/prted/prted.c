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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

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
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include <pmix_server.h>

#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/util/output.h"
#include "src/util/basename.h"
#include "src/util/cmd_line.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_path.h"
#include "src/util/printf.h"
#include "src/util/argv.h"
#include "src/util/fd.h"
#include "src/mca/base/prrte_mca_base_var.h"
#include "src/util/daemon_init.h"
#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/compress/compress.h"

#include "src/util/show_help.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/parse_options.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/threads/threads.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/ras/ras.h"
#include "src/mca/routed/routed.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"

/* need access to the create_jobid fn used by plm components
* so we can set singleton name, if necessary
*/
#include "src/mca/plm/base/plm_private.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/prrte_quit.h"
#include "src/runtime/prrte_wait.h"

#include "src/prted/prted.h"
#include "src/prted/pmix/pmix_server.h"

/*
 * Globals
 */
static void shutdown_callback(int fd, short flags, void *arg);
static void rollup(int status, prrte_process_name_t* sender,
                   prrte_buffer_t *buffer,
                   prrte_rml_tag_t tag, void *cbdata);
static void node_regex_report(int status, prrte_process_name_t* sender,
                              prrte_buffer_t *buffer,
                              prrte_rml_tag_t tag, void *cbdata);
static void report_prted(void);

static prrte_buffer_t *bucket, *mybucket = NULL;
static int ncollected = 0;
static bool node_regex_waiting = false;
static bool prted_abort = false;
static char *prrte_parent_uri = NULL;
static prrte_cmd_line_t *prrte_cmd_line = NULL;

/*
 * define the orted context table for obtaining parameters
 */
prrte_cmd_line_init_t prrte_cmd_line_opts[] = {
    /* DVM-specific options */
    /* uri of PMIx publish/lookup server, or at least where to get it */
    { '\0', "prrte-server", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "dvm-master-uri", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "URI for the DVM master",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "parent-uri", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "URI for the parent if tree launch is enabled.",
      PRRTE_CMD_LINE_OTYPE_DVM },
    { '\0', "tree-spawn", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Tree-based spawn in progress",
      PRRTE_CMD_LINE_OTYPE_DVM },

    /* Debug options */
    { '\0', "debug-daemons", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Debug daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { 'd', "debug-devel", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of PRRTE",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "debug-daemons-file", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of any PRRTE daemons used by this application, storing output in files",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0', "leave-session-attached", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not discard stdout/stderr of remote PRTE daemons",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { '\0',  "test-suicide", 1, PRRTE_CMD_LINE_TYPE_BOOL,
      "Suicide instead of clean abort after delay",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

typedef struct {
    prrte_pmix_lock_t lock;
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
    PRRTE_PMIX_WAKEUP_THREAD(&xfer->lock);
}

static int wait_pipe[2];

static int wait_dvm(pid_t pid) {
    char reply;
    int rc;
    int status;

    close(wait_pipe[1]);
    do {
        rc = read(wait_pipe[0], &reply, 1);
    } while (0 > rc && EINTR == errno);

    if (1 == rc && 'K' == reply) {
        return 0;
    } else if (0 == rc) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
    }
    return 255;
}

int main(int argc, char *argv[])
{
    int ret = 0;
    int i;
    prrte_buffer_t *buffer;
    pmix_value_t val;
    pmix_proc_t proc;
    pmix_status_t prc;
    myxfer_t xfer;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    prrte_byte_object_t bo, *boptr;
    prrte_process_name_t target;

    char *umask_str = getenv("PRRTE_DAEMON_UMASK_VALUE");
    if (NULL != umask_str) {
        char *endptr;
        long mask = strtol(umask_str, &endptr, 8);
        if ((! (0 == mask && (EINVAL == errno || ERANGE == errno))) &&
            (*endptr == '\0')) {
            umask(mask);
        }
    }

    /* initialize the globals */
    bucket = PRRTE_NEW(prrte_buffer_t);

    /* init the tiny part of PRRTE we use */
    prrte_init_util();
    prrte_tool_basename = prrte_basename(argv[0]);

    /* setup our cmd line */
    prrte_cmd_line = PRRTE_NEW(prrte_cmd_line_t);
    if (PRRTE_SUCCESS != (ret = prrte_cmd_line_add(prrte_cmd_line, prrte_cmd_line_opts))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    /* open the SCHIZO framework */
    if (PRRTE_SUCCESS != (ret = prrte_mca_base_framework_open(&prrte_schizo_base_framework, 0))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    if (PRRTE_SUCCESS != (ret = prrte_schizo_base_select())) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }
    /* scan for personalities */
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "prrte", false);
    prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "pmix", false);
    for (i=0; NULL != argv[i]; i++) {
        if (0 == strcmp(argv[i], "--personality")) {
            prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, argv[i+1], false);
        }
    }

    /* setup the rest of the cmd line only once */
    if (PRRTE_SUCCESS != (ret = prrte_schizo.define_cli(prrte_cmd_line))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    /* parse the result to get values - this will not include MCA params */
    if (PRRTE_SUCCESS != (ret = prrte_cmd_line_parse(prrte_cmd_line,
                                                    true, false, argc, argv)) ) {
        if (PRRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(ret));
        }
       return ret;
    }

    /* now let the schizo components take a pass at it to get the MCA params */
    if (PRRTE_SUCCESS != (ret = prrte_schizo.parse_cli(argc, 0, argv, NULL, NULL))) {
        if (PRRTE_ERR_SILENT != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    prrte_strerror(ret));
        }
       return ret;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
     if (prrte_cmd_line_is_taken(prrte_cmd_line, "version")) {
        fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                prrte_tool_basename, "PMIx Reference RunTime Environment",
                PRRTE_VERSION, PACKAGE_BUGREPORT);
        exit(0);
    }

    /* Check for help request */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "help")) {
        char *str, *args = NULL;
        args = prrte_cmd_line_get_usage_msg(prrte_cmd_line, false);
        str = prrte_show_help_string("help-prun.txt", "prun:usage", false,
                                    prrte_tool_basename, "PRRTE", PRRTE_VERSION,
                                    prrte_tool_basename, args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* check if we are running as root - if we are, then only allow
     * us to proceed if the allow-run-as-root flag was given. Otherwise,
     * exit with a giant warning flag
     */
    if (0 == geteuid()) {
        prrte_schizo.allow_run_as_root(prrte_cmd_line);  // will exit us if not allowed
    }

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars that were set in the enviro when the
     * orted was executed - e.g., by .csh
     */
    prrte_launch_environ = prrte_argv_copy(environ);

    /* purge any ess/prrte flags set in the environ when we were launched */
    prrte_unsetenv("PRRTE_MCA_ess", &prrte_launch_environ);

    /* if prrte_daemon_debug is set, let someone know we are alive right
     * away just in case we have a problem along the way
     */
    prrte_debug_flag = prrte_cmd_line_is_taken(prrte_cmd_line, "debug");
    prrte_debug_daemons_flag = prrte_cmd_line_is_taken(prrte_cmd_line, "debug-daemons");
    if (prrte_debug_daemons_flag) {
        fprintf(stderr, "Daemon was launched on %s - beginning to initialize\n", prrte_process_info.nodename);
    }

#if defined(HAVE_SETSID)
    /* see if we were directed to separate from current session */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "set-sid")) {
        setsid();
    }
#endif

    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if (!prrte_debug_flag &&
        !prrte_debug_daemons_flag &&
        prrte_cmd_line_is_taken(prrte_cmd_line, "daemonize")) {
        pipe(wait_pipe);
        prrte_state_base_parent_fd = wait_pipe[1];
        prrte_daemon_init_callback(NULL, wait_dvm);
        close(wait_pipe[0]);
    }

    if (PRRTE_SUCCESS != (ret = prrte_init(&argc, &argv, PRRTE_PROC_DAEMON))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    /* bind ourselves if so directed */
    if (NULL != prrte_daemon_cores) {
        char **cores=NULL, tmp[128];
        hwloc_obj_t pu;
        hwloc_cpuset_t ours, res;
        int core;

        /* could be a collection of comma-delimited ranges, so
         * use our handy utility to parse it
         */
        prrte_util_parse_range_options(prrte_daemon_cores, &cores);
        if (NULL != cores) {
            ours = hwloc_bitmap_alloc();
            hwloc_bitmap_zero(ours);
            res = hwloc_bitmap_alloc();
            for (i=0; NULL != cores[i]; i++) {
                core = strtoul(cores[i], NULL, 10);
                if (NULL == (pu = prrte_hwloc_base_get_pu(prrte_hwloc_topology, core, PRRTE_HWLOC_LOGICAL))) {
                    /* the message will now come out locally */
                    prrte_show_help("help-orted.txt", "orted:cannot-bind",
                                   true, prrte_process_info.nodename,
                                   prrte_daemon_cores);
                    ret = PRRTE_ERR_NOT_SUPPORTED;
                    hwloc_bitmap_free(ours);
                    hwloc_bitmap_free(res);
                    goto DONE;
                }
                hwloc_bitmap_or(res, ours, pu->cpuset);
                hwloc_bitmap_copy(ours, res);
            }
            /* if the result is all zeros, then don't bind */
            if (!hwloc_bitmap_iszero(ours)) {
                (void)hwloc_set_cpubind(prrte_hwloc_topology, ours, 0);
                if (prrte_hwloc_report_bindings) {
                    prrte_hwloc_base_cset2mapstr(tmp, sizeof(tmp), prrte_hwloc_topology, ours);
                    prrte_output(0, "Daemon %s is bound to cores %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), tmp);
                }
            }
            /* cleanup */
            hwloc_bitmap_free(ours);
            hwloc_bitmap_free(res);
            prrte_argv_free(cores);
        }
    }

    if ((int)PRRTE_VPID_INVALID != prted_debug_failure) {
        prted_abort=false;
        /* some vpid was ordered to fail. The value can be positive
         * or negative, depending upon the desired method for failure,
         * so need to check both here
         */
        if (0 > prted_debug_failure) {
            prted_debug_failure = -1*prted_debug_failure;
            prted_abort = true;
        }
        /* are we the specified vpid? */
        if ((int)PRRTE_PROC_MY_NAME->vpid == prted_debug_failure) {
            /* if the user specified we delay, then setup a timer
             * and have it kill us
             */
            if (0 < prted_debug_failure_delay) {
                PRRTE_TIMER_EVENT(prted_debug_failure_delay, 0, shutdown_callback, PRRTE_SYS_PRI);

            } else {
                prrte_output(0, "%s is executing clean %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            prted_abort ? "abort" : "abnormal termination");

                /* do -not- call finalize as this will send a message to the HNP
                 * indicating clean termination! Instead, just forcibly cleanup
                 * the local session_dir tree and exit
                 */
                prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);

                /* if we were ordered to abort, do so */
                if (prted_abort) {
                    abort();
                }

                /* otherwise, return with non-zero status */
                ret = PRRTE_ERROR_DEFAULT_EXIT_CODE;
                goto DONE;
            }
        }
    }

    /* insert our contact info into our process_info struct so we
     * have it for later use and set the local daemon field to our name
     */
    prrte_oob_base_get_addr(&prrte_process_info.my_daemon_uri);
    if (NULL == prrte_process_info.my_daemon_uri) {
        /* no way to communicate */
        ret = PRRTE_ERROR;
        goto DONE;
    }
    PRRTE_PROC_MY_DAEMON->jobid = PRRTE_PROC_MY_NAME->jobid;
    PRRTE_PROC_MY_DAEMON->vpid = PRRTE_PROC_MY_NAME->vpid;
    PMIX_VALUE_LOAD(&val, prrte_process_info.my_daemon_uri, PRRTE_STRING);
    (void)prrte_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, PRRTE_PROC_MY_NAME->jobid);
    proc.rank = PRRTE_PROC_MY_NAME->vpid;
    if (PMIX_SUCCESS != (prc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
        PMIX_ERROR_LOG(prc);
        PMIX_VALUE_DESTRUCT(&val);
        ret = PRRTE_ERROR;
        goto DONE;
    }
    PMIX_VALUE_DESTRUCT(&val);

    /* setup the primary daemon command receive function */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DAEMON,
                            PRRTE_RML_PERSISTENT, prrte_daemon_recv, NULL);

    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (prrte_debug_daemons_flag) {
        fprintf(stderr, "Daemon %s checking in as pid %ld on host %s\n",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (long)prrte_process_info.pid,
                prrte_process_info.nodename);
    }

    /* If I have a parent, then save his contact info so
     * any messages we send can flow thru him.
     */
    prrte_parent_uri = NULL;
    (void) prrte_mca_base_var_register ("prrte", "prrte", NULL, "parent_uri",
                                  "URI for the parent if tree launch is enabled.",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                  PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                  PRRTE_INFO_LVL_9,
                                  PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                  &prrte_parent_uri);
    if (NULL != prrte_parent_uri) {
        /* set the contact info into our local database */
        ret = prrte_rml_base_parse_uris(prrte_parent_uri, PRRTE_PROC_MY_PARENT, NULL);
        if (PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
            goto DONE;
        }
        PMIX_VALUE_LOAD(&val, prrte_parent_uri, PRRTE_STRING);
        PRRTE_PMIX_CONVERT_NAME(&proc, PRRTE_PROC_MY_PARENT);
        if (PMIX_SUCCESS != (prc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
            PMIX_ERROR_LOG(prc);
            PMIX_VALUE_DESTRUCT(&val);
            ret = PRRTE_ERROR;
            goto DONE;
        }
        PMIX_VALUE_DESTRUCT(&val);

        /* tell the routed module that we have a path
         * back to the HNP
         */
        if (PRRTE_SUCCESS != (ret = prrte_routed.update_route(PRRTE_PROC_MY_HNP, PRRTE_PROC_MY_PARENT))) {
            PRRTE_ERROR_LOG(ret);
            goto DONE;
        }
        /* and a path to our parent */
        if (PRRTE_SUCCESS != (ret = prrte_routed.update_route(PRRTE_PROC_MY_PARENT, PRRTE_PROC_MY_PARENT))) {
            PRRTE_ERROR_LOG(ret);
            goto DONE;
        }
        /* set the lifeline to point to our parent so that we
         * can handle the situation if that lifeline goes away
         */
        if (PRRTE_SUCCESS != (ret = prrte_routed.set_lifeline(PRRTE_PROC_MY_PARENT))) {
            PRRTE_ERROR_LOG(ret);
            goto DONE;
        }
    }

    /* setup the rollup callback */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_PRRTED_CALLBACK,
                            PRRTE_RML_PERSISTENT, rollup, NULL);

    /* define the target jobid */
    target.jobid = PRRTE_PROC_MY_NAME->jobid;
    if (prrte_static_ports || NULL != prrte_parent_uri) {
        /* we start by sending to ourselves */
        target.vpid = PRRTE_PROC_MY_NAME->vpid;
        /* since we will be waiting for any children to send us
         * their rollup info before sending to our parent, save
         * a little time in the launch phase by "warming up" the
         * connection to our parent while we wait for our children */
        buffer = PRRTE_NEW(prrte_buffer_t);  // zero-byte message
        prrte_rml.recv_buffer_nb(PRRTE_PROC_MY_PARENT, PRRTE_RML_TAG_NODE_REGEX_REPORT,
                                PRRTE_RML_PERSISTENT, node_regex_report, &node_regex_waiting);
        node_regex_waiting = true;
        if (0 > (ret = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_PARENT, buffer,
                                               PRRTE_RML_TAG_WARMUP_CONNECTION,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(buffer);
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

    buffer = PRRTE_NEW(prrte_buffer_t);
    /* insert our name for rollup purposes */
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, PRRTE_PROC_MY_NAME, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(buffer);
        goto DONE;
    }

    /* get any connection info we may have pushed */
    {
        pmix_info_t *info;
        size_t ninfo;
        pmix_value_t *vptr;

        PRRTE_PMIX_CONVERT_NAME(&proc, PRRTE_PROC_MY_NAME);
        boptr = &bo;
        bo.bytes = NULL;
        bo.size = 0;
        if (PMIX_SUCCESS == PMIx_Get(&proc, NULL, NULL, 0, &vptr) && NULL != vptr) {
            /* the data is returned as a pmix_data_array_t */
            if (PMIX_DATA_ARRAY != vptr->type || NULL == vptr->data.darray ||
                PMIX_INFO != vptr->data.darray->type || NULL == vptr->data.darray->array) {
                PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                PRRTE_RELEASE(buffer);
                goto DONE;
            }
            /* use the PMIx data support to pack it */
            info = (pmix_info_t*)vptr->data.darray->array;
            ninfo = vptr->data.darray->size;
            PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&proc, &pbuf, &ninfo, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(prc);
                ret = PRRTE_ERROR;
                PRRTE_RELEASE(buffer);
                goto DONE;
            }
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&proc, &pbuf, info, ninfo, PMIX_INFO))) {
                PMIX_ERROR_LOG(prc);
                ret = PRRTE_ERROR;
                PRRTE_RELEASE(buffer);
                goto DONE;
            }
            PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
            bo.bytes = (uint8_t*)pbo.bytes;
            bo.size = pbo.size;
            PMIX_VALUE_RELEASE(vptr);
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(buffer);
            goto DONE;
        }
    }

    /* include our node name */
    prrte_dss.pack(buffer, &prrte_process_info.nodename, 1, PRRTE_STRING);

    /* include any non-loopback aliases for this node */
    {
        uint8_t naliases, ni;
        char **nonlocal = NULL;
        int n;

        for (n=0; NULL != prrte_process_info.aliases[n]; n++) {
            if (0 != strcmp(prrte_process_info.aliases[n], "localhost") &&
                0 != strcmp(prrte_process_info.aliases[n], prrte_process_info.nodename)) {
                prrte_argv_append_nosize(&nonlocal, prrte_process_info.aliases[n]);
            }
        }
        naliases = prrte_argv_count(nonlocal);
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &naliases, 1, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(buffer);
            prrte_argv_free(nonlocal);
            goto DONE;
        }
        for (ni=0; ni < naliases; ni++) {
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &nonlocal[ni], 1, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_RELEASE(buffer);
                prrte_argv_free(nonlocal);
                goto DONE;
            }
        }
        prrte_argv_free(nonlocal);
    }

    /* always send back our topology signature - this is a small string
     * and won't hurt anything */
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &prrte_topo_signature, 1, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(buffer);
        goto DONE;
    }

    /* if we are rank=1, then send our topology back - otherwise, prte
     * will request it if necessary */
    if (1 == PRRTE_PROC_MY_NAME->vpid) {
        prrte_buffer_t data;
        int8_t flag;
        uint8_t *cmpdata;
        size_t cmplen;

        /* setup an intermediate buffer */
        PRRTE_CONSTRUCT(&data, prrte_buffer_t);

        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(&data, &prrte_hwloc_topology, 1, PRRTE_HWLOC_TOPO))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(buffer);
            goto DONE;
        }
        if (prrte_compress.compress_block((uint8_t*)data.base_ptr, data.bytes_used,
                                         &cmpdata, &cmplen)) {
            /* the data was compressed - mark that we compressed it */
            flag = 1;
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
            }
            /* pack the compressed length */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &cmplen, 1, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
            }
            /* pack the uncompressed length */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &data.bytes_used, 1, PRRTE_SIZE))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
            }
            /* pack the compressed info */
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, cmpdata, cmplen, PRRTE_UINT8))) {
                PRRTE_ERROR_LOG(ret);
                free(cmpdata);
                PRRTE_DESTRUCT(&data);
            }
            PRRTE_DESTRUCT(&data);
            free(cmpdata);
        } else {
            /* mark that it was not compressed */
            flag = 0;
            if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_DESTRUCT(&data);
                free(cmpdata);
            }
            /* transfer the payload across */
            prrte_dss.copy_payload(buffer, &data);
            PRRTE_DESTRUCT(&data);
        }
    }

    /* collect our network inventory */
    memset(&xfer, 0, sizeof(myxfer_t));
    PRRTE_PMIX_CONSTRUCT_LOCK(&xfer.lock);
    if (PMIX_SUCCESS != (prc = PMIx_server_collect_inventory(NULL, 0, infocbfunc, &xfer))) {
        PMIX_ERROR_LOG(prc);
        ret = PRRTE_ERR_NOT_SUPPORTED;
        goto DONE;
    }
    PRRTE_PMIX_WAIT_THREAD(&xfer.lock);
    if (NULL != xfer.info) {
        PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, &pbuf, &xfer.ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(prc);
            ret = PRRTE_ERROR;
            PRRTE_RELEASE(buffer);
            goto DONE;
        }
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(NULL, &pbuf, xfer.info, xfer.ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(prc);
            ret = PRRTE_ERROR;
            PRRTE_RELEASE(buffer);
            goto DONE;
        }
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        if (PRRTE_SUCCESS != (ret = prrte_dss.pack(buffer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(buffer);
            goto DONE;
        }
    }
    /* send it to the designated target */
    if (0 > (ret = prrte_rml.send_buffer_nb(&target, buffer,
                                           PRRTE_RML_TAG_PRRTED_CALLBACK,
                                           prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_RELEASE(buffer);
        goto DONE;
    }

    /* if we are tree-spawning, then we need to capture the MCA params
     * from our cmd line so we can pass them along to the daemons we spawn -
     * otherwise, only the first layer of daemons will ever see them
     */
    if (prrte_cmd_line_is_taken(prrte_cmd_line, "tree-spawn")) {
        int j, k;
        bool ignore;
        char *no_keep[] = {
            "prrte_hnp_uri",
            "prrte_ess_jobid",
            "prrte_ess_vpid",
            "prrte_ess_num_procs",
            "prrte_parent_uri",
            "mca_base_env_list",
            NULL
        };
        for (i=0; i < argc; i++) {
            if (0 == strcmp("-"PRRTE_MCA_CMD_LINE_ID,  argv[i]) ||
                0 == strcmp("--"PRRTE_MCA_CMD_LINE_ID, argv[i]) ) {
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
                    if (NULL != prted_cmd_line) {
                        for (j=0; NULL != prted_cmd_line[j]; j++) {
                            if (0 == strcmp(argv[i+1], prted_cmd_line[j])) {
                                /* already here - ignore it */
                                ignore = true;
                                break;
                            }
                        }
                    }
                    if (!ignore) {
                        prrte_argv_append_nosize(&prted_cmd_line, argv[i]);
                        prrte_argv_append_nosize(&prted_cmd_line, argv[i+1]);
                        prrte_argv_append_nosize(&prted_cmd_line, argv[i+2]);
                    }
                }
                i += 2;
            }
        }
    }

    if (prrte_debug_daemons_flag) {
        prrte_output(0, "%s prted: up and running - waiting for commands!", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    }
    ret = PRRTE_SUCCESS;

    /* loop the event lib until an exit event is detected */
    while (prrte_event_base_active) {
        prrte_event_loop(prrte_event_base, PRRTE_EVLOOP_ONCE);
    }
    PRRTE_ACQUIRE_OBJECT(prrte_event_base_active);

    /* ensure all local procs are dead */
    prrte_odls.kill_local_procs(NULL);

  DONE:
    /* update the exit status, in case it wasn't done */
    PRRTE_UPDATE_EXIT_STATUS(ret);

    /* cleanup and leave */
    prrte_finalize();

    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    /* cleanup the process info */
    prrte_proc_info_finalize();

    if (prrte_debug_flag) {
        fprintf(stderr, "exiting with status %d\n", prrte_exit_status);
    }
    exit(prrte_exit_status);
}

static void shutdown_callback(int fd, short flags, void *arg)
{
    prrte_timer_t *tm = (prrte_timer_t*)arg;
    bool suicide;

    if (NULL != tm) {
        /* release the timer */
        PRRTE_RELEASE(tm);
    }

    /* if we were ordered to abort, do so */
    if (prted_abort) {
        suicide = prrte_cmd_line_is_taken(prrte_cmd_line, "test-suicide");
        prrte_output(0, "%s is executing %s abort", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    suicide ? "suicide" : "clean");
        /* do -not- call finalize as this will send a message to the HNP
         * indicating clean termination! Instead, just kill our
         * local procs, forcibly cleanup the local session_dir tree, and abort
         */
        if (suicide) {
            exit(1);
        }
        prrte_odls.kill_local_procs(NULL);
        prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
        abort();
    }
    prrte_output(0, "%s is executing clean abnormal termination", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    /* do -not- call finalize as this will send a message to the HNP
     * indicating clean termination! Instead, just forcibly cleanup
     * the local session_dir tree and exit
     */
    prrte_odls.kill_local_procs(NULL);
    prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    exit(PRRTE_ERROR_DEFAULT_EXIT_CODE);
}

static void rollup(int status, prrte_process_name_t* sender,
                   prrte_buffer_t *buffer,
                   prrte_rml_tag_t tag, void *cbdata)
{
    int ret;
    prrte_process_name_t child;
    int32_t flag, cnt;
    prrte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;
    pmix_info_t *info;
    pmix_proc_t proc;
    pmix_status_t prc;

    ncollected++;

    /* if the sender is ourselves, then we save that buffer
     * so we can insert it at the beginning */
    if (sender->jobid == PRRTE_PROC_MY_NAME->jobid &&
        sender->vpid == PRRTE_PROC_MY_NAME->vpid) {
        mybucket = PRRTE_NEW(prrte_buffer_t);
        prrte_dss.copy_payload(mybucket, buffer);
    } else {
        /* xfer the contents of the rollup to our bucket */
        prrte_dss.copy_payload(bucket, buffer);
        /* the first entry in the bucket will be from our
         * direct child - harvest it for connection info */
        cnt = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &child, &cnt, PRRTE_NAME))) {
            PRRTE_ERROR_LOG(ret);
            goto report;
        }
        cnt = 1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &flag, &cnt, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
            goto report;
        }
        if (0 < flag) {
            (void)prrte_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, sender->jobid);
            proc.rank = sender->vpid;
            /* we have connection info */
            cnt = 1;
            if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &boptr, &cnt, PRRTE_BYTE_OBJECT))) {
                PRRTE_ERROR_LOG(ret);
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
    report_prted();
}

static void report_prted() {
    int nreqd, ret;

    /* get the number of children */
    nreqd = prrte_routed.num_routes() + 1;
    if (nreqd == ncollected && NULL != mybucket && !node_regex_waiting) {
        /* add the collection of our children's buckets to ours */
        prrte_dss.copy_payload(mybucket, bucket);
        PRRTE_RELEASE(bucket);
        /* relay this on to our parent */
        if (0 > (ret = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_PARENT, mybucket,
                                               PRRTE_RML_TAG_PRRTED_CALLBACK,
                                               prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(mybucket);
        }
    }
}

static void node_regex_report(int status, prrte_process_name_t* sender,
                              prrte_buffer_t *buffer,
                              prrte_rml_tag_t tag, void *cbdata) {
    int rc;
    bool * active = (bool *)cbdata;

    /* extract the node info if needed, and update the routing tree */
    if (PRRTE_SUCCESS != (rc = prrte_util_decode_nidmap(buffer))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* update the routing tree so any tree spawn operation
     * properly gets the number of children underneath us */
    prrte_routed.update_routing_plan();

    *active = false;

    /* now launch any child daemons of ours */
    prrte_plm.remote_spawn();

    report_prted();
}
