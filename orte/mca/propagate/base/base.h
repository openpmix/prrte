/*
 * Copyright (c) 2017-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_PROPAGATE_BASE_H
#define MCA_PROPAGATE_BASE_H

/*
 * includes
 */
#include "orte_config.h"

#include "opal/class/opal_list.h"
#include "opal/class/opal_hash_table.h"
#include "opal/dss/dss_types.h"
#include "orte/mca/mca.h"

#include "orte/mca/odls/odls_types.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/propagate/propagate.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

ORTE_DECLSPEC extern orte_propagate_base_module_t orte_propagate;
/*
 * MCA framework
 */
ORTE_DECLSPEC extern mca_base_framework_t orte_propagate_base_framework;
/*
 * Select an available component.
 */
ORTE_DECLSPEC int orte_propagate_base_select(void);

END_C_DECLS
#endif
