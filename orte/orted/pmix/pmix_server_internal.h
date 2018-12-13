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
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2018 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_SERVER_INTERNAL_H_
#define _PMIX_SERVER_INTERNAL_H_

#include "orte_config.h"
#include "orte/types.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <pmix_server.h>

#include "opal/types.h"
#include "opal/class/opal_hotel.h"
#include "opal/mca/base/base.h"
#include "opal/event/event-internal.h"
#include "opal/pmix/pmix-internal.h"
#include "opal/util/printf.h"
#include "opal/util/proc.h"
#include "opal/sys/atomic.h"

#include "orte/mca/grpcomm/base/base.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/threads.h"

BEGIN_C_DECLS

#define ORTED_PMIX_MIN_DMX_TIMEOUT      10
#define ORTE_ADJUST_TIMEOUT(a)                                      \
    do {                                                            \
        (a)->timeout = (2 * orte_process_info.num_daemons) / 1000;  \
        if ((a)->timeout < ORTED_PMIX_MIN_DMX_TIMEOUT) {            \
            (a)->timeout = ORTED_PMIX_MIN_DMX_TIMEOUT;              \
        }                                                           \
    } while(0)

/* object for tracking requests so we can
 * correctly route the eventual reply */
 typedef struct {
    opal_object_t super;
    opal_event_t ev;
    char *operation;
    int status;
    int timeout;
    int room_num;
    int remote_room_num;
    bool flag;
    pid_t pid;
    pmix_info_t *info;
    size_t ninfo;
    pmix_data_range_t range;
    orte_process_name_t proxy;
    orte_process_name_t target;
    orte_job_t *jdata;
    opal_buffer_t msg;
    pmix_op_cbfunc_t opcbfunc;
    pmix_modex_cbfunc_t mdxcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    pmix_lookup_cbfunc_t lkcbfunc;
    pmix_release_cbfunc_t rlcbfunc;
    pmix_tool_connection_cbfunc_t toolcbfunc;
    void *cbdata;
} pmix_server_req_t;
OBJ_CLASS_DECLARATION(pmix_server_req_t);

/* object for thread-shifting server operations */
typedef struct {
    opal_object_t super;
    opal_event_t ev;
    int status;
    pmix_status_t *codes;
    size_t ncodes;
    opal_process_name_t proc;
    const char *msg;
    void *server_object;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_proc_t *eprocs;
    size_t neprocs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *directives;
    size_t ndirs;
    pmix_app_t *apps;
    size_t napps;
    pmix_query_t *queries;
    size_t nqueries;
    pmix_op_cbfunc_t cbfunc;
    pmix_info_cbfunc_t infocbfunc;
    pmix_tool_connection_cbfunc_t toolcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    void *cbdata;
} orte_pmix_server_op_caddy_t;
OBJ_CLASS_DECLARATION(orte_pmix_server_op_caddy_t);

typedef struct {
    opal_object_t super;
    orte_grpcomm_signature_t *sig;
    opal_buffer_t *buf;
    pmix_modex_cbfunc_t cbfunc;
    pmix_info_cbfunc_t infocbfunc;
    void *cbdata;
} orte_pmix_mdx_caddy_t;
OBJ_CLASS_DECLARATION(orte_pmix_mdx_caddy_t);

#define ORTE_DMX_REQ(p, cf, ocf, ocd)                       \
    do {                                                     \
        pmix_server_req_t *_req;                             \
        _req = OBJ_NEW(pmix_server_req_t);                   \
        opal_asprintf(&_req->operation, "DMDX: %s:%d", __FILE__, __LINE__); \
        _req->target = (p);                                  \
        _req->mdxcbfunc = (ocf);                             \
        _req->cbdata = (ocd);                                \
        opal_event_set(orte_event_base, &(_req->ev),         \
                       -1, OPAL_EV_WRITE, (cf), _req);       \
        opal_event_set_priority(&(_req->ev), ORTE_MSG_PRI);  \
        ORTE_POST_OBJECT(_req);                              \
        opal_event_active(&(_req->ev), OPAL_EV_WRITE, 1);    \
    } while(0);

#define ORTE_SPN_REQ(j, cf, ocf, ocd)                       \
    do {                                                     \
        pmix_server_req_t *_req;                             \
        _req = OBJ_NEW(pmix_server_req_t);                   \
        opal_asprintf(&_req->operation, "SPAWN: %s:%d", __FILE__, __LINE__); \
        _req->jdata = (j);                                   \
        _req->spcbfunc = (ocf);                              \
        _req->cbdata = (ocd);                                \
        opal_event_set(orte_event_base, &(_req->ev),         \
                       -1, OPAL_EV_WRITE, (cf), _req);       \
        opal_event_set_priority(&(_req->ev), ORTE_MSG_PRI);  \
        ORTE_POST_OBJECT(_req);                              \
        opal_event_active(&(_req->ev), OPAL_EV_WRITE, 1);    \
    } while(0);

#define ORTE_PMIX_OPERATION(p, np, i, ni, fn, cf, cb)           \
    do {                                                        \
        orte_pmix_server_op_caddy_t *_cd;                       \
        _cd = OBJ_NEW(orte_pmix_server_op_caddy_t);             \
        _cd->procs = (pmix_proc_t*)(p);                         \
        _cd->nprocs = (np);                                     \
        _cd->info = (pmix_info_t*)(i);                          \
        _cd->ninfo = (ni);                                      \
        _cd->cbfunc = (cf);                                     \
        _cd->cbdata = (cb);                                     \
        opal_event_set(orte_event_base, &(_cd->ev), -1,         \
                       OPAL_EV_WRITE, (fn), _cd);               \
        opal_event_set_priority(&(_cd->ev), ORTE_MSG_PRI);      \
        ORTE_POST_OBJECT(_cd);                                  \
        opal_event_active(&(_cd->ev), OPAL_EV_WRITE, 1);        \
    } while(0);

#define ORTE_PMIX_THREADSHIFT(p, s, st, m, pl, pn, fn, cf, cb)  \
    do {                                                        \
        orte_pmix_server_op_caddy_t *_cd;                       \
        _cd = OBJ_NEW(orte_pmix_server_op_caddy_t);             \
        _cd->proc.jobid = (p)->jobid;                           \
        _cd->proc.vpid = (p)->vpid;                             \
        _cd->server_object = (s);                               \
        _cd->status = (st);                                     \
        _cd->msg = (m);                                         \
        _cd->procs = (pl);                                      \
        _cd->nprocs = (pn);                                     \
        _cd->cbfunc = (cf);                                     \
        _cd->cbdata = (cb);                                     \
        opal_event_set(orte_event_base, &(_cd->ev), -1,         \
                       OPAL_EV_WRITE, (fn), _cd);               \
        opal_event_set_priority(&(_cd->ev), ORTE_MSG_PRI);      \
        ORTE_POST_OBJECT(_cd);                                  \
        opal_event_active(&(_cd->ev), OPAL_EV_WRITE, 1);        \
    } while(0);

/* define the server module functions */
extern pmix_status_t pmix_server_client_connected_fn(const pmix_proc_t *proc, void* server_object,
                                                     pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc, void* server_object,
                                                     pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object,
                                          int status, const char msg[],
                                          pmix_proc_t procs[], size_t nprocs,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_fencenb_fn(const pmix_proc_t procs[], size_t nprocs,
                                            const pmix_info_t info[], size_t ninfo,
                                            char *data, size_t ndata,
                                            pmix_modex_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_dmodex_req_fn(const pmix_proc_t *proc,
                                               const pmix_info_t info[], size_t ninfo,
                                               pmix_modex_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_publish_fn(const pmix_proc_t *proc,
                                            const pmix_info_t info[], size_t ninfo,
                                            pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_lookup_fn(const pmix_proc_t *proc, char **keys,
                                           const pmix_info_t info[], size_t ninfo,
                                           pmix_lookup_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_unpublish_fn(const pmix_proc_t *proc, char **keys,
                                              const pmix_info_t info[], size_t ninfo,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_spawn_fn(const pmix_proc_t *proc,
                                          const pmix_info_t job_info[], size_t ninfo,
                                          const pmix_app_t apps[], size_t napps,
                                          pmix_spawn_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                            const pmix_info_t info[], size_t ninfo,
                                            pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                               const pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                                    const pmix_info_t info[], size_t ninfo,
                                                    pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_notify_event(pmix_status_t code,
                                              const pmix_proc_t *source,
                                              pmix_data_range_t range,
                                              pmix_info_t info[], size_t ninfo,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata);
extern pmix_status_t pmix_server_query_fn(pmix_proc_t *proct,
                                          pmix_query_t *queries, size_t nqueries,
                                          pmix_info_cbfunc_t cbfunc,
                                          void *cbdata);
extern void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                                   pmix_tool_connection_cbfunc_t cbfunc,
                                   void *cbdata);

extern void pmix_server_log_fn(const pmix_proc_t *client,
                               const pmix_info_t data[], size_t ndata,
                               const pmix_info_t directives[], size_t ndirs,
                               pmix_op_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                          pmix_alloc_directive_t directive,
                                          const pmix_info_t data[], size_t ndata,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t pmix_server_job_ctrl_fn(const pmix_proc_t *requestor,
                                             const pmix_proc_t targets[], size_t ntargets,
                                             const pmix_info_t directives[], size_t ndirs,
                                             pmix_info_cbfunc_t cbfunc, void *cbdata);

#if OPAL_PMIX_VERSION >= 4
extern pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                          const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata);
#endif

void orte_pmix_server_tool_conn_complete(orte_job_t *jdata,
                                         char *hostname,
                                         orte_vpid_t vpid, pid_t pid);

/* declare the RML recv functions for responses */
extern void pmix_server_launch_resp(int status, orte_process_name_t* sender,
                                    opal_buffer_t *buffer,
                                    orte_rml_tag_t tg, void *cbdata);

extern void pmix_server_keyval_client(int status, orte_process_name_t* sender,
                                      opal_buffer_t *buffer,
                                      orte_rml_tag_t tg, void *cbdata);

extern void pmix_server_notify(int status, orte_process_name_t* sender,
                               opal_buffer_t *buffer,
                               orte_rml_tag_t tg, void *cbdata);

/* exposed shared variables */
typedef struct {
  opal_list_item_t super;
  char *name;
  pmix_proc_t *members;
  size_t num_members;
} pmix_server_pset_t;
OBJ_CLASS_DECLARATION(pmix_server_pset_t);

typedef struct {
    bool initialized;
    int verbosity;
    int output;
    opal_hotel_t reqs;
    int num_rooms;
    int timeout;
    bool wait_for_server;
    orte_process_name_t server;
    opal_list_t notifications;
    bool pubsub_init;
    bool session_server;
    bool system_server;
    bool legacy;
    opal_list_t psets;
} pmix_server_globals_t;

extern pmix_server_globals_t orte_pmix_server_globals;

END_C_DECLS

#endif /* PMIX_SERVER_INTERNAL_H_ */
