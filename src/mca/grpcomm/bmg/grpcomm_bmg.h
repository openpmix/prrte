/*
 *
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef GRPCOMM_BMG_H
#define GRPCOMM_BMG_H

#include "prte_config.h"

#include "src/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/*
 * Grpcomm interfaces
 */

PRTE_MODULE_EXPORT extern prte_grpcomm_base_component_t prte_grpcomm_bmg_component;
extern prte_grpcomm_base_module_t prte_grpcomm_bmg_module;

END_C_DECLS

#endif
