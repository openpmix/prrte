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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_LIFO_H_HAS_BEEN_INCLUDED
#define PRRTE_LIFO_H_HAS_BEEN_INCLUDED

#include "prrte_config.h"
#include <time.h>
#include "src/class/prrte_list.h"

#include "src/sys/atomic.h"
#include "src/threads/threads.h"

BEGIN_C_DECLS

/* NTH: temporarily suppress warnings about this not being defined */
#if !defined(PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128)
#define PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 0
#endif

/**
 * Counted pointer to avoid the ABA problem.
 */
union prrte_counted_pointer_t {
    struct {
        /** update counter used when cmpset_128 is available */
        uint64_t counter;
        /** list item pointer */
        volatile prrte_atomic_intptr_t item;
    } data;
#if PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128 && HAVE_PRRTE_INT128_T
    /** used for atomics when there is a cmpset that can operate on
     * two 64-bit values */
    prrte_atomic_int128_t atomic_value;
    prrte_int128_t value;
#endif
};
typedef union prrte_counted_pointer_t prrte_counted_pointer_t;


#if PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the FIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline bool prrte_update_counted_pointer (volatile prrte_counted_pointer_t * volatile addr, prrte_counted_pointer_t *old,
                                                prrte_list_item_t *item)
{
    prrte_counted_pointer_t new_p;
    new_p.data.item = (intptr_t) item;
    new_p.data.counter = old->data.counter + 1;
    return prrte_atomic_compare_exchange_strong_128 (&addr->atomic_value, &old->value, new_p.value);
}

__prrte_attribute_always_inline__
static inline void prrte_read_counted_pointer (volatile prrte_counted_pointer_t * volatile addr, prrte_counted_pointer_t *value)
{
    /* most platforms do not read the value atomically so make sure we read the counted pointer in a specific order */
    value->data.counter = addr->data.counter;
    prrte_atomic_rmb ();
    value->data.item = addr->data.item;
}

#endif

/**
 * @brief Helper function for lifo/fifo to sleep this thread if excessive contention is detected
 */
static inline void _prrte_lifo_release_cpu (void)
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
struct prrte_lifo_t {
    prrte_object_t super;

    /** head element of the lifo. points to prrte_lifo_ghost if the lifo is empty */
    volatile prrte_counted_pointer_t prrte_lifo_head;

    /** list sentinel (always points to self) */
    prrte_list_item_t prrte_lifo_ghost;
};

typedef struct prrte_lifo_t prrte_lifo_t;

PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_lifo_t);


/* The ghost pointer will never change. The head will change via an atomic
 * compare-and-swap. On most architectures the reading of a pointer is an
 * atomic operation so we don't have to protect it.
 */
static inline bool prrte_lifo_is_empty( prrte_lifo_t* lifo )
{
    return (prrte_list_item_t *) lifo->prrte_lifo_head.data.item == &lifo->prrte_lifo_ghost;
}


#if PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the LIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prrte_list_item_t *prrte_lifo_push_atomic (prrte_lifo_t *lifo,
                                                       prrte_list_item_t *item)
{
    prrte_list_item_t *next = (prrte_list_item_t *) lifo->prrte_lifo_head.data.item;

    do {
        item->prrte_list_next = next;
        prrte_atomic_wmb ();

        /* to protect against ABA issues it is sufficient to only update the counter in pop */
        if (prrte_atomic_compare_exchange_strong_ptr (&lifo->prrte_lifo_head.data.item, (intptr_t *) &next, (intptr_t) item)) {
            return next;
        }
        /* DO some kind of pause to release the bus */
    } while (1);
}

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prrte_list_item_t *prrte_lifo_pop_atomic (prrte_lifo_t* lifo)
{
    prrte_counted_pointer_t old_head;
    prrte_list_item_t *item;

    prrte_read_counted_pointer (&lifo->prrte_lifo_head, &old_head);

    do {
        item = (prrte_list_item_t *) old_head.data.item;
        if (item == &lifo->prrte_lifo_ghost) {
            return NULL;
        }

        if (prrte_update_counted_pointer (&lifo->prrte_lifo_head, &old_head,
                                         (prrte_list_item_t *) item->prrte_list_next)) {
            prrte_atomic_wmb ();
            item->prrte_list_next = NULL;
            return item;
        }
    } while (1);
}

#else

/* Add one element to the LIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prrte_list_item_t *prrte_lifo_push_atomic (prrte_lifo_t *lifo,
                                                       prrte_list_item_t *item)
{
    prrte_list_item_t *next = (prrte_list_item_t *) lifo->prrte_lifo_head.data.item;

    /* item free acts as a mini lock to avoid ABA problems */
    item->item_free = 1;

    do {
        item->prrte_list_next = next;
        prrte_atomic_wmb();
        if (prrte_atomic_compare_exchange_strong_ptr (&lifo->prrte_lifo_head.data.item, (intptr_t *) &next, (intptr_t) item)) {
            prrte_atomic_wmb ();
            /* now safe to pop this item */
            item->item_free = 0;
            return next;
        }
        /* DO some kind of pause to release the bus */
    } while (1);
}

#if PRRTE_HAVE_ATOMIC_LLSC_PTR

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prrte_list_item_t *prrte_lifo_pop_atomic (prrte_lifo_t* lifo)
{
    register prrte_list_item_t *item, *next;
    int attempt = 0, ret;

    do {
        if (++attempt == 5) {
            /* deliberatly suspend this thread to allow other threads to run. this should
             * only occur during periods of contention on the lifo. */
            _prrte_lifo_release_cpu ();
            attempt = 0;
        }

        prrte_atomic_ll_ptr(&lifo->prrte_lifo_head.data.item, item);
        if (&lifo->prrte_lifo_ghost == item) {
            return NULL;
        }

        next = (prrte_list_item_t *) item->prrte_list_next;
        prrte_atomic_sc_ptr(&lifo->prrte_lifo_head.data.item, next, ret);
    } while (!ret);

    prrte_atomic_wmb ();

    item->prrte_list_next = NULL;
    return item;
}

#else

/* Retrieve one element from the LIFO. If we reach the ghost element then the LIFO
 * is empty so we return NULL.
 */
static inline prrte_list_item_t *prrte_lifo_pop_atomic (prrte_lifo_t* lifo)
{
    prrte_list_item_t *item, *head, *ghost = &lifo->prrte_lifo_ghost;

    while ((item=(prrte_list_item_t *)lifo->prrte_lifo_head.data.item) != ghost) {
        /* ensure it is safe to pop the head */
        if (prrte_atomic_swap_32((prrte_atomic_int32_t *) &item->item_free, 1)) {
            continue;
        }

        prrte_atomic_wmb ();

        head = item;
        /* try to swap out the head pointer */
        if (prrte_atomic_compare_exchange_strong_ptr (&lifo->prrte_lifo_head.data.item, (intptr_t *) &head,
                                                     (intptr_t) item->prrte_list_next)) {
            break;
        }

        /* NTH: don't need another atomic here */
        item->item_free = 0;
        item = head;

        /* Do some kind of pause to release the bus */
    }

    if (item == &lifo->prrte_lifo_ghost) {
        return NULL;
    }

    prrte_atomic_wmb ();

    item->prrte_list_next = NULL;
    return item;
}

#endif /* PRRTE_HAVE_ATOMIC_LLSC_PTR */

#endif

/* single-threaded versions of the lifo functions */
static inline prrte_list_item_t *prrte_lifo_push_st (prrte_lifo_t *lifo,
                                                   prrte_list_item_t *item)
{
    item->prrte_list_next = (prrte_list_item_t *) lifo->prrte_lifo_head.data.item;
    item->item_free = 0;
    lifo->prrte_lifo_head.data.item = (intptr_t) item;
    return (prrte_list_item_t *) item->prrte_list_next;
}

static inline prrte_list_item_t *prrte_lifo_pop_st (prrte_lifo_t *lifo)
{
    prrte_list_item_t *item;
    item = (prrte_list_item_t *) lifo->prrte_lifo_head.data.item;
    lifo->prrte_lifo_head.data.item = (intptr_t) item->prrte_list_next;
    if (item == &lifo->prrte_lifo_ghost) {
        return NULL;
    }

    item->prrte_list_next = NULL;
    item->item_free = 1;
    return item;
}

/* conditional versions of lifo functions. use atomics if prrte_using_threads is set */
static inline prrte_list_item_t *prrte_lifo_push (prrte_lifo_t *lifo,
                                                prrte_list_item_t *item)
{
    return prrte_lifo_push_atomic (lifo, item);

    return prrte_lifo_push_st (lifo, item);
}

static inline prrte_list_item_t *prrte_lifo_pop (prrte_lifo_t *lifo)
{
    return prrte_lifo_pop_atomic (lifo);

    return prrte_lifo_pop_st (lifo);
}

END_C_DECLS

#endif  /* PRRTE_LIFO_H_HAS_BEEN_INCLUDED */
