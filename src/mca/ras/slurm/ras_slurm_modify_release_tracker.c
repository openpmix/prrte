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

#include "ras_slurm_modify_release_tracker.h"

static void release_action_con(prte_ras_slurm_release_action_t *p);
static void release_action_des(prte_ras_slurm_release_action_t *p);
static void shrink_tracker_con(prte_ras_slurm_shrink_tracker_t *p);
static void shrink_tracker_des(prte_ras_slurm_shrink_tracker_t *p);
static bool prte_ras_slurm_tracker_has_jobid(prte_ras_slurm_shrink_tracker_t *tracker,
                                             const char *jobid);

PMIX_CLASS_INSTANCE(prte_ras_slurm_release_action_t,
                    pmix_list_item_t,
                    release_action_con,
                    release_action_des);
PMIX_CLASS_INSTANCE(prte_ras_slurm_shrink_tracker_t,
                    pmix_list_item_t,
                    shrink_tracker_con,
                    shrink_tracker_des);

static pmix_list_t *prte_slurm_shrink_trackers = NULL;

static void release_action_con(prte_ras_slurm_release_action_t *p)
{
    p->job_id = NULL;
    p->action = PRTE_RAS_SLURM_RELEASE_FULL_JOB;
    p->survivor_nodes = NULL;
}

static void release_action_des(prte_ras_slurm_release_action_t *p)
{
    free(p->job_id);
    if (NULL != p->survivor_nodes) {
        PMIx_Argv_free(p->survivor_nodes);
    }
}

static void shrink_tracker_con(prte_ras_slurm_shrink_tracker_t *p)
{
    p->campaign = NULL;
    PMIX_CONSTRUCT(&p->actions, pmix_list_t);
}

static void shrink_tracker_des(prte_ras_slurm_shrink_tracker_t *p)
{
    prte_ras_slurm_release_action_t *action;

    while (NULL != (action = (prte_ras_slurm_release_action_t *)
                               pmix_list_remove_first(&p->actions))) {
        PMIX_RELEASE(action);
    }
    PMIX_DESTRUCT(&p->actions);
}

int prte_ras_slurm_modify_release_init(void)
{
    prte_slurm_shrink_trackers = PMIX_NEW(pmix_list_t);
    if (NULL == prte_slurm_shrink_trackers) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    return PRTE_SUCCESS;
}

int prte_ras_slurm_modify_release_finalize(void)
{
    prte_ras_slurm_shrink_tracker_t *tracker;

    if (NULL == prte_slurm_shrink_trackers) {
        return PRTE_SUCCESS;
    }

    while (NULL != (tracker = (prte_ras_slurm_shrink_tracker_t *)
                                  pmix_list_remove_first(prte_slurm_shrink_trackers))) {
        PMIX_RELEASE(tracker);
    }
    PMIX_RELEASE(prte_slurm_shrink_trackers);
    prte_slurm_shrink_trackers = NULL;

    return PRTE_SUCCESS;
}

/**
 * @brief Check whether a shrink tracker includes a Slurm job ID.
 *
 * @param[in] tracker Shrink tracker to inspect.
 * @param[in] jobid Slurm job ID to find.
 */
static bool prte_ras_slurm_tracker_has_jobid(prte_ras_slurm_shrink_tracker_t *tracker,
                                             const char *jobid)
{
    prte_ras_slurm_release_action_t *action;

    if (NULL == tracker || NULL == jobid) {
        return false;
    }

    PMIX_LIST_FOREACH(action, &tracker->actions, prte_ras_slurm_release_action_t) {
        if (NULL != action->job_id && 0 == strcmp(action->job_id, jobid)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Check whether a Slurm job already has an active shrink.
 *
 * @param[in] jobid Slurm job ID to find.
 */
bool prte_ras_slurm_jobid_has_active_shrink(const char *jobid)
{
    prte_ras_slurm_shrink_tracker_t *tracker;

    if (NULL == prte_slurm_shrink_trackers || NULL == jobid) {
        return false;
    }

    PMIX_LIST_FOREACH(tracker, prte_slurm_shrink_trackers,
                      prte_ras_slurm_shrink_tracker_t) {
        if (prte_ras_slurm_tracker_has_jobid(tracker, jobid)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Add a Slurm release action to a shrink tracker.
 *
 * @param[in] tracker Tracker receiving the action.
 * @param[in] job_id Slurm job ID affected by the action.
 * @param[in] action Action to complete after the DVM shrink.
 * @param[in] survivor_nodes Nodes that should survive a partial release.
 */
int prte_ras_slurm_add_release_action(prte_ras_slurm_shrink_tracker_t *tracker,
                                      const char *job_id,
                                      prte_ras_slurm_release_action_type_t action,
                                      char **survivor_nodes)
{
    prte_ras_slurm_release_action_t *rel_action;

    if (NULL == tracker || NULL == job_id) {
        return PRTE_ERR_BAD_PARAM;
    }

    rel_action = PMIX_NEW(prte_ras_slurm_release_action_t);
    if (NULL == rel_action) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    rel_action->job_id = strdup(job_id);
    if (NULL == rel_action->job_id) {
        PMIX_RELEASE(rel_action);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    rel_action->action = action;
    if (NULL != survivor_nodes) {
        rel_action->survivor_nodes = PMIx_Argv_copy(survivor_nodes);
        if (NULL == rel_action->survivor_nodes) {
            PMIX_RELEASE(rel_action);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    pmix_list_append(&tracker->actions, &rel_action->super);
    return PRTE_SUCCESS;
}

const char *prte_ras_slurm_tracker_first_jobid(prte_ras_slurm_shrink_tracker_t *tracker)
{
    prte_ras_slurm_release_action_t *action;

    if (NULL == tracker || pmix_list_is_empty(&tracker->actions)) {
        return NULL;
    }

    action = (prte_ras_slurm_release_action_t *) pmix_list_get_first(&tracker->actions);
    return action->job_id;
}

void prte_ras_slurm_track_shrink_campaign(prte_ras_slurm_shrink_tracker_t *tracker,
                                          prte_shrink_campaign_t *campaign)
{
    tracker->campaign = campaign;
    pmix_list_append(prte_slurm_shrink_trackers, &tracker->super);
}

void prte_ras_slurm_untrack_shrink_campaign(prte_ras_slurm_shrink_tracker_t *tracker)
{
    pmix_list_remove_item(prte_slurm_shrink_trackers, &tracker->super);
    tracker->campaign = NULL;
}

prte_ras_slurm_shrink_tracker_t *prte_ras_slurm_find_tracker_by_campaign(prte_shrink_campaign_t *campaign)
{
    prte_ras_slurm_shrink_tracker_t *tracker;

    if (NULL == campaign || NULL == prte_slurm_shrink_trackers) {
        return NULL;
    }

    PMIX_LIST_FOREACH(tracker, prte_slurm_shrink_trackers,
                      prte_ras_slurm_shrink_tracker_t) {
        if (tracker->campaign == campaign) {
            return tracker;
        }
    }

    return NULL;
}
