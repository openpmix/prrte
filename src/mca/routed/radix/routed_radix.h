/*
 * Copyright (c) 2007      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_ROUTED_RADIX_H
#define MCA_ROUTED_RADIX_H

#include "prrte_config.h"

#include "src/mca/routed/routed.h"

BEGIN_C_DECLS

typedef struct {
    prrte_routed_component_t super;
    int radix;
} prrte_routed_radix_component_t;
PRRTE_MODULE_EXPORT extern prrte_routed_radix_component_t prrte_routed_radix_component;

extern prrte_routed_module_t prrte_routed_radix_module;

END_C_DECLS

#endif
