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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_SERVER_INTERNAL_H_
#define _PMIX_SERVER_INTERNAL_H_

#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <pmix_server.h>

#include "types.h"
#include "src/class/prrte_hotel.h"
#include "src/mca/base/base.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/printf.h"
#include "src/util/proc_info.h"
#include "src/sys/atomic.h"

#include "src/mca/grpcomm/base/base.h"
#include "src/runtime/prrte_globals.h"
#include "src/threads/threads.h"

BEGIN_C_DECLS

#define PRRTED_PMIX_MIN_DMX_TIMEOUT      120
#define PRRTE_ADJUST_TIMEOUT(a)                                         \
    do {                                                                \
        size_t _n;                                                      \
        bool _set = false;                                              \
        if (NULL != (a)->info) {                                        \
            for (_n=0; _n < (a)->ninfo; _n++) {                         \
                if (PMIX_CHECK_KEY(&(a)->info[_n], PMIX_TIMEOUT)) {     \
                    (a)->timeout = (a)->info[_n].value.data.integer;    \
                    _set = true;                                        \
                }                                                       \
            }                                                           \
        }                                                               \
        if (!_set) {                                                    \
            (a)->timeout = (2 * prrte_process_info.num_daemons) / 10;   \
            if ((a)->timeout < PRRTED_PMIX_MIN_DMX_TIMEOUT) {           \
                (a)->timeout = PRRTED_PMIX_MIN_DMX_TIMEOUT;             \
            }                                                           \
        }                                                               \
    } while(0)

/* object for tracking requests so we can
 * correctly route the eventual reply */
 typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    char *operation;
    char *cmdline;
    char *key;
    int status;
    pmix_status_t pstatus;
    int timeout;
    int room_num;
    int remote_room_num;
    bool flag;
    bool launcher;
    uid_t uid;
    gid_t gid;
    pid_t pid;
    pmix_info_t *info;
    size_t ninfo;
    char *data;
    size_t sz;
    pmix_data_range_t range;
    prrte_process_name_t proxy;
    prrte_process_name_t target;
    pmix_proc_t tproc;
    prrte_job_t *jdata;
    prrte_buffer_t msg;
    pmix_op_cbfunc_t opcbfunc;
    pmix_modex_cbfunc_t mdxcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    pmix_lookup_cbfunc_t lkcbfunc;
    pmix_release_cbfunc_t rlcbfunc;
    pmix_tool_connection_cbfunc_t toolcbfunc;
    void *cbdata;
} pmix_server_req_t;
PRRTE_CLASS_DECLARATION(pmix_server_req_t);

/* object for thread-shifting server operations */
typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    int status;
    pmix_status_t *codes;
    size_t ncodes;
    prrte_process_name_t proc;
    const char *msg;
    void *server_object;
    pmix_proc_t proct;
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
} prrte_pmix_server_op_caddy_t;
PRRTE_CLASS_DECLARATION(prrte_pmix_server_op_caddy_t);

typedef struct {
    prrte_object_t super;
    prrte_grpcomm_signature_t *sig;
    prrte_buffer_t *buf;
    pmix_modex_cbfunc_t cbfunc;
    pmix_info_cbfunc_t infocbfunc;
    int mode;
    pmix_info_t *info;
    size_t ninfo;
    void *cbdata;
} prrte_pmix_mdx_caddy_t;
PRRTE_CLASS_DECLARATION(prrte_pmix_mdx_caddy_t);

#define PRRTE_IO_OP(t, nt, b, fn, cfn, cbd)                     \
    do {                                                        \
        prrte_pmix_server_op_caddy_t *_cd;                      \
        _cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);          \
        _cd->procs = (t);                                       \
        _cd->nprocs = (nt);                                     \
        _cd->server_object = (void*)(b);                        \
        _cd->cbfunc = (cfn);                                    \
        _cd->cbdata = (cbd);                                    \
        prrte_event_set(prrte_event_base, &(_cd->ev), -1,       \
                        PRRTE_EV_WRITE, (fn), _cd);             \
        prrte_event_set_priority(&(_cd->ev), PRRTE_MSG_PRI);    \
        PRRTE_POST_OBJECT(_cd);                                 \
        prrte_event_active(&(_cd->ev), PRRTE_EV_WRITE, 1);      \
    } while(0);

#define PRRTE_DMX_REQ(p, i, ni, cf, ocf, ocd)                 \
    do {                                                      \
        pmix_server_req_t *_req;                              \
        _req = PRRTE_NEW(pmix_server_req_t);                  \
        prrte_asprintf(&_req->operation, "DMDX: %s:%d", __FILE__, __LINE__); \
        memcpy(&_req->tproc, (p), sizeof(pmix_proc_t));       \
        _req->info = (pmix_info_t*)(i);                       \
        _req->ninfo = (ni);                                   \
        _req->mdxcbfunc = (ocf);                              \
        _req->cbdata = (ocd);                                 \
        prrte_event_set(prrte_event_base, &(_req->ev),        \
                       -1, PRRTE_EV_WRITE, (cf), _req);       \
        prrte_event_set_priority(&(_req->ev), PRRTE_MSG_PRI);  \
        PRRTE_POST_OBJECT(_req);                              \
        prrte_event_active(&(_req->ev), PRRTE_EV_WRITE, 1);   \
    } while(0);

#define PRRTE_SPN_REQ(j, cf, ocf, ocd)                       \
    do {                                                     \
        pmix_server_req_t *_req;                             \
        _req = PRRTE_NEW(pmix_server_req_t);                   \
        prrte_asprintf(&_req->operation, "SPAWN: %s:%d", __FILE__, __LINE__); \
        _req->jdata = (j);                                   \
        _req->spcbfunc = (ocf);                              \
        _req->cbdata = (ocd);                                \
        prrte_event_set(prrte_event_base, &(_req->ev),         \
                       -1, PRRTE_EV_WRITE, (cf), _req);       \
        prrte_event_set_priority(&(_req->ev), PRRTE_MSG_PRI);  \
        PRRTE_POST_OBJECT(_req);                              \
        prrte_event_active(&(_req->ev), PRRTE_EV_WRITE, 1);    \
    } while(0);

#define PRRTE_PMIX_OPERATION(p, np, i, ni, fn, cf, cb)           \
    do {                                                        \
        prrte_pmix_server_op_caddy_t *_cd;                       \
        _cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);             \
        _cd->procs = (pmix_proc_t*)(p);                         \
        _cd->nprocs = (np);                                     \
        _cd->info = (pmix_info_t*)(i);                          \
        _cd->ninfo = (ni);                                      \
        _cd->cbfunc = (cf);                                     \
        _cd->cbdata = (cb);                                     \
        prrte_event_set(prrte_event_base, &(_cd->ev), -1,         \
                       PRRTE_EV_WRITE, (fn), _cd);               \
        prrte_event_set_priority(&(_cd->ev), PRRTE_MSG_PRI);      \
        PRRTE_POST_OBJECT(_cd);                                  \
        prrte_event_active(&(_cd->ev), PRRTE_EV_WRITE, 1);        \
    } while(0);

#define PRRTE_PMIX_THREADSHIFT(p, s, st, m, pl, pn, fn, cf, cb)  \
    do {                                                        \
        prrte_pmix_server_op_caddy_t *_cd;                       \
        _cd = PRRTE_NEW(prrte_pmix_server_op_caddy_t);             \
        _cd->proc.jobid = (p)->jobid;                           \
        _cd->proc.vpid = (p)->vpid;                             \
        _cd->server_object = (s);                               \
        _cd->status = (st);                                     \
        _cd->msg = (m);                                         \
        _cd->procs = (pl);                                      \
        _cd->nprocs = (pn);                                     \
        _cd->cbfunc = (cf);                                     \
        _cd->cbdata = (cb);                                     \
        prrte_event_set(prrte_event_base, &(_cd->ev), -1,         \
                       PRRTE_EV_WRITE, (fn), _cd);               \
        prrte_event_set_priority(&(_cd->ev), PRRTE_MSG_PRI);      \
        PRRTE_POST_OBJECT(_cd);                                  \
        prrte_event_active(&(_cd->ev), PRRTE_EV_WRITE, 1);        \
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

extern pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                             const pmix_info_t directives[], size_t ndirs,
                                             pmix_iof_channel_t channels,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source,
                                          const pmix_proc_t targets[], size_t ntargets,
                                          const pmix_info_t directives[], size_t ndirs,
                                          const pmix_byte_object_t *bo,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata);

#if PMIX_NUMERIC_VERSION >= 0x00040000
extern pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                          const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata);
#endif

void prrte_pmix_server_tool_conn_complete(prrte_job_t *jdata,
                                         pmix_server_req_t *req);

/* declare the RML recv functions for responses */
extern void pmix_server_launch_resp(int status, prrte_process_name_t* sender,
                                    prrte_buffer_t *buffer,
                                    prrte_rml_tag_t tg, void *cbdata);

extern void pmix_server_keyval_client(int status, prrte_process_name_t* sender,
                                      prrte_buffer_t *buffer,
                                      prrte_rml_tag_t tg, void *cbdata);

extern void pmix_server_notify(int status, prrte_process_name_t* sender,
                               prrte_buffer_t *buffer,
                               prrte_rml_tag_t tg, void *cbdata);

/* exposed shared variables */
typedef struct {
  prrte_list_item_t super;
  char *name;
  pmix_proc_t *members;
  size_t num_members;
} pmix_server_pset_t;
PRRTE_CLASS_DECLARATION(pmix_server_pset_t);

typedef struct {
    bool initialized;
    int verbosity;
    int output;
    prrte_hotel_t reqs;
    int num_rooms;
    int timeout;
    bool wait_for_server;
    prrte_process_name_t server;
    prrte_list_t notifications;
    bool pubsub_init;
    bool session_server;
    bool system_server;
    bool legacy;
    prrte_list_t psets;
} pmix_server_globals_t;

extern pmix_server_globals_t prrte_pmix_server_globals;

END_C_DECLS

#endif /* PMIX_SERVER_INTERNAL_H_ */
