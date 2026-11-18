/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
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
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/prte_show_help.h"

static pmix_pointer_array_t pending_reqs;
static bool initialized = false;

typedef struct {
    /* the namespace that made the request, and the req_id it named it by */
    pmix_nspace_t nspace;
    char *request_id;
    char *slurm_job_id;
    /* the user who made the request - who may cancel it */
    uid_t owner;
} prte_ras_slurm_pending_req_t;

/* Local functions */
static void prte_ras_slurm_pending_req_free(prte_ras_slurm_pending_req_t *pending_req);
static int prte_ras_slurm_find_pending_req(const char *nspace, const char *request_id,
                                          int *idx);
static int prte_ras_slurm_find_pending_job(const char *slurm_job_id, int *idx);
static void prte_ras_slurm_cancel_req_at(int idx);
static int serve_cancel_by_alloc_id(const char *alloc_id, uid_t uid);
static int answer_granted(prte_pmix_server_req_t *req, prte_session_t *session);

/**
 * @brief Serve a PMIX_ALLOC_REQ_CANCEL.
 *
 * Cancels the pending extend named by PMIX_ALLOC_ID, or by PMIX_ALLOC_REQ_ID
 * from the job that made it. For an extend Slurm has already granted, answers
 * PRTE_EXISTS with its PMIX_ALLOC_ID if the requester may release it, else
 * PRTE_ERR_NOT_FOUND. A req_id the job used for several requests is refused
 * with PRTE_ERR_BAD_PARAM.
 *
 * @param[in,out] req PMIx server request; req->info is replaced by the
 *                    PMIX_ALLOC_ID on PRTE_EXISTS.
 */
int prte_ras_slurm_serve_cancel_req(prte_pmix_server_req_t *req)
{
    if (!prte_ras_slurm_have_extensions(false)) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    int err = PRTE_SUCCESS;
    const char *request_id = NULL;
    const char *alloc_id = NULL;
    bool have_alloc_id = false;
    uid_t uid;

    for (size_t i = 0; i < req->ninfo; i++) {
        if (0 == strcmp(req->info[i].key, PMIX_ALLOC_REQ_ID)) {
            if (PMIX_STRING != req->info[i].value.type) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }
            request_id = req->info[i].value.data.string;
        } else if (0 == strcmp(req->info[i].key, PMIX_ALLOC_ID)) {
            if (PMIX_STRING != req->info[i].value.type) {
                err = PRTE_ERR_BAD_PARAM;
                goto cleanup;
            }
            alloc_id = req->info[i].value.data.string;
            have_alloc_id = true;
        }
    }

    /* an id that names nothing is not "no id": refuse it rather than fall
     * back to the req_id */
    if (have_alloc_id && (NULL == alloc_id || '\0' == alloc_id[0])) {
        err = PRTE_ERR_BAD_PARAM;
        goto cleanup;
    }

    if (!have_alloc_id && (NULL == request_id || '\0' == request_id[0])) {
        pmix_output(0, "ras:slurm:modify: cancel request invalid or unsupported.\n"
                       "Supported options: PMIX_ALLOC_ID, PMIX_ALLOC_REQ_ID");
        err = PRTE_ERR_REQUEST;
        goto cleanup;
    }

    /* a pending request is its maker's to cancel - its user, root, or the
     * user the DVM runs as. The PMIx server names the user of every
     * request, so one naming none is refused. */
    if (!prte_pmix_server_requester_uid(req->info, req->ninfo, &uid)) {
        err = PRTE_ERR_PERM;
        goto cleanup;
    }

    /* By PMIX_ALLOC_ID - the Slurm job id the extend was answered with
     * while Slurm had it queued, and the id PMIX_QUERY_ALLOC_IDS lists it
     * under. Unlike a req_id that names one request DVM-wide, so any
     * process the rule admits may use it, not only the requester's own
     * job; it takes precedence over a req_id given with it. */
    if (have_alloc_id) {
        err = serve_cancel_by_alloc_id(alloc_id, uid);
        if (PRTE_ERR_NOT_FOUND == err) {
            /* granted already, perhaps - see answer_granted */
            err = answer_granted(req, prte_get_session_object_from_id(alloc_id));
        }
        goto cleanup;
    }
    err = prte_ras_slurm_pending_req_permitted(req->tproc.nspace, request_id, uid);
    if (PRTE_ERR_NOT_FOUND == err) {
        prte_session_t *session;
        int rc;

        /* the requester's own allocation of that name, if it made just one;
         * matched case-sensitively, as the pending lookup is */
        rc = prte_get_session_object_from_refid(req->tproc.nspace, request_id, &session);
        if (PRTE_ERR_BAD_PARAM == rc) {
            /* several: refused, as for an extend, so it names one by
             * PMIX_ALLOC_ID */
            prte_show_help_proc(&req->tproc, "help-ras-base.txt",
                                "ras-base:req-id-ambiguous", true, request_id);
            err = PRTE_ERR_BAD_PARAM;
        } else if (PRTE_SUCCESS == rc &&
                   !PMIX_NSPACE_INVALID(session->requested_by.nspace) &&
                   0 == strcmp(session->user_refid, request_id)) {
            err = answer_granted(req, session);
        }
        goto cleanup;
    }
    if (PRTE_SUCCESS != err) {
        goto cleanup;
    }

    err = prte_ras_slurm_cancel_pending_req(req->tproc.nspace, request_id);

    if (PRTE_SUCCESS == err) {
        /* Answering the extend belongs to the extend half. After
         * cancel_pending_req, so the completion finds no record left and does
         * not cancel the job twice. */
        prte_ras_slurm_extend_abort_request(req->tproc.nspace, request_id);
    }

cleanup:

    return err;
}

/**
 * @brief Answer a cancel that names an extend Slurm has already granted.
 *
 * The grant removed the pending record, so there is nothing to cancel; the
 * requester is told the allocation exists (PRTE_EXISTS, PMIX_ERR_EXISTS to
 * it) and its PMIX_ALLOC_ID, to release it. Only for an extend's allocation
 * (dynamic, and not a reservation, which ras/slurm never makes) and a
 * requester that may release it; anything else is not found, as before.
 *
 * The id replaces req->info, which the answer carries back.
 */
static int answer_granted(prte_pmix_server_req_t *req, prte_session_t *session)
{
    pmix_info_t *result_info;

    if (NULL == session || NULL == session->alloc_refid ||
        !PRTE_FLAG_TEST(session, PRTE_SESSION_FLAG_DYNAMIC) ||
        PRTE_FLAG_TEST(session, PRTE_SESSION_FLAG_RESERVED) ||
        !prte_pmix_server_session_permitted(session, &req->tproc, req->info, req->ninfo)) {
        return PRTE_ERR_NOT_FOUND;
    }
    PMIX_INFO_CREATE(result_info, 1);
    if (NULL == result_info) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    PMIX_INFO_LOAD(&result_info[0], PMIX_ALLOC_ID, session->alloc_refid, PMIX_STRING);
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->info = result_info;
    req->ninfo = 1;
    req->copy = true;
    return PRTE_EXISTS;
}

static int serve_cancel_by_alloc_id(const char *alloc_id, uid_t uid)
{
    prte_ras_slurm_pending_req_t *pending_req;
    pmix_nspace_t nspace;
    char *request_id;
    int idx, err;

    /* a granted allocation is no longer pending, so has no request left to
     * cancel - it is given back with PMIX_ALLOC_RELEASE */
    err = prte_ras_slurm_find_pending_job(alloc_id, &idx);
    if (PRTE_SUCCESS != err) {
        return err;
    }
    pending_req = (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, idx);
    if (NULL == pending_req ||
        !prte_pmix_server_user_permitted(uid, pending_req->owner, PRTE_INVALID_GID)) {
        return PRTE_ERR_PERM;
    }
    /* the extend half finds its request by the name its maker gave it, and
     * cancelling frees the record that holds that name */
    PMIX_LOAD_NSPACE(nspace, pending_req->nspace);
    request_id = strdup(pending_req->request_id);
    if (NULL == request_id) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    prte_ras_slurm_cancel_req_at(idx);
    /* in that order, as for a cancel by req_id: see serve_cancel_req */
    prte_ras_slurm_extend_abort_request(nspace, request_id);
    free(request_id);
    return PRTE_SUCCESS;
}

/**
 * @brief Add a pending request to the cancellable request list.
 *
 * Stores copies of the PMIx-visible request ID and the Slurm job ID needed to
 * cancel the scheduler request. Duplicate additions are treated as success if
 * they refer to the same Slurm job.
 *
 * @param[in] request_id PMIx request identifier.
 * @param[in] slurm_job_id Slurm job ID backing the request.
 */
int prte_ras_slurm_add_pending_req(const char *nspace, const char *request_id,
                                   const char *slurm_job_id, uid_t owner)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]
        || NULL == slurm_job_id || '\0' == slurm_job_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(nspace, request_id, &idx);

    if (PRTE_SUCCESS == err) {
        prte_ras_slurm_pending_req_t *pending_req =
            (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, idx);
        if (NULL != pending_req && 0 == strcmp(pending_req->slurm_job_id, slurm_job_id)) {
            return PRTE_SUCCESS;
        }
        PRTE_ERROR_LOG(PRTE_EXISTS);
        return PRTE_EXISTS;
    }

    if (PRTE_ERR_NOT_FOUND != err) {
        PRTE_ERROR_LOG(err);
        return err;
    }

    prte_ras_slurm_pending_req_t *pending_req = calloc(1, sizeof(*pending_req));

    if (NULL == pending_req) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    PMIX_LOAD_NSPACE(pending_req->nspace, nspace);
    pending_req->request_id = strdup(request_id);
    pending_req->slurm_job_id = strdup(slurm_job_id);
    pending_req->owner = owner;

    if (NULL == pending_req->request_id || NULL == pending_req->slurm_job_id) {
        prte_ras_slurm_pending_req_free(pending_req);
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (0 > pmix_pointer_array_add(&pending_reqs, pending_req)) {
        prte_ras_slurm_pending_req_free(pending_req);
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    return PRTE_SUCCESS;
}

/**
 * @brief Remove a pending request from the cancellable request list.
 *
 * @param[in] request_id PMIx request identifier.
 */
int prte_ras_slurm_remove_pending_req(const char *nspace, const char *request_id)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(nspace, request_id, &idx);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    prte_ras_slurm_pending_req_t *pending_req =
        (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, idx);
    prte_ras_slurm_pending_req_free(pending_req);
    pmix_pointer_array_set_item(&pending_reqs, idx, NULL);

    return PRTE_SUCCESS;
}

/**
 * @brief Cancel a pending request.
 *
 * Invokes Slurm cancellation for the matching pending request and removes it
 * from the list.
 *
 * @param[in] request_id PMIx request identifier.
 */
int prte_ras_slurm_pending_req_permitted(const char *nspace, const char *request_id,
                                         uid_t uid)
{
    prte_ras_slurm_pending_req_t *pending_req;
    int idx, err;

    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }
    err = prte_ras_slurm_find_pending_req(nspace, request_id, &idx);
    if (PRTE_SUCCESS != err) {
        return err;
    }
    pending_req = (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, idx);
    if (NULL == pending_req ||
        !prte_pmix_server_user_permitted(uid, pending_req->owner, PRTE_INVALID_GID)) {
        return PRTE_ERR_PERM;
    }
    return PRTE_SUCCESS;
}

int prte_ras_slurm_cancel_pending_req(const char *nspace, const char *request_id)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0]) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    int idx;
    int err = prte_ras_slurm_find_pending_req(nspace, request_id, &idx);

    if (PRTE_SUCCESS != err) {
        return err;
    }

    prte_ras_slurm_cancel_req_at(idx);

    return PRTE_SUCCESS;
}

/**
 * @brief Cancel and drop the pending request held at an array index.
 *
 * @param[in] idx Array index containing the request.
 */
static void prte_ras_slurm_cancel_req_at(int idx)
{
    prte_ras_slurm_pending_req_t *pending_req =
        (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, idx);

    if (NULL == pending_req) {
        return;
    }

    prte_ras_slurm_kill_job(pending_req->slurm_job_id, NULL, 0);

    prte_ras_slurm_pending_req_free(pending_req);
    pmix_pointer_array_set_item(&pending_reqs, idx, NULL);
}

/**
 * @brief Free a pending request mapping.
 *
 * @param[in] pending_req Pending request entry.
 */
static void prte_ras_slurm_pending_req_free(prte_ras_slurm_pending_req_t *pending_req)
{
    if (NULL == pending_req) {
        return;
    }

    /* the request's session goes with it, unless it was granted - then it
     * is a session like any other, and no longer pending */
    prte_ras_slurm_drop_pending_session(pending_req->slurm_job_id);

    free(pending_req->request_id);
    free(pending_req->slurm_job_id);
    free(pending_req);
}

/* The pending request queued as Slurm job slurm_job_id - which is the
 * PMIX_ALLOC_ID the request was answered with - whoever made it */
static int prte_ras_slurm_find_pending_job(const char *slurm_job_id, int *idx)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    for (int i = 0; i < pending_reqs.size; i++) {
        prte_ras_slurm_pending_req_t *pending_req =
            (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, i);

        if (NULL != pending_req && 0 == strcmp(pending_req->slurm_job_id, slurm_job_id)) {
            *idx = i;
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

/**
 * @brief Find a pending request in the cancellable request list.
 *
 * @param[in] request_id PMIx request identifier.
 * @param[out] idx Array index containing the request.
 */
static int prte_ras_slurm_find_pending_req(const char *nspace, const char *request_id,
                                          int *idx)
{
    if (!initialized) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    if (NULL == request_id || '\0' == request_id[0] || NULL == idx) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    for (int i = 0; i < pending_reqs.size; i++) {
        prte_ras_slurm_pending_req_t *pending_req =
            (prte_ras_slurm_pending_req_t *) pmix_pointer_array_get_item(&pending_reqs, i);

        if (NULL != pending_req && PMIX_CHECK_NSPACE(pending_req->nspace, nspace) &&
            0 == strcmp(pending_req->request_id, request_id)) {
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
    if(initialized || !prte_ras_slurm_have_extensions(true)) {
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
 *
 * Anything still listed is an extend that never completed. Cancel rather than
 * forget: a job left behind holds its nodes until its time limit.
 */
int prte_ras_slurm_modify_cancel_finalize(void)
{
    if (!initialized) {
        return PRTE_SUCCESS;
    }

    for (int i = 0; i < pending_reqs.size; i++) {
        prte_ras_slurm_cancel_req_at(i);
    }

    PMIX_DESTRUCT(&pending_reqs);
    initialized = false;

    return PRTE_SUCCESS;
}
