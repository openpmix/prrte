/*
 * Copyright (c) 2017-2020  The University of Tennessee and The University
 *                          of Tennessee Research Foundation.  All rights
 *                          reserved.
 *
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

#include "prrte_config.h"

#include "src/mca/propagate/propagate.h"

BEGIN_C_DECLS

/*
 * Local Component structures
 */

PRRTE_MODULE_EXPORT extern prrte_propagate_base_component_t prrte_propagate_prperror_component;

PRRTE_EXPORT extern prrte_propagate_base_module_t prrte_propagate_prperror_module;

PRRTE_EXPORT extern prrte_list_t prrte_error_procs;

END_C_DECLS

#endif /* MCA_PROPAGATE_PRPERROR_EXPORT_H */
