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
    pmix_status_t rc;

    /* if we are not the DVM controller, then send it there */

    /* we are the DVM controller, so pass it down so PMIx can
     * send it to the scheduler */

    if (!prte_pmix_server_globals.scheduler_connected) {
        /* the scheduler has not attached to us - there is
         * nothing we can do */
        return PMIX_ERR_NOT_SUPPORTED;
    }
    /* if we have not yet set the scheduler as our server, do so */
    if (!prte_pmix_server_globals.scheduler_set_as_server) {
        rc = PMIx_tool_set_server(&prte_pmix_server_globals.scheduler, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        prte_pmix_server_globals.scheduler_set_as_server = true;
    }

}

#if PMIX_NUMERIC_VERSION >= 0x00050000

pmix_status_t pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                                          uint32_t sessionID,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;

    /* if we are not the DVM controller, then send it there */

    /* we are the DVM controller, so pass it down so PMIx can
     * send it to the scheduler */

    if (!prte_pmix_server_globals.scheduler_connected) {
        /* the scheduler has not attached to us - there is
         * nothing we can do */
        return PMIX_ERR_NOT_SUPPORTED;
    }
    /* if we have not yet set the scheduler as our server, do so */
    if (!prte_pmix_server_globals.scheduler_set_as_server) {
        rc = PMIx_tool_set_server(&prte_pmix_server_globals.scheduler, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        prte_pmix_server_globals.scheduler_set_as_server = true;
    }

}

#endif
