/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/runtime/prrte_locks.h"

/* for everyone */
prrte_atomic_lock_t prrte_finalize_lock = PRRTE_ATOMIC_LOCK_INIT;

/* for HNPs */
prrte_atomic_lock_t prrte_abort_inprogress_lock = PRRTE_ATOMIC_LOCK_INIT;
prrte_atomic_lock_t prrte_jobs_complete_lock = PRRTE_ATOMIC_LOCK_INIT;
prrte_atomic_lock_t prrte_quit_lock = PRRTE_ATOMIC_LOCK_INIT;

int prrte_locks_init(void)
{
    /* for everyone */
    prrte_atomic_lock_init(&prrte_finalize_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);

    /* for HNPs */
    prrte_atomic_lock_init(&prrte_abort_inprogress_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);
    prrte_atomic_lock_init(&prrte_jobs_complete_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);
    prrte_atomic_lock_init(&prrte_quit_lock, PRRTE_ATOMIC_LOCK_UNLOCKED);

    return PRRTE_SUCCESS;
}
