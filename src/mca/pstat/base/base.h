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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef PRTE_PSTAT_BASE_H
#define PRTE_PSTAT_BASE_H

#include "prte_config.h"
#include "src/mca/base/prte_mca_base_framework.h"
#include "src/mca/pstat/pstat.h"

/*
 * Global functions for MCA overall pstat open and close
 */

BEGIN_C_DECLS

/**
 * Framework structure declaration for this framework
 */
PRTE_EXPORT extern prte_mca_base_framework_t prte_pstat_base_framework;

/**
 * Select an available component.
 *
 * @return PRTE_SUCCESS Upon success.
 * @return PRTE_NOT_FOUND If no component can be selected.
 * @return PRTE_ERROR Upon other failure.
 *
 * At the end of this process, we'll either have a single
 * component that is selected and initialized, or no component was
 * selected.  If no component was selected, subsequent invocation
 * of the pstat functions will return an error indicating no data
 * could be obtained
 */
PRTE_EXPORT int prte_pstat_base_select(void);

PRTE_EXPORT extern prte_pstat_base_component_t *prte_pstat_base_component;

END_C_DECLS

#endif /* PRTE_BASE_PSTAT_H */
