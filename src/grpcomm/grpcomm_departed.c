/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Departed processes, and why the fence needs to hear about them.
 *
 * A fence over a whole namespace (PMIX_RANK_WILDCARD - which is what
 * PMIx_Fence(NULL, 0) sends, and so what MPI_Finalize uses) counts every
 * daemon that hosts one of the namespace's processes, and each of those
 * daemons contributes when its PMIx server has heard from every local
 * participant. The PMIx server takes a process that has gone out of that
 * count, so a daemon that loses some of its processes still contributes for
 * the rest. A daemon that loses ALL of them has nobody left to call the
 * fence, so its PMIx server never contributes - and the rollup waits for it
 * forever. In a job that ends with its first loss nobody is left to wait; in
 * a survivable one (a ULFM application, say) every survivor's next whole-job
 * fence hangs.
 *
 * So each daemon tells its parent in the routing tree which of its processes
 * have terminated, and passes up whatever its children tell it. A rollup can
 * then see that a child subtree - or this daemon itself - has no participant
 * left that has not departed, and stop waiting for it.
 *
 * Why up the routing tree, and not by broadcast: a daemon's contribution to
 * a fence travels the same link to the same parent. A process that called a
 * fence and then died therefore has its daemon's contribution ahead of the
 * news of its death, and the parent counts the contribution rather than the
 * departure. A broadcast from the master would arrive by another path, in no
 * particular order relative to the contribution. And a tracker records WHICH
 * subtrees have reported, so a subtree that reported and then departed is
 * never counted twice.
 *
 * Only survivable jobs: the notices cost a message per terminated process per
 * level of the tree, and a job that ends at its first loss would pay that at
 * every normal exit for nothing.
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <limits.h>
#include <string.h>

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"

#include "grpcomm_internal.h"
#include "src/grpcomm/grpcomm.h"

/* The entry for nspace, created if asked for. A job that has ended takes its
 * entry with it here, the next time anything is recorded, rather than through
 * a teardown hook of its own: the set only grows when something dies, so that
 * is often enough, and it keeps the job lifecycle out of this file. */
static prte_grpcomm_departed_t *find_departed(const pmix_nspace_t nspace, bool create)
{
    prte_grpcomm_departed_t *dp, *nxt, *found = NULL;

    PMIX_LIST_FOREACH_SAFE(dp, nxt, &prte_grpcomm_globals.departed, prte_grpcomm_departed_t) {
        if (PMIX_CHECK_NSPACE(dp->nspace, nspace)) {
            found = dp;
            continue;
        }
        if (create && NULL == prte_get_job_data_object(dp->nspace)) {
            pmix_list_remove_item(&prte_grpcomm_globals.departed, &dp->super);
            PMIX_RELEASE(dp);
        }
    }
    if (NULL != found || !create) {
        return found;
    }
    found = PMIX_NEW(prte_grpcomm_departed_t);
    if (NULL == found) {
        return NULL;
    }
    PMIX_LOAD_NSPACE(found->nspace, nspace);
    pmix_list_append(&prte_grpcomm_globals.departed, &found->super);
    return found;
}

size_t prte_grpcomm_departed_add(const pmix_nspace_t nspace,
                                 const pmix_rank_t *ranks, size_t nranks,
                                 pmix_rank_t *fresh)
{
    prte_grpcomm_departed_t *dp;
    size_t n, nfresh = 0;

    /* an empty namespace is a wildcard to PMIX_CHECK_NSPACE, and would match
     * every entry - so it names nobody */
    if (NULL == ranks || 0 == nranks || PMIX_NSPACE_INVALID(nspace)) {
        return 0;
    }
    dp = find_departed(nspace, true);
    if (NULL == dp) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return 0;
    }
    for (n = 0; n < nranks; n++) {
        /* a bit per rank, so only a real rank fits */
        if (PMIX_RANK_VALID < ranks[n] || INT_MAX <= (long long) ranks[n]) {
            continue;
        }
        if (pmix_bitmap_is_set_bit(&dp->ranks, (int) ranks[n])) {
            continue;
        }
        if (PMIX_SUCCESS != pmix_bitmap_set_bit(&dp->ranks, (int) ranks[n])) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            continue;
        }
        if (NULL != fresh) {
            fresh[nfresh] = ranks[n];
        }
        ++nfresh;
    }
    return nfresh;
}

bool prte_grpcomm_departed_has(const pmix_nspace_t nspace, pmix_rank_t rank)
{
    prte_grpcomm_departed_t *dp;

    if (pmix_list_is_empty(&prte_grpcomm_globals.departed) ||
        PMIX_RANK_VALID < rank || INT_MAX <= (long long) rank) {
        return false;
    }
    dp = find_departed(nspace, false);
    if (NULL == dp) {
        return false;
    }
    return pmix_bitmap_is_set_bit(&dp->ranks, (int) rank);
}

bool prte_grpcomm_departed_any(const pmix_nspace_t nspace)
{
    prte_grpcomm_departed_t *dp;

    if (pmix_list_is_empty(&prte_grpcomm_globals.departed)) {
        return false;
    }
    dp = find_departed(nspace, false);
    return (NULL != dp && !pmix_bitmap_is_clear(&dp->ranks));
}

/* Tell our parent. The master has none - it is where everything ends up. */
static void send_up(const pmix_nspace_t nspace, const pmix_rank_t *ranks, size_t nranks)
{
    pmix_data_buffer_t *buf;
    pmix_nspace_t ns;
    int rc;

    if (PRTE_PROC_IS_MASTER || 0 == nranks) {
        return;
    }
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_DATA_BUFFER_CREATE(buf);
    rc = PMIx_Data_pack(NULL, buf, &ns, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, &nranks, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, (void *) ranks, (int32_t) nranks, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_PARENT->rank, buf, PRTE_RML_TAG_FENCE_DEPARTED);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

void prte_grpcomm_proc_terminated(const pmix_proc_t *proc)
{
    prte_job_t *jdata;
    pmix_rank_t rank;

    if (NULL == proc) {
        return;
    }
    jdata = prte_get_job_data_object(proc->nspace);
    if (NULL == jdata ||
        !(PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_RECOVERABLE) ||
          PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_CONTINUOUS))) {
        return;
    }
    rank = proc->rank;
    if (0 == prte_grpcomm_departed_add(proc->nspace, &rank, 1, NULL)) {
        return;
    }
    PMIX_OUTPUT_VERBOSE((2, prte_grpcomm_globals.output,
                         "%s grpcomm: %s has departed",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc)));
    send_up(proc->nspace, &rank, 1);
    prte_grpcomm_fence_departed_recheck();
}

void prte_grpcomm_departed_recv(int status, pmix_proc_t *sender,
                                pmix_data_buffer_t *buffer,
                                prte_rml_tag_t tag, void *cbdata)
{
    pmix_nspace_t nspace;
    pmix_rank_t *ranks = NULL, *fresh = NULL;
    size_t nranks = 0, nfresh;
    int32_t cnt;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nranks, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* a count off the wire sizes two allocations, so it has to be one the
     * message can actually hold - every rank takes at least a byte */
    if (0 == nranks || INT32_MAX < nranks || buffer->bytes_used < nranks) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return;
    }
    ranks = (pmix_rank_t *) malloc(nranks * sizeof(pmix_rank_t));
    fresh = (pmix_rank_t *) malloc(nranks * sizeof(pmix_rank_t));
    if (NULL == ranks || NULL == fresh) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        goto done;
    }
    cnt = (int32_t) nranks;
    rc = PMIx_Data_unpack(NULL, buffer, ranks, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc || (size_t) cnt != nranks) {
        PMIX_ERROR_LOG((PMIX_SUCCESS != rc) ? rc : PMIX_ERR_UNPACK_FAILURE);
        goto done;
    }

    PMIX_OUTPUT_VERBOSE((2, prte_grpcomm_globals.output,
                         "%s grpcomm: %s reports %u departed from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender),
                         (unsigned) nranks, nspace));

    /* pass on only what is news here - a resend after a recovery repeats
     * everything, and our parent has had most of it already */
    nfresh = prte_grpcomm_departed_add(nspace, ranks, nranks, fresh);
    if (0 < nfresh) {
        send_up(nspace, fresh, nfresh);
        prte_grpcomm_fence_departed_recheck();
    }

done:
    free(ranks);
    free(fresh);
}

void prte_grpcomm_departed_resend(void)
{
    prte_grpcomm_departed_t *dp;
    pmix_rank_t *ranks;
    size_t n, nranks;
    int bit, nbits;

    if (PRTE_PROC_IS_MASTER) {
        return;
    }
    PMIX_LIST_FOREACH(dp, &prte_grpcomm_globals.departed, prte_grpcomm_departed_t) {
        nbits = (int) dp->ranks.array_size * 64;
        nranks = (size_t) pmix_bitmap_num_set_bits(&dp->ranks, nbits);
        if (0 == nranks) {
            continue;
        }
        ranks = (pmix_rank_t *) malloc(nranks * sizeof(pmix_rank_t));
        if (NULL == ranks) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            continue;
        }
        n = 0;
        for (bit = 0; bit < nbits && n < nranks; bit++) {
            if (pmix_bitmap_is_set_bit(&dp->ranks, bit)) {
                ranks[n++] = (pmix_rank_t) bit;
            }
        }
        send_up(dp->nspace, ranks, n);
        free(ranks);
    }
}
