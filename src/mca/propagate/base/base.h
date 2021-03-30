/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "prte_config.h"

#include "src/class/prte_hash_table.h"
#include "src/class/prte_list.h"
#include "src/mca/mca.h"

#include "src/mca/odls/odls_types.h"
#include "src/mca/propagate/propagate.h"
#include "src/mca/rml/rml_types.h"

/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

PRTE_EXPORT extern prte_propagate_base_module_t prte_propagate;
/*
 * MCA framework
 */
PRTE_EXPORT extern prte_mca_base_framework_t prte_propagate_base_framework;
/*
 * Select an available component.
 */
PRTE_EXPORT int prte_propagate_base_select(void);

END_C_DECLS
#endif
