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
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_THREAD_H
#define PRRTE_THREAD_H 1

#include "prrte_config.h"

#include <pthread.h>
#include <signal.h>

#include "src/class/prrte_object.h"
#if PRRTE_ENABLE_DEBUG
#include "src/util/output.h"
#endif

#include "mutex.h"

BEGIN_C_DECLS

typedef void *(*prrte_thread_fn_t) (prrte_object_t *);

#define PRRTE_THREAD_CANCELLED   ((void*)1);

struct prrte_thread_t {
    prrte_object_t super;
    prrte_thread_fn_t t_run;
    void* t_arg;
    pthread_t t_handle;
};

typedef struct prrte_thread_t prrte_thread_t;

#if PRRTE_ENABLE_DEBUG
PRRTE_EXPORT extern bool prrte_debug_threads;
#endif


PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_thread_t);

#define prrte_condition_wait(a,b)    pthread_cond_wait(a, &(b)->m_lock_pthread)
typedef pthread_cond_t prrte_condition_t;
#define prrte_condition_broadcast(a) pthread_cond_broadcast(a)
#define prrte_condition_signal(a)    pthread_cond_signal(a)
#define PRRTE_CONDITION_STATIC_INIT PTHREAD_COND_INITIALIZER

/* define a threadshift macro */
#define PRRTE_THREADSHIFT(x, eb, f, p)                                   \
    do {                                                                \
        prrte_event_set((eb), &((x)->ev), -1, PRRTE_EV_WRITE, (f), (x));  \
        prrte_event_set_priority(&((x)->ev), (p));                       \
        PRRTE_POST_OBJECT((x));                                          \
        prrte_event_active(&((x)->ev), PRRTE_EV_WRITE, 1);                \
    } while(0)


typedef struct {
    int status;
    prrte_mutex_t mutex;
    prrte_condition_t cond;
    volatile bool active;
} prrte_lock_t;

#define PRRTE_CONSTRUCT_LOCK(l)                          \
    do {                                                \
        PRRTE_CONSTRUCT(&(l)->mutex, prrte_mutex_t);      \
        pthread_cond_init(&(l)->cond, NULL);            \
        /* coverity[missing_lock : FALSE] */            \
        (l)->active = true;                             \
    } while(0)

#define PRRTE_DESTRUCT_LOCK(l)               \
    do {                                    \
        PRRTE_DESTRUCT(&(l)->mutex);         \
        pthread_cond_destroy(&(l)->cond);   \
    } while(0)


#if PRRTE_ENABLE_DEBUG
#define PRRTE_ACQUIRE_THREAD(lck)                                \
    do {                                                        \
        prrte_mutex_lock(&(lck)->mutex);                         \
        if (prrte_debug_threads) {                               \
            prrte_output(0, "Waiting for thread %s:%d",          \
                        __FILE__, __LINE__);                    \
        }                                                       \
        while ((lck)->active) {                                 \
            prrte_condition_wait(&(lck)->cond, &(lck)->mutex);   \
        }                                                       \
        if (prrte_debug_threads) {                               \
            prrte_output(0, "Thread obtained %s:%d",             \
                        __FILE__, __LINE__);                    \
        }                                                       \
        PRRTE_ACQUIRE_OBJECT(lck);                               \
        (lck)->active = true;                                   \
    } while(0)
#else
#define PRRTE_ACQUIRE_THREAD(lck)                                \
    do {                                                        \
        prrte_mutex_lock(&(lck)->mutex);                         \
        while ((lck)->active) {                                 \
            prrte_condition_wait(&(lck)->cond, &(lck)->mutex);   \
        }                                                       \
        PRRTE_ACQUIRE_OBJECT(lck);                               \
        (lck)->active = true;                                   \
    } while(0)
#endif


#if PRRTE_ENABLE_DEBUG
#define PRRTE_WAIT_THREAD(lck)                                   \
    do {                                                        \
        prrte_mutex_lock(&(lck)->mutex);                         \
        if (prrte_debug_threads) {                               \
            prrte_output(0, "Waiting for thread %s:%d",          \
                        __FILE__, __LINE__);                    \
        }                                                       \
        while ((lck)->active) {                                 \
            prrte_condition_wait(&(lck)->cond, &(lck)->mutex);   \
        }                                                       \
        if (prrte_debug_threads) {                               \
            prrte_output(0, "Thread obtained %s:%d",             \
                        __FILE__, __LINE__);                    \
        }                                                       \
        PRRTE_ACQUIRE_OBJECT(lck);                               \
        prrte_mutex_unlock(&(lck)->mutex);                       \
    } while(0)
#else
#define PRRTE_WAIT_THREAD(lck)                                   \
    do {                                                        \
        prrte_mutex_lock(&(lck)->mutex);                         \
        while ((lck)->active) {                                 \
            prrte_condition_wait(&(lck)->cond, &(lck)->mutex);   \
        }                                                       \
        PRRTE_ACQUIRE_OBJECT(lck);                               \
        prrte_mutex_unlock(&(lck)->mutex);                       \
    } while(0)
#endif


#if PRRTE_ENABLE_DEBUG
#define PRRTE_RELEASE_THREAD(lck)                        \
    do {                                                \
        if (prrte_debug_threads) {                       \
            prrte_output(0, "Releasing thread %s:%d",    \
                        __FILE__, __LINE__);            \
        }                                               \
        (lck)->active = false;                          \
        PRRTE_POST_OBJECT(lck);                  \
        prrte_condition_broadcast(&(lck)->cond);         \
        prrte_mutex_unlock(&(lck)->mutex);               \
    } while(0)
#else
#define PRRTE_RELEASE_THREAD(lck)                \
    do {                                        \
        (lck)->active = false;                  \
        PRRTE_POST_OBJECT(lck);                  \
        prrte_condition_broadcast(&(lck)->cond); \
        prrte_mutex_unlock(&(lck)->mutex);       \
    } while(0)
#endif


#define PRRTE_WAKEUP_THREAD(lck)                 \
    do {                                        \
        prrte_mutex_lock(&(lck)->mutex);         \
        (lck)->active = false;                  \
        PRRTE_POST_OBJECT(lck);                  \
        prrte_condition_broadcast(&(lck)->cond); \
        prrte_mutex_unlock(&(lck)->mutex);       \
    } while(0)


/* provide a macro for forward-proofing the shifting
 * of objects between threads - at some point, we
 * may revamp our threading model */

/* post an object to another thread - for now, we
 * only have a memory barrier */
#define PRRTE_POST_OBJECT(o)     prrte_atomic_wmb()

/* acquire an object from another thread - for now,
 * we only have a memory barrier */
#define PRRTE_ACQUIRE_OBJECT(o)  prrte_atomic_rmb()


PRRTE_EXPORT int  prrte_thread_start(prrte_thread_t *);
PRRTE_EXPORT int  prrte_thread_join(prrte_thread_t *, void **thread_return);
PRRTE_EXPORT bool prrte_thread_self_compare(prrte_thread_t*);
PRRTE_EXPORT prrte_thread_t *prrte_thread_get_self(void);
PRRTE_EXPORT void prrte_thread_kill(prrte_thread_t *, int sig);
PRRTE_EXPORT void prrte_thread_set_main(void);

END_C_DECLS

#endif /* PRRTE_THREAD_H */
