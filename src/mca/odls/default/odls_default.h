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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file:
 */

#ifndef PRRTE_ODLS_DEFAULT_H
#define PRRTE_ODLS_DEFAULT_H

#include "prrte_config.h"

#include "src/mca/mca.h"

#include "src/mca/odls/odls.h"

BEGIN_C_DECLS

/*
 * Module open / close
 */
int prrte_odls_default_component_open(void);
int prrte_odls_default_component_close(void);
int prrte_odls_default_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * ODLS Default module
 */
extern prrte_odls_base_module_t prrte_odls_default_module;
PRRTE_MODULE_EXPORT extern prrte_odls_base_component_t prrte_odls_default_component;

END_C_DECLS

#endif /* PRRTE_ODLS_H */
