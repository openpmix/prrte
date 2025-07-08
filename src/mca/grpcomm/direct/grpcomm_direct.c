/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

/* Static API's */
static int init(void);
static void finalize(void);

/* Module def */
prte_grpcomm_base_module_t prte_grpcomm_direct_module = {
    .init = init,
    .finalize = finalize,
    .xcast = prte_grpcomm_direct_xcast,
    .fence = prte_grpcomm_direct_fence,
    .group = prte_grpcomm_direct_group
};

/**
 * Initialize the module
 */
static int init(void)
{
    /* setup the trackers */
    PMIX_CONSTRUCT(&prte_mca_grpcomm_direct_component.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_mca_grpcomm_direct_component.group_ops, pmix_list_t);

    /* xcast receive */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST,
                  PRTE_RML_PERSISTENT, prte_grpcomm_direct_xcast_recv, NULL);

    /* fence receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_direct_fence_recv, NULL);
    /* setup recv for barrier release */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_direct_fence_release, NULL);

    /* group receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP,
                  PRTE_RML_PERSISTENT, prte_grpcomm_direct_grp_recv, NULL);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_direct_grp_release, NULL);
    return PRTE_SUCCESS;
}

/**
 * Finalize the module
 */
static void finalize(void)
{

    PMIX_LIST_DESTRUCT(&prte_mca_grpcomm_direct_component.fence_ops);
    PMIX_LIST_DESTRUCT(&prte_mca_grpcomm_direct_component.group_ops);

    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE);
    return;
}
