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
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#ifndef PRTE_RMAPS_MINDIST_H
#define PRTE_RMAPS_MINDIST_H

#include "prte_config.h"

#include "src/class/prte_list.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

PRTE_MODULE_EXPORT extern prte_rmaps_base_component_t prte_rmaps_mindist_component;
extern prte_rmaps_base_module_t prte_rmaps_mindist_module;

END_C_DECLS

#endif
