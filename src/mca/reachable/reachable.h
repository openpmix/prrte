/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_REACHABLE_H
#define PRRTE_REACHABLE_H

#include "prrte_config.h"
#include "src/include/types.h"
#include "src/class/prrte_object.h"

#include "src/mca/mca.h"
#include "src/mca/if/if.h"


BEGIN_C_DECLS

/**
 * Reachability matrix between endpoints of a given pair of hosts
 *
 * The output of the reachable() call is a prrte_reachable_t, which
 * gives an matrix of the connectivity between local and remote
 * ethernet endpoints.  Any given value in weights is the connectivity
 * between the local endpoint index (first index) and the remote
 * endpoint index (second index), and is a value between 0 and INT_MAX
 * representing a relative connectivity.
 */
struct prrte_reachable_t {
    prrte_object_t super;
    /** number of local interfaces passed to reachable() */
    int num_local;
    /** number of remote interfaces passed to reachable() */
    int num_remote;
    /** matric of connectivity weights */
    int **weights;
    /** \internal */
    void *memory;
};
typedef struct prrte_reachable_t prrte_reachable_t;
PRRTE_CLASS_DECLARATION(prrte_reachable_t);

/* Init */
typedef int (*prrte_reachable_base_module_init_fn_t)(void);

/* Finalize */
typedef int (*prrte_reachable_base_module_fini_fn_t)(void);

/* Build reachability matrix between local and remote ethernet
 * interfaces
 *
 * @param local_ifs (IN)     Local list of prrte_if_t objects
 *                           The prrte_if_t objects must be fully populated
 * @param remote_ifs (IN)    Remote list of prrte_if_t objects
 *                           The prrte_if_t objects must have the following fields populated:
 *                              uint16_t                 af_family;
 *                              struct sockaddr_storage  if_addr;
 *                              uint32_t                 if_mask;
 *                              uint32_t                 if_bandwidth;
 * @return prrte_reachable_t  The reachability matrix was successfully created
 * @return NULL              The reachability matrix could not be constructed
 *
 * Given a list of local interfaces and remote interfaces from a
 * single peer, build a reachability matrix between the two peers.
 * This function does not select the best pairing of local and remote
 * interfaces, but only a (comparable) reachability between any pair
 * of local/remote interfaces.
 *
 *
 */
typedef prrte_reachable_t*
(*prrte_reachable_base_module_reachable_fn_t)(prrte_list_t *local_ifs,
                                              prrte_list_t *remote_ifs);


/*
 * the standard public API data structure
 */
typedef struct {
    /* currently used APIs */
    prrte_reachable_base_module_init_fn_t                   init;
    prrte_reachable_base_module_fini_fn_t                   finalize;
    prrte_reachable_base_module_reachable_fn_t              reachable;
} prrte_reachable_base_module_t;

typedef struct {
    prrte_mca_base_component_t                      base_version;
    prrte_mca_base_component_data_t                 base_data;
    int priority;
} prrte_reachable_base_component_t;

/*
 * Macro for use in components that are of type reachable
 */
#define PRRTE_REACHABLE_BASE_VERSION_2_0_0             \
    PRRTE_MCA_BASE_VERSION_2_1_0("reachable", 2, 0, 0)

/* Global structure for accessing reachability functions */
PRRTE_EXPORT extern prrte_reachable_base_module_t prrte_reachable;


END_C_DECLS

#endif
