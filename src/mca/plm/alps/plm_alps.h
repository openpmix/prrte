/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_PLM_ALPS_EXPORT_H
#define PRRTE_PLM_ALPS_EXPORT_H

#include "prrte_config.h"

#include "src/mca/mca.h"
#include "src/mca/plm/plm.h"
#if CRAY_WLM_DETECT
#include "wlm_detect.h"
#endif

BEGIN_C_DECLS

struct prrte_plm_alps_component_t {
    prrte_plm_base_component_t super;
    int priority;
    bool debug;
    char *aprun_cmd;
    char *custom_args;
};
typedef struct prrte_plm_alps_component_t prrte_plm_alps_component_t;

/*
 * Globally exported variable
 */

PRRTE_MODULE_EXPORT extern prrte_plm_alps_component_t
    prrte_plm_alps_component;
PRRTE_EXPORT extern prrte_plm_base_module_t
    prrte_plm_alps_module;

END_C_DECLS

#endif /* PRRTE_PLM_ALPS_EXPORT_H */
