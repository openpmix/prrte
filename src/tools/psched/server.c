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
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#include <ctype.h>

#include "prte_stdint.h"
#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/error.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/schizo/base/base.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml_contact.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"

#include "src/tools/psched/psched.h"

static pmix_topology_t mytopology = {0};

static pmix_server_module_t psched_server = {
    .register_events = psched_register_events_fn,
    .deregister_events = psched_deregister_events_fn,
    .notify_event = psched_notify_event,
    .query = psched_query_fn,
    .tool_connected = psched_tool_connected_fn,
    .job_control = psched_job_ctrl_fn,
    .allocate = psched_alloc_fn,
#if PMIX_NUMERIC_VERSION >= 0x00050000
    .session_control = psched_session_ctrl_fn
#endif
};

psched_globals_t psched_globals = {
    .initialized = false,
    .requests = PMIX_POINTER_ARRAY_STATIC_INIT,
    .tools = PMIX_LIST_STATIC_INIT,
    .syscontroller = PMIX_PROC_STATIC_INIT,
    .controller_connected = false,
    .verbosity = -1,
    .output = -1
};

typedef struct {
    char *function;
    char **attrs;
} prte_regattr_input_t;

static prte_regattr_input_t prte_attributes[] = {
    {.function = "PMIx_Query_info",
     .attrs = (char *[]){"PMIX_QUERY_NAMESPACES",
                         "PMIX_QUERY_NAMESPACE_INFO",
                         "PMIX_QUERY_SPAWN_SUPPORT",
                         "PMIX_QUERY_DEBUG_SUPPORT",
                         "PMIX_HWLOC_XML_V1",
                         "PMIX_HWLOC_XML_V2",
                         "PMIX_PROC_URI",
                         "PMIX_QUERY_PROC_TABLE",
                         "PMIX_QUERY_LOCAL_PROC_TABLE",
                         "PMIX_QUERY_NUM_PSETS",
                         "PMIX_QUERY_PSET_NAMES",
                         "PMIX_JOB_SIZE",
                         "PMIX_QUERY_NUM_GROUPS",
                         "PMIX_QUERY_GROUP_NAMES",
                         "PMIX_QUERY_GROUP_MEMBERSHIP",
                         NULL}},
    {.function = "PMIx_Query_info_nb",
     .attrs = (char *[]){"PMIX_QUERY_NAMESPACES",
                         "PMIX_QUERY_NAMESPACE_INFO",
                         "PMIX_QUERY_SPAWN_SUPPORT",
                         "PMIX_QUERY_DEBUG_SUPPORT",
                         "PMIX_HWLOC_XML_V1",
                         "PMIX_HWLOC_XML_V2",
                         "PMIX_PROC_URI",
                         "PMIX_QUERY_PROC_TABLE",
                         "PMIX_QUERY_LOCAL_PROC_TABLE",
                         "PMIX_QUERY_NUM_PSETS",
                         "PMIX_QUERY_PSET_NAMES",
                         "PMIX_JOB_SIZE",
                         "PMIX_QUERY_NUM_GROUPS",
                         "PMIX_QUERY_GROUP_NAMES",
                         "PMIX_QUERY_GROUP_MEMBERSHIP",
                         "PMIX_QUERY_ALLOCATION",
                         "PMIX_QUERY_ALLOC_STATUS",
                         NULL}},
    {.function = "PMIx_Log", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Log_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Job_control",
     .attrs = (char *[]){"PMIX_JOB_CTRL_KILL",
                         "PMIX_JOB_CTRL_TERMINATE",
                         "PMIX_JOB_CTRL_SIGNAL",
                         NULL}},
    {.function = "PMIx_Job_control_nb",
     .attrs = (char *[]){"PMIX_JOB_CTRL_KILL",
                         "PMIX_JOB_CTRL_TERMINATE",
                         "PMIX_JOB_CTRL_SIGNAL",
                         NULL}},
    {.function = "PMIx_Group_construct",
     .attrs = (char *[]){"PMIX_GROUP_ASSIGN_CONTEXT_ID",
                         "PMIX_EMBED_BARRIER",
                         "PMIX_GROUP_ENDPT_DATA",
                         NULL}},
    {.function = "PMIx_Group_construct_nb",
     .attrs = (char *[]){"PMIX_GROUP_ASSIGN_CONTEXT_ID",
                         "PMIX_EMBED_BARRIER",
                         "PMIX_GROUP_ENDPT_DATA",
                         NULL}},
    {.function = "PMIx_Group_invite", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_invite_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_join", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_join_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_leave", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_leave_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Group_destruct", .attrs = (char *[]){"PMIX_EMBED_BARRIER", NULL}},
    {.function = "PMIx_Group_destruct_nb", .attrs = (char *[]){"PMIX_EMBED_BARRIER", NULL}},
    {.function = "PMIx_Register_event_handler", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Deregister_event_handler", .attrs = (char *[]){"N/A", NULL}},
    {.function = "PMIx_Notify_event", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Allocate_resources",
     .attrs = (char *[]){"PMIX_ALLOC_REQ_ID",
                         "PMIX_ALLOC_NUM_NODES",
                         "PMIX_ALLOC_NODE_LIST",
                         "PMIX_ALLOC_NUM_CPUS",
                         "PMIX_ALLOC_NUM_CPU_LIST",
                         "PMIX_ALLOC_CPU_LIST",
                         "PMIX_ALLOC_MEM_SIZE",
                         "PMIX_ALLOC_TIME",
                         "PMIX_ALLOC_QUEUE",
                         "PMIX_ALLOC_PREEMPTIBLE",
                         NULL}},
#if PMIX_NUMERIC_VERSION >= 0x00050000
    {.function = "PMIx_Session_control",
     .attrs = (char *[]){"PMIX_SESSION_CTRL_ID",
                         "PMIX_SESSION_APP",
                         "PMIX_SESSION_PAUSE",
                         "PMIX_SESSION_RESUME",
                         "PMIX_SESSION_TERMINATE",
                         "PMIX_SESSION_PREEMPT",
                         "PMIX_SESSION_RESTORE",
                         "PMIX_SESSION_SIGNAL",
                         "PMIX_SESSION_COMPLETE",
                         NULL}},
#endif
    {.function = ""},
};

static int gen_verbose = -1;

void psched_register_params(void)
{
    bool opened = false;

    /* register a verbosity */
    psched_globals.verbosity = -1;
    (void) pmix_mca_base_var_register("prte", "psched", NULL, "verbose",
                                      "Debug verbosity for PRRTE Scheduler",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &psched_globals.verbosity);
    if (0 < psched_globals.verbosity) {
        psched_globals.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(psched_globals.output,
                                  psched_globals.verbosity);
        prte_pmix_server_globals.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_pmix_server_globals.output,
                                  psched_globals.verbosity);
        opened = true;
    }

    /* register the general verbosity */
    (void) pmix_mca_base_var_register("prte", "pmix", NULL, "server_verbose",
                                      "Debug verbosity for PMIx server",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &gen_verbose);
    if (0 < gen_verbose && psched_globals.verbosity < gen_verbose) {
        if (!opened) {
            psched_globals.output = pmix_output_open(NULL);
            prte_pmix_server_globals.output = pmix_output_open(NULL);
        }
        pmix_output_set_verbosity(psched_globals.output, gen_verbose);
        pmix_output_set_verbosity(prte_pmix_server_globals.output, gen_verbose);
        psched_globals.verbosity = gen_verbose;
    }
}

/* provide a callback function for lost connections to allow us
 * to cleanup after any tools once they depart */
static void lost_connection_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                                 const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                 pmix_info_t *results, size_t nresults,
                                 pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    prte_pmix_tool_t *tl;
    PRTE_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status,
                            info, ninfo, results, nresults);

    /* scan the list of attached tools to see if this one is there */
    PMIX_LIST_FOREACH(tl, &psched_globals.tools, prte_pmix_tool_t)
    {
        if (PMIX_CHECK_PROCID(&tl->name, source)) {
            /* take this tool off the list */
            pmix_list_remove_item(&psched_globals.tools, &tl->super);
            /* release it */
            PMIX_RELEASE(tl);
            break;
        }
    }

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(ref);

    PMIX_ACQUIRE_OBJECT(lock);
    lock->status = status;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

/*
 * Initialize global variables used w/in the server.
 */
int psched_server_init(pmix_cli_result_t *results)
{
    int rc;
    void *ilist;
    pmix_data_array_t darray;
    pmix_info_t *info, myinf;
    size_t n, ninfo;
    char *tmp;
    pmix_status_t prc;
    prte_pmix_lock_t lock;
    bool flag;
    pmix_proc_t myproc;
    pmix_cli_item_t *opt;

    if (psched_globals.initialized) {
        return PRTE_SUCCESS;
    }
    psched_globals.initialized = true;

    /* setup the server's global variables */
    PMIX_CONSTRUCT(&psched_globals.requests, pmix_pointer_array_t);
    pmix_pointer_array_init(&psched_globals.requests, 128, INT_MAX, 2);
    PMIX_CONSTRUCT(&psched_globals.tools, pmix_list_t);
    psched_globals.syscontroller = *PRTE_NAME_INVALID;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s server:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_INFO_LIST_START(ilist);

#ifdef PMIX_SERVER_SCHEDULER
    /* tell the server that we are the scheduler */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_SERVER_SCHEDULER,
                       NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }
#endif

    /* tell the server our name so we agree on our identifier */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_TOOL_NSPACE,
                       prte_process_info.myproc.nspace, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_TOOL_RANK,
                       &prte_process_info.myproc.rank, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* tell the server our hostname so we agree on it */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_HOSTNAME,
                       prte_process_info.nodename, PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    // check for aliases
    if (NULL != prte_process_info.aliases) {
        tmp = PMIX_ARGV_JOIN_COMPAT(prte_process_info.aliases, ',');
        PMIX_INFO_LIST_ADD(prc, ilist, PMIX_HOSTNAME_ALIASES,
                           tmp, PMIX_STRING);
        free(tmp);
        if (PMIX_SUCCESS != prc) {
            PMIX_INFO_LIST_RELEASE(ilist);
            rc = prte_pmix_convert_status(prc);
            return rc;
        }
    }

    /* tell the server what we are doing with FQDN */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_HOSTNAME_KEEP_FQDN,
                       &prte_keep_fqdn_hostnames, PMIX_BOOL);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

#ifdef PMIX_EXTERNAL_AUX_EVENT_BASE
    /* give the server our event base to use for signal trapping */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_EXTERNAL_AUX_EVENT_BASE,
                       prte_event_base, PMIX_POINTER);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }
#endif

    // pass our topology object down to the server library for its use
    mytopology.source = "hwloc";
    mytopology.topology = prte_hwloc_topology;
    PMIX_INFO_CONSTRUCT(&myinf);
    PMIX_LOAD_KEY(myinf.key, PMIX_TOPOLOGY2);
    myinf.value.type = PMIX_TOPO;
    myinf.value.data.topo = &mytopology;
    PMIX_INFO_LIST_INSERT(prc, ilist, &myinf);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* tell the server our temp directory */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_SERVER_TMPDIR,
                       prte_process_info.top_session_dir,
                       PMIX_STRING);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* if requested, tell the server library to output our PMIx URI */
    if (NULL != prte_pmix_server_globals.report_uri) {
        PMIX_INFO_LIST_ADD(prc, ilist, PMIX_TCP_REPORT_URI,
                           prte_pmix_server_globals.report_uri, PMIX_STRING);
        if (PMIX_SUCCESS != prc) {
            PMIX_INFO_LIST_RELEASE(ilist);
            rc = prte_pmix_convert_status(prc);
            return rc;
        }
    }

    if (NULL != prte_progress_thread_cpus) {
        PMIX_INFO_LIST_ADD(prc, ilist, PMIX_BIND_PROGRESS_THREAD,
                           prte_progress_thread_cpus, PMIX_STRING);
        PMIX_INFO_LIST_ADD(prc, ilist, PMIX_BIND_REQUIRED,
                           &prte_bind_progress_thread_reqd, PMIX_BOOL);
    }

    /* schedulers always allow remote tool connections */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_SERVER_REMOTE_CONNECTIONS,
                       NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* if the system controller is present, connect to it */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_CONNECT_TO_SYS_CONTROLLER,
                       NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* don't require the connection else we will abort
     * if the system controller isn't found */
    PMIX_INFO_LIST_ADD(prc, ilist, PMIX_TOOL_CONNECT_OPTIONAL,
                       NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    /* convert to an info array */
    PMIX_INFO_LIST_CONVERT(prc, ilist, &darray);
    if (PMIX_SUCCESS != prc) {
        PMIX_INFO_LIST_RELEASE(ilist);
        rc = prte_pmix_convert_status(prc);
        return rc;
    }
    PMIX_INFO_LIST_RELEASE(ilist);
    info = (pmix_info_t*)darray.array;
    ninfo = darray.size;

    /* initialize as a tool */
    prc = PMIx_tool_init(&myproc, info, ninfo);
    if (PMIX_SUCCESS != prc) {
        /* pmix will provide a nice show_help output here */
        PMIX_INFO_FREE(info, ninfo);
        return prte_pmix_convert_status(prc);
    }
    PMIX_INFO_FREE(info, ninfo);
    rc = PRTE_SUCCESS;

    /* register our attributes */
    for (n = 0; 0 != strlen(prte_attributes[n].function); n++) {
        prc = PMIx_Register_attributes(prte_attributes[n].function, prte_attributes[n].attrs);
        if (PMIX_SUCCESS != prc) {
            return prte_pmix_convert_status(prc);
        }
    }

    /* register the "lost-connection" event handler */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    prc = PMIX_ERR_LOST_CONNECTION;
    PMIx_Register_event_handler(&prc, 1, NULL, 0, lost_connection_hdlr, regcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    rc = prte_pmix_convert_status(lock.status);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* register our server function module */
    prc = PMIx_tool_set_server_module(&psched_server);
    if (PMIX_SUCCESS != prc) {
        rc = prte_pmix_convert_status(prc);
        return rc;
    }

    return PRTE_SUCCESS;
}

void psched_server_finalize(void)
{
    pmix_status_t prc;

    if (!psched_globals.initialized) {
        return;
    }

    pmix_output_verbose(2, psched_globals.output,
                        "%s Finalizing PMIX server",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    prc = PMIx_tool_finalize();
    if (PMIX_SUCCESS != prc) {
        PMIX_ERROR_LOG(prc);
    }
}
