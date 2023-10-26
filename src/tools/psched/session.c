/*
 * Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"

#include "src/tools/psched/psched.h"

pmix_status_t psched_alloc_fn(const pmix_proc_t *client,
                              pmix_alloc_directive_t directive,
                              const pmix_info_t data[], size_t ndata,
                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    psched_req_t *req;

    pmix_output_verbose(2, psched_globals.output,
                        "%s allocate upcalled on behalf of proc %s:%u with %" PRIsize_t " infos",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), client->nspace, client->rank, ndata);

    req = PMIX_NEW(psched_req_t);
    PMIX_LOAD_PROCID(&req->requestor, client->nspace, client->rank);
    req->directive = directive;
    req->data = (pmix_info_t *) data;
    req->ndata = ndata;
    req->cbfunc = cbfunc;
    req->cbdata = cbdata;
    PRTE_ACTIVATE_SCHED_STATE(req, PSCHED_STATE_INIT);
    return PRTE_SUCCESS;
}

#if PMIX_NUMERIC_VERSION >= 0x00050000

pmix_status_t psched_session_ctrl_fn(const pmix_proc_t *requestor,
                                     uint32_t sessionID,
                                     const pmix_info_t directives[], size_t ndirs,
                                     pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    psched_req_t *req;


    pmix_output_verbose(2, psched_globals.output,
                        "%s session ctrl upcalled on behalf of proc %s:%u with %" PRIsize_t " directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), requestor->nspace, requestor->rank, ndirs);

    req = PMIX_NEW(psched_req_t);
    PMIX_LOAD_PROCID(&req->requestor, requestor->nspace, requestor->rank);
    req->sessionID = sessionID;
    req->data = (pmix_info_t *) directives;
    req->ndata = ndirs;
    req->cbfunc = cbfunc;
    req->cbdata = cbdata;
    PRTE_ACTIVATE_SCHED_STATE(req, PSCHED_STATE_SESSION_COMPLETE);
    return PRTE_SUCCESS;
}

#endif
