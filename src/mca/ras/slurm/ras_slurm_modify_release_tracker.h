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

#ifndef PRTE_RAS_SLURM_MODIFY_RELEASE_TRACKER_H
#define PRTE_RAS_SLURM_MODIFY_RELEASE_TRACKER_H

#include "ras_slurm.h"

BEGIN_C_DECLS

typedef enum {
    PRTE_RAS_SLURM_RELEASE_FULL_JOB,
    PRTE_RAS_SLURM_RELEASE_PARTIAL_JOB
} prte_ras_slurm_release_action_type_t;

typedef struct {
    pmix_list_item_t super;
    char *job_id;
    prte_ras_slurm_release_action_type_t action;
    char **survivor_nodes;
} prte_ras_slurm_release_action_t;
PMIX_CLASS_DECLARATION(prte_ras_slurm_release_action_t);

typedef struct {
    pmix_list_item_t super;
    prte_shrink_campaign_t *campaign;
    pmix_list_t actions;
} prte_ras_slurm_shrink_tracker_t;
PMIX_CLASS_DECLARATION(prte_ras_slurm_shrink_tracker_t);

bool prte_ras_slurm_jobid_has_active_shrink(const char *jobid);
int prte_ras_slurm_add_release_action(prte_ras_slurm_shrink_tracker_t *tracker,
                                      const char *job_id,
                                      prte_ras_slurm_release_action_type_t action,
                                      char **survivor_nodes);
const char *prte_ras_slurm_tracker_first_jobid(prte_ras_slurm_shrink_tracker_t *tracker);
void prte_ras_slurm_track_shrink_campaign(prte_ras_slurm_shrink_tracker_t *tracker,
                                          prte_shrink_campaign_t *campaign);
void prte_ras_slurm_untrack_shrink_campaign(prte_ras_slurm_shrink_tracker_t *tracker);
prte_ras_slurm_shrink_tracker_t *prte_ras_slurm_find_tracker_by_campaign(prte_shrink_campaign_t *campaign);

END_C_DECLS

#endif /* PRTE_RAS_SLURM_MODIFY_RELEASE_TRACKER_H */
