/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_ERRMGR_BASE_H
#define PRRTE_MCA_ERRMGR_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"

#include "src/class/prrte_list.h"

#include "src/mca/mca.h"
#include "src/mca/errmgr/errmgr.h"


BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_errmgr_base_framework;
/* select a component */
PRRTE_EXPORT    int prrte_errmgr_base_select(void);

END_C_DECLS

#endif
