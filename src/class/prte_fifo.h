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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_FIFO_H_HAS_BEEN_INCLUDED
#define PRTE_FIFO_H_HAS_BEEN_INCLUDED

#include "prte_config.h"
#include "src/class/prte_lifo.h"

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
struct prte_fifo_t {
    prte_object_t super;

    /** first element on the fifo */
    volatile prte_counted_pointer_t prte_fifo_head;
    /** last element on the fifo */
    volatile prte_counted_pointer_t prte_fifo_tail;

    /** list sentinel (always points to self) */
    prte_list_item_t prte_fifo_ghost;
};

typedef struct prte_fifo_t prte_fifo_t;

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_fifo_t);

static inline prte_list_item_t *prte_fifo_head(prte_fifo_t *fifo)
{
    return (prte_list_item_t *) fifo->prte_fifo_head.data.item;
}

static inline prte_list_item_t *prte_fifo_tail(prte_fifo_t *fifo)
{
    return (prte_list_item_t *) fifo->prte_fifo_tail.data.item;
}

/* The ghost pointer will never change. The head will change via an atomic
 * compare-and-swap. On most architectures the reading of a pointer is an
 * atomic operation so we don't have to protect it.
 */
static inline bool prte_fifo_is_empty(prte_fifo_t *fifo)
{
    return prte_fifo_head(fifo) == &fifo->prte_fifo_ghost;
}

#if PRTE_HAVE_ATOMIC_COMPARE_EXCHANGE_128

/* Add one element to the FIFO. We will return the last head of the list
 * to allow the upper level to detect if this element is the first one in the
 * list (if the list was empty before this operation).
 */
static inline prte_list_item_t *prte_fifo_push_atomic(prte_fifo_t *fifo, prte_list_item_t *item)
{
    prte_counted_pointer_t tail = {.value = fifo->prte_fifo_tail.value};
    const prte_list_item_t *const ghost = &fifo->prte_fifo_ghost;

    item->prte_list_next = (prte_list_item_t *) ghost;

    prte_atomic_wmb();

    do {
        if (prte_update_counted_pointer(&fifo->prte_fifo_tail, &tail, item)) {
            break;
        }
    } while (1);

    prte_atomic_wmb();

    if ((intptr_t) ghost == tail.data.item) {
        /* update the head */
        prte_counted_pointer_t head = {.value = fifo->prte_fifo_head.value};
        prte_update_counted_pointer(&fifo->prte_fifo_head, &head, item);
    } else {
        /* update previous item */
        ((prte_list_item_t *) tail.data.item)->prte_list_next = item;
    }

    return (prte_list_item_t *) tail.data.item;
}

/* Retrieve one element from the FIFO. If we reach the ghost element then the FIFO
 * is empty so we return NULL.
 */
static inline prte_list_item_t *prte_fifo_pop_atomic(prte_fifo_t *fifo)
{
    prte_list_item_t *item, *next, *ghost = &fifo->prte_fifo_ghost;
    prte_counted_pointer_t head, tail;

    prte_read_counted_pointer(&fifo->prte_fifo_head, &head);

    do {
        tail.value = fifo->prte_fifo_tail.value;
        prte_atomic_rmb();

        item = (prte_list_item_t *) head.data.item;
        next = (prte_list_item_t *) item->prte_list_next;

        if ((intptr_t) ghost == tail.data.item && ghost == item) {
            return NULL;
        }

        /* the head or next pointer are in an inconsistent state. keep looping. */
        if (tail.data.item != (intptr_t) item && (intptr_t) ghost != tail.data.item
            && ghost == next) {
            prte_read_counted_pointer(&fifo->prte_fifo_head, &head);
            continue;
        }

        /* try popping the head */
        if (prte_update_counted_pointer(&fifo->prte_fifo_head, &head, next)) {
            break;
        }
    } while (1);

    prte_atomic_wmb();

    /* check for tail and head consistency */
    if (ghost == next) {
        /* the head was just set to &fifo->prte_fifo_ghost. try to update the tail as well */
        if (!prte_update_counted_pointer(&fifo->prte_fifo_tail, &tail, ghost)) {
            /* tail was changed by a push operation. wait for the item's next pointer to be se then
             * update the head */

            /* wait for next pointer to be updated by push */
            do {
                prte_atomic_rmb();
            } while (ghost == item->prte_list_next);

            /* update the head with the real next value. note that no other thread
             * will be attempting to update the head until after it has been updated
             * with the next pointer. push will not see an empty list and other pop
             * operations will loop until the head is consistent. */
            fifo->prte_fifo_head.data.item = (intptr_t) item->prte_list_next;
            prte_atomic_wmb();
        }
    }

    item->prte_list_next = NULL;

    return item;
}

#else

/* When compare-and-set 128 is not available we avoid the ABA problem by
 * using a spin-lock on the head (using the head counter). Otherwise
 * the algorithm is identical to the compare-and-set 128 version. */
static inline prte_list_item_t *prte_fifo_push_atomic(prte_fifo_t *fifo, prte_list_item_t *item)
{
    const prte_list_item_t *const ghost = &fifo->prte_fifo_ghost;
    prte_list_item_t *tail_item;

    item->prte_list_next = (prte_list_item_t *) ghost;

    prte_atomic_wmb();

    /* try to get the tail */
    tail_item = (prte_list_item_t *) prte_atomic_swap_ptr(&fifo->prte_fifo_tail.data.item,
                                                          (intptr_t) item);

    prte_atomic_wmb();

    if (ghost == tail_item) {
        /* update the head */
        fifo->prte_fifo_head.data.item = (intptr_t) item;
    } else {
        /* update previous item */
        tail_item->prte_list_next = item;
    }

    prte_atomic_wmb();

    return (prte_list_item_t *) tail_item;
}

/* Retrieve one element from the FIFO. If we reach the ghost element then the FIFO
 * is empty so we return NULL.
 */
static inline prte_list_item_t *prte_fifo_pop_atomic(prte_fifo_t *fifo)
{
    const prte_list_item_t *const ghost = &fifo->prte_fifo_ghost;

#    if PRTE_HAVE_ATOMIC_LLSC_PTR
    register prte_list_item_t *item, *next;
    int attempt = 0, ret = 0;

    /* use load-linked store-conditional to avoid ABA issues */
    do {
        if (++attempt == 5) {
            /* deliberatly suspend this thread to allow other threads to run. this should
             * only occur during periods of contention on the lifo. */
            _prte_lifo_release_cpu();
            attempt = 0;
        }

        prte_atomic_ll_ptr(&fifo->prte_fifo_head.data.item, item);
        if (ghost == item) {
            if ((intptr_t) ghost == fifo->prte_fifo_tail.data.item) {
                return NULL;
            }

            /* fifo does not appear empty. wait for the fifo to be made
             * consistent by conflicting thread. */
            continue;
        }

        next = (prte_list_item_t *) item->prte_list_next;
        prte_atomic_sc_ptr(&fifo->prte_fifo_head.data.item, next, ret);
    } while (!ret);

#    else
    prte_list_item_t *item, *next;

    /* protect against ABA issues by "locking" the head */
    do {
        if (!prte_atomic_swap_32((prte_atomic_int32_t *) &fifo->prte_fifo_head.data.counter, 1)) {
            break;
        }

        prte_atomic_wmb();
    } while (1);

    prte_atomic_wmb();

    item = prte_fifo_head(fifo);
    if (ghost == item) {
        fifo->prte_fifo_head.data.counter = 0;
        return NULL;
    }

    next = (prte_list_item_t *) item->prte_list_next;
    fifo->prte_fifo_head.data.item = (uintptr_t) next;
#    endif

    if (ghost == next) {
        void *tmp = item;

        if (!prte_atomic_compare_exchange_strong_ptr(&fifo->prte_fifo_tail.data.item,
                                                     (intptr_t *) &tmp, (intptr_t) ghost)) {
            do {
                prte_atomic_rmb();
            } while (ghost == item->prte_list_next);

            fifo->prte_fifo_head.data.item = (intptr_t) item->prte_list_next;
        }
    }

    prte_atomic_wmb();

    /* unlock the head */
    fifo->prte_fifo_head.data.counter = 0;

    item->prte_list_next = NULL;

    return item;
}

#endif

/* single threaded versions of push/pop */
static inline prte_list_item_t *prte_fifo_push_st(prte_fifo_t *fifo, prte_list_item_t *item)
{
    prte_list_item_t *prev = prte_fifo_tail(fifo);

    item->prte_list_next = &fifo->prte_fifo_ghost;

    fifo->prte_fifo_tail.data.item = (intptr_t) item;
    if (&fifo->prte_fifo_ghost == prte_fifo_head(fifo)) {
        fifo->prte_fifo_head.data.item = (intptr_t) item;
    } else {
        prev->prte_list_next = item;
    }

    return (prte_list_item_t *) item->prte_list_next;
}

static inline prte_list_item_t *prte_fifo_pop_st(prte_fifo_t *fifo)
{
    prte_list_item_t *item = prte_fifo_head(fifo);

    if (item == &fifo->prte_fifo_ghost) {
        return NULL;
    }

    fifo->prte_fifo_head.data.item = (intptr_t) item->prte_list_next;
    if (&fifo->prte_fifo_ghost == prte_fifo_head(fifo)) {
        fifo->prte_fifo_tail.data.item = (intptr_t) &fifo->prte_fifo_ghost;
    }

    item->prte_list_next = NULL;
    return item;
}

/* push/pop versions conditioned off prte_using_threads() */
static inline prte_list_item_t *prte_fifo_push(prte_fifo_t *fifo, prte_list_item_t *item)
{
    return prte_fifo_push_atomic(fifo, item);
}

static inline prte_list_item_t *prte_fifo_pop(prte_fifo_t *fifo)
{
    return prte_fifo_pop_atomic(fifo);
}

END_C_DECLS

#endif /* PRTE_FIFO_H_HAS_BEEN_INCLUDED */
