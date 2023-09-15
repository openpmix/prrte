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

void psched_schizo_init(void);
void psched_state_init(void);
void psched_errmgr_init(void);
int psched_server_init(pmix_cli_result_t *results);
void psched_server_finalize(void);
void psched_scheduler_init(void);
void psched_scheduler_finalize(void);

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

extern void psched_log_fn(const pmix_proc_t *client,
                          const pmix_info_t data[], size_t ndata,
                          const pmix_info_t directives[], size_t ndirs,
                          pmix_op_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_job_ctrl_fn(const pmix_proc_t *requestor,
                                        const pmix_proc_t targets[], size_t ntargets,
                                        const pmix_info_t directives[], size_t ndirs,
                                        pmix_info_cbfunc_t cbfunc, void *cbdata);

extern pmix_status_t psched_group_fn(pmix_group_operation_t op, char *gpid,
                                     const pmix_proc_t procs[], size_t nprocs,
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

extern prte_schizo_base_module_t psched_schizo_module;

END_C_DECLS

#endif /* PSCHED_H */
