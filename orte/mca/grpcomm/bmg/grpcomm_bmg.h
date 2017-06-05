/*
 *
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef GRPCOMM_BMG_H
#define GRPCOMM_BMG_H

#include "orte_config.h"

#include "orte/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/*
 * Grpcomm interfaces
 */

ORTE_MODULE_DECLSPEC extern orte_grpcomm_base_component_t mca_grpcomm_bmg_component;
extern orte_grpcomm_base_module_t orte_grpcomm_bmg_module;

END_C_DECLS

#endif

