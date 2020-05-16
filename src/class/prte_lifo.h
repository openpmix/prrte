/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Voltaire All rights reserved.
 * Copyright (c) 2010      IBM Corporation.  All rights reserved.
 * Copyright (c) 2014-2018 Los Alamos National Security, LLC. All rights
 *                         reseved.
 * Copyright (c) 2016-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_LIFO_H_HAS_BEEN_INCLUDED
#define PRTE_LIFO_H_HAS_BEEN_INCLUDED

#include "prte_config.h"
#include <time.h>
#include "src/class/prte_list.h"

#include "src/sys/atomic.h"
#include "src/threads/threads.h"

BEGIN_C_DECLS

/* NTH: temporarily suppress warnings about this not being defined */
#if !defined(PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128)
#define PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 0
#endif

/**
 * Counted pointer to avoid the ABA problem.
 */
union prte_counted_pointer_t {
    struct {
        /** update counter used when cmpset_128 is available */
        uint64_t counter;
        /** list item pointer */
        volatile prte_atomic_intptr_t item;
    } data;
#if PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 && HAVE_PRTE_INT128_T
    /** used for atomics when there is a cmpset that can operate on
     * two 64-bit values */
    prte_atomic_int128_t atomic_value;
    prte_int128_t value;
#endif
};
typedef union prte_counted_pointer_t prte_counted_pointer_t;


#if PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the FIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline bool prte_update_counted_pointer (volatile prte_counted_pointer_t * volatile addr, prte_counted_pointer_t *old,
                                                prte_list_item_t *item)
{
    prte_counted_pointer_t new_p;
    new_p.data.item = (intptr_t) item;
    new_p.data.counter = old->data.counter + 1;
    return prte_atomic_compare_exchange_strong_128 (&addr->atomic_value, &old->value, new_p.value);
}

__prte_attribute_always_inline__
static inline void prte_read_counted_pointer (volatile prte_counted_pointer_t * volatile addr, prte_counted_pointer_t *value)
{
    /* most platforms do not read the value atomically so make sure we read the counted pointer in a specific order */
    value->data.counter = addr->data.counter;
    prte_atomic_rmb ();
    value->data.item = addr->data.item;
}

#endif

/**
 * @brief Helper function for lifo/fifo to sleep this thread if excessive contention is detected
 */
static inline void _prte_lifo_release_cpu (void)
{
    /* NTH: there are many ways to cause the current thread to be suspended. This one
     * should work well in most cases. Another approach would be to use poll (NULL, 0, ) but
     * the interval will be forced to be in ms (instead of ns or us). Note that there
     * is a performance improvement for the lifo test when this call is made on detection
     * of contention but it may not translate into actually MPI or application performance
     * improvements. */
    static struct timespec interval = { .tv_sec = 0, .tv_nsec = 100 };
    nanosleep (&interval, NULL);
}


/* Atomic Last In First Out lists. If we are in a multi-threaded environment then the
 * atomicity is insured via the compare-and-swap operation, if not we simply do a read
 * and/or a write.
 *
 * There is a trick. The ghost element at the end of the list. This ghost element has
 * the next pointer pointing to itself, therefore we cannot go past the end of the list.
 * With this approach we will never have a NULL element in the list, so we never have
 * to test for the NULL.
 */
struct prte_lifo_t {
    prte_object_t super;

    /** head element of the lifo. points to prte_lifo_ghost if the lifo is empty */
    volatile prte_counted_pointer_t prte_lifo_head;

    /** list sentinel (always points to self) */
    prte_list_item_t prte_lifo_ghost;
};

typedef struct prte_lifo_t prte_lifo_t;

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_lifo_t);


/* The ghost pointer will never change. The head will change via an atomic
 * compare-and-swap. On most architectures the reading of a pointer is an
 * atomic operation so we don't have to protect it.
 */
static inline bool prte_lifo_is_empty( prte_lifo_t* lifo )
{
    return (prte_list_item_t *) lifo->prte_lifo_head.data.item == &lifo->prte_lifo_ghost;
}


#if PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the LIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prte_list_item_t *prte_lifo_push_atomic (prte_lifo_t *lifo,
                                                       prte_list_item_t *item)
{
    prte_list_item_t *next = (prte_list_item_t *) lifo->prte_lifo_head.data.item;

    do {
        item->prte_list_next = next;
        prte_atomic_wmb ();

        /* to protect against ABA issues it is sufficient to only update the counter in pop */
        if (prte_atomic_compare_exchange_strong_ptr (&lifo->prte_lifo_head.data.item, (intptr_t *) &next, (intptr_t) item)) {
            return next;
        }
        /* DO some kind of pause to release the bus */
    } while (1);
}

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prte_list_item_t *prte_lifo_pop_atomic (prte_lifo_t* lifo)
{
    prte_counted_pointer_t old_head;
    prte_list_item_t *item;

    prte_read_counted_pointer (&lifo->prte_lifo_head, &old_head);

    do {
        item = (prte_list_item_t *) old_head.data.item;
        if (item == &lifo->prte_lifo_ghost) {
            return NULL;
        }

        if (prte_update_counted_pointer (&lifo->prte_lifo_head, &old_head,
                                         (prte_list_item_t *) item->prte_list_next)) {
            prte_atomic_wmb ();
            item->prte_list_next = NULL;
            return item;
        }
    } while (1);
}

#else

/* Add one element to the LIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prte_list_item_t *prte_lifo_push_atomic (prte_lifo_t *lifo,
                                                       prte_list_item_t *item)
{
    prte_list_item_t *next = (prte_list_item_t *) lifo->prte_lifo_head.data.item;

    /* item free acts as a mini lock to avoid ABA problems */
    item->item_free = 1;

    do {
        item->prte_list_next = next;
        prte_atomic_wmb();
        if (prte_atomic_compare_exchange_strong_ptr (&lifo->prte_lifo_head.data.item, (intptr_t *) &next, (intptr_t) item)) {
            prte_atomic_wmb ();
            /* now safe to pop this item */
            item->item_free = 0;
            return next;
        }
        /* DO some kind of pause to release the bus */
    } while (1);
}

#if PRTE_HAVE_ATOMIC_LLSC_PTR

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prte_list_item_t *prte_lifo_pop_atomic (prte_lifo_t* lifo)
{
    register prte_list_item_t *item, *next;
    int attempt = 0, ret;

    do {
        if (++attempt == 5) {
            /* deliberatly suspend this thread to allow other threads to run. this should
             * only occur during periods of contention on the lifo. */
            _prte_lifo_release_cpu ();
            attempt = 0;
        }

        prte_atomic_ll_ptr(&lifo->prte_lifo_head.data.item, item);
        if (&lifo->prte_lifo_ghost == item) {
            return NULL;
        }

        next = (prte_list_item_t *) item->prte_list_next;
        prte_atomic_sc_ptr(&lifo->prte_lifo_head.data.item, next, ret);
    } while (!ret);

    prte_atomic_wmb ();

    item->prte_list_next = NULL;
    return item;
}

#else

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prte_list_item_t *prte_lifo_pop_atomic (prte_lifo_t* lifo)
{
    prte_list_item_t *item, *head, *ghost = &lifo->prte_lifo_ghost;

    while ((item=(prte_list_item_t *)lifo->prte_lifo_head.data.item) != ghost) {
        /* ensure it is safe to pop the head */
        if (prte_atomic_swap_32((prte_atomic_int32_t *) &item->item_free, 1)) {
            continue;
        }

        prte_atomic_wmb ();

        head = item;
        /* try to swap out the head pointer */
        if (prte_atomic_compare_exchange_strong_ptr (&lifo->prte_lifo_head.data.item, (intptr_t *) &head,
                                                     (intptr_t) item->prte_list_next)) {
            break;
        }

        /* NTH: don't need another atomic here */
        item->item_free = 0;
        item = head;

        /* Do some kind of pause to release the bus */
    }

    if (item == &lifo->prte_lifo_ghost) {
        return NULL;
    }

    prte_atomic_wmb ();

    item->prte_list_next = NULL;
    return item;
}

#endif /* PRTE_HAVE_ATOMIC_LLSC_PTR */

#endif

/* single-threaded versions of the lifo functions */
static inline prte_list_item_t *prte_lifo_push_st (prte_lifo_t *lifo,
                                                   prte_list_item_t *item)
{
    item->prte_list_next = (prte_list_item_t *) lifo->prte_lifo_head.data.item;
    item->item_free = 0;
    lifo->prte_lifo_head.data.item = (intptr_t) item;
    return (prte_list_item_t *) item->prte_list_next;
}

static inline prte_list_item_t *prte_lifo_pop_st (prte_lifo_t *lifo)
{
    prte_list_item_t *item;
    item = (prte_list_item_t *) lifo->prte_lifo_head.data.item;
    lifo->prte_lifo_head.data.item = (intptr_t) item->prte_list_next;
    if (item == &lifo->prte_lifo_ghost) {
        return NULL;
    }

    item->prte_list_next = NULL;
    item->item_free = 1;
    return item;
}

/* conditional versions of lifo functions. use atomics if prte_using_threads is set */
static inline prte_list_item_t *prte_lifo_push (prte_lifo_t *lifo,
                                                prte_list_item_t *item)
{
    return prte_lifo_push_atomic (lifo, item);

    return prte_lifo_push_st (lifo, item);
}

static inline prte_list_item_t *prte_lifo_pop (prte_lifo_t *lifo)
{
    return prte_lifo_pop_atomic (lifo);

    return prte_lifo_pop_st (lifo);
}

END_C_DECLS

#endif  /* PRTE_LIFO_H_HAS_BEEN_INCLUDED */
