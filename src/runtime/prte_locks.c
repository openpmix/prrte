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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "constants.h"

#include "src/runtime/prte_locks.h"

/* for everyone */
prte_atomic_lock_t prte_finalize_lock = PRTE_ATOMIC_LOCK_INIT;

/* for HNPs */
prte_atomic_lock_t prte_abort_inprogress_lock = PRTE_ATOMIC_LOCK_INIT;
prte_atomic_lock_t prte_jobs_complete_lock = PRTE_ATOMIC_LOCK_INIT;
prte_atomic_lock_t prte_quit_lock = PRTE_ATOMIC_LOCK_INIT;

int prte_locks_init(void)
{
    /* for everyone */
    prte_atomic_lock_init(&prte_finalize_lock, PRTE_ATOMIC_LOCK_UNLOCKED);

    /* for HNPs */
    prte_atomic_lock_init(&prte_abort_inprogress_lock, PRTE_ATOMIC_LOCK_UNLOCKED);
    prte_atomic_lock_init(&prte_jobs_complete_lock, PRTE_ATOMIC_LOCK_UNLOCKED);
    prte_atomic_lock_init(&prte_quit_lock, PRTE_ATOMIC_LOCK_UNLOCKED);

    return PRTE_SUCCESS;
}
