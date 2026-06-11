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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ras_slurm.h"
#include "src/mca/ras/base/base.h"

static pmix_pointer_array_t pending_reqs;
static bool initialized = false;

/* Local functions */
static int prte_ras_slurm_find_pending_req(const char *request_id, int *idx);

/**
 * @brief Process a PMIx pending resource cancellation request.
 *
 * Cancels a previously registered pending Slurm request identified by
 * PMIX_ALLOC_REQ_ID.
 *
 * @param[in] req PMIx server request containing a Slurm job ID as PMIX_ALLOC_REQ_ID.
 */
int prte_ras_slurm_serve_cancel_req(prte_pmix_server_req_t *req)
{
    if(!prte_ras_slurm_have_jansson()) {
        pmix_output(0, "ras:slurm:modify: "
            "Jansson support is required but not enabled in this build");
        return PRTE_ERR_NOT_AVAILABLE;
    }

    int err = PRTE_SUCCESS;
    const char *request_id = NULL;

    for (size_t i = 0; i < req->ninfo; i++) {
        if (0 != strcmp(req->info[i].key, PMIX_ALLOC_REQ_ID)) {
            continue;
        }

        if (PMIX_STRING != req->info[i].value.type) {
            err = PRTE_ERR_BAD_PARAM;
            goto cleanup;
        }

        request_id = req->info[i].value.data.string;
        break;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        pmix_output(0, "ras:slurm:modify: cancel request invalid or unsupported.\n"
                       "Supported options: PMIX_ALLOC_REQ_ID");
        err = PRTE_ERR_REQUEST;
        goto cleanup;
    }

    err = prte_ras_slurm_cancel_pending_req(request_id);

cleanup:

    return err;
}

/**
 * @brief Add a pending request to the cancellable request list.
 *
 * Stores a copy of the provided request ID string. Duplicate additions are
 * treated as success.
 *
 * @param[in] request_id Pending request identifier; must be a Slurm job ID.
 */
int prte_ras_slurm_add_pending_req(const char *request_id)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(request_id, &idx);

    if (PRTE_SUCCESS == err) {
        return PRTE_SUCCESS;
    }

    if (PRTE_ERR_NOT_FOUND != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    char *request_copy = strdup(request_id);

    if (NULL == request_copy) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    int pmix_err = pmix_pointer_array_add(&pending_reqs, request_copy);

    if (0 > pmix_err) {
        free(request_copy);
        err = prte_pmix_convert_status(pmix_err);
        PRTE_ERROR_LOG(err);
        return err;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Remove a pending request from the cancellable request list.
 *
 * @param[in] request_id Pending request identifier; must be a Slurm job ID.
 */
int prte_ras_slurm_remove_pending_req(const char *request_id)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(request_id, &idx);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    char *stored_req = (char *) pmix_pointer_array_get_item(&pending_reqs, idx);
    free(stored_req);
    pmix_pointer_array_set_item(&pending_reqs, idx, NULL);

    return PRTE_SUCCESS;
}

/**
 * @brief Check whether a request is still pending cancellation.
 *
 * @param[in] request_id Pending request identifier; must be a Slurm job ID.
 */
bool prte_ras_slurm_pending_req_exists(const char *request_id)
{
    int idx;

    return PRTE_SUCCESS == prte_ras_slurm_find_pending_req(request_id, &idx);
}

/**
 * @brief Cancel a pending request.
 *
 * Invokes Slurm cancellation for the matching pending request and removes it
 * from the list.
 *
 * @param[in] request_id Pending request identifier; must be a Slurm job ID.
 */
int prte_ras_slurm_cancel_pending_req(const char *request_id)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(request_id, &idx);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    prte_ras_slurm_kill_job(request_id, NULL, 0);

    char *stored_req = (char *) pmix_pointer_array_get_item(&pending_reqs, idx);
    free(stored_req);
    pmix_pointer_array_set_item(&pending_reqs, idx, NULL);

    return PRTE_SUCCESS;
}

/**
 * @brief Find a pending request in the cancellable request list.
 *
 * @param[in] request_id Pending request identifier.
 * @param[out] idx Array index containing the request.
 */
static int prte_ras_slurm_find_pending_req(const char *request_id, int *idx)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0] || NULL == idx) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; i < pending_reqs.size; i++) {
        char *stored_req = (char *) pmix_pointer_array_get_item(&pending_reqs, i);

        if (NULL != stored_req && 0 == strcmp(stored_req, request_id)) {
            *idx = i;
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

/**
 * @brief Initialize required data structures for record keeping.
 */
int prte_ras_slurm_modify_cancel_init(void)
{
    if(initialized || !prte_ras_slurm_have_jansson()) {
        return PRTE_SUCCESS;
    }

    int err = PRTE_SUCCESS;

    PMIX_CONSTRUCT(&pending_reqs, pmix_pointer_array_t);

    err = pmix_pointer_array_init(&pending_reqs, 0, INT_MAX, 16);
    
    if (PMIX_SUCCESS != err) {
        PMIX_DESTRUCT(&pending_reqs);
        return prte_pmix_convert_status(err);
    }

    initialized = true;

    return err;
}

/**
 * @brief Destroy required data structures for record keeping.
 */
int prte_ras_slurm_modify_cancel_finalize(void)
{
    if (!initialized) {
        return PRTE_SUCCESS;
    }

    for (int i = 0; i < pending_reqs.size; i++) {
        void *ptr = pmix_pointer_array_get_item(&pending_reqs, i);
        if (NULL != ptr) {
            free(ptr);
            pmix_pointer_array_set_item(&pending_reqs, i, NULL);
        }
    }

    PMIX_DESTRUCT(&pending_reqs);
    initialized = false;

    return PRTE_SUCCESS;
}
