/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef PRRTE_BACKTRACE_BASE_H
#define PRRTE_BACKTRACE_BASE_H

#include "prrte_config.h"

#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/mca/backtrace/backtrace.h"

/*
 * Global functions for MCA overall backtrace open and close
 */

BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_backtrace_base_framework;

END_C_DECLS
#endif /* PRRTE_BASE_BACKTRACE_H */
