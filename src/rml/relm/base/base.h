/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
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
