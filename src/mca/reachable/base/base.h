/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_REACHABLE_BASE_H
#define MCA_REACHABLE_BASE_H

#include "prrte_config.h"
#include "src/include/types.h"

#include "src/mca/mca.h"
#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/mca/reachable/reachable.h"

BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_reachable_base_framework;

/**
 * Select a reachable module
 */
PRRTE_EXPORT int prrte_reachable_base_select(void);

PRRTE_EXPORT prrte_reachable_t * prrte_reachable_allocate(unsigned int num_local,
                                                          unsigned int num_remote);


END_C_DECLS

#endif
