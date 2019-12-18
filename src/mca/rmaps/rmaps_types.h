/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/* Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_RMAPS_TYPES_H
#define PRRTE_MCA_RMAPS_TYPES_H

#include "prrte_config.h"
#include "constants.h"

#include "src/class/prrte_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/runtime/prrte_globals.h"

/*
 * General MAP types - instanced in runtime/prrte_globals_class_instances.h
 */

BEGIN_C_DECLS

typedef uint16_t prrte_mapping_policy_t;
#define PRRTE_MAPPING_POLICY PRRTE_UINT16
typedef uint16_t prrte_ranking_policy_t;
#define PRRTE_RANKING_POLICY PRRTE_UINT16

/*
 * Structure that represents the mapping of a job to an
 * allocated set of resources.
 */
struct prrte_job_map_t {
    prrte_object_t super;
    /* user-specified mapping params */
    char *req_mapper;  /* requested mapper */
    char *last_mapper; /* last mapper used */
    prrte_mapping_policy_t mapping;
    prrte_ranking_policy_t ranking;
    prrte_binding_policy_t binding;
    /* mapping options */
    char *ppr;
    int16_t cpus_per_rank;
    bool display_map;
    /* *** */
    /* number of new daemons required to be launched
     * to support this job map
     */
    prrte_std_cntr_t num_new_daemons;
    /* starting vpid of the new daemons - they will
     * be sequential from that point
     */
    prrte_vpid_t daemon_vpid_start;
    /* number of nodes participating in this job */
    prrte_std_cntr_t num_nodes;
    /* array of pointers to nodes in this map for this job */
    prrte_pointer_array_t *nodes;
};
typedef struct prrte_job_map_t prrte_job_map_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_job_map_t);

/**
 * Macro for use in components that are of type rmaps
 */
#define PRRTE_RMAPS_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("rmaps", 2, 0, 0)

/* define map-related directives */
#define PRRTE_MAPPING_NO_USE_LOCAL      0x0100
#define PRRTE_MAPPING_NO_OVERSUBSCRIBE  0x0200
#define PRRTE_MAPPING_SUBSCRIBE_GIVEN   0x0400
#define PRRTE_MAPPING_SPAN              0x0800
/* an error flag */
#define PRRTE_MAPPING_CONFLICTED        0x1000
/* directives given */
#define PRRTE_MAPPING_LOCAL_GIVEN       0x2000
#define PRRTE_MAPPING_GIVEN             0x4000
/* mapping a debugger job */
#define PRRTE_MAPPING_DEBUGGER          0x8000
#define PRRTE_SET_MAPPING_DIRECTIVE(target, pol) \
    (target) |= (pol)
#define PRRTE_UNSET_MAPPING_DIRECTIVE(target, pol) \
    (target) &= ~(pol)
#define PRRTE_GET_MAPPING_DIRECTIVE(pol) \
    ((pol) & 0xff00)

/* round-robin policies */
/* start with hardware-based options
 * so the values match the corresponding
 * levels in src/hwloc/hwloc-internal.h
 */
#define PRRTE_MAPPING_BYNODE            1
#define PRRTE_MAPPING_BYBOARD           2
#define PRRTE_MAPPING_BYNUMA            3
#define PRRTE_MAPPING_BYSOCKET          4
#define PRRTE_MAPPING_BYL3CACHE         5
#define PRRTE_MAPPING_BYL2CACHE         6
#define PRRTE_MAPPING_BYL1CACHE         7
#define PRRTE_MAPPING_BYCORE            8
#define PRRTE_MAPPING_BYHWTHREAD        9
/* now take the other round-robin options */
#define PRRTE_MAPPING_BYSLOT            10
#define PRRTE_MAPPING_BYDIST            11
/* convenience - declare anything <= 15 to be round-robin*/
#define PRRTE_MAPPING_RR                0x000f

/* sequential policy */
#define PRRTE_MAPPING_SEQ               20
/* staged execution mapping */
#define PRRTE_MAPPING_STAGED            21
/* rank file and other user-defined mapping */
#define PRRTE_MAPPING_BYUSER            22
/* pattern-based mapping */
#define PRRTE_MAPPING_PPR               23
/* macro to separate out the mapping policy
 * from the directives
 */
#define PRRTE_GET_MAPPING_POLICY(pol) \
    ((pol) & 0x00ff)
/* macro to determine if mapping policy is set */
#define PRRTE_MAPPING_POLICY_IS_SET(pol) \
    ((pol) & 0x00ff)
#define PRRTE_SET_MAPPING_POLICY(target, pol)     \
    (target) = (pol) | ((target) & 0xff00)

/* define ranking directives */
#define PRRTE_RANKING_SPAN           0x1000
#define PRRTE_RANKING_FILL           0x2000
#define PRRTE_RANKING_GIVEN          0x4000
#define PRRTE_SET_RANKING_DIRECTIVE(target, pol) \
    (target) |= (pol)
#define PRRTE_UNSET_RANKING_DIRECTIVE(target, pol) \
    (target) &= ~(pol)
#define PRRTE_GET_RANKING_DIRECTIVE(pol) \
    ((pol) & 0xf000)

/* define ranking policies */
#define PRRTE_RANK_BY_NODE           1
#define PRRTE_RANK_BY_BOARD          2
#define PRRTE_RANK_BY_NUMA           3
#define PRRTE_RANK_BY_SOCKET         4
#define PRRTE_RANK_BY_L3CACHE        5
#define PRRTE_RANK_BY_L2CACHE        6
#define PRRTE_RANK_BY_L1CACHE        7
#define PRRTE_RANK_BY_CORE           8
#define PRRTE_RANK_BY_HWTHREAD       9
#define PRRTE_RANK_BY_SLOT           10
#define PRRTE_GET_RANKING_POLICY(pol) \
    ((pol) & 0x0fff)
/* macro to determine if ranking policy is set */
#define PRRTE_RANKING_POLICY_IS_SET(pol) \
    ((pol) & 0x0fff)
#define PRRTE_SET_RANKING_POLICY(target, pol)     \
    (target) = (pol) | ((target) & 0xf000)

END_C_DECLS

#endif
