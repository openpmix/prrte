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

/* Local functions */
static int prte_ras_slurm_remove_nodes_by_count(uint64_t node_count);
static int prte_ras_slurm_shrink_job(const char *slurm_jobid, int new_node_count, char *err_msg);

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

    int nodes_removed = 0;

    do {
        if (0 == pmix_list_get_size(prte_slurm_session_stack)) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;
        }

        /* get the most recently added allocation to remove from */
        prte_session_stack_item_t *session_item = (prte_session_stack_item_t *) pmix_list_get_last(prte_slurm_session_stack);

        if(NULL == session_item || NULL == session_item->session) {
            err = PRTE_ERR_NOT_FOUND;
            PRTE_ERROR_LOG(err);
            goto cleanup;   
        }

        prte_session_t *session = session_item->session;

        int nodes_left_to_rem = nodes_to_remove-nodes_removed;

        char err_msg[PRTE_SLURM_ERR_STR_MAX_SIZE+1] = {0};

        if(nodes_left_to_rem >= session_item->nodes_in_session) {

            err = prte_ras_slurm_kill_job(session->alloc_refid, err_msg);

            if (PRTE_SUCCESS != err) {

                if(PRTE_ERR_SLURM_CANCEL_FAILURE == err) {
                    pmix_output(0, "ras:slurm:remove_resources: failed to kill job %s: %s.",
                                    session->alloc_refid, err_msg);
                } else {
                    PRTE_ERROR_LOG(err);
                }

                goto cleanup;
            }

            pmix_list_remove_last(prte_slurm_session_stack);
            
            /* TODO: take nodes in session and remove them */

            nodes_removed += session_item->nodes_in_session;

            /* This also removes from prte_sessions */
            PMIX_RELEASE(session);
            PMIX_RELEASE(session_item);
        } else {

            int new_node_count = session_item->nodes_in_session-nodes_left_to_rem;

            err = prte_ras_slurm_shrink_job(session->alloc_refid, new_node_count, err_msg);

            if (PRTE_SUCCESS != err) {

                if(PRTE_ERR_SLURM_SHRINK_FAILURE == err) {
                    pmix_output(0, "ras:slurm:remove_resources: failed to shrink job %s" 
                                    ": %s.", session->alloc_refid, err_msg);
                } else {
                    PRTE_ERROR_LOG(err);
                }

                goto cleanup;
            }

            /* Magic needs to happen to remove nodes from PRRTE */

            pmix_pointer_array_t nodes_in_removal;
            PMIX_CONSTRUCT(&nodes_in_removal, pmix_pointer_array_t);

            /* Remove target nodes from session and add to nodes_in_removal */
            err = prte_ras_slurm_detach_nodes(session->alloc_refid, session, &nodes_in_removal);

            if (PRTE_SUCCESS != err) {
                PMIX_DESTRUCT(&nodes_in_removal);
                goto cleanup;
            }
            
            /* TODO: use nodes_in_removal to remove from PRRTE node lists */

            PMIX_DESTRUCT(&nodes_in_removal);

            session_item->nodes_in_session = new_node_count;
            nodes_removed += nodes_left_to_rem;
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
 * err_msg is populated with the command output when provided.
 *
 * @param[in] slurm_jobid Slurm job ID to resize.
 * @param[in] new_node_count New number of nodes for the job.
 * @param[out] err_msg Optional buffer for Slurm error output.
 */
static int prte_ras_slurm_shrink_job(const char *slurm_jobid, int new_node_count, char *err_msg) {

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

    static const char *cmd_format = "scontrol update job %s NumNodes=%d 2>&1";

    char *cmd = NULL;

    FILE *fp = NULL;

    if(0 > asprintf(&cmd, cmd_format, slurm_jobid, new_node_count)) {
        cmd = NULL;
        err = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    fp = popen(cmd, "r");

    if(NULL == fp) {
        err = PRTE_ERR_FILE_OPEN_FAILURE;
        PRTE_ERROR_LOG(err);
        goto cleanup;
    }

    if(NULL != err_msg) {
        char *buf = fgets(err_msg, PRTE_SLURM_ERR_STR_MAX_SIZE, fp);

        /* Copy output into provided memory, truncating if necessary */
        if(NULL != buf) {
            size_t len = strcspn(buf, "\n");

            if (buf[len] == '\n') {
                buf[len] = '\0';
            } else if (len == PRTE_SLURM_ERR_STR_MAX_SIZE - 1) {
                memcpy(buf + PRTE_SLURM_ERR_STR_MAX_SIZE - 4, "...", 3);
            }
        } 
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
        pmix_output(0, "ras:slurm:shrink_job: asprintf failed during cleanup.");
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
        pmix_output(0, "ras:slurm:shrink_job: asprintf failed during cleanup.");
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

    return err;
}