/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Resource Mapping
 */


#ifndef PRRTE_RMAPS_RF_H
#define PRRTE_RMAPS_RF_H

#include "prrte_config.h"

#include "src/class/prrte_object.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

int prrte_rmaps_rank_file_lex_destroy (void);

struct prrte_rmaps_rf_component_t {
    prrte_rmaps_base_component_t super;
    char *slot_list;
    bool physical;
};
typedef struct prrte_rmaps_rf_component_t prrte_rmaps_rf_component_t;

PRRTE_MODULE_EXPORT extern prrte_rmaps_rf_component_t prrte_rmaps_rank_file_component;
extern prrte_rmaps_base_module_t prrte_rmaps_rank_file_module;


typedef struct cpu_socket_t cpu_socket_t;

struct prrte_rmaps_rank_file_map_t {
    prrte_object_t super;
    char* node_name;
    char slot_list[64];
};
typedef struct prrte_rmaps_rank_file_map_t prrte_rmaps_rank_file_map_t;

PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rmaps_rank_file_map_t);

END_C_DECLS

#endif
