/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_PROPAGATE_BASE_H
#define MCA_PROPAGATE_BASE_H

/*
 * includes
 */
#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "src/class/prrte_hash_table.h"
#include "src/dss/dss_types.h"
#include "src/mca/mca.h"

#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/propagate/propagate.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_propagate_base_module_t prrte_propagate;
/*
 * MCA framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_propagate_base_framework;
/*
 * Select an available component.
 */
PRRTE_EXPORT int prrte_propagate_base_select(void);

END_C_DECLS
#endif
