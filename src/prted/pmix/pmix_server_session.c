/*
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"

pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    /* if we are not the DVM controller, then send it there */

    /* we are the DVM controller, so pass it down so PMIx can
     * send it to the scheduler */

}

pmix_status_t pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                                          uint32_t sessionID,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    /* if we are not the DVM controller, then send it there */

    /* we are the DVM controller, so pass it down so PMIx can
     * send it to the scheduler */

}
