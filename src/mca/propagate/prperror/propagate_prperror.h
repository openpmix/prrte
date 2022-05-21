/*
 * Copyright (c) 2017-2020  The University of Tennessee and The University
 *                          of Tennessee Research Foundation.  All rights
 *                          reserved.
 *
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 */

#ifndef MCA_PROPAGATE_PRPERROR_EXPORT_H
#define MCA_PROPAGATE_PRPERROR_EXPORT_H

#include "prte_config.h"

#include "src/mca/propagate/propagate.h"

BEGIN_C_DECLS

/*
 * Local Component structures
 */

PRTE_MODULE_EXPORT extern prte_propagate_base_component_t prte_propagate_prperror_component;

PRTE_EXPORT extern prte_propagate_base_module_t prte_propagate_prperror_module;

PRTE_EXPORT extern pmix_list_t prte_error_procs;

END_C_DECLS

#endif /* MCA_PROPAGATE_PRPERROR_EXPORT_H */
