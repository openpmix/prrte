/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/runtime/prte_globals.h"

#include "src/util/proc_info.h"

#include "grpcomm_direct.h"

static int direct_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Struct of function pointers that need to be initialized
 */
prte_grpcomm_direct_component_t prte_mca_grpcomm_direct_component = {
    .super = {
        PRTE_GRPCOMM_BASE_VERSION_4_0_0,

        .pmix_mca_component_name = "direct",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),
        .pmix_mca_query_component = direct_query,
    },
    .fence_ops = PMIX_LIST_STATIC_INIT,
    .group_ops = PMIX_LIST_STATIC_INIT
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, grpcomm, direct)

static int direct_query(pmix_mca_base_module_t **module, int *priority)
{
    /* we are always available */
    *priority = 5;
    *module = (pmix_mca_base_module_t *) &prte_grpcomm_direct_module;
    return PRTE_SUCCESS;
}

static void scon(prte_grpcomm_direct_fence_signature_t *p)
{
    p->signature = NULL;
    p->sz = 0;
}
static void sdes(prte_grpcomm_direct_fence_signature_t *p)
{
    if (NULL != p->signature) {
        free(p->signature);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_direct_fence_signature_t,
                    pmix_object_t,
                    scon, sdes);

static void sgcon(prte_grpcomm_direct_group_signature_t *p)\
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
}
static void sgdes(prte_grpcomm_direct_group_signature_t *p)
{
    if (NULL != p->groupID) {
        free(p->groupID);
    }
    if (NULL != p->members) {
        free(p->members);
    }
    if (NULL != p->addmembers) {
        free(p->addmembers);
    }
}
PMIX_CLASS_INSTANCE(prte_grpcomm_direct_group_signature_t,
                    pmix_object_t,
                    sgcon, sgdes);

static void ccon(prte_grpcomm_fence_t *p)
{
    p->sig = NULL;
    p->status = PMIX_SUCCESS;
    PMIX_DATA_BUFFER_CONSTRUCT(&p->bucket);
    p->dmns = NULL;
    p->ndmns = 0;
    p->nexpected = 0;
    p->nreported = 0;
    p->timeout = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void cdes(prte_grpcomm_fence_t *p)
{
    if (NULL != p->sig) {
        PMIX_RELEASE(p->sig);
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
    p->nexpected = 0;
    p->nreported = 0;
    p->nleaders = 0;
    p->nleaders_reported = 0;
    p->nfollowers = 0;
    p->nfollowers_reported = 0;
    p->assignID = false;
    p->timeout = 0;
    p->memsize = 0;
    p->grpinfo = PMIx_Info_list_start();
    p->endpts = PMIx_Info_list_start();
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void gcdes(prte_grpcomm_group_t *p)
{
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


static void mdcon(prte_pmix_fence_caddy_t *p)
{
    p->sig = NULL;
    p->buf = NULL;
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
    if (NULL != p->sig) {
        PMIX_RELEASE(p->sig);
    }
    if (NULL != p->buf) {
        PMIX_DATA_BUFFER_RELEASE(p->buf);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_fence_caddy_t,
                    pmix_object_t,
                    mdcon, mddes);
