/*
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#ifndef MCA_STATE_DVM_EXPORT_H
#define MCA_STATE_DVM_EXPORT_H

#include "prrte_config.h"

#include "src/mca/state/state.h"

BEGIN_C_DECLS

/*
 * Local Component structures
 */

PRRTE_MODULE_EXPORT extern prrte_state_base_component_t prrte_state_dvm_component;

PRRTE_EXPORT extern prrte_state_base_module_t prrte_state_dvm_module;

END_C_DECLS

#endif /* MCA_STATE_DVM_EXPORT_H */
