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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <ctype.h>

#include "prte_stdint.h"
#include "src/class/prte_hotel.h"
#include "src/class/prte_list.h"
#include "src/mca/base/prte_mca_base_var.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/prte_environ.h"
#include "src/util/show_help.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/os_path.h"
#include "src/util/argv.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_data_server.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

/*
 * Local utility functions
 */
static void pmix_server_dmdx_recv(int status, prte_process_name_t* sender,
                                  prte_buffer_t *buffer,
                                  prte_rml_tag_t tg, void *cbdata);
static void pmix_server_dmdx_resp(int status, prte_process_name_t* sender,
                                  prte_buffer_t *buffer,
                                  prte_rml_tag_t tg, void *cbdata);
static void pmix_server_log(int status, prte_process_name_t* sender,
                            prte_buffer_t *buffer,
                            prte_rml_tag_t tg, void *cbdata);

#define PRTE_PMIX_SERVER_MIN_ROOMS    4096

pmix_server_globals_t prte_pmix_server_globals = {0};
#ifdef PMIX_TOPOLOGY2
static pmix_topology_t mytopology = {0};
#endif

static pmix_server_module_t pmix_server = {
    .client_connected = pmix_server_client_connected_fn,
    .client_finalized = pmix_server_client_finalized_fn,
    .abort = pmix_server_abort_fn,
    .fence_nb = pmix_server_fencenb_fn,
    .direct_modex = pmix_server_dmodex_req_fn,
    .publish = pmix_server_publish_fn,
    .lookup = pmix_server_lookup_fn,
    .unpublish = pmix_server_unpublish_fn,
    .spawn = pmix_server_spawn_fn,
    .connect = pmix_server_connect_fn,
    .disconnect = pmix_server_disconnect_fn,
    .register_events = pmix_server_register_events_fn,
    .deregister_events = pmix_server_deregister_events_fn,
    .notify_event = pmix_server_notify_event,
    .query = pmix_server_query_fn,
    .tool_connected = pmix_tool_connected_fn,
    .log = pmix_server_log_fn,
    .allocate = pmix_server_alloc_fn,
    .job_control = pmix_server_job_ctrl_fn,
    .iof_pull = pmix_server_iof_pull_fn,
    .push_stdin = pmix_server_stdin_fn,
#if PMIX_NUMERIC_VERSION >= 0x00040000
    .group = pmix_server_group_fn
#endif
};

#if PMIX_NUMERIC_VERSION >= 0x00040000
typedef struct {
    char *function;
    char **attrs;
} prte_regattr_input_t;

static prte_regattr_input_t prte_attributes[] = {
    {.function = "PMIx_Init", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Finalize", .attrs = (char *[]){"PMIX_EMBED_BARRIER", NULL}},
    {.function = "PMIx_Abort", .attrs = (char *[]){"N/A", NULL}},
    {.function = "PMIx_Fence", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Fence_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Get", .attrs = (char *[]){"PMIX_GET_REFRESH_CACHE", "PMIX_REQUIRED_KEY",
                                                 "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Get_nb", .attrs = (char *[]){"PMIX_GET_REFRESH_CACHE", "PMIX_REQUIRED_KEY",
                                                    "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Publish", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Publish_nb", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Lookup", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Lookup_nb", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Unpublish", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Unpublish_nb", .attrs = (char *[]){"PMIX_RANGE", "PMIX_TIMEOUT", NULL}},
    {.function = "PMIx_Spawn", .attrs = (char *[]){"PMIX_HOST", "PMIX_HOSTFILE", "PMIX_ADD_HOSTFILE",
                                                   "PMIX_ADD_HOST", "PMIX_PREFIX", "PMIX_WDIR",
                                                   "PMIX_PRELOAD_BIN", "PMIX_PRELOAD_FILES",
                                                   "PMIX_COSPAWN_APP", "PMIX_SET_ENVAR",
                                                   "PMIX_ADD_ENVAR", "PMIX_UNSET_ENVAR",
                                                   "PMIX_PREPEND_ENVAR", "PMIX_APPEND_ENVAR",
                                                   "PMIX_PSET_NAME", "PMIX_PERSONALITY",
                                                   "PMIX_MAPPER", "PMIX_DISPLAY_MAP",
                                                   "PMIX_MAPBY", "PMIX_RANKBY", "PMIX_BINDTO",
                                                   "PMIX_JOB_RECOVERABLE", "PMIX_MAX_RESTARTS",
                                                   "PMIX_JOB_CONTINUOUS", "PMIX_REQUESTOR_IS_TOOL",
                                                   "PMIX_NOTIFY_COMPLETION", "PMIX_DEBUG_STOP_ON_EXEC",
                                                   "PMIX_TAG_OUTPUT", "PMIX_TIMESTAMP_OUTPUT",
                                                   "PMIX_OUTPUT_TO_FILE", "PMIX_MERGE_STDERR_STDOUT",
                                                   "PMIX_STDIN_TGT", "PMIX_INDEX_ARGV",
                                                   "PMIX_DEBUGGER_DAEMONS", "PMIX_SPAWN_TOOL",
                                                   "PMIX_TIMEOUT", "PMIX_TIMEOUT_STACKTRACES",
                                                   "PMIX_TIMEOUT_REPORT_STATE", NULL}},
    {.function = "PMIx_Spawn_nb", .attrs = (char *[]){"PMIX_HOST", "PMIX_HOSTFILE", "PMIX_ADD_HOSTFILE",
                                                   "PMIX_ADD_HOST", "PMIX_PREFIX", "PMIX_WDIR",
                                                   "PMIX_PRELOAD_BIN", "PMIX_PRELOAD_FILES",
                                                   "PMIX_COSPAWN_APP", "PMIX_SET_ENVAR",
                                                   "PMIX_ADD_ENVAR", "PMIX_UNSET_ENVAR",
                                                   "PMIX_PREPEND_ENVAR", "PMIX_APPEND_ENVAR",
                                                   "PMIX_PSET_NAME", "PMIX_PERSONALITY",
                                                   "PMIX_MAPPER", "PMIX_DISPLAY_MAP",
                                                   "PMIX_MAPBY", "PMIX_RANKBY", "PMIX_BINDTO",
                                                   "PMIX_JOB_RECOVERABLE", "PMIX_MAX_RESTARTS",
                                                   "PMIX_JOB_CONTINUOUS", "PMIX_REQUESTOR_IS_TOOL",
                                                   "PMIX_NOTIFY_COMPLETION", "PMIX_DEBUG_STOP_ON_EXEC",
                                                   "PMIX_TAG_OUTPUT", "PMIX_TIMESTAMP_OUTPUT",
                                                   "PMIX_OUTPUT_TO_FILE", "PMIX_MERGE_STDERR_STDOUT",
                                                   "PMIX_STDIN_TGT", "PMIX_INDEX_ARGV",
                                                   "PMIX_DEBUGGER_DAEMONS", "PMIX_SPAWN_TOOL",
                                                   "PMIX_TIMEOUT", "PMIX_TIMEOUT_STACKTRACES",
                                                   "PMIX_TIMEOUT_REPORT_STATE", NULL}},
    {.function = "PMIx_Connect", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Connect_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Disconnect", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Disconnect_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Query_info", .attrs = (char *[]){"PMIX_QUERY_NAMESPACES", "PMIX_QUERY_NAMESPACE_INFO",
                                                        "PMIX_QUERY_SPAWN_SUPPORT", "PMIX_QUERY_DEBUG_SUPPORT",
                                                        "PMIX_QUERY_MEMORY_USAGE", "PMIX_TIME_REMAINING",
                                                        "PMIX_HWLOC_XML_V1", "PMIX_HWLOC_XML_V2",
                                                        "PMIX_PROC_URI", "PMIX_QUERY_PROC_TABLE",
                                                        "PMIX_QUERY_LOCAL_PROC_TABLE", "PMIX_QUERY_NUM_PSETS",
                                                        "PMIX_QUERY_PSET_NAMES", "PMIX_JOB_SIZE", NULL}},
    {.function = "PMIx_Query_info_nb", .attrs = (char *[]){"PMIX_QUERY_NAMESPACES", "PMIX_QUERY_NAMESPACE_INFO",
                                                        "PMIX_QUERY_SPAWN_SUPPORT", "PMIX_QUERY_DEBUG_SUPPORT",
                                                        "PMIX_QUERY_MEMORY_USAGE", "PMIX_TIME_REMAINING",
                                                        "PMIX_HWLOC_XML_V1", "PMIX_HWLOC_XML_V2",
                                                        "PMIX_PROC_URI", "PMIX_QUERY_PROC_TABLE",
                                                        "PMIX_QUERY_LOCAL_PROC_TABLE", "PMIX_QUERY_NUM_PSETS",
                                                        "PMIX_QUERY_PSET_NAMES", "PMIX_JOB_SIZE", NULL}},
    {.function = "PMIx_Log", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Log_nb", .attrs = (char *[]){"NONE", NULL}},
    {.function = "PMIx_Job_control", .attrs = (char *[]){"PMIX_JOB_CTRL_KILL", "PMIX_JOB_CTRL_TERMINATE",
                                                         "PMIX_JOB_CTRL_SIGNAL", NULL}},
    {.function = "PMIx_Job_control_nb", .attrs = (char *[]){"PMIX_JOB_CTRL_KILL", "PMIX_JOB_CTRL_TERMINATE",
                                                         "PMIX_JOB_CTRL_SIGNAL", NULL}},
    {.function = "PMIx_Group_construct", .attrs = (char *[]){"PMIX_GROUP_ASSIGN_CONTEXT_ID", "PMIX_EMBED_BARRIER",
                                                             "PMIX_GROUP_ENDPT_DATA", NULL}},
    {.function = "PMIx_Group_construct_nb", .attrs = (char *[]){"PMIX_GROUP_ASSIGN_CONTEXT_ID", "PMIX_EMBED_BARRIER",
                                                             "PMIX_GROUP_ENDPT_DATA", NULL}},
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
    {.function = ""},
};
#endif

static void send_error(int status, pmix_proc_t *idreq,
                       prte_process_name_t *remote, int remote_room);
static void _mdxresp(int sd, short args, void *cbdata);
static void modex_resp(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata);


void pmix_server_register_params(void)
{
    /* register a verbosity */
    prte_pmix_server_globals.verbosity = -1;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "server_verbose",
                                  "Debug verbosity for PMIx server",
                                  PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.verbosity);
    if (0 <= prte_pmix_server_globals.verbosity) {
        prte_pmix_server_globals.output = prte_output_open(NULL);
        prte_output_set_verbosity(prte_pmix_server_globals.output,
                                  prte_pmix_server_globals.verbosity);
    }
    /* specify the size of the hotel */
    prte_pmix_server_globals.num_rooms = -1;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "server_max_reqs",
                                  "Maximum number of backlogged PMIx server direct modex requests",
                                  PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.num_rooms);
    /* specify the timeout for the hotel */
    prte_pmix_server_globals.timeout = 2;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "server_max_wait",
                                  "Maximum time (in seconds) the PMIx server should wait to service direct modex requests",
                                  PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.timeout);

    /* whether or not to wait for the universal server */
    prte_pmix_server_globals.wait_for_server = false;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "wait_for_server",
                                  "Whether or not to wait for the session-level server to start",
                                  PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.wait_for_server);

    /* whether or not to support legacy usock connections as well as tcp */
    prte_pmix_server_globals.legacy = false;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "server_usock_connections",
                                  "Whether or not to support legacy usock connections",
                                  PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.legacy);

    /* whether or not to drop a session-level tool rendezvous point */
    prte_pmix_server_globals.session_server = false;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "session_server",
                                  "Whether or not to drop a session-level tool rendezvous point",
                                  PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.session_server);

    /* whether or not to drop a system-level tool rendezvous point */
    prte_pmix_server_globals.system_server = false;
    (void) prte_mca_base_var_register ("prte", "pmix", NULL, "system_server",
                                  "Whether or not to drop a system-level tool rendezvous point",
                                  PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prte_pmix_server_globals.system_server);
}

static void eviction_cbfunc(struct prte_hotel_t *hotel,
                            int room_num, void *occupant)
{
    pmix_server_req_t *req = (pmix_server_req_t*)occupant;
    bool timeout = false;
    int rc=PRTE_ERR_TIMEOUT;
    pmix_value_t *pval = NULL;
    pmix_status_t prc;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                         "EVICTION FROM ROOM %d", room_num);

    /* decrement the request timeout */
    req->timeout -= prte_pmix_server_globals.timeout;
    if (req->timeout > 0) {
        req->timeout -= prte_pmix_server_globals.timeout;
    }
    if (0 >= req->timeout) {
        timeout = true;
    }
    if (!timeout) {
        /* see if this is a dmdx request waiting for a key to arrive */
        if (NULL != req->key) {
            prte_output_verbose(2, prte_pmix_server_globals.output,
                                 "%s server:evict timeout - checking for key %s from proc %s:%u",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->key, req->tproc.nspace, req->tproc.rank);

            /* see if the key has arrived */
            if (PMIX_SUCCESS == PMIx_Get(&req->tproc, req->key, req->info, req->ninfo, &pval)) {
                prte_output_verbose(2, prte_pmix_server_globals.output,
                                     "%s server:evict key %s found - retrieving payload",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->key);
                /* it has - ask our local pmix server for the data */
                PMIX_VALUE_RELEASE(pval);
                /* check us back into hotel so the modex_resp function can safely remove us */
                prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num);
                if (PMIX_SUCCESS != (prc = PMIx_server_dmodex_request(&req->tproc, modex_resp, req))) {
                    PMIX_ERROR_LOG(prc);
                    send_error(rc, &req->tproc, &req->proxy, req->remote_room_num);
                    prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
                    PRTE_RELEASE(req);
                }
                return;
            }
            /* if not, then we continue to wait */
            prte_output_verbose(2, prte_pmix_server_globals.output,
                                 "%s server:evict key %s not found - returning to hotel",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->key);
        }
        /* not done yet - check us back in */
        if (PRTE_SUCCESS == (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
            prte_output_verbose(2, prte_pmix_server_globals.output,
                                 "%s server:evict checked back in to room %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->room_num);
            return;
        }
        /* fall thru and return an error so the caller doesn't hang */
    } else {
        prte_show_help("help-prted.txt", "timedout", true, req->operation);
    }

    /* don't let the caller hang */
    if (0 <= req->remote_room_num) {
        send_error(rc, &req->tproc, &req->proxy, req->remote_room_num);
    } else if (NULL != req->opcbfunc) {
        req->opcbfunc(PMIX_ERR_TIMEOUT, req->cbdata);
    } else if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, req->cbdata, NULL, NULL);
    } else if (NULL != req->spcbfunc) {
        req->spcbfunc(PMIX_ERR_TIMEOUT, NULL, req->cbdata);
    } else if (NULL != req->lkcbfunc) {
        req->lkcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, req->cbdata);
    }
    PRTE_RELEASE(req);
}

/* NOTE: this function must be called from within an event! */
void prte_pmix_server_clear(pmix_proc_t *pname)
{
    int n;
    pmix_server_req_t *req;

    for (n=0; n < prte_pmix_server_globals.reqs.num_rooms; n++) {
        prte_hotel_knock(&prte_pmix_server_globals.reqs, n, (void**)&req);
        if (NULL != req) {
            if (PMIX_CHECK_PROCID(&req->tproc, pname)) {
                prte_hotel_checkout(&prte_pmix_server_globals.reqs, n);
                PRTE_RELEASE(req);
            }
        }
    }
}
/*
 * Initialize global variables used w/in the server.
 */
int pmix_server_init(void)
{
    int rc;
    prte_list_t ilist;
    prte_info_item_t *kv;
    pmix_info_t *info;
    size_t n, ninfo;
    char *tmp;
    pmix_status_t prc;

    if (prte_pmix_server_globals.initialized) {
        return PRTE_SUCCESS;
    }
    prte_pmix_server_globals.initialized = true;

    /* setup the server's state variables */
    PRTE_CONSTRUCT(&prte_pmix_server_globals.reqs, prte_hotel_t);
    PRTE_CONSTRUCT(&prte_pmix_server_globals.psets, prte_list_t);

    /* by the time we init the server, we should know how many nodes we
     * have in our environment - with the exception of mpirun. If the
     * user specified the size of the hotel, then use that value. Otherwise,
     * set the value to something large to avoid running out of rooms on
     * large machines */
    if (-1 == prte_pmix_server_globals.num_rooms) {
        prte_pmix_server_globals.num_rooms = prte_process_info.num_daemons * 2;
        if (prte_pmix_server_globals.num_rooms < PRTE_PMIX_SERVER_MIN_ROOMS) {
            prte_pmix_server_globals.num_rooms = PRTE_PMIX_SERVER_MIN_ROOMS;
        }
    }
    if (PRTE_SUCCESS != (rc = prte_hotel_init(&prte_pmix_server_globals.reqs,
                                              prte_pmix_server_globals.num_rooms,
                                              prte_event_base, prte_pmix_server_globals.timeout,
                                              PRTE_ERROR_PRI, eviction_cbfunc))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    PRTE_CONSTRUCT(&prte_pmix_server_globals.notifications, prte_list_t);
    prte_pmix_server_globals.server = *PRTE_NAME_INVALID;

    PRTE_CONSTRUCT(&ilist, prte_list_t);

    /* tell the server our hostname so we agree on it */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, prte_process_info.nodename, PMIX_STRING);
    prte_list_append(&ilist, &kv->super);

    /* if PMIx is version 4 or higher, then we can pass our
     * topology object down to the server library for its use
     * and for passing to any local clients */
#ifdef PMIX_TOPOLOGY2
    mytopology.source = strdup("hwloc");
    mytopology.topology = prte_hwloc_topology;
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_TOPOLOGY2, &mytopology, PMIX_TOPO);
    prte_list_append(&ilist, &kv->super);
    // tell the server to share this topology for us
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_SHARE_TOPOLOGY, NULL, PMIX_BOOL);
    prte_list_append(&ilist, &kv->super);
#else
#if HWLOC_API_VERSION < 0x20000
     /* pass the topology string as we don't
      * have HWLOC shared memory available - we do
      * this so the procs won't read the topology
      * themselves as this could overwhelm the local
      * system on large-scale SMPs */
    if (NULL != prte_hwloc_topology) {
        char *xmlbuffer=NULL;
        int len;
        kv = PRTE_NEW(prte_info_item_t);
        if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len)) {
            PRTE_RELEASE(kv);
            PRTE_DESTRUCT(&ilist);
            return PRTE_ERROR;
        }
        PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, xmlbuffer, PMIX_STRING);
        free(xmlbuffer);
        prte_list_append(&ilist, &kv->super);
    }
#else
    /* if shmem support is available, then we will share
     * it since earlier versions of PMIx aren't able to
     * do so on our behalf - if it isn't available, then export
     * the topology as a v2 xml string */
    if (!prte_hwloc_shmem_available) {
        if (NULL != prte_hwloc_topology) {
            char *xmlbuffer=NULL;
            int len;
            kv = PRTE_NEW(prte_info_item_t);
            if (0 != hwloc_topology_export_xmlbuffer(prte_hwloc_topology, &xmlbuffer, &len, 0)) {
                PRTE_RELEASE(kv);
                PRTE_DESTRUCT(&ilist);
                return PRTE_ERROR;
            }
            PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, xmlbuffer, PMIX_STRING);
            free(xmlbuffer);
            prte_list_append(&ilist, &kv->super);
        }
    }
#endif
#endif

    /* tell the server our temp directory */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_TMPDIR, prte_process_info.jobfam_session_dir, PMIX_STRING);
    prte_list_append(&ilist, &kv->super);
    if (!prte_pmix_server_globals.legacy) {
        /* use only one listener */
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_SINGLE_LISTENER, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
    }
    /* tell the server to use its own internal monitoring */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_ENABLE_MONITORING, NULL, PMIX_BOOL);
    prte_list_append(&ilist, &kv->super);
    /* if requested, tell the server to drop a session-level
     * PMIx connection point */
    if (prte_pmix_server_globals.session_server) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
    }

    /* if requested, tell the server to drop a system-level
     * PMIx connection point - only do this for the HNP as, in
     * at least one case, a daemon can be colocated with the
     * HNP and would overwrite the server rendezvous file */
    if (prte_pmix_server_globals.system_server && PRTE_PROC_IS_MASTER) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_SYSTEM_SUPPORT, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
    }

    /* if we are the MASTER, then we are the scheduler
     * as well as a gateway */
    if (PRTE_PROC_IS_MASTER) {
#ifdef PMIX_SERVER_SCHEDULER
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
#endif
#ifdef PMIX_SERVER_GATEWAY
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_GATEWAY, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
#endif
    }

    /* PRTE always allows remote tool connections */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_REMOTE_CONNECTIONS, NULL, PMIX_BOOL);
    prte_list_append(&ilist, &kv->super);

    /* if we were launched by a debugger, then we need to have
     * notification of our termination sent */
    if (NULL != getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL")) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_EVENT_SILENT_TERMINATION, NULL, PMIX_BOOL);
        prte_list_append(&ilist, &kv->super);
    }

#ifdef PMIX_HOSTNAME_KEEP_FQDN
    /* tell the server what we are doing with FQDN */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_KEEP_FQDN, &prte_keep_fqdn_hostnames, PMIX_BOOL);
    prte_list_append(&ilist, &kv->super);
#endif

    /* convert to an info array */
    ninfo = prte_list_get_size(&ilist) + 2;
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    while (NULL != (kv = (prte_info_item_t*)prte_list_remove_first(&ilist))) {
        PMIX_INFO_XFER(&info[n], &kv->info);
        ++n;
        PRTE_RELEASE(kv);
    }
    PRTE_LIST_DESTRUCT(&ilist);
    /* tell the server our name so we agree on our identifier */
    PMIX_INFO_LOAD(&info[n], PMIX_SERVER_NSPACE, prte_process_info.myproc.nspace, PMIX_STRING);
    PMIX_INFO_LOAD(&info[n+1], PMIX_SERVER_RANK, &prte_process_info.myproc.rank, PMIX_PROC_RANK);

    /* setup the local server */
    if (PMIX_SUCCESS != (prc = PMIx_server_init(&pmix_server, info, ninfo))) {
        /* pmix will provide a nice show_help output here */
        PMIX_INFO_FREE(info, ninfo);
        return prte_pmix_convert_status(prc);
    }
    PMIX_INFO_FREE(info, ninfo);
    rc = PRTE_SUCCESS;

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* register our support */
    for (n=0; 0 != strlen(prte_attributes[n].function); n++) {
        prc = PMIx_Register_attributes(prte_attributes[n].function, prte_attributes[n].attrs);
        if (PMIX_SUCCESS != prc) {
            return prte_pmix_convert_status(prc);
        }
    }
    /* register our local resources */
    PRTE_CONSTRUCT(&ilist, prte_list_t);

    /* register our hostname so everyone agrees on it */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, prte_process_info.nodename, PMIX_STRING);
    prte_list_append(&ilist, &kv->super);
#ifdef PMIX_HOSTNAME_ALIASES
    // check for aliases
    if (NULL != prte_process_info.aliases) {
        kv = PRTE_NEW(prte_info_item_t);
        tmp = pmix_argv_join(prte_process_info.aliases, ',');
        PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_ALIASES, tmp, PMIX_STRING);
        free(tmp);
        prte_list_append(&ilist, &kv->super);
    }
#endif
    /* convert to an info array */
    ninfo = prte_list_get_size(&ilist);
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    while (NULL != (kv = (prte_info_item_t*)prte_list_remove_first(&ilist))) {
        PMIX_INFO_XFER(&info[n], &kv->info);
        ++n;
        PRTE_RELEASE(kv);
    }
    PRTE_LIST_DESTRUCT(&ilist);
    prc = PMIx_server_register_resources(info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    rc = prte_pmix_convert_status(prc);
#endif

    return rc;
}

void pmix_server_start(void)
{
    /* setup our local data server */
    prte_data_server_init();

    /* setup recv for direct modex requests */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DIRECT_MODEX,
                            PRTE_RML_PERSISTENT, pmix_server_dmdx_recv, NULL);

    /* setup recv for replies to direct modex requests */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DIRECT_MODEX_RESP,
                            PRTE_RML_PERSISTENT, pmix_server_dmdx_resp, NULL);

    /* setup recv for replies to proxy launch requests */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_LAUNCH_RESP,
                            PRTE_RML_PERSISTENT, pmix_server_launch_resp, NULL);

    /* setup recv for replies from data server */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DATA_CLIENT,
                            PRTE_RML_PERSISTENT, pmix_server_keyval_client, NULL);

    /* setup recv for notifications */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_NOTIFICATION,
                            PRTE_RML_PERSISTENT, pmix_server_notify, NULL);

    if (PRTE_PROC_IS_MASTER || PRTE_PROC_IS_MASTER) {
        /* setup recv for logging requests */
        prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD, PRTE_RML_TAG_LOGGING,
                                PRTE_RML_PERSISTENT, pmix_server_log, NULL);
    }
}

void pmix_server_finalize(void)
{
    if (!prte_pmix_server_globals.initialized) {
        return;
    }

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s Finalizing PMIX server",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* stop receives */
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DIRECT_MODEX);
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DIRECT_MODEX_RESP);
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_LAUNCH_RESP);
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DATA_CLIENT);
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_NOTIFICATION);
    if (PRTE_PROC_IS_MASTER || PRTE_PROC_IS_MASTER) {
        prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_LOGGING);
    }

    /* finalize our local data server */
    prte_data_server_finalize();

    /* shutdown the local server */
    PMIx_server_finalize();

    /* cleanup collectives */
    PRTE_DESTRUCT(&prte_pmix_server_globals.reqs);
    PRTE_LIST_DESTRUCT(&prte_pmix_server_globals.notifications);
    PRTE_LIST_DESTRUCT(&prte_pmix_server_globals.psets);
#ifdef PMIX_TOPOLOGY2
    free(mytopology.source);
#endif
    prte_pmix_server_globals.initialized = false;
}

static void send_error(int status, pmix_proc_t *idreq,
                       prte_process_name_t *remote, int remote_room)
{
    prte_buffer_t *reply;
    pmix_status_t prc, pstatus;
    pmix_data_buffer_t pbuf;
    char *data=NULL;
    int32_t sz=0;

    /* pack the status */
    pstatus = prte_pmix_convert_rc(status);
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &pstatus, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }
    /* pack the id of the requested proc */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, idreq, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }

    /* pack the remote daemon's request room number */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &remote_room, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }

    /* send the response */
    reply = PRTE_NEW(prte_buffer_t);
    PMIX_DATA_BUFFER_UNLOAD(&pbuf, data, sz);
    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
    prte_dss.load(reply, data, sz);
    prte_rml.send_buffer_nb(remote, reply,
                            PRTE_RML_TAG_DIRECT_MODEX_RESP,
                            prte_rml_send_callback, NULL);

error:
    PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
    return;
}

static void _mdxresp(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    prte_buffer_t *reply;
    pmix_status_t prc;
    pmix_data_buffer_t pbuf;
    char *data;
    size_t sz;

    PRTE_ACQUIRE_OBJECT(req);

    prte_output_verbose(2, prte_pmix_server_globals.output,
                         "%s XMITTING DATA FOR PROC %s:%u",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         req->tproc.nspace, req->tproc.rank);

    /* check us out of the hotel */
    prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);

    /* pack the status */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->pstatus, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }
    /* pack the id of the requested proc */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->tproc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }

    /* pack the remote daemon's request room number */
    if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->remote_room_num, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        goto error;
    }
    if (PMIX_SUCCESS == req->pstatus) {
        /* return any provided data */
        if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, &req->sz, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(prc);
            goto error;
        }
        if (0 < req->sz) {
            if (PMIX_SUCCESS != (prc = PMIx_Data_pack(&prte_process_info.myproc, &pbuf, req->data, req->sz, PMIX_BYTE))) {
                PMIX_ERROR_LOG(prc);
                goto error;
            }
            free(req->data);
        }
    }

    /* send the response */
    reply = PRTE_NEW(prte_buffer_t);
    PMIX_DATA_BUFFER_UNLOAD(&pbuf, data, sz);
    prte_dss.load(reply, data, sz);
    prte_rml.send_buffer_nb(&req->proxy, reply,
                            PRTE_RML_TAG_DIRECT_MODEX_RESP,
                            prte_rml_send_callback, NULL);

  error:
    PRTE_RELEASE(req);
    return;
}
/* the modex_resp function takes place in the local PMIx server's
 * progress thread - we must therefore thread-shift it so we can
 * access our global data */
static void modex_resp(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;

    PRTE_ACQUIRE_OBJECT(req);

    req->pstatus = status;
    if (PMIX_SUCCESS == status && NULL != data) {
        /* we need to preserve the data as the caller
         * will free it upon our return */
        req->data = (char*)malloc(sz);
        if (NULL == req->data) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        }
        memcpy(req->data, data, sz);
        req->sz = sz;
    }
    prte_event_set(prte_event_base, &(req->ev),
                   -1, PRTE_EV_WRITE, _mdxresp, req);
    prte_event_set_priority(&(req->ev), PRTE_MSG_PRI);
    PRTE_POST_OBJECT(req);
    prte_event_active(&(req->ev), PRTE_EV_WRITE, 1);
}
static void pmix_server_dmdx_recv(int status, prte_process_name_t* sender,
                                  prte_buffer_t *buffer,
                                  prte_rml_tag_t tg, void *cbdata)
{
    int rc, room_num;
    int32_t cnt;
    prte_process_name_t name;
    prte_job_t *jdata;
    prte_proc_t *proc;
    pmix_server_req_t *req;
    pmix_proc_t pproc;
    pmix_status_t prc;
    pmix_info_t *info=NULL;
    size_t ninfo;
    pmix_data_buffer_t pbuf;
    char *data, *key=NULL;
    size_t sz;
    pmix_value_t *pval = NULL;

    prte_dss.unload(buffer, (void**)&data, &cnt);
    sz = cnt;
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    PMIX_DATA_BUFFER_LOAD(&pbuf, data, sz);

    cnt = 1;
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &pproc, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        return;
    }
    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s dmdx:recv request from proc %s for proc %s:%u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender),
                        pproc.nspace, pproc.rank);
    /* and the remote daemon's tracking room number */
    cnt = 1;
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &room_num, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        return;
    }
    cnt = 1;
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(prc);
            prte_dss.load(buffer, data, sz);
            return;
        }
    }
    prte_dss.load(buffer, data, sz);  // restore the buffer as we are done with it

#if PMIX_VERSION_MAJOR >=4
    /* see if they want us to await a particular key before sending
     * the response */
    if (NULL != info) {
        for (sz=0; sz < ninfo; sz++) {
            if (PMIX_CHECK_KEY(&info[sz], PMIX_REQUIRED_KEY)) {
                key = info[sz].value.data.string;
                break;
            }
        }
    }
#endif

    /* is this proc one of mine? */
    PRTE_PMIX_CONVERT_PROCT(rc, &name, &pproc);
    if (NULL == (jdata = prte_get_job_data_object(name.jobid))) {
        /* not having the jdata means that we haven't unpacked the
         * the launch message for this job yet - this is a race
         * condition, so just log the request and we will fill
         * it later */
        prte_output_verbose(2, prte_pmix_server_globals.output,
                             "%s dmdx:recv request no job - checking into hotel",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        req = PRTE_NEW(pmix_server_req_t);
        prte_asprintf(&req->operation, "DMDX: %s:%d", __FILE__, __LINE__);
        req->proxy = *sender;
        memcpy(&req->tproc, &pproc, sizeof(pmix_proc_t));
        req->info = info;
        req->ninfo = ninfo;
        if (NULL != key) {
            req->key = strdup(key);
        }
        req->remote_room_num = room_num;
        /* adjust the timeout to reflect the size of the job as it can take some
         * amount of time to start the job */
        PRTE_ADJUST_TIMEOUT(req);
        if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
            prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
            PRTE_RELEASE(req);
            send_error(rc, &pproc, sender, room_num);
        }
        return;
    }
    if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, name.vpid))) {
        /* this is truly an error, so notify the sender */
        send_error(PRTE_ERR_NOT_FOUND, &pproc, sender, room_num);
        return;
    }
    if (!PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_LOCAL)) {
        /* send back an error - they obviously have made a mistake */
        send_error(PRTE_ERR_NOT_FOUND, &pproc, sender, room_num);
        return;
    }

    if (NULL != key) {
        prte_output_verbose(2, prte_pmix_server_globals.output,
                             "%s dmdx:recv checking for key %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), key);
        /* see if we have it */
        if (PMIX_SUCCESS != PMIx_Get(&pproc, key, info, ninfo, &pval)) {
            prte_output_verbose(2, prte_pmix_server_globals.output,
                                 "%s dmdx:recv key %s not found - checking into hotel",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), key);
            /* we don't - wait for awhile */
            req = PRTE_NEW(pmix_server_req_t);
            prte_asprintf(&req->operation, "DMDX: %s:%d", __FILE__, __LINE__);
            req->proxy = *sender;
            memcpy(&req->tproc, &pproc, sizeof(pmix_proc_t));
            req->info = info;
            req->ninfo = ninfo;
            req->key = strdup(key);
            req->remote_room_num = room_num;
            /* adjust the timeout to reflect the size of the job as it can take some
             * amount of time to start the job */
            PRTE_ADJUST_TIMEOUT(req);
            /* we no longer need the info */
            PMIX_INFO_FREE(info, ninfo);
            /* check us into the hotel */
            if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
                prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
                PRTE_RELEASE(req);
                send_error(rc, &pproc, sender, room_num);
            }
                prte_output_verbose(2, prte_pmix_server_globals.output,
                                     "%s:%d CHECKING REQ FOR KEY %s TO %d REMOTE ROOM %d",
                                     __FILE__, __LINE__, req->key, req->room_num, req->remote_room_num);
            return;
        }
        /* we do already have it, so go get the payload */
        PMIX_VALUE_RELEASE(pval);
        prte_output_verbose(2, prte_pmix_server_globals.output,
                             "%s dmdx:recv key %s found - retrieving payload",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), key);
    }

    /* track the request since the call down to the PMIx server
     * is asynchronous */
    req = PRTE_NEW(pmix_server_req_t);
    prte_asprintf(&req->operation, "DMDX: %s:%d", __FILE__, __LINE__);
    req->proxy = *sender;
    memcpy(&req->tproc, &pproc, sizeof(pmix_proc_t));
    req->info = info;
    req->ninfo = ninfo;
    req->remote_room_num = room_num;
    /* adjust the timeout to reflect the size of the job as it can take some
     * amount of time to start the job */
    PRTE_ADJUST_TIMEOUT(req);
    if (PRTE_SUCCESS != (rc = prte_hotel_checkin(&prte_pmix_server_globals.reqs, req, &req->room_num))) {
        prte_show_help("help-prted.txt", "noroom", true, req->operation, prte_pmix_server_globals.num_rooms);
        PRTE_RELEASE(req);
        send_error(rc, &pproc, sender, room_num);
        return;
    }

    /* ask our local pmix server for the data */
    if (PMIX_SUCCESS != (prc = PMIx_server_dmodex_request(&pproc, modex_resp, req))) {
        PMIX_ERROR_LOG(prc);
        prte_hotel_checkout(&prte_pmix_server_globals.reqs, req->room_num);
        PRTE_RELEASE(req);
        send_error(rc, &pproc, sender, room_num);
        return;
    }
    return;
}

typedef struct {
    prte_object_t super;
    char *data;
    int32_t ndata;
} datacaddy_t;
static void dccon(datacaddy_t *p)
{
    p->data = NULL;
    p->ndata = 0;
}
static void dcdes(datacaddy_t *p)
{
    if (NULL != p->data) {
        free(p->data);
    }
}
static PRTE_CLASS_INSTANCE(datacaddy_t,
                            prte_object_t,
                            dccon, dcdes);

static void relcbfunc(void *relcbdata)
{
    datacaddy_t *d = (datacaddy_t*)relcbdata;

    PRTE_RELEASE(d);
}

static void pmix_server_dmdx_resp(int status, prte_process_name_t* sender,
                                  prte_buffer_t *buffer,
                                  prte_rml_tag_t tg, void *cbdata)
{
    int room_num, rnum;
    int32_t cnt;
    pmix_server_req_t *req;
    datacaddy_t *d;
    pmix_proc_t pproc;
    pmix_data_buffer_t pbuf;
    char *data;
    size_t sz, psz;
    pmix_status_t prc, pret;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s dmdx:recv response from proc %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    prte_dss.unload(buffer, (void**)&data, &cnt);
    sz = cnt;
    PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
    PMIX_DATA_BUFFER_LOAD(&pbuf, data, sz);
    d = PRTE_NEW(datacaddy_t);

    /* unpack the status */
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &pret, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        PRTE_RELEASE(d);
        return;
    }

    /* unpack the id of the target whose info we just received */
    cnt = 1;
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &pproc, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        PRTE_RELEASE(d);
        return;
    }

    /* unpack our tracking room number */
    cnt = 1;
    if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &room_num, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(prc);
        prte_dss.load(buffer, data, sz);
        PRTE_RELEASE(d);
        return;
    }

    /* unload the remainder of the buffer */
    if (PMIX_SUCCESS == pret) {
        cnt = 1;
        if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, &psz, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(prc);
            prte_dss.load(buffer, data, sz);
            PRTE_RELEASE(d);
            return;
        }
        if (0 < psz) {
            d->ndata = psz;
            d->data = (char*)malloc(psz);
            if (NULL == d->data) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            }
            cnt = psz;
            if (PMIX_SUCCESS != (prc = PMIx_Data_unpack(&prte_process_info.myproc, &pbuf, d->data, &cnt, PMIX_BYTE))) {
                PMIX_ERROR_LOG(prc);
                prte_dss.load(buffer, data, sz);
                PRTE_RELEASE(d);
                return;
            }
        }
    }
    prte_dss.load(buffer, data, sz);

    /* check the request out of the tracking hotel */
    prte_hotel_checkout_and_return_occupant(&prte_pmix_server_globals.reqs, room_num, (void**)&req);
    /* return the returned data to the requestor */
    if (NULL != req) {
        if (NULL != req->mdxcbfunc) {
            PRTE_RETAIN(d);
            req->mdxcbfunc(pret, d->data, d->ndata, req->cbdata, relcbfunc, d);
        }
        PRTE_RELEASE(req);
    } else {
        prte_output_verbose(2, prte_pmix_server_globals.output,
                             "REQ WAS NULL IN ROOM %d", room_num);
    }

    /* now see if anyone else was waiting for data from this target */
    for (rnum=0; rnum < prte_pmix_server_globals.reqs.num_rooms; rnum++) {
        prte_hotel_knock(&prte_pmix_server_globals.reqs, rnum, (void**)&req);
        if (NULL == req) {
            continue;
        }
        if (PMIX_CHECK_PROCID(&req->tproc, &pproc)) {
            if (NULL != req->mdxcbfunc) {
                PRTE_RETAIN(d);
                req->mdxcbfunc(pret, d->data, d->ndata, req->cbdata, relcbfunc, d);
            }
            prte_hotel_checkout(&prte_pmix_server_globals.reqs, rnum);
            PRTE_RELEASE(req);
        }
    }
    PRTE_RELEASE(d);  // maintain accounting
}

static void pmix_server_log(int status, prte_process_name_t* sender,
                            prte_buffer_t *buffer,
                            prte_rml_tag_t tg, void *cbdata)
{
    int rc;
    int32_t cnt;
    size_t n, ninfo;
    pmix_info_t *info, directives[2];
    pmix_status_t ret;
    prte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;

    /* unpack the number of info */
    cnt = 1;
    rc = prte_dss.unpack(buffer, &ninfo, &cnt, PRTE_SIZE);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the blob */
    cnt = 1;
    rc = prte_dss.unpack(buffer, &boptr, &cnt, PRTE_BYTE_OBJECT);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    PMIX_INFO_CREATE(info, ninfo);
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    for (n=0; n < ninfo; n++) {
        cnt = 1;
        ret = PMIx_Data_unpack(&prte_process_info.myproc, &pbkt, (void*)&info[n], &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

    /* mark that we only want it logged once */
    PMIX_INFO_LOAD(&directives[0], PMIX_LOG_ONCE, NULL, PMIX_BOOL);
    /* protect against infinite loop should the PMIx server push
     * this back up to us */
    PMIX_INFO_LOAD(&directives[1], "prte.log.noloop", NULL, PMIX_BOOL);

    /* pass the array down to be logged */
    PMIx_Log(info, ninfo, directives, 2);
    PMIX_INFO_FREE(info, ninfo+1);
    PMIX_INFO_DESTRUCT(&directives[1]);
}


/****    INSTANTIATE LOCAL OBJECTS    ****/
static void opcon(prte_pmix_server_op_caddy_t *p)
{
    memset(&p->proct, 0, sizeof(pmix_proc_t));
    p->procs = NULL;
    p->nprocs = 0;
    p->eprocs = NULL;
    p->neprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->directives = NULL;
    p->ndirs = 0;
    p->apps = NULL;
    p->napps = 0;
    p->cbfunc = NULL;
    p->infocbfunc = NULL;
    p->toolcbfunc = NULL;
    p->spcbfunc = NULL;
    p->cbdata = NULL;
    p->server_object = NULL;
}
PRTE_CLASS_INSTANCE(prte_pmix_server_op_caddy_t,
                   prte_object_t,
                   opcon, NULL);

static void rqcon(pmix_server_req_t *p)
{
    p->operation = NULL;
    p->cmdline = NULL;
    p->key = NULL;
    p->flag = true;
    p->launcher = false;
    p->remote_room_num = -1;
    p->uid = 0;
    p->gid = 0;
    p->pid = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->data = NULL;
    p->sz = 0;
    p->range = PMIX_RANGE_SESSION;
    p->proxy = *PRTE_NAME_INVALID;
    p->target = *PRTE_NAME_INVALID;
    p->jdata = NULL;
    PRTE_CONSTRUCT(&p->msg, prte_buffer_t);
    p->timeout = prte_pmix_server_globals.timeout;
    p->opcbfunc = NULL;
    p->mdxcbfunc = NULL;
    p->spcbfunc = NULL;
    p->lkcbfunc = NULL;
    p->rlcbfunc = NULL;
    p->toolcbfunc = NULL;
    p->cbdata = NULL;
}
static void rqdes(pmix_server_req_t *p)
{
    if (NULL != p->operation) {
        free(p->operation);
    }
    if (NULL != p->cmdline) {
        free(p->cmdline);
    }
    if (NULL != p->key) {
        free(p->key);
    }
    if (NULL != p->jdata) {
        PRTE_RELEASE(p->jdata);
    }
    PRTE_DESTRUCT(&p->msg);
}
PRTE_CLASS_INSTANCE(pmix_server_req_t,
                   prte_object_t,
                   rqcon, rqdes);

static void mdcon(prte_pmix_mdx_caddy_t *p)
{
    p->sig = NULL;
    p->buf = NULL;
    p->cbfunc = NULL;
    p->mode = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->cbdata = NULL;
}
static void mddes(prte_pmix_mdx_caddy_t *p)
{
    if (NULL != p->sig) {
        PRTE_RELEASE(p->sig);
    }
    if (NULL != p->buf) {
        PRTE_RELEASE(p->buf);
    }
}
PRTE_CLASS_INSTANCE(prte_pmix_mdx_caddy_t,
                   prte_object_t,
                   mdcon, mddes);

static void pscon(pmix_server_pset_t *p)
{
    p->name = NULL;
    p->members = NULL;
    p->num_members = 0;
}
static void psdes(pmix_server_pset_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->members) {
        free(p->members);
    }
}
PRTE_CLASS_INSTANCE(pmix_server_pset_t,
                   prte_list_item_t,
                   pscon, psdes);
