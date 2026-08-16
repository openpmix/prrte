/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2010 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2012-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The PRTE Environment-Specific Services
 *
 */

#ifndef PRTE_ESS_H
#define PRTE_ESS_H

#include "prte_config.h"
#include "types.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/mca.h"

#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

BEGIN_C_DECLS

/*
 * API functions
 */

/*
 * Initialize the RTE for this environment
 */
typedef int (*prte_ess_base_module_init_fn_t)(int argc, char **argv);

/*
 * Finalize the RTE for this environment
 */
typedef int (*prte_ess_base_module_finalize_fn_t)(void);

/*
 * the standard module data structure
 */
struct prte_ess_base_module_3_0_0_t {
    prte_ess_base_module_init_fn_t init;
    prte_ess_base_module_finalize_fn_t finalize;
};
typedef struct prte_ess_base_module_3_0_0_t prte_ess_base_module_3_0_0_t;
typedef struct prte_ess_base_module_3_0_0_t prte_ess_base_module_t;

/*
 * the standard component data structure
 */
typedef pmix_mca_base_component_t prte_ess_base_component_t;

/* The ess framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PRTE_MCA_BASE_VERSION(ess), and the framework's declaration reaches
 * the same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component built
 * against the previous one would not survive. */
#define PRTE_MCA_ess_MAJOR_VERSION   3
#define PRTE_MCA_ess_MINOR_VERSION   0
#define PRTE_MCA_ess_RELEASE_VERSION 0

/* Global structure for accessing ESS functions */
PRTE_EXPORT extern prte_ess_base_module_t prte_ess; /* holds selected module's function pointers */

END_C_DECLS

#endif
