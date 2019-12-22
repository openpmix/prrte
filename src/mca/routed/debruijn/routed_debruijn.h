/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_ROUTED_DEBRUIJN_H
#define MCA_ROUTED_DEBRUIJN_H

#include "prrte_config.h"

#include "src/mca/routed/routed.h"

BEGIN_C_DECLS

PRRTE_MODULE_EXPORT extern prrte_routed_component_t prrte_routed_debruijn_component;

extern prrte_routed_module_t prrte_routed_debruijn_module;

END_C_DECLS

#endif
