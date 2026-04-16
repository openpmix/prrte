/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_base.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/ras/base/base.h"
#include "ras_hosts.h"

/*
 * Local functions
 */
static int ras_hosts_component_query(pmix_mca_base_module_t **module, int *priority);

prte_ras_base_component_t prte_mca_ras_hosts_component = {
    PRTE_RAS_BASE_VERSION_2_0_0,

    /* Component name and version */
    .pmix_mca_component_name = "hosts",
    PMIX_MCA_BASE_MAKE_VERSION(component,
                               PRTE_MAJOR_VERSION,
                               PRTE_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),
    .pmix_mca_query_component = ras_hosts_component_query,
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, ras, hosts)

static int ras_hosts_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* always make ourselves available with lowest priority among ras components */
    *module = (pmix_mca_base_module_t *) &prte_ras_hosts_module;
    *priority = 1;
    return PRTE_SUCCESS;
}
