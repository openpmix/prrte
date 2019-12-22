/* -*- C -*-
 *
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef GRPCOMM_DIRECT_H
#define GRPCOMM_DIRECT_H

#include "prrte_config.h"


#include "src/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/*
 * Grpcomm interfaces
 */

PRRTE_MODULE_EXPORT extern prrte_grpcomm_base_component_t prrte_grpcomm_direct_component;
extern prrte_grpcomm_base_module_t prrte_grpcomm_direct_module;

END_C_DECLS

#endif
