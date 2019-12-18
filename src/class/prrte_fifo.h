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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_FIFO_H_HAS_BEEN_INCLUDED
#define PRRTE_FIFO_H_HAS_BEEN_INCLUDED

#include "prrte_config.h"
#include "src/class/prrte_lifo.h"

#include "src/sys/atomic.h"
#include "src/threads/mutex.h"

BEGIN_C_DECLS

/* Atomic First In First Out lists. If we are in a multi-threaded environment then the
 * atomicity is insured via the compare-and-swap operation, if not we simply do a read
 * and/or a write.
 *
 * There is a trick. The ghost element at the end of the list. This ghost element has
 * the next pointer pointing to itself, therefore we cannot go past the end of the list.
 * With this approach we will never have a NULL element in the list, so we never have
 * to test for the NULL.
 */
struct prrte_fifo_t {
    prrte_object_t super;

    /** first element on the fifo */
    volatile prrte_counted_pointer_t prrte_fifo_head;
    /** last element on the fifo */
    volatile prrte_counted_pointer_t prrte_fifo_tail;

    /** list sentinel (always points to self) */
    prrte_list_item_t prrte_fifo_ghost;
};

typedef struct prrte_fifo_t prrte_fifo_t;

PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_fifo_t);

static inline prrte_list_item_t *prrte_fifo_head (prrte_fifo_t* fifo)
{
    return (prrte_list_item_t *) fifo->prrte_fifo_head.data.item;
}

static inline prrte_list_item_t *prrte_fifo_tail (prrte_fifo_t* fifo)
{
    return (prrte_list_item_t *) fifo->prrte_fifo_tail.data.item;
}

/* The ghost pointer will never change. The head will change via an atomic
 * compare-and-swap. On most architectures the reading of a pointer is an
 * atomic operation so we don't have to protect it.
 */
static inline bool prrte_fifo_is_empty( prrte_fifo_t* fifo )
{
    return prrte_fifo_head (fifo) == &fifo->prrte_fifo_ghost;
}

#if PRRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the FIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prrte_list_item_t *prrte_fifo_push_atomic (prrte_fifo_t *fifo,
                                                       prrte_list_item_t *item)
{
    prrte_counted_pointer_t tail = {.value = fifo->prrte_fifo_tail.value};
    const prrte_list_item_t * const ghost = &fifo->prrte_fifo_ghost;

    item->prrte_list_next = (prrte_list_item_t *) ghost;

    prrte_atomic_wmb ();

    do {
        if (prrte_update_counted_pointer (&fifo->prrte_fifo_tail, &tail, item)) {
            break;
        }
    } while (1);

    prrte_atomic_wmb ();

  if ((intptr_t) ghost == tail.data.item) {
        /* update the head */
        prrte_counted_pointer_t head = {.value = fifo->prrte_fifo_head.value};
        prrte_update_counted_pointer (&fifo->prrte_fifo_head, &head, item);
    } else {
        /* update previous item */
        ((prrte_list_item_t *) tail.data.item)->prrte_list_next = item;
    }

    return (prrte_list_item_t *) tail.data.item;
}

/* Retrieve one element from the FIFO. If we reach the ghost element then the FIFO
 * is empty so we return NULL.
 */
static inline prrte_list_item_t *prrte_fifo_pop_atomic (prrte_fifo_t *fifo)
{
    prrte_list_item_t *item, *next, *ghost = &fifo->prrte_fifo_ghost;
    prrte_counted_pointer_t head, tail;

    prrte_read_counted_pointer (&fifo->prrte_fifo_head, &head);

    do {
        tail.value = fifo->prrte_fifo_tail.value;
        prrte_atomic_rmb ();

        item = (prrte_list_item_t *) head.data.item;
        next = (prrte_list_item_t *) item->prrte_list_next;

        if ((intptr_t) ghost == tail.data.item && ghost == item) {
            return NULL;
        }

        /* the head or next pointer are in an inconsistent state. keep looping. */
        if (tail.data.item != (intptr_t) item && (intptr_t) ghost != tail.data.item && ghost == next) {
            prrte_read_counted_pointer (&fifo->prrte_fifo_head, &head);
            continue;
        }

        /* try popping the head */
        if (prrte_update_counted_pointer (&fifo->prrte_fifo_head, &head, next)) {
            break;
        }
    } while (1);

    prrte_atomic_wmb ();

    /* check for tail and head consistency */
    if (ghost == next) {
        /* the head was just set to &fifo->prrte_fifo_ghost. try to update the tail as well */
        if (!prrte_update_counted_pointer (&fifo->prrte_fifo_tail, &tail, ghost)) {
            /* tail was changed by a push operation. wait for the item's next pointer to be se then
             * update the head */

            /* wait for next pointer to be updated by push */
            do {
                prrte_atomic_rmb ();
            } while (ghost == item->prrte_list_next);

            /* update the head with the real next value. note that no other thread
             * will be attempting to update the head until after it has been updated
             * with the next pointer. push will not see an empty list and other pop
             * operations will loop until the head is consistent. */
            fifo->prrte_fifo_head.data.item = (intptr_t) item->prrte_list_next;
            prrte_atomic_wmb ();
        }
    }

    item->prrte_list_next = NULL;

    return item;
}

#else

/* When compare-and-set 128 is not available we avoid the ABA problem by
 * using a spin-lock on the head (using the head counter). Otherwise
 * the algorithm is identical to the compare-and-set 128 version. */
static inline prrte_list_item_t *prrte_fifo_push_atomic (prrte_fifo_t *fifo,
                                                       prrte_list_item_t *item)
{
    const prrte_list_item_t * const ghost = &fifo->prrte_fifo_ghost;
    prrte_list_item_t *tail_item;

    item->prrte_list_next = (prrte_list_item_t *) ghost;

    prrte_atomic_wmb ();

    /* try to get the tail */
    tail_item = (prrte_list_item_t *) prrte_atomic_swap_ptr (&fifo->prrte_fifo_tail.data.item, (intptr_t) item);

    prrte_atomic_wmb ();

    if (ghost == tail_item) {
        /* update the head */
        fifo->prrte_fifo_head.data.item = (intptr_t) item;
    } else {
        /* update previous item */
        tail_item->prrte_list_next = item;
    }

    prrte_atomic_wmb ();

    return (prrte_list_item_t *) tail_item;
}

/* Retrieve one element from the FIFO. If we reach the ghost element then the FIFO
 * is empty so we return NULL.
 */
static inline prrte_list_item_t *prrte_fifo_pop_atomic (prrte_fifo_t *fifo)
{
    const prrte_list_item_t * const ghost = &fifo->prrte_fifo_ghost;

#if PRRTE_HAVE_ATOMIC_LLSC_PTR
    register prrte_list_item_t *item, *next;
    int attempt = 0, ret = 0;

    /* use load-linked store-conditional to avoid ABA issues */
    do {
        if (++attempt == 5) {
            /* deliberatly suspend this thread to allow other threads to run. this should
             * only occur during periods of contention on the lifo. */
            _prrte_lifo_release_cpu ();
            attempt = 0;
        }

        prrte_atomic_ll_ptr(&fifo->prrte_fifo_head.data.item, item);
        if (ghost == item) {
            if ((intptr_t) ghost == fifo->prrte_fifo_tail.data.item) {
                return NULL;
            }

            /* fifo does not appear empty. wait for the fifo to be made
             * consistent by conflicting thread. */
            continue;
        }

        next = (prrte_list_item_t *) item->prrte_list_next;
        prrte_atomic_sc_ptr(&fifo->prrte_fifo_head.data.item, next, ret);
    } while (!ret);

#else
    prrte_list_item_t *item, *next;

    /* protect against ABA issues by "locking" the head */
    do {
        if (!prrte_atomic_swap_32 ((prrte_atomic_int32_t *) &fifo->prrte_fifo_head.data.counter, 1)) {
            break;
        }

        prrte_atomic_wmb ();
    } while (1);

    prrte_atomic_wmb();

    item = prrte_fifo_head (fifo);
    if (ghost == item) {
        fifo->prrte_fifo_head.data.counter = 0;
        return NULL;
    }

    next = (prrte_list_item_t *) item->prrte_list_next;
    fifo->prrte_fifo_head.data.item = (uintptr_t) next;
#endif

    if (ghost == next) {
        void *tmp = item;

        if (!prrte_atomic_compare_exchange_strong_ptr (&fifo->prrte_fifo_tail.data.item, (intptr_t *) &tmp, (intptr_t) ghost)) {
            do {
                prrte_atomic_rmb ();
            } while (ghost == item->prrte_list_next);

            fifo->prrte_fifo_head.data.item = (intptr_t) item->prrte_list_next;
        }
    }

    prrte_atomic_wmb ();

    /* unlock the head */
    fifo->prrte_fifo_head.data.counter = 0;

    item->prrte_list_next = NULL;

    return item;
}

#endif

/* single threaded versions of push/pop */
static inline prrte_list_item_t *prrte_fifo_push_st (prrte_fifo_t *fifo,
                                                   prrte_list_item_t *item)
{
    prrte_list_item_t *prev = prrte_fifo_tail (fifo);

    item->prrte_list_next = &fifo->prrte_fifo_ghost;

    fifo->prrte_fifo_tail.data.item = (intptr_t) item;
    if (&fifo->prrte_fifo_ghost == prrte_fifo_head (fifo)) {
        fifo->prrte_fifo_head.data.item = (intptr_t) item;
    } else {
        prev->prrte_list_next = item;
    }

    return (prrte_list_item_t *) item->prrte_list_next;
}

static inline prrte_list_item_t *prrte_fifo_pop_st (prrte_fifo_t *fifo)
{
    prrte_list_item_t *item = prrte_fifo_head (fifo);

    if (item == &fifo->prrte_fifo_ghost) {
        return NULL;
    }

    fifo->prrte_fifo_head.data.item = (intptr_t) item->prrte_list_next;
    if (&fifo->prrte_fifo_ghost == prrte_fifo_head (fifo)) {
        fifo->prrte_fifo_tail.data.item = (intptr_t) &fifo->prrte_fifo_ghost;
    }

    item->prrte_list_next = NULL;
    return item;
}

/* push/pop versions conditioned off prrte_using_threads() */
static inline prrte_list_item_t *prrte_fifo_push (prrte_fifo_t *fifo,
                                                prrte_list_item_t *item)
{
    return prrte_fifo_push_atomic (fifo, item);
}

static inline prrte_list_item_t *prrte_fifo_pop (prrte_fifo_t *fifo)
{
    return prrte_fifo_pop_atomic (fifo);
}

END_C_DECLS

#endif  /* PRRTE_FIFO_H_HAS_BEEN_INCLUDED */
