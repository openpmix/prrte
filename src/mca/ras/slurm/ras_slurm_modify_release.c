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

/* Local functions */
static void release_action_con(prte_ras_slurm_release_action_t *p);
static void release_action_des(prte_ras_slurm_release_action_t *p);
static void shrink_tracker_con(prte_ras_slurm_shrink_tracker_t *p);
static void shrink_tracker_des(prte_ras_slurm_shrink_tracker_t *p);
static bool prte_ras_slurm_tracker_has_jobid(prte_ras_slurm_shrink_tracker_t *tracker,
                                             const char *jobid);
static bool prte_ras_slurm_jobid_has_active_shrink(const char *jobid);
static bool prte_ras_slurm_session_contains_invoker(prte_session_t *session,
                                                    const char *launching_jobid);
static int prte_ras_slurm_session_removable_count(prte_session_stack_item_t *session_item,
                                                  const char *launching_jobid);
static prte_session_stack_item_t *prte_ras_slurm_find_releasable_session(const char *launching_jobid);
static int prte_ras_slurm_validate_count_release(int nodes_to_remove,
                                                 const char *launching_jobid);
static int prte_ras_slurm_remove_nodes_by_count(uint64_t node_count);
static int prte_ras_slurm_shrink_job(const char *slurm_jobid, const char *exclude_hostname, int new_node_count, char *err_msg, size_t err_msg_size);
static int prte_ras_slurm_build_req_nodelist(const char *slurm_jobid, const char *protected_hostname, int new_node_count, char **req_nodes_arg);

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
static bool prte_ras_slurm_jobid_has_active_shrink(const char *jobid)
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

    if (prte_ras_slurm_session_contains_invoker(session_item->session, launching_jobid)) {
        if (1 >= session_item->nodes_in_session) {
            return 0;
        }
        return session_item->nodes_in_session - 1;
    }

    return session_item->nodes_in_session;
}

/**
 * @brief Find the newest Slurm session with removable nodes.
 *
 * @param[in] launching_jobid Slurm job ID whose current node must survive.
 */
static prte_session_stack_item_t *prte_ras_slurm_find_releasable_session(const char *launching_jobid)
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
 * @brief Process a PMIx resource release request.
 */
int prte_ras_slurm_serve_release_req(prte_pmix_server_req_t *req)
{
    if(!prte_ras_slurm_have_jansson()) {
        pmix_output(0, "ras:slurm:modify: "
            "Jansson support is required but not enabled in this build");
        return PRTE_ERR_NOT_AVAILABLE;
    }

    int err = PRTE_SUCCESS;

    uint64_t num_nodes = 0;
    bool found = false;

    for (size_t i = 0; i < req->ninfo; i++) {
        if (0 != strcmp(req->info[i].key, PMIX_ALLOC_NUM_NODES)) {
            continue;
        }

        if (PMIX_UINT64 != req->info[i].value.type) {
            err = PRTE_ERR_BAD_PARAM;
            goto cleanup;
        }
    
        num_nodes = req->info[i].value.data.uint64;
        found = true;
        break;
    }

    if(!found) {
        pmix_output(0, "ras:slurm:modify: modify request invalid or unsupported.\n"
                       "Supported options: PMIX_ALLOC_NUM_NODES");
        err = PRTE_ERR_REQUEST;
        goto cleanup;
    }

    err = prte_ras_slurm_remove_nodes_by_count(num_nodes);

    if(err != PRTE_SUCCESS) {
        goto cleanup;
    }

cleanup:

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
static int prte_ras_slurm_remove_nodes_by_count(uint64_t node_count) {

    if(0 == node_count || node_count > INT_MAX) {
        pmix_output(0, "ras:slurm:remove_nodes_by_count: invalid node count.");
        return PRTE_ERR_REQUEST;
    }

    int err = PRTE_SUCCESS;

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

    int nodes_removed = 0;

    do {
        if (0 == pmix_list_get_size(prte_slurm_session_stack)) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        prte_session_stack_item_t *session_item =
            prte_ras_slurm_find_releasable_session(launching_jobid);

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

        char err_msg[PRTE_SLURM_ERR_STR_MAX_LEN+1] = {0};

        if(!contains_invoker && nodes_left_to_rem >= session_item->nodes_in_session) {
            err = prte_ras_slurm_kill_job(session->alloc_refid, err_msg, PRTE_SLURM_ERR_STR_MAX_LEN+1);

            if (PRTE_SUCCESS != err) {

                if(PRTE_ERR_SLURM_CANCEL_FAILURE == err) {
                    pmix_output(0, "ras:slurm:remove_nodes_by_count: failed to kill job %s: %s.",
                                    session->alloc_refid, err_msg);
                } else {
                    PRTE_ERROR_LOG(err);
                }

                goto cleanup;
            }

            pmix_list_remove_item(prte_slurm_session_stack, &session_item->super);
            
            /* TODO: nodes in session need to be detached
             * from PRRTE. */

            nodes_removed += session_item->nodes_in_session;

            /* This also removes from prte_sessions */
            PMIX_RELEASE(session);
            PMIX_RELEASE(session_item);
        } else {

            int nodes_to_rem_from_session = nodes_left_to_rem;

            if (nodes_to_rem_from_session > removable_count) {
                nodes_to_rem_from_session = removable_count;
            }

            int new_node_count = session_item->nodes_in_session-nodes_to_rem_from_session;

            char *whitelist_node = NULL;

            if(contains_invoker) {
                whitelist_node = getenv("SLURMD_NODENAME");

                if (NULL == whitelist_node) {
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

            err = prte_ras_slurm_shrink_job(session->alloc_refid, whitelist_node, new_node_count, err_msg, PRTE_SLURM_ERR_STR_MAX_LEN+1);

            if (PRTE_SUCCESS != err) {

                if(PRTE_ERR_SLURM_SHRINK_FAILURE == err) {
                    pmix_output(0, "ras:slurm:remove_nodes_by_count: failed to shrink job %s" 
                                    ": %s.", session->alloc_refid, err_msg);
                } else {
                    PRTE_ERROR_LOG(err);
                }

                goto cleanup;
            }

            pmix_pointer_array_t nodes_in_removal;
            PMIX_CONSTRUCT(&nodes_in_removal, pmix_pointer_array_t);

            /* Remove target nodes from session and add to nodes_in_removal */
            err = prte_ras_slurm_detach_nodes(session->alloc_refid, session, &nodes_in_removal);

            if (PRTE_SUCCESS != err) {
                PMIX_DESTRUCT(&nodes_in_removal);
                goto cleanup;
            }
            
            /* TODO: use nodes_in_removal 
             * to detach removed nodes from PRRTE */

            PMIX_DESTRUCT(&nodes_in_removal);

            session_item->nodes_in_session = new_node_count;
            nodes_removed += nodes_to_rem_from_session;
        }
    } while (nodes_to_remove > nodes_removed);

cleanup:

    prte_num_allocated_nodes-=nodes_removed;

    if(PRTE_SUCCESS != err && nodes_removed > 0) {
        err = PRTE_ERR_PARTIAL_SUCCESS;
    }

    return err;
}

/**
 * @brief Shrink a Slurm job to a new node count.
 *
 * Runs scontrol to resize the given Slurm job. On Slurm resize failure,
 * err_msg is populated with the command output when provided. This failure
 * mode is indicated by a return of PRTE_ERR_SLURM_SHRINK_FAILURE.
 *
 * @param[in] slurm_jobid Slurm job ID to resize.
 * @param[in] exclude_hostname Hostname to exclude from resize.
 * @param[in] new_node_count New number of nodes for the job.
 * @param[out] err_msg Optional buffer for Slurm error output.
 * @param[in] err_msg_size Size of err_msg buffer, if applicable.
 */
static int prte_ras_slurm_shrink_job(const char *slurm_jobid, const char *exclude_hostname, int new_node_count, char *err_msg, size_t err_msg_size) {

    if(NULL == slurm_jobid || 0 >= new_node_count) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    if(NULL != err_msg) {
        err_msg[0] = '\0';
    }

    int err = PRTE_SUCCESS;

    /* Make sure the job ID given is something reasonable */
    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:shrink_job: shrink job %s to %d nodes",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), slurm_jobid, new_node_count));

    char *resize_script_sh = NULL;
    char *resize_script_csh = NULL;

    char *req_nodes_arg = NULL; 
    char *num_nodes_arg = NULL;

    char *cmd = NULL;

    static const char *cmd_format = "scontrol update job %s %s 2>&1";

    FILE *fp = NULL;

    /* if we want to exclude a node from the shrink,
     * then we have to specify all nodes to keep explicitly */
    if(NULL != exclude_hostname) {

        err = prte_ras_slurm_build_req_nodelist(slurm_jobid, exclude_hostname, new_node_count, &req_nodes_arg);

        if (err != PRTE_SUCCESS) {
            goto cleanup;
        }

        if(0 > asprintf(&cmd, cmd_format, slurm_jobid, req_nodes_arg)) {
            cmd = NULL;
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        free(req_nodes_arg);
        req_nodes_arg = NULL;

    } else {

        static const char *num_nodes_format = "NumNodes=%d";

        if(0 > asprintf(&num_nodes_arg, num_nodes_format, new_node_count)) {
            num_nodes_arg = NULL;
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        if(0 > asprintf(&cmd, cmd_format, slurm_jobid, num_nodes_arg)) {
            cmd = NULL;
            err = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        free(num_nodes_arg);
        num_nodes_arg = NULL;
    }

    PMIX_OUTPUT_VERBOSE((20, prte_ras_base_framework.framework_output,
                        "%s ras:slurm:shrink_job: shrink command is:\n%s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cmd));

    fp = popen(cmd, "r");

    if(NULL == fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    err = prte_ras_slurm_drain_cmd_output(fp, err_msg, err_msg_size);

    if (PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    int status = pclose(fp);
    fp = NULL;

    if (-1 == status) {
        pmix_output(0, "ras:slurm:shrink_job: pclose failed: %s.", strerror(errno));
        err = PRTE_ERR_IN_ERRNO;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        err = PRTE_ERR_SLURM_SHRINK_FAILURE;
        goto cleanup;
    }

    /* On success, the resize operation creates some helper 
     * scripts to update environment variables which
     * we do not want cluttering the user environment.
     * 
     * NOTE: return success even if we fail here, as the
     * shrink itself succeeded and the resources are gone. */

    static const char *resize_script_format = "slurm_job_%s_resize.%s";

    int rc = asprintf(&resize_script_sh, resize_script_format, slurm_jobid, "sh");

    if(0 > rc) {
        pmix_output(0, "ras:slurm:shrink_job: asprintf failed after successful shrink.");
        resize_script_sh = NULL;
        goto cleanup;
    }

    if (0 != remove(resize_script_sh)) {
        int saved_errno = errno;
        if (ENOENT != saved_errno) {
            pmix_output(0,
                        "ras:slurm:shrink_job: failed to remove helper script %s: %s.",
                        resize_script_sh, strerror(saved_errno));
        }
    }

    rc = asprintf(&resize_script_csh, resize_script_format, slurm_jobid, "csh");

    if(0 > rc) {
        pmix_output(0, "ras:slurm:shrink_job: asprintf failed after successful shrink.");
        resize_script_csh = NULL;
        goto cleanup;
    }

    if (0 != remove(resize_script_csh)) {
        int saved_errno = errno;
        if (ENOENT != saved_errno) {
            pmix_output(0,
                        "ras:slurm:shrink_job: failed to remove helper script %s: %s.",
                        resize_script_csh, strerror(saved_errno));
        }
    }

cleanup:

    if(NULL != err_msg && PRTE_ERR_SLURM_SHRINK_FAILURE != err) {
        err_msg[0] = '\0';
    }

    if(NULL != fp) {
        pclose(fp);
    }

    free(cmd);
    free(resize_script_sh);
    free(resize_script_csh);
    free(req_nodes_arg);
    free(num_nodes_arg);

    return err;
}

/**
 * Build a required-node list for a Slurm shrink operation.
 *
 * Ensures that protected_hostname is included in the resulting
 * comma-separated node list. Validates each hostname before it is
 * included in the output string.
 *
 * @param[in]  slurm_jobid         Slurm job ID.
 * @param[in]  protected_hostname  Hostname to preserve.
 * @param[out] req_nodes_arg       Allocated string of the form
 *                                 "ReqNodeList=node0,node1,...".
 */
static int prte_ras_slurm_build_req_nodelist(const char *slurm_jobid, const char *protected_hostname, int new_node_count, char **req_nodes_arg)
{
    if(!slurm_jobid || !protected_hostname || !req_nodes_arg) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    static const char *req_nodes_prefix = "ReqNodeList=";

    *req_nodes_arg = NULL;

    int err = prte_ras_slurm_validate_hostname(protected_hostname);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    err = prte_ras_slurm_validate_jobid(slurm_jobid);

    if(PRTE_SUCCESS != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    prte_session_t *session = prte_get_session_object_from_id(slurm_jobid);

    if (NULL == session) {
        err = PRTE_ERR_NOT_FOUND;
        PRTE_ERROR_LOG(err);
        return err;
    }

    /* estimate the required size for the nodes */
    size_t size_for_nodes = strlen(protected_hostname) * new_node_count;
    const size_t prefix_size = strlen(req_nodes_prefix);

    /* prefix, commas, and nullchar */
    const size_t size_additional = prefix_size + new_node_count;

    const size_t max_size_for_nodes = PRTE_SLURM_HOSTNAME_MAX_LEN * new_node_count;

    size_t curr_size = size_for_nodes + size_additional;
    size_t curr_occupied = size_additional; /* pre-reserve space for non-node content */

    *req_nodes_arg = malloc(curr_size);
    if (NULL == *req_nodes_arg) {
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    char *req_nodes_ptr = *req_nodes_arg;

    /* append the prefix */
    memcpy(*req_nodes_arg, req_nodes_prefix, prefix_size);
    req_nodes_ptr += prefix_size;

    bool found_protected = false;
    int nodecount = 0;

    for (int i = 0; i < session->nodes->size; i++) {
        prte_node_t *curr = pmix_pointer_array_get_item(session->nodes, i);

        if (NULL == curr || NULL == curr->name) {
            continue;
        }

        /* must be included on the list */
        if (!found_protected && 
            0 == strcmp(curr->name, protected_hostname)) {
            found_protected = true;
        } else if (!found_protected && nodecount >= new_node_count-1) {
            /* we have enough nodes, only look for node to protect */
            continue;   
        }

        err = prte_ras_slurm_validate_hostname(curr->name);

        if(PRTE_SUCCESS != err) {
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }
        
        size_t item_size = strlen(curr->name);

        if(curr_occupied+item_size > curr_size) {
            size_t new_size = curr_size * 2;
            size_t req_nodes_ptr_offset = req_nodes_ptr - *req_nodes_arg; 

            if(new_size > max_size_for_nodes + size_additional) {
                new_size = max_size_for_nodes + size_additional;
            }

            if(curr_occupied+item_size > new_size) {
                err = PRTE_ERR_OUT_OF_RESOURCE;
                PRTE_ERROR_LOG(err);
                goto cleanup;                    
            }

            char *new_req_nodes_arg = realloc(*req_nodes_arg, new_size);

            if (NULL == new_req_nodes_arg) {
                err = PRTE_ERR_OUT_OF_RESOURCE;
                PRTE_ERROR_LOG(err);
                goto cleanup;
            }

            *req_nodes_arg = new_req_nodes_arg;
            req_nodes_ptr = new_req_nodes_arg + req_nodes_ptr_offset;
            curr_size = new_size;
        }

        /* insert a comma if not first item */
        if(nodecount > 0) {
            *req_nodes_ptr = ',';
            req_nodes_ptr++;
        }

        memcpy(req_nodes_ptr, curr->name, item_size);

        req_nodes_ptr += item_size;
        curr_occupied += item_size;

        if(++nodecount >= new_node_count) {
            break;
        }
    }

    if (nodecount != new_node_count || !found_protected) {
        err = PRTE_ERR_NOT_FOUND;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    *req_nodes_ptr = '\0';

cleanup:

    if (PRTE_SUCCESS != err) {
        free(*req_nodes_arg);
        *req_nodes_arg = NULL;
    }

    return err;
}
