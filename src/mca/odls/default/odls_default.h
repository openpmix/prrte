/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file:
 */

#ifndef PRTE_ODLS_DEFAULT_H
#define PRTE_ODLS_DEFAULT_H

#include "prte_config.h"

#include "src/mca/mca.h"

#include "src/mca/odls/odls.h"

BEGIN_C_DECLS

/*
 * Module open / close
 */
int prte_mca_odls_default_component_open(void);
int prte_mca_odls_default_component_close(void);
int prte_mca_odls_default_component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * ODLS Default module
 */
extern prte_odls_base_module_t prte_odls_default_module;
PRTE_MODULE_EXPORT extern prte_odls_base_component_t prte_mca_odls_default_component;

END_C_DECLS

#endif /* PRTE_ODLS_H */
