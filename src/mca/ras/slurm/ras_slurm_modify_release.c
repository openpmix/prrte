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
#include "ras_slurm_modify_release_tracker.h"
#include "src/mca/preg/preg.h"
#include "src/mca/ras/base/base.h"

/* Local functions */
static bool prte_ras_slurm_session_contains_invoker(prte_session_t *session, const char *launching_jobid);
static int prte_ras_slurm_session_removable_count(prte_session_stack_item_t *session_item, const char *launching_jobid);
static prte_session_stack_item_t *prte_ras_slurm_find_releasable_session(const char *launching_jobid, char **excluded_jobids);
static int prte_ras_slurm_validate_count_release(int nodes_to_remove, const char *launching_jobid);
static void localrelease(void *cbdata);
static bool prte_ras_slurm_session_is_dynamic(prte_session_t *session);
static int prte_ras_slurm_remove_nodes_by_name(prte_pmix_server_req_t *req, char **nodes);
static int prte_ras_slurm_remove_allocation_by_id(prte_pmix_server_req_t *req, const char *alloc_id);
static int prte_ras_slurm_remove_nodes_by_count(prte_pmix_server_req_t *req, uint64_t node_count);
static int prte_ras_slurm_append_session_targets(prte_session_t *session, char ***target_nodes);
static int prte_ras_slurm_append_release_targets(prte_session_t *session, char **requested_nodes, char ***target_nodes);
static int prte_ras_slurm_append_count_session_targets(prte_session_t *session, const char *protected_node, int nodes_to_remove, char ***target_nodes);
static int prte_ras_slurm_attach_shrink_tracker(prte_shrink_campaign_t *campaign, prte_ras_slurm_shrink_tracker_t *tracker);
static int prte_ras_slurm_start_shrink(prte_pmix_server_req_t *req, prte_ras_slurm_shrink_tracker_t *tracker, char **target_nodes);
static int prte_ras_slurm_complete_release_request(prte_pmix_server_req_t *req);
static int prte_ras_slurm_parse_release_node_list(pmix_info_t *info, char ***nodes);
static prte_session_stack_item_t *prte_ras_slurm_find_session_item_by_alloc_id(const char *alloc_id);
static prte_session_stack_item_t *prte_ras_slurm_find_session_item_by_node(const char *node_name);
static bool prte_ras_slurm_node_in_argv(char **nodes, const char *node_name);
static int prte_ras_slurm_count_matching_session_nodes(prte_session_t *session, char **nodes);
static int prte_ras_slurm_build_survivor_list(prte_session_t *session, char **nodes_to_remove, char ***survivors);
static int prte_ras_slurm_shrink_job_to_survivors(const char *slurm_jobid, char **survivor_nodes, char *err_msg, size_t err_msg_size);
static void prte_ras_slurm_cleanup_resize_scripts(const char *slurm_jobid);

static void localrelease(void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/**
 * @brief Check whether a Slurm session was created by a modify request.
 *
 * @param[in] session Slurm-backed session to inspect.
 */
static bool prte_ras_slurm_session_is_dynamic(prte_session_t *session)
{
    return NULL != session &&
           PRTE_FLAG_TEST(session, PRTE_SESSION_FLAG_DYNAMIC);
}

/**
 * @brief Check whether a session contains the requesting Slurm job.
 *
 * @param[in] session Slurm-backed session to inspect.
 * @param[in] launching_jobid Slurm job ID of the requester.
 */
static bool prte_ras_slurm_session_contains_invoker(prte_session_t *session,
                                                    const char *launching_jobid)
{
    return NULL != session && NULL != session->alloc_refid &&
           NULL != launching_jobid &&
           0 == strcmp(session->alloc_refid, launching_jobid);
}

/**
 * @brief Return how many nodes may be removed from a session.
 *
 * @param[in] session_item Slurm session stack item.
 * @param[in] launching_jobid Slurm job ID whose current node must survive.
 */
static int prte_ras_slurm_session_removable_count(prte_session_stack_item_t *session_item,
                                                  const char *launching_jobid)
{
    if (NULL == session_item || NULL == session_item->session) {
        return 0;
    }

    if (!prte_ras_slurm_session_is_dynamic(session_item->session) ||
        prte_ras_slurm_session_contains_invoker(session_item->session, launching_jobid)) {
        if (1 >= session_item->nodes_in_session) {
            return 0;
        }
        return session_item->nodes_in_session - 1;
    }

    return session_item->nodes_in_session;
}

/**
 * @brief Find the newest removable session outside an exclusion list.
 *
 * @param[in] launching_jobid Slurm job ID whose current node must survive.
 * @param[in] excluded_jobids Slurm job IDs already planned for release.
 */
static prte_session_stack_item_t *prte_ras_slurm_find_releasable_session(const char *launching_jobid,
                                                                         char **excluded_jobids)
{
    pmix_list_item_t *item;

    if (NULL == prte_slurm_session_stack ||
        0 == pmix_list_get_size(prte_slurm_session_stack)) {
        return NULL;
    }

    for (item = pmix_list_get_last(prte_slurm_session_stack);
         item != pmix_list_get_end(prte_slurm_session_stack);
         item = pmix_list_get_prev(item)) {
        prte_session_stack_item_t *session_item = (prte_session_stack_item_t *) item;
        prte_session_t *session;

        if (NULL == session_item || NULL == session_item->session) {
            return NULL;
        }

        session = session_item->session;
        if (NULL == session->alloc_refid) {
            return NULL;
        }

        if (prte_ras_slurm_node_in_argv(excluded_jobids, session->alloc_refid)) {
            continue;
        }

        if (prte_ras_slurm_jobid_has_active_shrink(session->alloc_refid)) {
            continue;
        }

        if (0 >= prte_ras_slurm_session_removable_count(session_item, launching_jobid)) {
            continue;
        }

        return session_item;
    }

    return NULL;
}

/**
 * @brief Validate that a count release can avoid active shrinks.
 *
 * @param[in] nodes_to_remove Number of nodes requested for release.
 * @param[in] launching_jobid Slurm job ID whose current node must survive.
 */
static int prte_ras_slurm_validate_count_release(int nodes_to_remove,
                                                 const char *launching_jobid)
{
    pmix_list_item_t *item;
    int nodes_remaining = nodes_to_remove;
    bool skipped_conflict = false;

    if (NULL == prte_slurm_session_stack ||
        0 == pmix_list_get_size(prte_slurm_session_stack)) {
        return PRTE_ERR_NOT_FOUND;
    }

    for (item = pmix_list_get_last(prte_slurm_session_stack);
         item != pmix_list_get_end(prte_slurm_session_stack) && 0 < nodes_remaining;
         item = pmix_list_get_prev(item)) {
        prte_session_stack_item_t *session_item = (prte_session_stack_item_t *) item;
        prte_session_t *session;

        if (NULL == session_item || NULL == session_item->session) {
            return PRTE_ERR_NOT_FOUND;
        }

        session = session_item->session;
        if (NULL == session->alloc_refid) {
            return PRTE_ERR_NOT_FOUND;
        }

        if (prte_ras_slurm_jobid_has_active_shrink(session->alloc_refid)) {
            skipped_conflict = true;
            continue;
        }

        nodes_remaining -= prte_ras_slurm_session_removable_count(session_item,
                                                                  launching_jobid);
    }

    if (0 < nodes_remaining) {
        if (skipped_conflict) {
            return PRTE_ERR_RESOURCE_BUSY;
        }
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Append every node in a session to a shrink target list.
 *
 * @param[in] session Session whose daemons should be removed.
 * @param[in,out] target_nodes Target node argv.
 */
static int prte_ras_slurm_append_session_targets(prte_session_t *session, char ***target_nodes)
{
    pmix_status_t pmix_err;

    if (NULL == session || NULL == session->nodes || NULL == target_nodes) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

        if (NULL == node || NULL == node->name) {
            continue;
        }

        pmix_err = PMIx_Argv_append_nosize(target_nodes, node->name);
        if (PMIX_SUCCESS != pmix_err) {
            return prte_pmix_convert_status(pmix_err);
        }
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Append requested nodes that belong to a session.
 *
 * @param[in] session Session to inspect.
 * @param[in] requested_nodes Requested release node names.
 * @param[in,out] target_nodes Target node argv.
 */
static int prte_ras_slurm_append_release_targets(prte_session_t *session,
                                                 char **requested_nodes,
                                                 char ***target_nodes)
{
    pmix_status_t pmix_err;

    if (NULL == session || NULL == session->nodes || NULL == requested_nodes ||
        NULL == target_nodes) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

        if (NULL == node || NULL == node->name ||
            !prte_ras_slurm_node_in_argv(requested_nodes, node->name)) {
            continue;
        }

        pmix_err = PMIx_Argv_append_nosize(target_nodes, node->name);
        if (PMIX_SUCCESS != pmix_err) {
            return prte_pmix_convert_status(pmix_err);
        }
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Append count-selected nodes from a session.
 *
 * @param[in] session Session to inspect.
 * @param[in] protected_node Node that must not be selected.
 * @param[in] nodes_to_remove Number of nodes to append.
 * @param[in,out] target_nodes Target node argv.
 */
static int prte_ras_slurm_append_count_session_targets(prte_session_t *session,
                                                       const char *protected_node,
                                                       int nodes_to_remove,
                                                       char ***target_nodes)
{
    pmix_status_t pmix_err;
    int added = 0;

    if (NULL == session || NULL == session->nodes || NULL == target_nodes ||
        0 > nodes_to_remove) {
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = session->nodes->size - 1; 0 <= i && added < nodes_to_remove; i--) {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

        if (NULL == node || NULL == node->name) {
            continue;
        }
        if (NULL != protected_node && 0 == strcmp(node->name, protected_node)) {
            continue;
        }

        pmix_err = PMIx_Argv_append_nosize(target_nodes, node->name);
        if (PMIX_SUCCESS != pmix_err) {
            return prte_pmix_convert_status(pmix_err);
        }
        added++;
    }

    if (added != nodes_to_remove) {
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Attach Slurm release work to a prepared DVM shrink campaign.
 *
 * @param[in] campaign DVM shrink campaign created by RAS base.
 * @param[in] tracker Tracker describing RM-side completion work.
 */
static int prte_ras_slurm_attach_shrink_tracker(prte_shrink_campaign_t *campaign,
                                                prte_ras_slurm_shrink_tracker_t *tracker)
{
    if (NULL == campaign || NULL == tracker) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (NULL == campaign->alloc_id) {
        const char *jobid = prte_ras_slurm_tracker_first_jobid(tracker);

        if (NULL != jobid) {
            campaign->alloc_id = strdup(jobid);
            if (NULL == campaign->alloc_id) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
        }
    }

    prte_ras_slurm_track_shrink_campaign(tracker, campaign);
    return PRTE_SUCCESS;
}

/**
 * @brief Start a DVM shrink campaign for planned Slurm release actions.
 *
 * @param[in] req PMIx release request.
 * @param[in] tracker Tracker describing RM-side completion work.
 * @param[in] target_nodes Nodes whose daemons should leave the DVM.
 */
static int prte_ras_slurm_start_shrink(prte_pmix_server_req_t *req,
                                       prte_ras_slurm_shrink_tracker_t *tracker,
                                       char **target_nodes)
{
    prte_shrink_campaign_t *campaign = NULL;
    pmix_rank_t *ranks = NULL;
    int32_t nranks;
    bool tracked = false;
    int err = PRTE_SUCCESS;

    if (NULL == req || NULL == tracker || NULL == target_nodes ||
        0 == (nranks = PMIx_Argv_count(target_nodes))) {
        return PRTE_ERR_BAD_PARAM;
    }

    ranks = (pmix_rank_t *) malloc(nranks * sizeof(pmix_rank_t));
    if (NULL == ranks) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    for (int i = 0; NULL != target_nodes[i]; i++) {
        prte_node_t *node = prte_node_match(NULL, target_nodes[i]);

        if (NULL == node || NULL == node->daemon) {
            err = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }

        ranks[i] = node->daemon->name.rank;
    }

    err = prte_ras_base_prepare_dvm_shrink(req, ranks, nranks, &campaign);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }
    if (NULL == campaign) {
        err = PRTE_ERR_BAD_PARAM;
        goto cleanup;
    }

    err = prte_ras_slurm_attach_shrink_tracker(campaign, tracker);
    if (PRTE_SUCCESS != err) {
        prte_ras_base_abort_dvm_shrink(campaign);
        goto cleanup;
    }
    tracked = true;

    err = prte_ras_base_commit_dvm_shrink(campaign);
    if (PRTE_SUCCESS != err && tracked) {
        prte_ras_slurm_untrack_shrink_campaign(tracker);
    }

cleanup:
    free(ranks);

    return err;
}

/**
 * @brief Return phase-one release acceptance to the requester.
 *
 * @param[in] req PMIx release request.
 */
static int prte_ras_slurm_complete_release_request(prte_pmix_server_req_t *req)
{
    req->pstatus = PMIX_OPERATION_IN_PROGRESS;
    if (NULL != req->infocbfunc) {
        req->infocbfunc(req->pstatus, req->info, req->ninfo, req->cbdata,
                        localrelease, req);
        return PRTE_ERR_OP_IN_PROGRESS;
    }

    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index,
                                NULL);
    PMIX_RELEASE(req);

    return PRTE_ERR_OP_IN_PROGRESS;
}

/**
 * @brief Release a Slurm-backed session allocation.
 *
 * @param[in] session Session being destroyed.
 */
int prte_ras_slurm_release_allocation(prte_session_t *session)
{
    char err_msg[PRTE_SLURM_ERR_STR_MAX_LEN + 1] = {0};
    int err;

    if (NULL == session || NULL == session->alloc_refid ||
        NULL == prte_ras_slurm_find_session_item_by_alloc_id(session->alloc_refid)) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (!prte_ras_slurm_session_is_dynamic(session)) {
        /* This allocation was not added by PRRTE, so we do not manage its lifetime. */
        return PRTE_SUCCESS;
    }

    err = prte_ras_slurm_kill_job(session->alloc_refid, err_msg,
                                  sizeof(err_msg));
    if (PRTE_ERR_SLURM_CANCEL_FAILURE == err) {
        pmix_output(0, "ras:slurm:release_allocation: failed to kill job %s: %s.",
                    session->alloc_refid, err_msg);
    } else if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
    }

    return err;
}

/**
 * @brief Release Slurm resources after a DVM shrink completes.
 *
 * @param[in] campaign Completed shrink campaign.
 */
void prte_ras_slurm_shrink_complete(prte_shrink_campaign_t *campaign)
{
    prte_ras_slurm_shrink_tracker_t *found;
    prte_ras_slurm_release_action_t *action;

    found = prte_ras_slurm_find_tracker_by_campaign(campaign);
    if (NULL == found) {
        return;
    }

    PMIX_LIST_FOREACH(action, &found->actions, prte_ras_slurm_release_action_t) {
        prte_session_stack_item_t *session_item;
        char err_msg[PRTE_SLURM_ERR_STR_MAX_LEN + 1] = {0};
        int err;

        if (NULL == action->job_id) {
            continue;
        }

        session_item = prte_ras_slurm_find_session_item_by_alloc_id(action->job_id);
        if (NULL == session_item || NULL == session_item->session) {
            continue;
        }

        if (PRTE_RAS_SLURM_RELEASE_FULL_JOB == action->action) {
            int node_count = session_item->nodes_in_session;

            if (!prte_ras_slurm_session_is_dynamic(session_item->session)) {
                pmix_output(0, "ras:slurm:shrink_complete: refusing to terminate job %s "
                               "because it was not dynamically added by PRRTE.",
                            action->job_id);
                continue;
            }

            PMIX_RELEASE(session_item->session);
            pmix_list_remove_item(prte_slurm_session_stack, &session_item->super);
            prte_num_allocated_nodes -= node_count;
            PMIX_RELEASE(session_item);
        } else if (PRTE_RAS_SLURM_RELEASE_PARTIAL_JOB == action->action) {
            pmix_pointer_array_t nodes_in_removal;
            int old_count = session_item->nodes_in_session;
            int new_count = PMIx_Argv_count(action->survivor_nodes);

            if (0 >= new_count || old_count <= new_count) {
                continue;
            }

            err = prte_ras_slurm_shrink_job_to_survivors(action->job_id,
                                                         action->survivor_nodes,
                                                         err_msg, sizeof(err_msg));
            if (PRTE_SUCCESS != err) {
                if (PRTE_ERR_SLURM_SHRINK_FAILURE == err) {
                    pmix_output(0, "ras:slurm:shrink_complete: failed to shrink job %s: %s.",
                                action->job_id, err_msg);
                } else {
                    PRTE_ERROR_LOG(err);
                }
                continue;
            }

            PMIX_CONSTRUCT(&nodes_in_removal, pmix_pointer_array_t);
            err = prte_ras_slurm_detach_nodes(action->job_id, session_item->session,
                                              &nodes_in_removal);
            PMIX_DESTRUCT(&nodes_in_removal);
            if (PRTE_SUCCESS != err) {
                PRTE_ERROR_LOG(err);
                continue;
            }

            session_item->nodes_in_session = new_count;
            prte_num_allocated_nodes -= old_count - new_count;
        }
    }

    prte_ras_slurm_untrack_shrink_campaign(found);
    PMIX_RELEASE(found);
}

/**
 * @brief Process a PMIx resource release request.
 */
int prte_ras_slurm_serve_release_req(prte_pmix_server_req_t *req)
{
    if(!prte_ras_slurm_have_jansson()) {
        pmix_output(0, "ras:slurm:modify: "
            "Jansson support is required but not enabled in this build");
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (!prte_elastic_mode) {
        pmix_output(0, "ras:slurm:modify: release requests require elastic DVM mode. "
                       "Set the prte_elastic_mode MCA parameter to 1.");
        return PRTE_ERR_NOT_SUPPORTED;
    }

    int err = PRTE_SUCCESS;

    uint64_t num_nodes = 0;
    const char *alloc_id = NULL;
    char **nodes = NULL;
    bool found_num_nodes = false;
    bool found_alloc_id = false;
    bool found_node_list = false;

    for (size_t i = 0; i < req->ninfo; i++) {
        if (0 == strcmp(req->info[i].key, PMIX_ALLOC_NUM_NODES)) {

            if (PMIX_UINT64 != req->info[i].value.type) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }

            num_nodes = req->info[i].value.data.uint64;
            found_num_nodes = true;
        } else if (0 == strcmp(req->info[i].key, PMIX_ALLOC_ID)) {

            if (PMIX_STRING != req->info[i].value.type) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }

            alloc_id = req->info[i].value.data.string;
            found_alloc_id = true;
        } else if (0 == strcmp(req->info[i].key, PMIX_ALLOC_NODE_LIST)) {

            err = prte_ras_slurm_parse_release_node_list(&req->info[i], &nodes);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            found_node_list = true;
        } else if (0 == strcmp(req->info[i].key, PMIX_ALLOC_REQ_ID)) {
            continue;
        }
    }

    if (1 != (int) found_num_nodes + (int) found_alloc_id + (int) found_node_list) {
        pmix_output(0, "ras:slurm:modify: modify request invalid or unsupported.\n"
                       "Supported options: exactly one of PMIX_ALLOC_NODE_LIST, "
                       "PMIX_ALLOC_ID, or PMIX_ALLOC_NUM_NODES");
        err = PRTE_ERR_REQUEST;
        goto cleanup;
    }

    if (found_node_list) {
        err = prte_ras_slurm_remove_nodes_by_name(req, nodes);
    } else if (found_alloc_id) {
        err = prte_ras_slurm_remove_allocation_by_id(req, alloc_id);
    } else {
        err = prte_ras_slurm_remove_nodes_by_count(req, num_nodes);
    }

    if(err != PRTE_SUCCESS) {
        goto cleanup;
    }

cleanup:

    if (NULL != nodes) {
        PMIx_Argv_free(nodes);
    }

    return err;
}

/**
 * @brief Parse a PMIX_ALLOC_NODE_LIST release selector.
 *
 * @param[in] info PMIx info carrying the node-list value.
 * @param[out] nodes Parsed node names.
 */
static int prte_ras_slurm_parse_release_node_list(pmix_info_t *info, char ***nodes)
{
    char **parsed_nodes = NULL;
    char *ndstring = NULL;
    int rc;

    if (NULL == info || NULL == nodes) {
        return PRTE_ERR_BAD_PARAM;
    }

    *nodes = NULL;

    if (PMIX_STRING == info->value.type ||
        PMIX_REGEX == info->value.type) {
        rc = pmix_preg.parse_nodes(info->value.data.string, &parsed_nodes);
        if (PMIX_SUCCESS != rc) {
            return prte_pmix_convert_status(rc);
        }
        ndstring = PMIx_Argv_join(parsed_nodes, ',');
        PMIx_Argv_free(parsed_nodes);
        parsed_nodes = NULL;

#if PRTE_PMIX_HAVE_REGEX2
    } else if (PMIX_REGEX2 == info->value.type) {
        rc = PMIx_parse_regex2(info->value.data.regex2, NULL, 0, &ndstring);
        if (PMIX_SUCCESS != rc) {
            return prte_pmix_convert_status(rc);
        }
#endif

    } else {
        return PRTE_ERR_BAD_PARAM;
    }

    if (NULL == ndstring || '\0' == ndstring[0]) {
        free(ndstring);
        return PRTE_ERR_NOT_FOUND;
    }

    *nodes = PMIx_Argv_split(ndstring, ',');
    free(ndstring);

    if (NULL == *nodes || 0 == PMIx_Argv_count(*nodes)) {
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Find the Slurm session stack item for an allocation ID.
 *
 * @param[in] alloc_id Slurm allocation/job ID.
 */
static prte_session_stack_item_t *prte_ras_slurm_find_session_item_by_alloc_id(const char *alloc_id)
{
    prte_session_stack_item_t *item;

    if (NULL == prte_slurm_session_stack || NULL == alloc_id) {
        return NULL;
    }

    PMIX_LIST_FOREACH(item, prte_slurm_session_stack, prte_session_stack_item_t) {
        if (NULL != item->session && NULL != item->session->alloc_refid &&
            0 == strcmp(item->session->alloc_refid, alloc_id)) {
            return item;
        }
    }

    return NULL;
}

/**
 * @brief Find the Slurm session stack item containing a node.
 *
 * @param[in] node_name Node name to find.
 */
static prte_session_stack_item_t *prte_ras_slurm_find_session_item_by_node(const char *node_name)
{
    prte_session_stack_item_t *item;

    if (NULL == prte_slurm_session_stack || NULL == node_name) {
        return NULL;
    }

    PMIX_LIST_FOREACH(item, prte_slurm_session_stack, prte_session_stack_item_t) {
        prte_session_t *session = item->session;

        if (NULL == session || NULL == session->nodes) {
            continue;
        }

        for (int i = 0; i < session->nodes->size; i++) {
            prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

            if (NULL != node && NULL != node->name &&
                0 == strcmp(node->name, node_name)) {
                return item;
            }
        }
    }

    return NULL;
}

/**
 * @brief Check whether a node name appears in an argv.
 *
 * @param[in] nodes NULL-terminated node-name array.
 * @param[in] node_name Node name to find.
 */
static bool prte_ras_slurm_node_in_argv(char **nodes, const char *node_name)
{
    if (NULL == nodes || NULL == node_name) {
        return false;
    }

    for (int i = 0; NULL != nodes[i]; i++) {
        if (0 == strcmp(nodes[i], node_name)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Count requested nodes that belong to a Slurm session.
 *
 * @param[in] session Session to inspect.
 * @param[in] nodes Requested node names.
 */
static int prte_ras_slurm_count_matching_session_nodes(prte_session_t *session, char **nodes)
{
    int matches = 0;

    if (NULL == session || NULL == session->nodes || NULL == nodes) {
        return 0;
    }

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

        if (NULL != node && NULL != node->name &&
            prte_ras_slurm_node_in_argv(nodes, node->name)) {
            matches++;
        }
    }

    return matches;
}

/**
 * @brief Build survivor node names for an exact node removal.
 *
 * @param[in] session Slurm session to inspect.
 * @param[in] nodes_to_remove Node names selected for release.
 * @param[out] survivors Nodes that should remain in the Slurm allocation.
 */
static int prte_ras_slurm_build_survivor_list(prte_session_t *session, char **nodes_to_remove,
                                              char ***survivors)
{
    pmix_status_t pmix_err;

    if (NULL == session || NULL == session->nodes || NULL == survivors) {
        return PRTE_ERR_BAD_PARAM;
    }

    *survivors = NULL;

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *node = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, i);

        if (NULL == node || NULL == node->name ||
            prte_ras_slurm_node_in_argv(nodes_to_remove, node->name)) {
            continue;
        }

        pmix_err = PMIx_Argv_append_nosize(survivors, node->name);
        if (PMIX_SUCCESS != pmix_err) {
            PMIx_Argv_free(*survivors);
            *survivors = NULL;
            return prte_pmix_convert_status(pmix_err);
        }
    }

    if (NULL == *survivors || 0 == PMIx_Argv_count(*survivors)) {
        return PRTE_ERR_BAD_PARAM;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Remove nodes from Slurm allocations by exact node name.
 *
 * @param[in] nodes Node names to remove.
 */
static int prte_ras_slurm_remove_nodes_by_name(prte_pmix_server_req_t *req, char **nodes)
{
    int err = PRTE_SUCCESS;
    int node_count;
    char *launching_jobid;
    char *protected_node = NULL;
    char **target_nodes = NULL;
    char **processed_jobs = NULL;
    prte_ras_slurm_shrink_tracker_t *tracker = NULL;

    if (NULL == nodes || 0 == (node_count = PMIx_Argv_count(nodes))) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (node_count >= prte_num_allocated_nodes) {
        pmix_output(0, "ras:slurm:remove_nodes_by_name: cannot remove all allocated nodes or more.");
        return PRTE_ERR_REQUEST;
    }

    if (NULL == (launching_jobid = getenv("SLURM_JOBID"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    for (int i = 0; NULL != nodes[i]; i++) {
        prte_session_stack_item_t *session_item;

        err = prte_ras_slurm_validate_hostname(nodes[i]);
        if (PRTE_SUCCESS != err) {
            return err;
        }

        for (int j = i + 1; NULL != nodes[j]; j++) {
            if (0 == strcmp(nodes[i], nodes[j])) {
                return PRTE_ERR_BAD_PARAM;
            }
        }

        session_item = prte_ras_slurm_find_session_item_by_node(nodes[i]);
        if (NULL == session_item || NULL == session_item->session ||
            NULL == session_item->session->alloc_refid) {
            return PRTE_ERR_NOT_FOUND;
        }

        if (prte_ras_slurm_jobid_has_active_shrink(session_item->session->alloc_refid)) {
            return PRTE_ERR_RESOURCE_BUSY;
        }

        if (prte_ras_slurm_session_contains_invoker(session_item->session, launching_jobid)) {
            if (NULL == protected_node) {
                protected_node = getenv("SLURMD_NODENAME");
                if (NULL == protected_node) {
                    return PRTE_ERR_BAD_PARAM;
                }
            }

            if (0 == strcmp(nodes[i], protected_node)) {
                pmix_output(0, "ras:slurm:remove_nodes_by_name: refusing to remove current node %s",
                            protected_node);
                return PRTE_ERR_BAD_PARAM;
            }
        }
    }

    tracker = PMIX_NEW(prte_ras_slurm_shrink_tracker_t);
    if (NULL == tracker) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    for (int i = 0; NULL != nodes[i]; i++) {
        prte_session_stack_item_t *session_item;
        prte_session_t *session;
        int selected;
        pmix_status_t pmix_err;

        session_item = prte_ras_slurm_find_session_item_by_node(nodes[i]);
        if (NULL == session_item || NULL == session_item->session ||
            NULL == session_item->session->alloc_refid) {
            err = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }

        session = session_item->session;
        if (prte_ras_slurm_node_in_argv(processed_jobs, session->alloc_refid)) {
            continue;
        }

        pmix_err = PMIx_Argv_append_nosize(&processed_jobs, session->alloc_refid);
        if (PMIX_SUCCESS != pmix_err) {
            err = prte_pmix_convert_status(pmix_err);
            goto cleanup;
        }

        selected = prte_ras_slurm_count_matching_session_nodes(session, nodes);
        if (0 == selected) {
            continue;
        }

        if (selected == session_item->nodes_in_session) {
            if (!prte_ras_slurm_session_is_dynamic(session)) {
                pmix_output(0, "ras:slurm:remove_nodes_by_name: refusing to terminate job %s "
                               "because it was not dynamically added by PRRTE",
                            session->alloc_refid);
                err = PRTE_ERR_NOT_SUPPORTED;
                goto cleanup;
            }

            if (prte_ras_slurm_session_contains_invoker(session, launching_jobid)) {
                pmix_output(0, "ras:slurm:remove_nodes_by_name: refusing to kill job %s "
                               "because it contains the invoking process",
                            session->alloc_refid);
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }

            err = prte_ras_slurm_append_release_targets(session, nodes, &target_nodes);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            err = prte_ras_slurm_add_release_action(tracker, session->alloc_refid,
                                                    PRTE_RAS_SLURM_RELEASE_FULL_JOB,
                                                    NULL);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }
        } else {
            char **survivors = NULL;

            err = prte_ras_slurm_append_release_targets(session, nodes, &target_nodes);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            err = prte_ras_slurm_build_survivor_list(session, nodes, &survivors);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            err = prte_ras_slurm_add_release_action(tracker, session->alloc_refid,
                                                    PRTE_RAS_SLURM_RELEASE_PARTIAL_JOB,
                                                    survivors);
            PMIx_Argv_free(survivors);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }
        }
    }

    err = prte_ras_slurm_start_shrink(req, tracker, target_nodes);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    tracker = NULL;
    err = prte_ras_slurm_complete_release_request(req);

cleanup:
    if (NULL != processed_jobs) {
        PMIx_Argv_free(processed_jobs);
    }
    if (NULL != target_nodes) {
        PMIx_Argv_free(target_nodes);
    }
    if (NULL != tracker) {
        PMIX_RELEASE(tracker);
    }
    return err;
}

/**
 * @brief Remove a whole Slurm allocation by allocation ID.
 *
 * @param[in] alloc_id Slurm allocation/job ID.
 */
static int prte_ras_slurm_remove_allocation_by_id(prte_pmix_server_req_t *req, const char *alloc_id)
{
    prte_session_stack_item_t *session_item;
    char *launching_jobid;
    char **target_nodes = NULL;
    prte_ras_slurm_shrink_tracker_t *tracker = NULL;
    int err;

    if (NULL == alloc_id || '\0' == alloc_id[0]) {
        return PRTE_ERR_BAD_PARAM;
    }

    session_item = prte_ras_slurm_find_session_item_by_alloc_id(alloc_id);
    if (NULL == session_item || NULL == session_item->session) {
        return PRTE_ERR_NOT_FOUND;
    }

    if (!prte_ras_slurm_session_is_dynamic(session_item->session)) {
        pmix_output(0, "ras:slurm:remove_allocation_by_id: refusing to terminate job %s "
                       "because it was not dynamically added by PRRTE",
                    alloc_id);
        return PRTE_ERR_NOT_SUPPORTED;
    }

    if (session_item->nodes_in_session >= prte_num_allocated_nodes) {
        pmix_output(0, "ras:slurm:remove_allocation_by_id: cannot remove all allocated nodes.");
        return PRTE_ERR_REQUEST;
    }

    if (prte_ras_slurm_jobid_has_active_shrink(alloc_id)) {
        return PRTE_ERR_RESOURCE_BUSY;
    }

    if (NULL == (launching_jobid = getenv("SLURM_JOBID"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    if (prte_ras_slurm_session_contains_invoker(session_item->session, launching_jobid)) {
        pmix_output(0, "ras:slurm:remove_allocation_by_id: refusing to kill job %s "
                       "because it contains the invoking process", alloc_id);
        return PRTE_ERR_BAD_PARAM;
    }

    tracker = PMIX_NEW(prte_ras_slurm_shrink_tracker_t);
    if (NULL == tracker) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    err = prte_ras_slurm_add_release_action(tracker, alloc_id,
                                            PRTE_RAS_SLURM_RELEASE_FULL_JOB,
                                            NULL);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    err = prte_ras_slurm_append_session_targets(session_item->session, &target_nodes);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    err = prte_ras_slurm_start_shrink(req, tracker, target_nodes);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    tracker = NULL;
    err = prte_ras_slurm_complete_release_request(req);

cleanup:
    if (NULL != target_nodes) {
        PMIx_Argv_free(target_nodes);
    }
    if (NULL != tracker) {
        PMIX_RELEASE(tracker);
    }

    return err;
}

/**
 * @brief Remove nodes from Slurm allocations by count.
 *
 * Removes nodes from the most recently added Slurm allocation first. Whole
 * allocations are cancelled when possible; otherwise the newest allocation is
 * shrunk.
 *
 * @param[in] node_count Number of nodes to remove.
 */
static int prte_ras_slurm_remove_nodes_by_count(prte_pmix_server_req_t *req, uint64_t node_count) {
    prte_ras_slurm_shrink_tracker_t *tracker = NULL;
    char **processed_jobs = NULL;
    char **target_nodes = NULL;
    int err = PRTE_SUCCESS;

    if(0 == node_count || node_count > INT_MAX) {
        pmix_output(0, "ras:slurm:remove_nodes_by_count: invalid node count.");
        return PRTE_ERR_REQUEST;
    }

    int nodes_initial = prte_num_allocated_nodes;
    int nodes_to_remove = (int)node_count;
    
    if(nodes_to_remove >= nodes_initial) {
        pmix_output(0, "ras:slurm:remove_nodes_by_count: cannot remove all allocated nodes or more.");
        return PRTE_ERR_REQUEST;
    }

    char *launching_jobid;
    if (NULL == (launching_jobid = getenv("SLURM_JOBID"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    err = prte_ras_slurm_validate_count_release(nodes_to_remove, launching_jobid);
    if (PRTE_SUCCESS != err) {
        if (PRTE_ERR_RESOURCE_BUSY == err) {
            pmix_output(0, "ras:slurm:remove_nodes_by_count: insufficient non-conflicting "
                           "Slurm resources available for release");
        }
        PRTE_ERROR_LOG(err);
        return err;
    }

    tracker = PMIX_NEW(prte_ras_slurm_shrink_tracker_t);
    if (NULL == tracker) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    int nodes_removed = 0;

    do {
        if (0 == pmix_list_get_size(prte_slurm_session_stack)) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        prte_session_stack_item_t *session_item =
            prte_ras_slurm_find_releasable_session(launching_jobid, processed_jobs);

        if(NULL == session_item || NULL == session_item->session) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;   
        }

        prte_session_t *session = session_item->session;

        int nodes_left_to_rem = nodes_to_remove-nodes_removed;

        bool contains_invoker = prte_ras_slurm_session_contains_invoker(session,
                                                                        launching_jobid);
        int removable_count = prte_ras_slurm_session_removable_count(session_item,
                                                                     launching_jobid);

        if(prte_ras_slurm_session_is_dynamic(session) && !contains_invoker &&
           nodes_left_to_rem >= session_item->nodes_in_session) {
            pmix_status_t pmix_err;

            /* Case 1: remove the whole Slurm job after the DVM shrink. */
            err = prte_ras_slurm_add_release_action(tracker, session->alloc_refid,
                                                    PRTE_RAS_SLURM_RELEASE_FULL_JOB,
                                                    NULL);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            err = prte_ras_slurm_append_session_targets(session, &target_nodes);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            pmix_err = PMIx_Argv_append_nosize(&processed_jobs, session->alloc_refid);
            if (PMIX_SUCCESS != pmix_err) {
                err = prte_pmix_convert_status(pmix_err);
                goto cleanup;
            }

            nodes_removed += session_item->nodes_in_session;
        } else {
            char **session_targets = NULL;
            char **survivors = NULL;
            pmix_status_t pmix_err;
            int nodes_to_rem_from_session = nodes_left_to_rem;

            if (nodes_to_rem_from_session > removable_count) {
                nodes_to_rem_from_session = removable_count;
            }

            /* Case 2: keep the Slurm job alive and shrink it to its survivors. */
            char *protected_node = NULL;

            if(contains_invoker) {
                protected_node = getenv("SLURMD_NODENAME");

                if (NULL == protected_node) {
                    pmix_output(0,
                                "ras:slurm:remove_nodes_by_count: SLURMD_NODENAME is not set. "
                                "Refusing to shrink job %s because the current node cannot be "
                                "excluded from the resize operation.",
                                session->alloc_refid);
                    err = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(err);
                    goto cleanup; 
                }
            }

            err = prte_ras_slurm_append_count_session_targets(session, protected_node,
                                                             nodes_to_rem_from_session,
                                                             &session_targets);
            if (PRTE_SUCCESS != err) {
                PMIx_Argv_free(session_targets);
                goto cleanup;
            }

            for (int i = 0; NULL != session_targets[i]; i++) {
                pmix_err = PMIx_Argv_append_nosize(&target_nodes,
                                                   session_targets[i]);
                if (PMIX_SUCCESS != pmix_err) {
                    err = prte_pmix_convert_status(pmix_err);
                    PMIx_Argv_free(session_targets);
                    goto cleanup;
                }
            }

            err = prte_ras_slurm_build_survivor_list(session, session_targets, &survivors);
            if (PRTE_SUCCESS != err) {
                PMIx_Argv_free(session_targets);
                goto cleanup;
            }

            err = prte_ras_slurm_add_release_action(tracker, session->alloc_refid,
                                                    PRTE_RAS_SLURM_RELEASE_PARTIAL_JOB,
                                                    survivors);
            PMIx_Argv_free(survivors);
            PMIx_Argv_free(session_targets);
            if (PRTE_SUCCESS != err) {
                goto cleanup;
            }

            pmix_err = PMIx_Argv_append_nosize(&processed_jobs, session->alloc_refid);
            if (PMIX_SUCCESS != pmix_err) {
                err = prte_pmix_convert_status(pmix_err);
                goto cleanup;
            }

            nodes_removed += nodes_to_rem_from_session;
        }
    } while (nodes_to_remove > nodes_removed);

    err = prte_ras_slurm_start_shrink(req, tracker, target_nodes);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    tracker = NULL;
    err = prte_ras_slurm_complete_release_request(req);

cleanup:
    if (NULL != processed_jobs) {
        PMIx_Argv_free(processed_jobs);
    }
    if (NULL != target_nodes) {
        PMIx_Argv_free(target_nodes);
    }
    if (NULL != tracker) {
        PMIX_RELEASE(tracker);
    }

    return err;
}

static void prte_ras_slurm_cleanup_resize_scripts(const char *slurm_jobid)
{
    char *resize_script_sh = NULL;
    char *resize_script_csh = NULL;
    static const char *resize_script_format = "slurm_job_%s_resize.%s";

    if (NULL == slurm_jobid) {
        return;
    }

    if (0 <= asprintf(&resize_script_sh, resize_script_format, slurm_jobid, "sh")) {
        if (0 != remove(resize_script_sh)) {
            int saved_errno = errno;
            if (ENOENT != saved_errno) {
                pmix_output(0,
                            "ras:slurm:shrink_job: failed to remove helper script %s: %s.",
                            resize_script_sh, strerror(saved_errno));
            }
        }
    }

    if (0 <= asprintf(&resize_script_csh, resize_script_format, slurm_jobid, "csh")) {
        if (0 != remove(resize_script_csh)) {
            int saved_errno = errno;
            if (ENOENT != saved_errno) {
                pmix_output(0,
                            "ras:slurm:shrink_job: failed to remove helper script %s: %s.",
                            resize_script_csh, strerror(saved_errno));
            }
        }
    }

    free(resize_script_sh);
    free(resize_script_csh);
}

/**
 * @brief Shrink a Slurm job to an exact survivor node list.
 *
 * @param[in] slurm_jobid Slurm job ID to resize.
 * @param[in] survivor_nodes Nodes that must remain in the job.
 * @param[out] err_msg Optional buffer for Slurm error output.
 * @param[in] err_msg_size Size of err_msg buffer, if applicable.
 */
static int prte_ras_slurm_shrink_job_to_survivors(const char *slurm_jobid, char **survivor_nodes,
                                                  char *err_msg, size_t err_msg_size)
{
    int err = PRTE_SUCCESS;
    char *survivor_string = NULL;
    char *req_nodes_arg = NULL;
    char *cmd = NULL;
    FILE *fp = NULL;
    static const char *cmd_format = "scontrol update job %s %s 2>&1";

    if (NULL == slurm_jobid || NULL == survivor_nodes ||
        0 == PMIx_Argv_count(survivor_nodes)) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (NULL != err_msg) {
        err_msg[0] = '\0';
    }

    err = prte_ras_slurm_validate_jobid(slurm_jobid);
    if (PRTE_SUCCESS != err) {
        return err;
    }

    for (int i = 0; NULL != survivor_nodes[i]; i++) {
        err = prte_ras_slurm_validate_hostname(survivor_nodes[i]);
        if (PRTE_SUCCESS != err) {
            return err;
        }
    }

    survivor_string = PMIx_Argv_join(survivor_nodes, ',');
    if (NULL == survivor_string) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (0 > asprintf(&req_nodes_arg, "ReqNodeList=%s", survivor_string)) {
        req_nodes_arg = NULL;
        err = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    if (0 > asprintf(&cmd, cmd_format, slurm_jobid, req_nodes_arg)) {
        cmd = NULL;
        err = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    PMIX_OUTPUT_VERBOSE((20, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:shrink_job: shrink command is:\n%s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cmd));

    fp = popen(cmd, "r");
    if (NULL == fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        goto cleanup;
    }

    err = prte_ras_slurm_drain_cmd_output(fp, err_msg, err_msg_size);
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    int status = pclose(fp);
    fp = NULL;

    if (-1 == status) {
        pmix_output(0, "ras:slurm:shrink_job: pclose failed: %s.", strerror(errno));
        err = PRTE_ERR_IN_ERRNO;
        goto cleanup;
    }

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        err = PRTE_ERR_SLURM_SHRINK_FAILURE;
        goto cleanup;
    }

    prte_ras_slurm_cleanup_resize_scripts(slurm_jobid);

cleanup:
    if (NULL != err_msg && PRTE_ERR_SLURM_SHRINK_FAILURE != err) {
        err_msg[0] = '\0';
    }

    if (NULL != fp) {
        pclose(fp);
    }

    free(survivor_string);
    free(req_nodes_arg);
    free(cmd);

    return err;
}
