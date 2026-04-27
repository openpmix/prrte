/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "ras_slurm.h"
#include "src/mca/ras/base/base.h"

int prte_ras_slurm_serve_release_req(prte_pmix_server_req_t *req)
{
    PRTE_HIDE_UNUSED_PARAMS(req);
    return PRTE_ERR_NOT_IMPLEMENTED;
}
