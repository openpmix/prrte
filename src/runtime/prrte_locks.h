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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Locks to prevent loops inside PRRTE
 */
#ifndef PRRTE_LOCKS_H
#define PRRTE_LOCKS_H

#include "prrte_config.h"

#include "src/sys/atomic.h"

BEGIN_C_DECLS

/* for everyone */
PRRTE_EXPORT extern prrte_atomic_lock_t prrte_finalize_lock;

/* for HNPs */
PRRTE_EXPORT extern prrte_atomic_lock_t prrte_abort_inprogress_lock;
PRRTE_EXPORT extern prrte_atomic_lock_t prrte_jobs_complete_lock;
PRRTE_EXPORT extern prrte_atomic_lock_t prrte_quit_lock;

/**
 * Initialize the locks
 */
PRRTE_EXPORT int prrte_locks_init(void);

END_C_DECLS

#endif /* #ifndef PRRTE_LOCKS_H */
