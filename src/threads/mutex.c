/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/threads/mutex.h"

static void prrte_mutex_construct(prrte_mutex_t *m)
{
#if PRRTE_ENABLE_DEBUG
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);

    /* set type to ERRORCHECK so that we catch recursive locks */
#if PRRTE_HAVE_PTHREAD_MUTEX_ERRORCHECK_NP
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);
#elif PRRTE_HAVE_PTHREAD_MUTEX_ERRORCHECK
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#endif /* PRRTE_HAVE_PTHREAD_MUTEX_ERRORCHECK_NP */

    pthread_mutex_init(&m->m_lock_pthread, &attr);
    pthread_mutexattr_destroy(&attr);

    m->m_lock_debug = 0;
    m->m_lock_file = NULL;
    m->m_lock_line = 0;
#else

    /* Without debugging, choose the fastest available mutexes */
    pthread_mutex_init(&m->m_lock_pthread, NULL);

#endif /* PRRTE_ENABLE_DEBUG */

#if PRRTE_HAVE_ATOMIC_SPINLOCKS
    prrte_atomic_lock_init( &m->m_lock_atomic, PRRTE_ATOMIC_LOCK_UNLOCKED );
#endif
}

static void prrte_mutex_destruct(prrte_mutex_t *m)
{
    pthread_mutex_destroy(&m->m_lock_pthread);
}

PRRTE_CLASS_INSTANCE(prrte_mutex_t,
                   prrte_object_t,
                   prrte_mutex_construct,
                   prrte_mutex_destruct);

static void prrte_recursive_mutex_construct(prrte_recursive_mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);

#if PRRTE_ENABLE_DEBUG
    m->m_lock_debug = 0;
    m->m_lock_file = NULL;
    m->m_lock_line = 0;
#endif

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&m->m_lock_pthread, &attr);
    pthread_mutexattr_destroy(&attr);

#if PRRTE_HAVE_ATOMIC_SPINLOCKS
    prrte_atomic_lock_init( &m->m_lock_atomic, PRRTE_ATOMIC_LOCK_UNLOCKED );
#endif
}

PRRTE_CLASS_INSTANCE(prrte_recursive_mutex_t,
                   prrte_object_t,
                   prrte_recursive_mutex_construct,
                   prrte_mutex_destruct);
