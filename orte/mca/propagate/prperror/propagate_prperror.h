/*
 * Copyright (c) 2017-2018  The University of Tennessee and The University
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

#include "orte_config.h"

#include "orte/mca/propagate/propagate.h"

BEGIN_C_DECLS

/*
 * Local Component structures
 */

ORTE_MODULE_DECLSPEC extern orte_propagate_base_component_t mca_propagate_prperror_component;

ORTE_DECLSPEC extern orte_propagate_base_module_t orte_propagate_prperror_module;

ORTE_DECLSPEC extern opal_list_t orte_error_procs;

END_C_DECLS

#endif /* MCA_PROPAGATE_PRPERROR_EXPORT_H */
