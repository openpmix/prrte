/*
 * Copyright (c) 2008-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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


#ifndef PRRTE_MCA_ROUTED_TYPES_H_
#define PRRTE_MCA_ROUTED_TYPES_H_

#include "prrte_config.h"
#include "types.h"

#include "src/class/prrte_bitmap.h"
#include "src/class/prrte_list.h"

BEGIN_C_DECLS

/* struct for tracking routing trees */
typedef struct {
    prrte_list_item_t super;
    prrte_vpid_t vpid;
    prrte_bitmap_t relatives;
} prrte_routed_tree_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_routed_tree_t);

/* struct for tracking external routes */
typedef struct {
    prrte_object_t super;
    uint16_t job_family;
    prrte_process_name_t route;
    char *hnp_uri;
} prrte_routed_jobfam_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_routed_jobfam_t);

END_C_DECLS

#endif
