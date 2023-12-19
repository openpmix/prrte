/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PSCHED_H
#define PSCHED_H

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"

BEGIN_C_DECLS

typedef struct {
    bool initialized;
    pmix_pointer_array_t requests;
    pmix_list_t tools;
    pmix_proc_t syscontroller;
    bool controller_connected;
    int verbosity;
    int output;
    int scheduler_output;
} psched_globals_t;

extern psched_globals_t psched_globals;

extern void psched_schizo_init(void);
extern void psched_state_init(void);
extern void psched_errmgr_init(void);
extern int psched_server_init(pmix_cli_result_t *results);
extern void psched_server_finalize(void);
extern void psched_scheduler_init(void);
extern void psched_scheduler_finalize(void);
extern void psched_register_params(void);

extern pmix_status_t psched_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                               const pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_deregister_events_fn(pmix_status_t *codes,
                                                 size_t ncodes,
                                                 pmix_op_cbfunc_t cbfunc,
                                                 void *cbdata);

extern pmix_status_t psched_notify_event(pmix_status_t code,
                                         const pmix_proc_t *source,
                                         pmix_data_range_t range,
                                         pmix_info_t info[], size_t ninfo,
                                         pmix_op_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_query_fn(pmix_proc_t *proct,
                                     pmix_query_t *queries, size_t nqueries,
                                     pmix_info_cbfunc_t cbfunc, void *cbdata);

extern void psched_tool_connected_fn(pmix_info_t *info, size_t ninfo,
                                    pmix_tool_connection_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_job_ctrl_fn(const pmix_proc_t *requestor,
                                        const pmix_proc_t targets[], size_t ntargets,
                                        const pmix_info_t directives[], size_t ndirs,
                                        pmix_info_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_alloc_fn(const pmix_proc_t *client,
                                     pmix_alloc_directive_t directive,
                                     const pmix_info_t data[], size_t ndata,
                                     pmix_info_cbfunc_t cbfunc, void *cbdata);

#if PMIX_NUMERIC_VERSION >= 0x00050000
extern pmix_status_t psched_session_ctrl_fn(const pmix_proc_t *requestor,
                                            uint32_t sessionID,
                                            const pmix_info_t directives[], size_t ndirs,
                                            pmix_info_cbfunc_t cbfunc, void *cbdata);
#endif

// global objects
extern prte_schizo_base_module_t psched_schizo_module;
extern pmix_list_t prte_psched_states;

typedef int32_t prte_sched_state_t;
#define PSCHED_STATE_ANY                INT32_MAX
#define PSCHED_STATE_UNDEF               0
#define PSCHED_STATE_INIT                1
#define PSCHED_STATE_QUEUE               2
#define PSCHED_STATE_SESSION_COMPLETE   30

/* Define a boundary so we can easily and quickly determine
 * if a scheduler operation abnormally terminated - leave a little room
 * for future expansion
 */
#define PSCHED_STATE_ERROR          50

typedef struct {
    pmix_list_item_t super;
    prte_sched_state_t sched_state;
    prte_state_cbfunc_t cbfunc;
} psched_state_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(psched_state_t);

PRTE_EXPORT extern pmix_list_t prte_psched_states;

/* track a session throughout its lifecycle */
typedef struct {
    /** Base object so this can be put on a list */
    pmix_list_item_t super;
    prte_event_t ev;
    // allocation request info
    pmix_proc_t requestor;
    pmix_alloc_directive_t directive;
    // whether the data is a local copy
    bool copy;
    // original info keys
    pmix_info_t *data;
    size_t ndata;
    // callback upon completion
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
    // processed directives
    char *user_refid;
    char *alloc_refid;
    uint64_t num_nodes;
    char *nlist;
    char *exclude;
    uint64_t num_cpus;
    char *ncpulist;
    char *cpulist;
    float memsize;
    char *time;
    char *queue;
    bool preemptible;
    char *lend;
    char *image;
    bool waitall;
    bool share;
    bool noshell;
    char *dependency;
    char *begintime;
    // internal tracking info
    prte_sched_state_t state;
    // assigned session info
    uint32_t sessionID;
} psched_req_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(psched_req_t);

extern const char* prte_sched_state_to_str(prte_sched_state_t s);
// scheduler operations
extern void psched_activate_sched_state(psched_req_t *req, prte_sched_state_t state);
extern void psched_request_init(int fd, short args, void *cbdata);
extern void psched_request_queue(int fd, short args, void *cbdata);
extern void psched_session_complete(int fd, short args, void *cbdata);


#define PRTE_ACTIVATE_SCHED_STATE(j, s)                                             \
    do {                                                                            \
        psched_req_t *shadow = (j);                                                 \
        if (psched_globals.verbosity > 0) {                                         \
            double timestamp = 0.0;                                                 \
            PRTE_STATE_GET_TIMESTAMP(timestamp);                                    \
            pmix_output_verbose(1, psched_globals.output,      \
                                "%s [%f] ACTIVATE SCHED %s STATE %s AT %s:%d",      \
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), timestamp,      \
                                (NULL == shadow->alloc_refid) ? "NO REFID" : shadow->alloc_refid,    \
                                prte_sched_state_to_str((s)), __FILE__, __LINE__);  \
        }                                                                           \
        psched_activate_sched_state(shadow, (s));                                   \
    } while (0);

#define PRTE_REACHING_SCHED_STATE(j, s)                                             \
    do {                                                                            \
        psched_req_t *shadow = (j);                                                 \
        if (psched_globals.verbosity > 0) {                      \
            double timestamp = 0.0;                                                 \
            PRTE_STATE_GET_TIMESTAMP(timestamp);                                    \
            pmix_output_verbose(1, psched_globals.output,      \
                                "%s [%f] ACTIVATING SCHED %s STATE %s AT %s:%d",             \
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), timestamp,      \
                                (NULL == shadow->alloc_refid) ? "NO REFID" : shadow->alloc_refid,    \
                                prte_sched_state_to_str((s)), __FILE__, __LINE__);                      \
            shadow->state = (s);                                                    \
        }                                                                           \
    } while (0);

#define PSCHED_THREADSHIFT(c, fn)                                                   \
    do {                                                                            \
        prte_event_set(prte_event_base, &((c)->ev), -1, PRTE_EV_WRITE, (fn), (c));  \
        PMIX_POST_OBJECT(c);                                                        \
        prte_event_active(&((c)->ev), PRTE_EV_WRITE, 1);                            \
    } while (0);

END_C_DECLS

#endif /* PSCHED_H */
