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
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file:
 *
 * Reliable Messaging (RELM) base framework
 */

#ifndef PRTE_RELM_BASE_H
#define PRTE_RELM_BASE_H

#include "src/rml/relm/relm.h"

extern prte_relm_module_t prte_relm_base_module;

PRTE_EXPORT void prte_relm_base_register(void);

typedef struct {
    int output;
    int verbosity;
    int cache_ms;
    int cache_max_count;
} prte_relm_base_t;
PRTE_EXPORT extern prte_relm_base_t prte_relm_base;

#endif
