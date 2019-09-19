/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef  PRRTE_MUTEX_UNIX_H
#define  PRRTE_MUTEX_UNIX_H 1

/**
 * @file:
 *
 * Mutual exclusion functions: Unix implementation.
 *
 * Functions for locking of critical sections.
 *
 * On unix, use pthreads or our own atomic operations as
 * available.
 */

#include "prrte_config.h"

#include <pthread.h>
#include <errno.h>
#include <stdio.h>

#include "src/class/prrte_object.h"
#include "src/sys/atomic.h"

BEGIN_C_DECLS

struct prrte_mutex_t {
    prrte_object_t super;

    pthread_mutex_t m_lock_pthread;

#if PRRTE_ENABLE_DEBUG
    int m_lock_debug;
    const char *m_lock_file;
    int m_lock_line;
#endif

    prrte_atomic_lock_t m_lock_atomic;
};
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_mutex_t);
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_recursive_mutex_t);

#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
#define PRRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#elif defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
#define PRRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

#if PRRTE_ENABLE_DEBUG
#define PRRTE_MUTEX_STATIC_INIT                                          \
    {                                                                   \
        .super = PRRTE_OBJ_STATIC_INIT(prrte_mutex_t),                    \
        .m_lock_pthread = PTHREAD_MUTEX_INITIALIZER,                    \
        .m_lock_debug = 0,                                              \
        .m_lock_file = NULL,                                            \
        .m_lock_line = 0,                                               \
        .m_lock_atomic = {PRRTE_ATOMIC_LOCK_UNLOCKED},                   \
    }
#else
#define PRRTE_MUTEX_STATIC_INIT                                          \
    {                                                                   \
        .super = PRRTE_OBJ_STATIC_INIT(prrte_mutex_t),                    \
        .m_lock_pthread = PTHREAD_MUTEX_INITIALIZER,                    \
        .m_lock_atomic = {PRRTE_ATOMIC_LOCK_UNLOCKED},                   \
    }
#endif

#if defined(PRRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER)

#if PRRTE_ENABLE_DEBUG
#define PRRTE_RECURSIVE_MUTEX_STATIC_INIT                                \
    {                                                                   \
        .super = PRRTE_OBJ_STATIC_INIT(prrte_mutex_t),                    \
        .m_lock_pthread = PRRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER,     \
        .m_lock_debug = 0,                                              \
        .m_lock_file = NULL,                                            \
        .m_lock_line = 0,                                               \
        .m_lock_atomic = {PRRTE_ATOMIC_LOCK_UNLOCKED},                   \
    }
#else
#define PRRTE_RECURSIVE_MUTEX_STATIC_INIT                                \
    {                                                                   \
        .super = PRRTE_OBJ_STATIC_INIT(prrte_mutex_t),                    \
        .m_lock_pthread = PRRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER,     \
        .m_lock_atomic = {PRRTE_ATOMIC_LOCK_UNLOCKED},                   \
    }
#endif

#endif

/************************************************************************
 *
 * mutex operations (non-atomic versions)
 *
 ************************************************************************/

static inline int prrte_mutex_trylock(prrte_mutex_t *m)
{
#if PRRTE_ENABLE_DEBUG
    int ret = pthread_mutex_trylock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("prrte_mutex_trylock()");
        abort();
    }
    return ret;
#else
    return pthread_mutex_trylock(&m->m_lock_pthread);
#endif
}

static inline void prrte_mutex_lock(prrte_mutex_t *m)
{
#if PRRTE_ENABLE_DEBUG
    int ret = pthread_mutex_lock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("prrte_mutex_lock()");
        abort();
    }
#else
    pthread_mutex_lock(&m->m_lock_pthread);
#endif
}

static inline void prrte_mutex_unlock(prrte_mutex_t *m)
{
#if PRRTE_ENABLE_DEBUG
    int ret = pthread_mutex_unlock(&m->m_lock_pthread);
    if (ret == EPERM) {
        errno = ret;
        perror("prrte_mutex_unlock");
        abort();
    }
#else
    pthread_mutex_unlock(&m->m_lock_pthread);
#endif
}

/************************************************************************
 *
 * mutex operations (atomic versions)
 *
 ************************************************************************/

#if PRRTE_HAVE_ATOMIC_SPINLOCKS

/************************************************************************
 * Spin Locks
 ************************************************************************/

static inline int prrte_mutex_atomic_trylock(prrte_mutex_t *m)
{
    return prrte_atomic_trylock(&m->m_lock_atomic);
}

static inline void prrte_mutex_atomic_lock(prrte_mutex_t *m)
{
    prrte_atomic_lock(&m->m_lock_atomic);
}

static inline void prrte_mutex_atomic_unlock(prrte_mutex_t *m)
{
    prrte_atomic_unlock(&m->m_lock_atomic);
}

#else

/************************************************************************
 * Standard locking
 ************************************************************************/

static inline int prrte_mutex_atomic_trylock(prrte_mutex_t *m)
{
    return prrte_mutex_trylock(m);
}

static inline void prrte_mutex_atomic_lock(prrte_mutex_t *m)
{
    prrte_mutex_lock(m);
}

static inline void prrte_mutex_atomic_unlock(prrte_mutex_t *m)
{
    prrte_mutex_unlock(m);
}

#endif

END_C_DECLS

#endif                          /* PRRTE_MUTEX_UNIX_H */
