/*
 * Copyright (c) 2008-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Type definitions to support routed framework
 */

#ifndef PRTE_MCA_ROUTED_TYPES_H_
#define PRTE_MCA_ROUTED_TYPES_H_

#include "prte_config.h"
#include "types.h"

#include "src/class/prte_bitmap.h"
#include "src/class/prte_list.h"

BEGIN_C_DECLS

/* struct for tracking routing trees */
typedef struct {
    prte_list_item_t super;
    pmix_rank_t rank;
    prte_bitmap_t relatives;
} prte_routed_tree_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_routed_tree_t);

/* struct for tracking external routes */
typedef struct {
    prte_object_t super;
    uint16_t job_family;
    pmix_proc_t route;
    char *hnp_uri;
} prte_routed_jobfam_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_routed_jobfam_t);

END_C_DECLS

#endif
