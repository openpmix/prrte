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
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_MUTEX_UNIX_H
#define PRTE_MUTEX_UNIX_H 1

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

#include "prte_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#include "src/class/prte_object.h"
#include "src/sys/atomic.h"

BEGIN_C_DECLS

struct prte_mutex_t {
    prte_object_t super;

    pthread_mutex_t m_lock_pthread;

#if PRTE_ENABLE_DEBUG
    int m_lock_debug;
    const char *m_lock_file;
    int m_lock_line;
#endif
};
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_mutex_t);
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_recursive_mutex_t);

#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
#    define PRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#elif defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
#    define PRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

#if PRTE_ENABLE_DEBUG
#    define PRTE_MUTEX_STATIC_INIT                                                               \
        {                                                                                        \
            .super = PRTE_OBJ_STATIC_INIT(prte_mutex_t),                                         \
            .m_lock_pthread = PTHREAD_MUTEX_INITIALIZER, .m_lock_debug = 0, .m_lock_file = NULL, \
            .m_lock_line = 0,                                                                    \
        }
#else
#    define PRTE_MUTEX_STATIC_INIT                                                               \
        {                                                                                        \
            .super = PRTE_OBJ_STATIC_INIT(prte_mutex_t),                                         \
            .m_lock_pthread = PTHREAD_MUTEX_INITIALIZER,                                         \
        }
#endif

#if defined(PRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER)

#    if PRTE_ENABLE_DEBUG
#        define PRTE_RECURSIVE_MUTEX_STATIC_INIT                                               \
            {                                                                                  \
                .super = PRTE_OBJ_STATIC_INIT(prte_mutex_t),                                   \
                .m_lock_pthread = PRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER, .m_lock_debug = 0, \
                .m_lock_file = NULL, .m_lock_line = 0,                                         \
            }
#    else
#        define PRTE_RECURSIVE_MUTEX_STATIC_INIT                            \
            {                                                               \
                .super = PRTE_OBJ_STATIC_INIT(prte_mutex_t),                \
                .m_lock_pthread = PRTE_PTHREAD_RECURSIVE_MUTEX_INITIALIZER, \
            }
#    endif

#endif

/************************************************************************
 *
 * mutex operations
 *
 ************************************************************************/

static inline int prte_mutex_trylock(prte_mutex_t *m)
{
#if PRTE_ENABLE_DEBUG
    int ret = pthread_mutex_trylock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("prte_mutex_trylock()");
        abort();
    }
    return ret;
#else
    return pthread_mutex_trylock(&m->m_lock_pthread);
#endif
}

static inline void prte_mutex_lock(prte_mutex_t *m)
{
#if PRTE_ENABLE_DEBUG
    int ret = pthread_mutex_lock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("prte_mutex_lock()");
        abort();
    }
#else
    pthread_mutex_lock(&m->m_lock_pthread);
#endif
}

static inline void prte_mutex_unlock(prte_mutex_t *m)
{
#if PRTE_ENABLE_DEBUG
    int ret = pthread_mutex_unlock(&m->m_lock_pthread);
    if (ret == EPERM) {
        errno = ret;
        perror("prte_mutex_unlock");
        abort();
    }
#else
    pthread_mutex_unlock(&m->m_lock_pthread);
#endif
}

END_C_DECLS

#endif /* PRTE_MUTEX_UNIX_H */
