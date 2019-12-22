/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
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

#ifndef prrte_errmgr_dvm_EXPORT_H
#define prrte_errmgr_dvm_EXPORT_H

#include "prrte_config.h"

#include "src/mca/errmgr/errmgr.h"

BEGIN_C_DECLS

/*
 * Local Component structures
 */

PRRTE_MODULE_EXPORT extern prrte_errmgr_base_component_t prrte_errmgr_dvm_component;

PRRTE_EXPORT extern prrte_errmgr_base_module_t prrte_errmgr_dvm_module;

END_C_DECLS

#endif /* prrte_errmgr_dvm_EXPORT_H */
