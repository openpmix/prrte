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
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_SERVER_INTERNAL_H_
#define _PMIX_SERVER_INTERNAL_H_

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#include <pmix_server.h>

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_hotel.h"
#include "src/event/event-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/pmix/pmix-internal.h"
#include "src/include/pmix_atomic.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "types.h"

#include "src/rml/rml_types.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"

BEGIN_C_DECLS

/* object for tracking requests so we can
 * correctly route the eventual reply */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    bool event_active;
    prte_event_t cycle;
    bool cycle_active;
    bool inprogress;
    bool timed_out;
    char *operation;
    char *cmdline;
    char *key;
    int status;
    pmix_status_t pstatus;
    int timeout;
    int local_index;
    int remote_index;
    bool flag;
    bool launcher;
    bool scheduler;
    bool copy;  // info array has been copied and must be released
    /* this request cleared prte_dvm_ready and parked the requesting job, so
     * whoever fails it owes both back - see prte_ras_base_modify() */
    bool dvm_held;
    bool moncopy;  // monitor was allocated and must be released
    bool dircopy;   // directives array has been copied and must be released
    uid_t uid;
    gid_t gid;
    pid_t pid;
    pmix_alloc_directive_t allocdir;
    uint32_t sessionID;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *monitor;
    pmix_info_t *directives;
    size_t ndirs;
    char *data;
    size_t sz;
    uint32_t ndaemons;
    uint32_t nreported;
    uint32_t nsuccess;
    /* Which daemons a fan-out-and-count operation is waiting on, and which
     * of them have been accounted for.  Both are indexed by daemon vpid and
     * are used only by the monitor collective; every other request leaves
     * them as the constructor makes them, which costs nothing - a bitmap is
     * usable empty and set_bit grows it from nothing.
     *
     * Two sets rather than a counter because a daemon failure has to be
     * accounted exactly once, and a bare count cannot say whether the daemon
     * that just died had already reported (in which case there is nothing to
     * adjust) or had not (in which case it never will).  Recording the
     * identity also makes the accounting idempotent, which it has to be: the
     * routing tree's fault handler fires twice for every death, once at
     * LOCAL scope and again at GLOBAL. */
    pmix_bitmap_t expected_dmns;
    pmix_bitmap_t reported_dmns;
    pmix_data_range_t range;
    pmix_proc_t proxy;
    pmix_proc_t target;
    pmix_proc_t tproc;
    /* a copy of an operation's participant list, owned by this request */
    pmix_proc_t *procs;
    size_t nprocs;
    prte_job_t *jdata;
    pmix_data_buffer_t msg;
    pmix_op_cbfunc_t opcbfunc;
    pmix_modex_cbfunc_t mdxcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    pmix_lookup_cbfunc_t lkcbfunc;
    pmix_release_cbfunc_t rlcbfunc;
    pmix_tool_connection_cbfunc_t toolcbfunc;
    pmix_info_cbfunc_t infocbfunc;
    void *cbdata;
    void *rlcbdata;
    /* A PMIx query relayed to the DVM master because this daemon does not
     * hold the state it asks for - see pmix_server_queries.c.  On the asking
     * daemon these carry the client's caddy, the results already gathered
     * locally (a PMIx info list the master's reply is merged into), and the
     * number of keys the client asked for, which is what partial success is
     * measured against.  On the master, qcaddy holds the caddy whose unpacked
     * query array must be freed once the answer has been sent. */
    void *qcaddy;
    void *qresults;
    size_t nkeys;
} prte_pmix_server_req_t;
PMIX_CLASS_DECLARATION(prte_pmix_server_req_t);

/* Answering for a proc out of the job object rather than out of the local
 * PMIx server's store.  prte_pmix_server_derivable_key() is the closed set
 * of keys this DVM is itself the authority for - the placement and binding
 * it computed when it mapped the job - and
 * prte_pmix_server_derive_proc_data() builds the blob a direct modex would
 * have returned for them.  Both live in pmix_server_fence.c; the dmodex
 * receiver in pmix_server.c uses them too, so that a request that does
 * reach the hosting daemon is answered from what PRRTE knows rather than
 * from what the process has published.  See src/prted/pmix/AGENTS.md. */
PRTE_EXPORT bool prte_pmix_server_derivable_key(const char *key);
PRTE_EXPORT pmix_status_t prte_pmix_server_derive_proc_data(prte_job_t *jdata,
                                                            prte_proc_t *proct,
                                                            pmix_data_buffer_t *buf);

/* object for thread-shifting server operations */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_status_t status;
    pmix_status_t *codes;
    size_t ncodes;
    pmix_proc_t proc;
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
    pmix_iof_channel_t channels;
    pmix_data_range_t range;
    bool flag;
    pmix_op_cbfunc_t cbfunc;
    pmix_info_cbfunc_t infocbfunc;
    pmix_tool_connection_cbfunc_t toolcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    pmix_event_notification_cbfunc_fn_t evcbfunc;
    void *cbdata;
} prte_pmix_server_op_caddy_t;
PMIX_CLASS_DECLARATION(prte_pmix_server_op_caddy_t);

#define PRTE_IO_OP(t, nt, b, fn, cfn, cbd)                                         \
    do {                                                                           \
        prte_pmix_server_op_caddy_t *_cd;                                          \
        _cd = PMIX_NEW(prte_pmix_server_op_caddy_t);                               \
        _cd->procs = (pmix_proc_t *) (t);                                          \
        _cd->nprocs = (nt);                                                        \
        _cd->server_object = (void *) (b);                                         \
        _cd->cbfunc = (cfn);                                                       \
        _cd->cbdata = (cbd);                                                       \
        prte_event_set(prte_event_base, &(_cd->ev), -1, PRTE_EV_WRITE, (fn), _cd); \
        PMIX_POST_OBJECT(_cd);                                                     \
        prte_event_active(&(_cd->ev), PRTE_EV_WRITE, 1);                           \
    } while (0);

#define PRTE_DMX_REQ(p, i, ni, cf, ocf, ocd)                                         \
    do {                                                                             \
        prte_pmix_server_req_t *_req;                                                \
        _req = PMIX_NEW(prte_pmix_server_req_t);                                     \
        pmix_asprintf(&_req->operation, "DMDX: %s:%d", __FILE__, __LINE__);          \
        memcpy(&_req->tproc, (p), sizeof(pmix_proc_t));                              \
        _req->info = (pmix_info_t *) (i);                                            \
        _req->ninfo = (ni);                                                          \
        _req->mdxcbfunc = (ocf);                                                     \
        _req->cbdata = (ocd);                                                        \
        prte_event_set(prte_event_base, &(_req->ev), -1, PRTE_EV_WRITE, (cf), _req); \
        PMIX_POST_OBJECT(_req);                                                      \
        prte_event_active(&(_req->ev), PRTE_EV_WRITE, 1);                            \
    } while (0);

#define PRTE_SPN_REQ(j, cf, ocf, ocd)                                                \
    do {                                                                             \
        prte_pmix_server_req_t *_req;                                                \
        _req = PMIX_NEW(prte_pmix_server_req_t);                                     \
        pmix_asprintf(&_req->operation, "SPAWN: %s:%d", __FILE__, __LINE__);         \
        _req->jdata = (j);                                                           \
        _req->spcbfunc = (ocf);                                                      \
        _req->cbdata = (ocd);                                                        \
        prte_event_set(prte_event_base, &(_req->ev), -1, PRTE_EV_WRITE, (cf), _req); \
        PMIX_POST_OBJECT(_req);                                                      \
        prte_event_active(&(_req->ev), PRTE_EV_WRITE, 1);                            \
    } while (0);

#define PRTE_PMIX_OPERATION(p, np, i, ni, fn, cf, cb)                              \
    do {                                                                           \
        prte_pmix_server_op_caddy_t *_cd;                                          \
        _cd = PMIX_NEW(prte_pmix_server_op_caddy_t);                               \
        _cd->procs = (pmix_proc_t *) (p);                                          \
        _cd->nprocs = (np);                                                        \
        _cd->info = (pmix_info_t *) (i);                                           \
        _cd->ninfo = (ni);                                                         \
        _cd->cbfunc = (cf);                                                        \
        _cd->cbdata = (cb);                                                        \
        prte_event_set(prte_event_base, &(_cd->ev), -1, PRTE_EV_WRITE, (fn), _cd); \
        PMIX_POST_OBJECT(_cd);                                                     \
        prte_event_active(&(_cd->ev), PRTE_EV_WRITE, 1);                           \
    } while (0);

#define PRTE_SERVER_PMIX_THREADSHIFT(p, s, st, m, pl, pn, fn, cf, cb)              \
    do {                                                                           \
        prte_pmix_server_op_caddy_t *_cd;                                          \
        _cd = PMIX_NEW(prte_pmix_server_op_caddy_t);                               \
        PMIX_LOAD_PROCID(&_cd->proc, (p)->nspace, (p)->rank);                      \
        _cd->server_object = (s);                                                  \
        _cd->status = (st);                                                        \
        _cd->msg = (m);                                                            \
        _cd->procs = (pl);                                                         \
        _cd->nprocs = (pn);                                                        \
        _cd->cbfunc = (cf);                                                        \
        _cd->cbdata = (cb);                                                        \
        prte_event_set(prte_event_base, &(_cd->ev), -1, PRTE_EV_WRITE, (fn), _cd); \
        PMIX_POST_OBJECT(_cd);                                                     \
        prte_event_active(&(_cd->ev), PRTE_EV_WRITE, 1);                           \
    } while (0);

/* Release callback to hand to the PMIx server when relaying the
 * results of a request tracked in the local request array. The PMIx
 * library invokes it on the PMIx progress thread once it is done
 * with the results, so it thread-shifts before clearing the request
 * from prte_pmix_server_globals.local_reqs and releasing it */
PRTE_EXPORT void prte_pmix_server_req_release(void *cbdata);

/* define the server module functions */
PRTE_EXPORT extern pmix_status_t pmix_server_client_connected2_fn(const pmix_proc_t *proc,
                                                                  void *server_object,
                                                                  pmix_info_t *info, size_t ninfo,
                                                                  pmix_op_cbfunc_t cbfunc,
                                                                  void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_client_finalized_fn(const pmix_proc_t *proc,
                                                                 void *server_object,
                                                                 pmix_op_cbfunc_t cbfunc,
                                                                 void *cbdata);
/* Retire a tool that has left us - locally if we are the DVM master, else by
 * telling the master, which is the only place a tool's job object lives. */
PRTE_EXPORT void prte_pmix_server_tool_departed(pmix_proc_t *tool);

PRTE_EXPORT extern pmix_status_t pmix_server_abort_fn(const pmix_proc_t *proc, void *server_object,
                                                      int status, const char msg[],
                                                      pmix_proc_t procs[], size_t nprocs,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_fencenb_fn(const pmix_proc_t procs[], size_t nprocs,
                                                        const pmix_info_t info[], size_t ninfo,
                                                        char *data, size_t ndata,
                                                        pmix_modex_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_dmodex_req_fn(const pmix_proc_t *proc,
                                                           const pmix_info_t info[], size_t ninfo,
                                                           pmix_modex_cbfunc_t cbfunc,
                                                           void *cbdata);
/* Attach to the data server named by prte_data_server_uri, if that has not
 * already happened.  Idempotent.  The master needs this even when it has no
 * publishing client of its own: it is the only daemon holding the tool
 * connection every other daemon's request is relayed over. */
PRTE_EXPORT int prte_pmix_server_init_pubsub(void);

PRTE_EXPORT extern pmix_status_t pmix_server_publish_fn(const pmix_proc_t *proc,
                                                        const pmix_info_t info[], size_t ninfo,
                                                        pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_lookup_fn(const pmix_proc_t *proc, char **keys,
                                                       const pmix_info_t info[], size_t ninfo,
                                                       pmix_lookup_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_unpublish_fn(const pmix_proc_t *proc, char **keys,
                                                          const pmix_info_t info[], size_t ninfo,
                                                          pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_spawn_fn(const pmix_proc_t *proc,
                                                      const pmix_info_t job_info[], size_t ninfo,
                                                      const pmix_app_t apps[], size_t napps,
                                                      pmix_spawn_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_connect_fn(const pmix_proc_t procs[], size_t nprocs,
                                                        const pmix_info_t info[], size_t ninfo,
                                                        pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_disconnect_fn(const pmix_proc_t procs[], size_t nprocs,
                                                           const pmix_info_t info[], size_t ninfo,
                                                           pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t
pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes, const pmix_info_t info[],
                               size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes,
                                                                  size_t ncodes,
                                                                  pmix_op_cbfunc_t cbfunc,
                                                                  void *cbdata);
PRTE_EXPORT extern pmix_status_t
pmix_server_notify_event(pmix_status_t code, const pmix_proc_t *source, pmix_data_range_t range,
                         pmix_info_t info[], size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata);
PRTE_EXPORT extern pmix_status_t pmix_server_query_fn(pmix_proc_t *proct, pmix_query_t *queries,
                                                      size_t nqueries, pmix_info_cbfunc_t cbfunc,
                                                      void *cbdata);
PRTE_EXPORT extern void pmix_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                                               pmix_tool_connection_cbfunc_t cbfunc, void *cbdata);

#if PRTE_PMIX_SERVER2_UPCALLS
PRTE_EXPORT extern pmix_status_t pmix_tool_connected2_fn(pmix_info_t *info, size_t ninfo,
                                                         pmix_tool_connection_cbfunc_t cbfunc, void *cbdata);
#endif

PRTE_EXPORT extern void pmix_server_log_fn(const pmix_proc_t *client, const pmix_info_t data[],
                                           size_t ndata, const pmix_info_t directives[],
                                           size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata);

#if PRTE_PMIX_SERVER2_UPCALLS
PRTE_EXPORT extern pmix_status_t pmix_server_log2_fn(const pmix_proc_t *client, const pmix_info_t data[],
                                                     size_t ndata, const pmix_info_t directives[],
                                                     size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata);
#endif

PRTE_EXPORT extern void pmix_server_logging_resp(int status, pmix_proc_t *sender,
                                                 pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                 void *cbdata);

PRTE_EXPORT extern pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                                      pmix_alloc_directive_t directive,
                                                      const pmix_info_t data[], size_t ndata,
                                                      pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern pmix_status_t
pmix_server_job_ctrl_fn(const pmix_proc_t *requestor, const pmix_proc_t targets[], size_t ntargets,
                        const pmix_info_t directives[], size_t ndirs, pmix_info_cbfunc_t cbfunc,
                        void *cbdata);

PRTE_EXPORT extern pmix_status_t
pmix_server_monitor_fn(const pmix_proc_t *requestor,
                       const pmix_info_t *monitor, pmix_status_t error,
                       const pmix_info_t directives[], size_t ndirs,
                       pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern pmix_status_t pmix_server_iof_pull_fn(const pmix_proc_t procs[], size_t nprocs,
                                                         const pmix_info_t directives[],
                                                         size_t ndirs, pmix_iof_channel_t channels,
                                                         pmix_op_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern pmix_status_t pmix_server_stdin_fn(const pmix_proc_t *source,
                                                      const pmix_proc_t targets[], size_t ntargets,
                                                      const pmix_info_t directives[], size_t ndirs,
                                                      const pmix_byte_object_t *bo,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Account a daemon departure against every outstanding monitor collective.
 * Called from the routing tree's fault handler, on the PRRTE progress thread,
 * once at LOCAL scope and again at GLOBAL for the same death. */
PRTE_EXPORT void prte_pmix_server_fault_handler(const prte_rml_recovery_status_t *status);

PRTE_EXPORT extern pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *gpid,
                                                      const pmix_proc_t procs[], size_t nprocs,
                                                      const pmix_info_t directives[], size_t ndirs,
                                                      pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern pmix_status_t pmix_server_monitor_fn(const pmix_proc_t *requestor,
                                                        const pmix_info_t *monitor, pmix_status_t error,
                                                        const pmix_info_t directives[], size_t ndirs,
                                                        pmix_info_cbfunc_t cbfunc, void *cbdata);

/* Hand a caller-constructed job object to the PLM for launch, exactly as a
 * PMIx_Spawn would once its directives have been translated. Must be called
 * on the PRRTE progress thread; cbfunc reports the launched namespace. */
PRTE_EXPORT extern void prte_pmix_server_launch_job(prte_job_t *jdata,
                                                    pmix_spawn_cbfunc_t cbfunc,
                                                    void *cbdata);

/* Report to the scheduler that a session it instantiated has completed - all
 * of its jobs have terminated and its resources have been recovered. Called
 * from the DVM state machine as each job retires. A no-op for a session the
 * scheduler did not instantiate. */
PRTE_EXPORT extern void prte_pmix_server_session_complete(prte_session_t *session);

/* Notice from the DVM state machine that a job in the given session has
 * terminated. Records the job's termination status for the completion report
 * and, when the session was defined to run that job (or is being torn down)
 * and nothing is left running in it, reclaims the session. Must be called
 * AFTER the job has been removed from session->jobs, and only on the DVM
 * master. */
PRTE_EXPORT extern void prte_pmix_server_session_job_terminated(prte_session_t *session,
                                                                prte_job_t *jdata);

/* declare the RML recv functions for responses */
PRTE_EXPORT extern void pmix_server_launch_resp(int status, pmix_proc_t *sender,
                                                pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                void *cbdata);

PRTE_EXPORT extern void pmix_server_keyval_client(int status, pmix_proc_t *sender,
                                                  pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                  void *cbdata);

PRTE_EXPORT extern void pmix_server_notify(int status, pmix_proc_t *sender,
                                           pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                           void *cbdata);

PRTE_EXPORT extern void pmix_server_tconn_return(int status, pmix_proc_t *sender,
                                                 pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                 void *cbdata);

/* A query a daemon cannot answer, relayed to the DVM master, and the answer
 * coming back - see the block comment in pmix_server_queries.c */
PRTE_EXPORT extern void pmix_server_query_request(int status, pmix_proc_t *sender,
                                                  pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                  void *cbdata);

PRTE_EXPORT extern void pmix_server_query_resp(int status, pmix_proc_t *sender,
                                               pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                               void *cbdata);

PRTE_EXPORT extern int prte_pmix_server_register_tool(prte_pmix_server_req_t *cd,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern int pmix_server_cache_job_info(prte_job_t *jdata, pmix_info_t *info);

PRTE_EXPORT extern int prte_pmix_xfer_job_info(prte_job_t *jdata,
                                               pmix_info_t *iptr,
                                               size_t ninfo);

PRTE_EXPORT extern int prte_pmix_xfer_app(prte_job_t *jdata, pmix_app_t *app);

PRTE_EXPORT extern void pmix_server_alloc_request_resp(int status, pmix_proc_t *sender,
                                                pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                void *cbdata);

PRTE_EXPORT extern pmix_status_t prte_pmix_set_scheduler(void);

/* Record the directives to hand PMIx_tool_attach_to_server when we go looking
 * for the scheduler.  Takes ownership of the array, which must have been
 * built with PMIX_INFO_CREATE; replaces anything recorded before.
 *
 * This exists because the knowledge and the call live on opposite sides of a
 * boundary that only crosses one way.  The parameters that say where the
 * scheduler is belong to the ras/pmix component, which may be a run-time
 * loadable plugin - so nothing in libprrte may name its symbols - while the
 * attach belongs here, is made once for the whole daemon, and is triggered by
 * whichever of allocation, session control or tool connection needs a
 * scheduler first.  The component pushes what it knows at selection time
 * instead. */
PRTE_EXPORT void prte_pmix_set_scheduler_directives(pmix_info_t *directives,
                                                    size_t ndirs);

/* Designate an attached server as the primary one, so that the client-side
 * PMIx calls that follow go to it.  Only one server can be primary at a
 * time, so any operation that uses one of our tool connections must call
 * this first rather than assume the primary is still whatever the last
 * operation left in place.  Cheap when it is: a no-op if the named server
 * is already primary.  Blocks briefly - PMIx_tool_set_server completes on
 * the PMIx progress thread - so it must not be called from that thread. */
PRTE_EXPORT extern pmix_status_t prte_pmix_set_primary_server(const pmix_proc_t *target);

PRTE_EXPORT extern pmix_status_t prte_server_send_request(uint8_t cmd, prte_pmix_server_req_t *req);

PRTE_EXPORT extern void prte_server_lost_connection(size_t evhdlr_registration_id,
                                                    pmix_status_t status,
                                                    const pmix_proc_t *source,
                                                    pmix_info_t info[], size_t ninfo,
                                                    pmix_info_t *results, size_t nresults,
                                                    pmix_event_notification_cbfunc_fn_t cbfunc,
                                                    void *cbdata);


PRTE_EXPORT extern void pmix_server_monitor_request(int status, pmix_proc_t *sender,
                                                    pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                    void *cbdata);

PRTE_EXPORT extern void pmix_server_monitor_resp(int status, pmix_proc_t *sender,
                                                 pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                 void *cbdata);

#define PRTE_PMIX_ALLOC_REQ      0
#define PRTE_PMIX_SESSION_CTRL   1
/* Ask the DVM master for a group context id. Unlike its two siblings this
 * command carries nothing of its own - the id does not depend on anything the
 * requestor knows - so the relay packs only the common header. */
#define PRTE_PMIX_GROUP_CTXID    2

PRTE_EXPORT extern pmix_status_t
pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                            uint32_t sessionID,
                            const pmix_info_t directives[], size_t ndirs,
                            pmix_info_cbfunc_t cbfunc, void *cbdata);

/* Apply a PMIX_GROUP_LEFT notification to this daemon's copy of the group
 * registry, dropping the departing proc from the membership we hold. The
 * info array is the generating client's own and is validated here. Runs on
 * the PRRTE progress thread; exported so the unit test can reach it. */
PRTE_EXPORT extern void prte_pmix_server_group_member_left(pmix_status_t code,
                                                           const pmix_proc_t *source,
                                                           pmix_info_t *info, size_t ninfo);

/* Interpret a PMIX_ALLOC_TIME session time limit -
 * "[[[[months:]days:]hours:]minutes:]seconds", scanned from the right - and
 * return it in seconds, or -1 if the string is not a well-formed time. */
PRTE_EXPORT extern long prte_pmix_server_parse_session_time(const char *str);

/* Execute a session control directive against this DVM's own sessions, and
 * answer the request. Used when the directive came from the scheduler (a
 * directive TO us) or when there is no scheduler to defer to, in which case
 * the DVM master is the authority over its own sessions. Takes over the
 * request: the caller must neither answer nor release it afterwards. Must be
 * called on the PRRTE progress thread, on the DVM master. */
PRTE_EXPORT extern void
prte_pmix_server_session_ctrl_local(prte_pmix_server_req_t *req);


/* exposed shared variables */
typedef struct {
    pmix_list_item_t super;
    char *name;
    prte_job_t *jdata;
    pmix_proc_t *members;
    size_t num_members;
} prte_pmix_server_pset_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_pmix_server_pset_t);

/* A set of processes recorded as "connected" by PMIx_Connect.
 *
 * "Connected" means only that the host is to treat the members as one
 * assemblage for fault and event-notification purposes: any member that
 * terminates without first calling PMIx_Disconnect owes the others a
 * PMIX_ERR_PROC_TERM_WO_SYNC event.  Holding the membership is what makes
 * that promise keepable, and it is held on the DVM master alone, because
 * that is the one process that sees every proc in the DVM terminate -
 * including the procs of a node whose daemon died, which no one else is
 * left to report.
 *
 * The members are the participant array exactly as the connect named it, so
 * an entry may be a wildcard rank standing for a whole namespace. */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t *members;
    size_t nmembers;
    /* this assemblage is already being torn down after a member's failure,
     * so a second failure inside it must not drive the teardown again */
    bool terminating;
} prte_pmix_server_connection_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_pmix_server_connection_t);

/* Record/forget an assemblage.  Both are driven by the connect and disconnect
 * collectives as they complete on the DVM master, and both are no-ops
 * anywhere else.  Recording an assemblage that is already recorded, or
 * dropping one that is not, is not an error: the two collectives are the only
 * things that call these, and a repeat of either is harmless. */
PRTE_EXPORT void prte_pmix_server_connection_record(const pmix_proc_t *members, size_t nmembers);
PRTE_EXPORT void prte_pmix_server_connection_drop(const pmix_proc_t *members, size_t nmembers);

/* Is this proc a member of any recorded assemblage?  A member entry naming a
 * wildcard rank covers every proc of that namespace, which is how a connect
 * between two jobs is almost always expressed. */
PRTE_EXPORT bool prte_pmix_server_is_connected(const pmix_proc_t *proc);

/* A proc has terminated.  Tell every assemblage it belonged to, since it
 * plainly did not disconnect first.  Called once per proc from the DVM
 * master's proc-termination path. */
PRTE_EXPORT void prte_pmix_server_connection_terminated(prte_proc_t *proc);

/* A job has launched successfully: connect it to the process that spawned
 * it, if it was spawned by a process at all.  The PMIx definition makes that
 * the default for PMIx_Spawn; PMIX_SPAWN_CHILD_SEP opts out. */
PRTE_EXPORT void prte_pmix_server_connection_spawned(prte_job_t *jdata);

/* A failure has cost this job its life.  "Connected" means the host treats
 * the assemblage as a single application, so every other job connected to it
 * goes too - each one announced with PMIX_ERR_JOB_TERM_WO_SYNC.  Called from
 * the DVM master's error manager as it terminates the job that failed. */
PRTE_EXPORT void prte_pmix_server_connection_job_failed(const pmix_nspace_t nspace);

/* A job's data object is going away: forget any assemblage that has nothing
 * left in it to notify. */
PRTE_EXPORT void prte_pmix_server_connection_purge(const pmix_nspace_t nspace);

/* Report a completed connect/disconnect to the DVM master, which is where the
 * membership is held.  Called on each daemon that had a participant, from the
 * completion of the collective that carried the operation. */
PRTE_EXPORT void prte_pmix_server_connection_report(const pmix_proc_t *members, size_t nmembers);
PRTE_EXPORT void prte_pmix_server_connection_report_drop(const pmix_proc_t *members,
                                                         size_t nmembers);

/* Receive such a report - registered on PRTE_RML_TAG_CONNECTED by the DVM
 * master. */
PRTE_EXPORT void prte_pmix_server_connection_recv(int status, pmix_proc_t *sender,
                                                  pmix_data_buffer_t *buffer,
                                                  prte_rml_tag_t tag, void *cbdata);

typedef struct {
    bool initialized;
    int verbosity;
    int output;
    pmix_pointer_array_t remote_reqs;
    pmix_pointer_array_t local_reqs;
    int timeout;
    bool wait_for_server;
    pmix_proc_t server;
    pmix_list_t notifications;
    bool pubsub_init;
    bool session_server;
    bool system_server;
    bool no_foreign_tools;
    bool system_controller;
    bool scheduler_connected;
    /* we have already looked for a scheduler to attach to and found none - do
     * not look again. See prte_pmix_set_scheduler(). */
    bool scheduler_lookup_done;
    /* Where to look for the scheduler, as told to us by whichever component
     * knows - ras/pmix, out of its own MCA parameters.  Empty when nobody has
     * said anything, which is the ordinary case and leaves the attach exactly
     * the bare rendezvous scan it has always been.  See
     * prte_pmix_set_scheduler_directives(). */
    pmix_info_t *scheduler_directives;
    size_t nscheddirs;
    bool remote_connections;
    bool tool_support;
    bool require_pid_match;
    bool allow_client_clones;
    pmix_proc_t scheduler;
    /* PMIx directs a tool's client-side operations at whichever attached
     * server is currently PRIMARY, and only one server may be primary at a
     * time.  A daemon can be attached to more than one - a scheduler and an
     * external data server - so the primary in force is tracked here and
     * every operation that goes out over one of those connections names its
     * own server first.  See prte_pmix_set_primary_server(). */
    pmix_proc_t primary_server;
    bool primary_server_set;
    char *report_uri;
    char *singleton;
    pmix_device_type_t generate_dist;
    /* Publish per-proc data to the local PMIx server only for the procs this
     * daemon actually hosts, and derive the rest on demand when PMIx asks for
     * them through the direct-modex upcall.  Registering every proc in the job
     * on every daemon costs a table that grows with the total process count on
     * a node that will run a fixed slice of it. */
    pmix_list_t psets;
    pmix_list_t groups;
    /* assemblages formed by PMIx_Connect - see prte_pmix_server_connection_t.
     * Populated on the DVM master only. */
    pmix_list_t connections;
    /* whether a failure that terminates one job of an assemblage terminates
     * the rest of it, which is what the PMIx definition of "connected" asks
     * of a host that terminates an application when one of its processes
     * fails.  An MCA parameter because it changes what happens to a job the
     * user did not ask about. */
    bool terminate_connected;
    /* jobs whose local procs have all departed, so that a direct modex for
     * one of them can be told "not found" instead of waiting for a job
     * object that is never coming back - see prte_pmix_server_job_departed() */
    pmix_list_t departed_jobs;
} prte_pmix_server_globals_t;

PRTE_EXPORT extern prte_pmix_server_globals_t prte_pmix_server_globals;

END_C_DECLS

#endif /* PMIX_SERVER_INTERNAL_H_ */
