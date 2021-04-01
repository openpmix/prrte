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
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_THREAD_H
#define PRTE_THREAD_H 1

#include "prte_config.h"

#include <pthread.h>
#include <signal.h>

#include "src/class/prte_object.h"
#if PRTE_ENABLE_DEBUG
#    include "src/util/output.h"
#endif

#include "mutex.h"

BEGIN_C_DECLS

typedef void *(*prte_thread_fn_t)(prte_object_t *);

#define PRTE_THREAD_CANCELLED ((void *) 1);

struct prte_thread_t {
    prte_object_t super;
    prte_thread_fn_t t_run;
    void *t_arg;
    pthread_t t_handle;
};

typedef struct prte_thread_t prte_thread_t;

#if PRTE_ENABLE_DEBUG
PRTE_EXPORT extern bool prte_debug_threads;
#endif

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_thread_t);

#define prte_condition_wait(a, b) pthread_cond_wait(a, &(b)->m_lock_pthread)
typedef pthread_cond_t prte_condition_t;
#define prte_condition_broadcast(a) pthread_cond_broadcast(a)
#define prte_condition_signal(a)    pthread_cond_signal(a)
#define PRTE_CONDITION_STATIC_INIT  PTHREAD_COND_INITIALIZER

/* define a threadshift macro */
#define PRTE_THREADSHIFT(x, eb, f, p)                                  \
    do {                                                               \
        prte_event_set((eb), &((x)->ev), -1, PRTE_EV_WRITE, (f), (x)); \
        prte_event_set_priority(&((x)->ev), (p));                      \
        PRTE_POST_OBJECT((x));                                         \
        prte_event_active(&((x)->ev), PRTE_EV_WRITE, 1);               \
    } while (0)

typedef struct {
    size_t hdlr_id;
    int status;
    prte_mutex_t mutex;
    prte_condition_t cond;
    volatile bool active;
} prte_lock_t;

#define PRTE_LOCK_STATIC_INIT                                                             \
    {                                                                                     \
        .hdlr_id = 0, .status = 0, .mutex = PRTE_MUTEX_STATIC_INIT,                       \
        .cond = PRTE_CONDITION_STATIC_INIT, .active = false                               \
    }

#define PRTE_CONSTRUCT_LOCK(l)                     \
    do {                                           \
        PRTE_CONSTRUCT(&(l)->mutex, prte_mutex_t); \
        pthread_cond_init(&(l)->cond, NULL);       \
        /* coverity[missing_lock : FALSE] */       \
        (l)->active = true;                        \
    } while (0)

#define PRTE_DESTRUCT_LOCK(l)             \
    do {                                  \
        PRTE_DESTRUCT(&(l)->mutex);       \
        pthread_cond_destroy(&(l)->cond); \
    } while (0)

#if PRTE_ENABLE_DEBUG
#    define PRTE_ACQUIRE_THREAD(lck)                                            \
        do {                                                                    \
            prte_mutex_lock(&(lck)->mutex);                                     \
            if (prte_debug_threads) {                                           \
                prte_output(0, "Waiting for thread %s:%d", __FILE__, __LINE__); \
            }                                                                   \
            while ((lck)->active) {                                             \
                prte_condition_wait(&(lck)->cond, &(lck)->mutex);               \
            }                                                                   \
            if (prte_debug_threads) {                                           \
                prte_output(0, "Thread obtained %s:%d", __FILE__, __LINE__);    \
            }                                                                   \
            PRTE_ACQUIRE_OBJECT(lck);                                           \
            (lck)->active = true;                                               \
        } while (0)
#else
#    define PRTE_ACQUIRE_THREAD(lck)                              \
        do {                                                      \
            prte_mutex_lock(&(lck)->mutex);                       \
            while ((lck)->active) {                               \
                prte_condition_wait(&(lck)->cond, &(lck)->mutex); \
            }                                                     \
            PRTE_ACQUIRE_OBJECT(lck);                             \
            (lck)->active = true;                                 \
        } while (0)
#endif

#if PRTE_ENABLE_DEBUG
#    define PRTE_WAIT_THREAD(lck)                                               \
        do {                                                                    \
            prte_mutex_lock(&(lck)->mutex);                                     \
            if (prte_debug_threads) {                                           \
                prte_output(0, "Waiting for thread %s:%d", __FILE__, __LINE__); \
            }                                                                   \
            while ((lck)->active) {                                             \
                prte_condition_wait(&(lck)->cond, &(lck)->mutex);               \
            }                                                                   \
            if (prte_debug_threads) {                                           \
                prte_output(0, "Thread obtained %s:%d", __FILE__, __LINE__);    \
            }                                                                   \
            PRTE_ACQUIRE_OBJECT(lck);                                           \
            prte_mutex_unlock(&(lck)->mutex);                                   \
        } while (0)
#else
#    define PRTE_WAIT_THREAD(lck)                                 \
        do {                                                      \
            prte_mutex_lock(&(lck)->mutex);                       \
            while ((lck)->active) {                               \
                prte_condition_wait(&(lck)->cond, &(lck)->mutex); \
            }                                                     \
            PRTE_ACQUIRE_OBJECT(lck);                             \
            prte_mutex_unlock(&(lck)->mutex);                     \
        } while (0)
#endif

#if PRTE_ENABLE_DEBUG
#    define PRTE_RELEASE_THREAD(lck)                                          \
        do {                                                                  \
            if (prte_debug_threads) {                                         \
                prte_output(0, "Releasing thread %s:%d", __FILE__, __LINE__); \
            }                                                                 \
            (lck)->active = false;                                            \
            PRTE_POST_OBJECT(lck);                                            \
            prte_condition_broadcast(&(lck)->cond);                           \
            prte_mutex_unlock(&(lck)->mutex);                                 \
        } while (0)
#else
#    define PRTE_RELEASE_THREAD(lck)                \
        do {                                        \
            (lck)->active = false;                  \
            PRTE_POST_OBJECT(lck);                  \
            prte_condition_broadcast(&(lck)->cond); \
            prte_mutex_unlock(&(lck)->mutex);       \
        } while (0)
#endif

#define PRTE_WAKEUP_THREAD(lck)                 \
    do {                                        \
        prte_mutex_lock(&(lck)->mutex);         \
        (lck)->active = false;                  \
        PRTE_POST_OBJECT(lck);                  \
        prte_condition_broadcast(&(lck)->cond); \
        prte_mutex_unlock(&(lck)->mutex);       \
    } while (0)

/* provide a macro for forward-proofing the shifting
 * of objects between threads - at some point, we
 * may revamp our threading model */

/* post an object to another thread - for now, we
 * only have a memory barrier */
#define PRTE_POST_OBJECT(o) prte_atomic_wmb()

/* acquire an object from another thread - for now,
 * we only have a memory barrier */
#define PRTE_ACQUIRE_OBJECT(o) prte_atomic_rmb()

PRTE_EXPORT int prte_thread_start(prte_thread_t *);
PRTE_EXPORT int prte_thread_join(prte_thread_t *, void **thread_return);
PRTE_EXPORT bool prte_thread_self_compare(prte_thread_t *);
PRTE_EXPORT prte_thread_t *prte_thread_get_self(void);
PRTE_EXPORT void prte_thread_kill(prte_thread_t *, int sig);
PRTE_EXPORT void prte_thread_set_main(void);

END_C_DECLS

#endif /* PRTE_THREAD_H */
