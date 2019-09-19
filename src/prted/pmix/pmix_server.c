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
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
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

#include "prrte_stdint.h"
#include "src/class/prrte_hotel.h"
#include "src/class/prrte_list.h"
#include "src/mca/base/prrte_mca_base_var.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/prrte_environ.h"
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
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_data_server.h"

#include "pmix_server.h"
#include "pmix_server_internal.h"

/*
 * Local utility functions
 */
static void pmix_server_dmdx_recv(int status, prrte_process_name_t* sender,
                                  prrte_buffer_t *buffer,
                                  prrte_rml_tag_t tg, void *cbdata);
static void pmix_server_dmdx_resp(int status, prrte_process_name_t* sender,
                                  prrte_buffer_t *buffer,
                                  prrte_rml_tag_t tg, void *cbdata);
static void pmix_server_log(int status, prrte_process_name_t* sender,
                            prrte_buffer_t *buffer,
                            prrte_rml_tag_t tg, void *cbdata);

#define PRRTE_PMIX_SERVER_MIN_ROOMS    4096

pmix_server_globals_t prrte_pmix_server_globals = {0};

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
static char *prrte_fns[] = {
    "fence_nb",
    "direct_modex"
};

typedef struct {
    char *name;
    char *string;
    pmix_data_type_t type;
    char **description;
} prrte_regattr_input_t;

static prrte_regattr_input_t prrte_attributes[] = {
    // fence_nb
    {.name = "PMIX_TIMEOUT", .string = PMIX_TIMEOUT, .type = PMIX_INT, .description = (char *[]){"POSITIVE INTEGERS", "Time in seconds before", "declaring not found", NULL}},
    {.name = ""},
    // direct_modex
    {.name = "PMIX_TIMEOUT", .string = PMIX_TIMEOUT, .type = PMIX_INT, .description = (char *[]){"POSITIVE INTEGERS", "Time in seconds before", "declaring not found", NULL}},
    {.name = ""},
};
#endif

void pmix_server_register_params(void)
{
    /* register a verbosity */
    prrte_pmix_server_globals.verbosity = -1;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "server_verbose",
                                  "Debug verbosity for PMIx server",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.verbosity);
    if (0 <= prrte_pmix_server_globals.verbosity) {
        prrte_pmix_server_globals.output = prrte_output_open(NULL);
        prrte_output_set_verbosity(prrte_pmix_server_globals.output,
                                  prrte_pmix_server_globals.verbosity);
    }
    /* specify the size of the hotel */
    prrte_pmix_server_globals.num_rooms = -1;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "server_max_reqs",
                                  "Maximum number of backlogged PMIx server direct modex requests",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.num_rooms);
    /* specify the timeout for the hotel */
    prrte_pmix_server_globals.timeout = 2;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "server_max_wait",
                                  "Maximum time (in seconds) the PMIx server should wait to service direct modex requests",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.timeout);

    /* whether or not to wait for the universal server */
    prrte_pmix_server_globals.wait_for_server = false;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "wait_for_server",
                                  "Whether or not to wait for the session-level server to start",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.wait_for_server);

    /* whether or not to support legacy usock connections as well as tcp */
    prrte_pmix_server_globals.legacy = false;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "server_usock_connections",
                                  "Whether or not to support legacy usock connections",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.legacy);

    /* whether or not to drop a session-level tool rendezvous point */
    prrte_pmix_server_globals.session_server = false;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "session_server",
                                  "Whether or not to drop a session-level tool rendezvous point",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.session_server);

    /* whether or not to drop a system-level tool rendezvous point */
    prrte_pmix_server_globals.system_server = false;
    (void) prrte_mca_base_var_register ("prrte", "pmix", NULL, "system_server",
                                  "Whether or not to drop a system-level tool rendezvous point",
                                  PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_pmix_server_globals.system_server);
}

static void eviction_cbfunc(struct prrte_hotel_t *hotel,
                            int room_num, void *occupant)
{
    pmix_server_req_t *req = (pmix_server_req_t*)occupant;
    bool timeout = false;
    int rc=PRRTE_ERR_TIMEOUT;

    /* decrement the request timeout */
    req->timeout -= prrte_pmix_server_globals.timeout;
    if (req->timeout > 0) {
        req->timeout -= prrte_pmix_server_globals.timeout;
        if (0 >= req->timeout) {
            timeout = true;
        }
    }
    if (!timeout) {
        /* not done yet - check us back in */
        if (PRRTE_SUCCESS == (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
            return;
        }
        PRRTE_ERROR_LOG(rc);
        /* fall thru and return an error so the caller doesn't hang */
    } else {
        prrte_show_help("help-orted.txt", "timedout", true, req->operation);
    }
    /* don't let the caller hang */
    if (NULL != req->opcbfunc) {
        req->opcbfunc(PMIX_ERR_TIMEOUT, req->cbdata);
    } else if (NULL != req->mdxcbfunc) {
        req->mdxcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, req->cbdata, NULL, NULL);
    } else if (NULL != req->spcbfunc) {
        req->spcbfunc(PMIX_ERR_TIMEOUT, NULL, req->cbdata);
    } else if (NULL != req->lkcbfunc) {
        req->lkcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, req->cbdata);
    }
    PRRTE_RELEASE(req);
}

/*
 * Initialize global variables used w/in the server.
 */
int pmix_server_init(void)
{
    int rc;
    prrte_list_t ilist;
    prrte_value_t *kv;
    pmix_info_t *info;
    size_t n, ninfo;
    pmix_proc_t pname;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    pmix_regattr_t *attrs;
    size_t m, nregs, nattrs, cnt;
#endif

    if (prrte_pmix_server_globals.initialized) {
        return PRRTE_SUCCESS;
    }
    prrte_pmix_server_globals.initialized = true;

    /* setup the server's state variables */
    PRRTE_CONSTRUCT(&prrte_pmix_server_globals.reqs, prrte_hotel_t);
    PRRTE_CONSTRUCT(&prrte_pmix_server_globals.psets, prrte_list_t);

    /* by the time we init the server, we should know how many nodes we
     * have in our environment - with the exception of mpirun. If the
     * user specified the size of the hotel, then use that value. Otherwise,
     * set the value to something large to avoid running out of rooms on
     * large machines */
    if (-1 == prrte_pmix_server_globals.num_rooms) {
        prrte_pmix_server_globals.num_rooms = prrte_process_info.num_procs * 2;
        if (prrte_pmix_server_globals.num_rooms < PRRTE_PMIX_SERVER_MIN_ROOMS) {
            prrte_pmix_server_globals.num_rooms = PRRTE_PMIX_SERVER_MIN_ROOMS;
        }
    }
    if (PRRTE_SUCCESS != (rc = prrte_hotel_init(&prrte_pmix_server_globals.reqs,
                                              prrte_pmix_server_globals.num_rooms,
                                              prrte_event_base, prrte_pmix_server_globals.timeout*1000000,
                                              PRRTE_ERROR_PRI, eviction_cbfunc))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    PRRTE_CONSTRUCT(&prrte_pmix_server_globals.notifications, prrte_list_t);
    prrte_pmix_server_globals.server = *PRRTE_NAME_INVALID;

    PRRTE_CONSTRUCT(&ilist, prrte_list_t);

#if HWLOC_API_VERSION < 0x20000
     /* pass the topology string as we don't
      * have HWLOC shared memory available - we do
      * this so the procs won't read the topology
      * themselves as this could overwhelm the local
      * system on large-scale SMPs */
    if (NULL != prrte_hwloc_topology) {
        char *xmlbuffer=NULL;
        int len;
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_HWLOC_XML_V1);
        if (0 != hwloc_topology_export_xmlbuffer(prrte_hwloc_topology, &xmlbuffer, &len)) {
            PRRTE_RELEASE(kv);
            PRRTE_DESTRUCT(&ilist);
            return PRRTE_ERROR;
        }
        kv->data.string = xmlbuffer;
        kv->type = PRRTE_STRING;
        prrte_list_append(&ilist, &kv->super);
    }
#else
    /* if shmem support isn't available, then export
     * the topology as a v2 xml string */
    if (!prrte_hwloc_shmem_available) {
        if (NULL != prrte_hwloc_topology) {
            char *xmlbuffer=NULL;
            int len;
            kv = PRRTE_NEW(prrte_value_t);
            kv->key = strdup(PMIX_HWLOC_XML_V2);
            if (0 != hwloc_topology_export_xmlbuffer(prrte_hwloc_topology, &xmlbuffer, &len, 0)) {
                PRRTE_RELEASE(kv);
                PRRTE_DESTRUCT(&ilist);
                return PRRTE_ERROR;
            }
            kv->data.string = xmlbuffer;
            kv->type = PRRTE_STRING;
            prrte_list_append(&ilist, &kv->super);
        }
    }
#endif

    /* tell the server our temp directory */
    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_SERVER_TMPDIR);
    kv->type = PRRTE_STRING;
    kv->data.string = prrte_os_path(false, prrte_process_info.jobfam_session_dir, NULL);
    prrte_list_append(&ilist, &kv->super);
    if (!prrte_pmix_server_globals.legacy) {
        /* use only one listener */
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_SINGLE_LISTENER);
        kv->type = PRRTE_BOOL;
        kv->data.flag = true;
        prrte_list_append(&ilist, &kv->super);
    }
    /* tell the server to use its own internal monitoring */
    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_SERVER_ENABLE_MONITORING);
    kv->type = PRRTE_BOOL;
    kv->data.flag = true;
    prrte_list_append(&ilist, &kv->super);
    /* if requested, tell the server to drop a session-level
     * PMIx connection point */
    if (prrte_pmix_server_globals.session_server) {
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_SERVER_TOOL_SUPPORT);
        kv->type = PRRTE_BOOL;
        kv->data.flag = true;
        prrte_list_append(&ilist, &kv->super);
    }

    /* if requested, tell the server to drop a system-level
     * PMIx connection point - only do this for the HNP as, in
     * at least one case, a daemon can be colocated with the
     * HNP and would overwrite the server rendezvous file */
    if (prrte_pmix_server_globals.system_server &&
        (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_MASTER)) {
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_SERVER_SYSTEM_SUPPORT);
        kv->type = PRRTE_BOOL;
        kv->data.flag = true;
        prrte_list_append(&ilist, &kv->super);
    }

    /* if we are the MASTER, then we are the scheduler
     * as well as a gateway */
    if (PRRTE_PROC_IS_MASTER) {
#ifdef PMIX_SERVER_SCHEDULER
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_SERVER_SCHEDULER);
        kv->type = PRRTE_BOOL;
        kv->data.flag = true;
        prrte_list_append(&ilist, &kv->super);
#endif
#ifdef PMIX_SERVER_GATEWAY
        kv = PRRTE_NEW(prrte_value_t);
        kv->key = strdup(PMIX_SERVER_GATEWAY);
        kv->type = PRRTE_BOOL;
        kv->data.flag = true;
        prrte_list_append(&ilist, &kv->super);
#endif
    }

    /* PRRTE always allows remote tool connections */
    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_SERVER_REMOTE_CONNECTIONS);
    kv->type = PRRTE_BOOL;
    kv->data.flag = true;
    prrte_list_append(&ilist, &kv->super);

    /* convert to an info array */
    ninfo = prrte_list_get_size(&ilist) + 2;
    PMIX_INFO_CREATE(info, ninfo);
    n = 0;
    while (NULL != (kv = (prrte_value_t*)prrte_list_remove_first(&ilist))) {
        if (PRRTE_BOOL == kv->type) {
            PMIX_INFO_LOAD(&info[n], kv->key, &kv->data.flag, PMIX_BOOL);
        } else {
            PMIX_INFO_LOAD(&info[n], kv->key, kv->data.string, PMIX_STRING);
        }
        ++n;
        PRRTE_RELEASE(kv);
    }
    PRRTE_LIST_DESTRUCT(&ilist);
    /* tell the server our name so we agree on our identifier */
    PRRTE_PMIX_CONVERT_NAME(&pname, PRRTE_PROC_MY_NAME);
    PMIX_INFO_LOAD(&info[n], PMIX_SERVER_NSPACE, pname.nspace, PMIX_STRING);
    PMIX_INFO_LOAD(&info[n+1], PMIX_SERVER_RANK, &pname.rank, PMIX_PROC_RANK);

    /* setup the local server */
    if (PRRTE_SUCCESS != (rc = PMIx_server_init(&pmix_server, info, ninfo))) {
        /* pmix will provide a nice show_help output here */
        PMIX_INFO_FREE(info, ninfo);
        return rc;
    }
    PMIX_INFO_FREE(info, ninfo);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* register our support */
    nregs = sizeof(prrte_fns) / sizeof(char*);
    cnt = 0;
    for (n=0; n < nregs; n++) {
        nattrs = 0;
        while (0 != strlen(prrte_attributes[cnt+nattrs].name)) {
            ++nattrs;
        }
        PMIX_REGATTR_CREATE(attrs, nattrs);
        for (m=0; m < nattrs; m++) {
            attrs[m].name = strdup(prrte_attributes[m+cnt].name);
            PMIX_LOAD_KEY(attrs[m].string, prrte_attributes[m+cnt].string);
            attrs[m].type = prrte_attributes[m+cnt].type;
            PMIX_ARGV_COPY(attrs[m].description, prrte_attributes[m+cnt].description);
        }
        rc = PMIx_Register_attributes(prrte_fns[n], attrs, nattrs);
        PMIX_REGATTR_FREE(attrs, nattrs);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        cnt += nattrs + 1;
    }
#endif

    return rc;
}

void pmix_server_start(void)
{
    /* setup our local data server */
    prrte_data_server_init();

    /* setup recv for direct modex requests */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DIRECT_MODEX,
                            PRRTE_RML_PERSISTENT, pmix_server_dmdx_recv, NULL);

    /* setup recv for replies to direct modex requests */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DIRECT_MODEX_RESP,
                            PRRTE_RML_PERSISTENT, pmix_server_dmdx_resp, NULL);

    /* setup recv for replies to proxy launch requests */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_LAUNCH_RESP,
                            PRRTE_RML_PERSISTENT, pmix_server_launch_resp, NULL);

    /* setup recv for replies from data server */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DATA_CLIENT,
                            PRRTE_RML_PERSISTENT, pmix_server_keyval_client, NULL);

    /* setup recv for notifications */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_NOTIFICATION,
                            PRRTE_RML_PERSISTENT, pmix_server_notify, NULL);

    if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_MASTER) {
        /* setup recv for logging requests */
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_LOGGING,
                                PRRTE_RML_PERSISTENT, pmix_server_log, NULL);
    }
}

void pmix_server_finalize(void)
{
    if (!prrte_pmix_server_globals.initialized) {
        return;
    }

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s Finalizing PMIX server",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* stop receives */
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DIRECT_MODEX);
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DIRECT_MODEX_RESP);
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_LAUNCH_RESP);
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_DATA_CLIENT);
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_NOTIFICATION);
    if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_MASTER) {
        prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_LOGGING);
    }

    /* finalize our local data server */
    prrte_data_server_finalize();

    /* shutdown the local server */
    PMIx_server_finalize();

    /* cleanup collectives */
    PRRTE_DESTRUCT(&prrte_pmix_server_globals.reqs);
    PRRTE_LIST_DESTRUCT(&prrte_pmix_server_globals.notifications);
    PRRTE_LIST_DESTRUCT(&prrte_pmix_server_globals.psets);
    prrte_pmix_server_globals.initialized = false;
}

static void send_error(int status, prrte_process_name_t *idreq,
                       prrte_process_name_t *remote, int remote_room)
{
    prrte_buffer_t *reply;
    int rc;

    reply = PRRTE_NEW(prrte_buffer_t);
    /* pack the status */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &status, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        goto error;
    }
    /* pack the id of the requested proc */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, idreq, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        goto error;
    }

    /* pack the remote daemon's request room number */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &remote_room, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        goto error;
    }

    /* send the response */
    prrte_rml.send_buffer_nb(remote, reply,
                            PRRTE_RML_TAG_DIRECT_MODEX_RESP,
                            prrte_rml_send_callback, NULL);
    return;
error:
    PRRTE_RELEASE(reply);
    return;
}

static void _mdxresp(int sd, short args, void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    int rc;
    prrte_buffer_t *reply;

    PRRTE_ACQUIRE_OBJECT(req);

    /* check us out of the hotel */
    prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);

    reply = PRRTE_NEW(prrte_buffer_t);
    /* return the status */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &req->status, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(reply);
        goto done;
    }
    /* pack the id of the requested proc */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &req->target, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(reply);
        goto done;
    }
    /* pack the remote daemon's request room number */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &req->remote_room_num, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(reply);
        goto done;
    }
    /* return any provided data */
    prrte_dss.copy_payload(reply, &req->msg);

    /* send the response */
    prrte_rml.send_buffer_nb(&req->proxy, reply,
                            PRRTE_RML_TAG_DIRECT_MODEX_RESP,
                            prrte_rml_send_callback, NULL);

  done:
    PRRTE_RELEASE(req);
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
    prrte_buffer_t xfer;

    PRRTE_ACQUIRE_OBJECT(req);

    req->status = status;
    /* we need to preserve the data as the caller
     * will free it upon our return */
    PRRTE_CONSTRUCT(&xfer, prrte_buffer_t);
    prrte_dss.load(&xfer, (void*)data, sz);
    prrte_dss.copy_payload(&req->msg, &xfer);
    xfer.base_ptr = NULL; // protect the incoming data
    PRRTE_DESTRUCT(&xfer);
    prrte_event_set(prrte_event_base, &(req->ev),
                   -1, PRRTE_EV_WRITE, _mdxresp, req);
    prrte_event_set_priority(&(req->ev), PRRTE_MSG_PRI);
    PRRTE_POST_OBJECT(req);
    prrte_event_active(&(req->ev), PRRTE_EV_WRITE, 1);
}
static void pmix_server_dmdx_recv(int status, prrte_process_name_t* sender,
                                  prrte_buffer_t *buffer,
                                  prrte_rml_tag_t tg, void *cbdata)
{
    int rc, room_num;
    int32_t cnt;
    prrte_process_name_t idreq;
    prrte_process_name_t name;
    prrte_job_t *jdata;
    prrte_proc_t *proc;
    pmix_server_req_t *req;
    pmix_proc_t pproc;

    /* unpack the id of the proc whose data is being requested */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &idreq, &cnt, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }
    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s dmdx:recv request from proc %s for proc %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(sender),
                        PRRTE_NAME_PRINT(&idreq));
    /* and the remote daemon's tracking room number */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &room_num, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }
    /* is this proc one of mine? */
    memcpy((char*)&name, (char*)&idreq, sizeof(prrte_process_name_t));
    if (NULL == (jdata = prrte_get_job_data_object(name.jobid))) {
        /* not having the jdata means that we haven't unpacked the
         * the launch message for this job yet - this is a race
         * condition, so just log the request and we will fill
         * it later */
        req = PRRTE_NEW(pmix_server_req_t);
        prrte_asprintf(&req->operation, "DMDX: %s:%d", __FILE__, __LINE__);
        req->proxy = *sender;
        req->target = idreq;
        req->remote_room_num = room_num;
        /* adjust the timeout to reflect the size of the job as it can take some
         * amount of time to start the job */
        PRRTE_ADJUST_TIMEOUT(req);
        if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
            prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
            PRRTE_RELEASE(req);
            send_error(rc, &idreq, sender, room_num);
        }
        return;
    }
    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, name.vpid))) {
        /* this is truly an error, so notify the sender */
        send_error(PRRTE_ERR_NOT_FOUND, &idreq, sender, room_num);
        return;
    }
    if (!PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_LOCAL)) {
        /* send back an error - they obviously have made a mistake */
        send_error(PRRTE_ERR_NOT_FOUND, &idreq, sender, room_num);
        return;
    }
    /* track the request since the call down to the PMIx server
     * is asynchronous */
    req = PRRTE_NEW(pmix_server_req_t);
    prrte_asprintf(&req->operation, "DMDX: %s:%d", __FILE__, __LINE__);
    req->proxy = *sender;
    req->target = idreq;
    req->remote_room_num = room_num;
    /* adjust the timeout to reflect the size of the job as it can take some
     * amount of time to start the job */
    PRRTE_ADJUST_TIMEOUT(req);
    if (PRRTE_SUCCESS != (rc = prrte_hotel_checkin(&prrte_pmix_server_globals.reqs, req, &req->room_num))) {
        prrte_show_help("help-orted.txt", "noroom", true, req->operation, prrte_pmix_server_globals.num_rooms);
        PRRTE_RELEASE(req);
        send_error(rc, &idreq, sender, room_num);
        return;
    }

    /* ask our local pmix server for the data */
    PRRTE_PMIX_CONVERT_NAME(&pproc, &idreq);
    if (PRRTE_SUCCESS != (rc = PMIx_server_dmodex_request(&pproc, modex_resp, req))) {
        PRRTE_ERROR_LOG(rc);
        prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, req->room_num);
        PRRTE_RELEASE(req);
        send_error(rc, &idreq, sender, room_num);
        return;
    }
    return;
}

typedef struct {
    prrte_object_t super;
    char *data;
    int32_t ndata;
} datacaddy_t;
static void dccon(datacaddy_t *p)
{
    p->data = NULL;
}
static void dcdes(datacaddy_t *p)
{
    if (NULL != p->data) {
        free(p->data);
    }
}
static PRRTE_CLASS_INSTANCE(datacaddy_t,
                          prrte_object_t,
                          dccon, dcdes);

static void relcbfunc(void *relcbdata)
{
    datacaddy_t *d = (datacaddy_t*)relcbdata;

    PRRTE_RELEASE(d);
}

static void pmix_server_dmdx_resp(int status, prrte_process_name_t* sender,
                                  prrte_buffer_t *buffer,
                                  prrte_rml_tag_t tg, void *cbdata)
{
    int rc, ret, room_num, rnum;
    int32_t cnt;
    prrte_process_name_t target;
    pmix_server_req_t *req;
    datacaddy_t *d;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s dmdx:recv response from proc %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(sender));

    /* unpack the status */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &ret, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the id of the target whose info we just received */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &target, &cnt, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack our tracking room number */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &room_num, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unload the remainder of the buffer */
    d = PRRTE_NEW(datacaddy_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.unload(buffer, (void**)&d->data, &d->ndata))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* check the request out of the tracking hotel */
    prrte_hotel_checkout_and_return_occupant(&prrte_pmix_server_globals.reqs, room_num, (void**)&req);
    /* return the returned data to the requestor */
    if (NULL != req) {
        if (NULL != req->mdxcbfunc) {
            PRRTE_RETAIN(d);
            req->mdxcbfunc(ret, d->data, d->ndata, req->cbdata, relcbfunc, d);
        }
        PRRTE_RELEASE(req);
    }

    /* now see if anyone else was waiting for data from this target */
    for (rnum=0; rnum < prrte_pmix_server_globals.reqs.num_rooms; rnum++) {
        prrte_hotel_knock(&prrte_pmix_server_globals.reqs, rnum, (void**)&req);
        if (NULL == req) {
            continue;
        }
        if (req->target.jobid == target.jobid &&
            req->target.vpid == target.vpid) {
            if (NULL != req->mdxcbfunc) {
                PRRTE_RETAIN(d);
                req->mdxcbfunc(ret, d->data, d->ndata, req->cbdata, relcbfunc, d);
            }
            prrte_hotel_checkout(&prrte_pmix_server_globals.reqs, rnum);
            PRRTE_RELEASE(req);
        }
    }
    PRRTE_RELEASE(d);  // maintain accounting
}

static void pmix_server_log(int status, prrte_process_name_t* sender,
                            prrte_buffer_t *buffer,
                            prrte_rml_tag_t tg, void *cbdata)
{
    int rc;
    int32_t cnt;
    size_t n, ninfo;
    pmix_info_t *info, directives[2];
    pmix_status_t ret;
    pmix_proc_t proc;
    prrte_byte_object_t *boptr;
    pmix_data_buffer_t pbkt;

    (void)prrte_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, sender->jobid);
    proc.rank = sender->vpid;

    /* unpack the number of info */
    cnt = 1;
    rc = prrte_dss.unpack(buffer, &ninfo, &cnt, PRRTE_SIZE);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the blob */
    cnt = 1;
    rc = prrte_dss.unpack(buffer, &boptr, &cnt, PRRTE_BYTE_OBJECT);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    PMIX_INFO_CREATE(info, ninfo);
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    for (n=0; n < ninfo; n++) {
        cnt = 1;
        ret = PMIx_Data_unpack(&proc, &pbkt, (void*)&info[n], &cnt, PMIX_INFO);
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
    PMIX_INFO_LOAD(&directives[1], "prrte.log.noloop", NULL, PMIX_BOOL);

    /* pass the array down to be logged */
    PMIx_Log(info, ninfo, directives, 2);
    PMIX_INFO_FREE(info, ninfo+1);
    PMIX_INFO_DESTRUCT(&directives[1]);
}


/****    INSTANTIATE LOCAL OBJECTS    ****/
static void opcon(prrte_pmix_server_op_caddy_t *p)
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
PRRTE_CLASS_INSTANCE(prrte_pmix_server_op_caddy_t,
                   prrte_object_t,
                   opcon, NULL);

static void rqcon(pmix_server_req_t *p)
{
    p->operation = NULL;
    p->cmdline = NULL;
    p->flag = true;
    p->launcher = false;
    p->uid = 0;
    p->gid = 0;
    p->pid = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->range = PMIX_RANGE_SESSION;
    p->proxy = *PRRTE_NAME_INVALID;
    p->target = *PRRTE_NAME_INVALID;
    p->jdata = NULL;
    PRRTE_CONSTRUCT(&p->msg, prrte_buffer_t);
    p->timeout = prrte_pmix_server_globals.timeout;
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
    if (NULL != p->jdata) {
        PRRTE_RELEASE(p->jdata);
    }
    PRRTE_DESTRUCT(&p->msg);
}
PRRTE_CLASS_INSTANCE(pmix_server_req_t,
                   prrte_object_t,
                   rqcon, rqdes);

static void mdcon(prrte_pmix_mdx_caddy_t *p)
{
    p->sig = NULL;
    p->buf = NULL;
    p->cbfunc = NULL;
    p->mode = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->cbdata = NULL;
}
static void mddes(prrte_pmix_mdx_caddy_t *p)
{
    if (NULL != p->sig) {
        PRRTE_RELEASE(p->sig);
    }
    if (NULL != p->buf) {
        PRRTE_RELEASE(p->buf);
    }
}
PRRTE_CLASS_INSTANCE(prrte_pmix_mdx_caddy_t,
                   prrte_object_t,
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
PRRTE_CLASS_INSTANCE(pmix_server_pset_t,
                   prrte_list_item_t,
                   pscon, psdes);
