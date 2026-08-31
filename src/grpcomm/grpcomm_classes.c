/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/runtime/prte_globals.h"

#include "src/util/proc_info.h"

#include "grpcomm_internal.h"

static void grpcon(prte_pmix_grp_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->op = PMIX_GROUP_NONE;
    p->grpid = NULL;
    p->procs = NULL;
    p->nprocs = 0;
    p->directives = NULL;
    p->ndirs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void grpdes(prte_pmix_grp_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->grpid) {
        free(p->grpid);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_grp_caddy_t,
                    pmix_object_t,
                    grpcon, grpdes);

static void scon(prte_grpcomm_fence_signature_t *p)
{
    p->signature = NULL;
    p->sz = 0;
}
static void sdes(prte_grpcomm_fence_signature_t *p)
{
    /* PMIX_PROC_FREE, not free(): every populator of this array builds it
     * with PMIX_PROC_CREATE, which allocates inside the PMIx library */
    if (NULL != p->signature) {
        PMIX_PROC_FREE(p->signature, p->sz);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_fence_signature_t,
                    pmix_object_t,
                    scon, sdes);

static void sgcon(prte_grpcomm_group_signature_t *p)
{
    p->op = PMIX_GROUP_NONE;
    p->groupID = NULL;
    p->assignID = false;
    p->ctxid = 0;
    p->ctxid_assigned = false;
    p->members = NULL;
    p->nmembers = 0;
    p->bootstrap = 0;
    p->follower = false;
    p->addmembers = NULL;
    p->naddmembers = 0;
    p->final_order = NULL;
    p->nfinal = 0;
    p->ft_collective = false;
}
static void sgdes(prte_grpcomm_group_signature_t *p)
{
    if (NULL != p->groupID) {
        free(p->groupID);
    }
    /* PMIX_PROC_FREE, not free(): see the note in sdes() above */
    if (NULL != p->members) {
        PMIX_PROC_FREE(p->members, p->nmembers);
    }
    if (NULL != p->addmembers) {
        PMIX_PROC_FREE(p->addmembers, p->naddmembers);
    }
    if (NULL != p->final_order) {
        PMIX_PROC_FREE(p->final_order, p->nfinal);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_group_signature_t,
                    pmix_object_t,
                    sgcon, sgdes);

static void ccon(prte_grpcomm_fence_t *p)
{
    p->sig = NULL;
    p->status = PMIX_SUCCESS;
    p->op = PRTE_GRPCOMM_FENCE_OP_UNKNOWN;
    PMIX_DATA_BUFFER_CONSTRUCT(&p->bucket);
    p->dmns = NULL;
    p->ndmns = 0;
    p->nexpected = 0;
    p->nreported = 0;
    PMIX_CONSTRUCT(&p->reported_slots, pmix_bitmap_t);
    pmix_bitmap_init(&p->reported_slots, 1);
    p->self_reported = false;
    p->converged = false;
    p->aborting = false;
    p->my_contribution = NULL;
    p->timeout = 0;
    p->tev_active = false;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void cdes(prte_grpcomm_fence_t *p)
{
    if (p->tev_active) {
        prte_event_del(&p->tev);
        p->tev_active = false;
    }
    if (NULL != p->sig) {
        PMIX_RELEASE(p->sig);
    }
    PMIX_DESTRUCT(&p->reported_slots);
    if (NULL != p->my_contribution) {
        PMIX_DATA_BUFFER_RELEASE(p->my_contribution);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&p->bucket);
    if (NULL != p->dmns) {
        free(p->dmns);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_fence_t,
                    pmix_list_item_t,
                    ccon, cdes);


static void gccon(prte_grpcomm_group_t *p)
{
    p->sig = NULL;
    p->status = PMIX_SUCCESS;
    p->dmns = NULL;
    p->ndmns = 0;
    p->bootstrap = false;
    p->nexpected = 0;
    p->nreported = 0;
    p->nleaders = 0;
    p->nleaders_reported = 0;
    p->nfollowers = 0;
    p->nfollowers_reported = 0;
    /* the bitmap grows on demand as slots are set, so size it minimally
     * here - a tracker can be constructed with no routing tree in place */
    PMIX_CONSTRUCT(&p->reported_slots, pmix_bitmap_t);
    pmix_bitmap_init(&p->reported_slots, 1);
    p->self_reported = false;
    p->converged = false;
    p->aborting = false;
    p->timeout = 0;
    p->tev_active = false;
    p->my_contribution = NULL;
    p->departed = NULL;
    p->ndeparted = 0;
    p->grpinfo = PMIx_Info_list_start();
    p->endpts = PMIx_Info_list_start();
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void gcdes(prte_grpcomm_group_t *p)
{
    if (p->tev_active) {
        prte_event_del(&p->tev);
        p->tev_active = false;
    }
    PMIX_DESTRUCT(&p->reported_slots);
    if (NULL != p->my_contribution) {
        PMIX_DATA_BUFFER_RELEASE(p->my_contribution);
    }
    if (NULL != p->departed) {
        PMIX_PROC_FREE(p->departed, p->ndeparted);
    }
    if (NULL != p->sig) {
        PMIX_RELEASE(p->sig);
    }
    PMIx_Info_list_release(p->grpinfo);
    PMIx_Info_list_release(p->endpts);
    if (NULL != p->dmns) {
        free(p->dmns);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_group_t,
                    pmix_list_item_t,
                    gccon, gcdes);

static void memocon(prte_grpcomm_group_memo_t *p)
{
    p->groupID = NULL;
    p->op = PMIX_GROUP_NONE;
}
static void memodes(prte_grpcomm_group_memo_t *p)
{
    if (NULL != p->groupID) {
        free(p->groupID);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_group_memo_t,
                    pmix_list_item_t,
                    memocon, memodes);

static void mdcon(prte_pmix_fence_caddy_t *p)
{
    p->procs = NULL;
    p->nprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->data = NULL;
    p->ndata = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void mddes(prte_pmix_fence_caddy_t *p)
{
    /* The modex bucket PMIx handed up with the fence.  Ownership transfers on
     * that call and cannot not: PMIx unloads the buffer into a bare pointer
     * and returns, while we park it here and read it later from another
     * thread, so PMIx has nothing left it could free it from.  Nothing copies
     * it out on our side either - PMIx_Data_embed() copies the bytes into the
     * relay and says in its own comment that it deliberately does not free
     * the source - so this is the only place it can go. */
    if (NULL != p->data) {
        free(p->data);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_fence_caddy_t,
                    pmix_object_t,
                    mdcon, mddes);
