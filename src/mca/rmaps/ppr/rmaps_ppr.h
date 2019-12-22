/*
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_RMAPS_PPR_H
#define PRRTE_RMAPS_PPR_H

#include "prrte_config.h"

#include "src/hwloc/hwloc-internal.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

PRRTE_MODULE_EXPORT extern prrte_rmaps_base_component_t prrte_rmaps_ppr_component;
extern prrte_rmaps_base_module_t prrte_rmaps_ppr_module;


END_C_DECLS

#endif
