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

/**
 * Check if we have the Jansson library available in compilation
 */
bool prte_ras_slurm_have_jansson(void)
{
    return false;
}

/*
 * Extract SLURM job fields; returns PRTE_ERR_NOT_AVAILABLE if built without Jansson.
 */
int prte_ras_slurm_extract_job_fields(pmix_hash_table_t *values_table)
{
    PRTE_HIDE_UNUSED_PARAMS(values_table);
    pmix_output(0, "ras:slurm:extract_job_fields: "
                "Jansson support is required but not enabled in this build");
    return PRTE_ERR_NOT_AVAILABLE;
}

/**
 * Add new SLURM job resources; returns PRTE_ERR_NOT_AVAILABLE if built without Jansson.
 */
int prte_ras_slurm_add_modified_resources(const char *slurm_jobid,
                                                 pmix_list_t *node_list)
{
    PRTE_HIDE_UNUSED_PARAMS(slurm_jobid, node_list);

    pmix_output(0, "ras:slurm:add_modified_resources: "
                "Jansson support is required but not enabled in this build");
    return PRTE_ERR_NOT_AVAILABLE;
}

/**
 * Wait for SLURM job resources; returns PRTE_ERR_NOT_AVAILABLE if built without Jansson.
 */
int prte_ras_slurm_check_resources(const char *slurm_jobid)
{
    PRTE_HIDE_UNUSED_PARAMS(slurm_jobid);
    pmix_output(0, "ras:slurm:wait_resources: "
                "Jansson support is required but not enabled in this build");
    return PRTE_ERR_NOT_AVAILABLE;
}
