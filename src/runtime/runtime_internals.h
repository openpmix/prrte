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
 * Copyright (c) 2007-2008 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Interface into the PRRTE Run Time Environment
 */
#ifndef PRRTE_RUNTIME_INTERNALS_H
#define PRRTE_RUNTIME_INTERNALS_H

#include "prrte_config.h"

BEGIN_C_DECLS

/**
 * Init the PRRTE datatype support
 */
PRRTE_EXPORT   int prrte_dt_init(void);


END_C_DECLS

#endif /* PRRTE_RUNTIME_INTERNALS_H */
