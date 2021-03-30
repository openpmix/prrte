/* -*- Mode: C; c-basic-offset:4 ; -*- */
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
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "src/class/prte_ring_buffer.h"
#include "src/threads/threads.h"
#include "src/util/output.h"

static void prte_ring_buffer_construct(prte_ring_buffer_t *);
static void prte_ring_buffer_destruct(prte_ring_buffer_t *);

PRTE_CLASS_INSTANCE(prte_ring_buffer_t, prte_object_t, prte_ring_buffer_construct,
                    prte_ring_buffer_destruct);

/*
 * prte_ring_buffer constructor
 */
static void prte_ring_buffer_construct(prte_ring_buffer_t *ring)
{
    PRTE_CONSTRUCT_LOCK(&ring->lock);
    ring->in_use = false;
    ring->head = 0;
    ring->tail = -1;
    ring->size = 0;
    ring->addr = NULL;
}

/*
 * prte_ring_buffer destructor
 */
static void prte_ring_buffer_destruct(prte_ring_buffer_t *ring)
{
    if (NULL != ring->addr) {
        free(ring->addr);
        ring->addr = NULL;
    }

    ring->size = 0;

    PRTE_DESTRUCT_LOCK(&ring->lock);
}

/**
 * initialize a ring object
 */
int prte_ring_buffer_init(prte_ring_buffer_t *ring, int size)
{
    /* check for errors */
    if (NULL == ring) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Allocate and set the ring to NULL */
    ring->addr = (char **) calloc(size * sizeof(char *), 1);
    if (NULL == ring->addr) { /* out of memory */
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    ring->size = size;

    return PRTE_SUCCESS;
}

void *prte_ring_buffer_push(prte_ring_buffer_t *ring, void *ptr)
{
    char *p = NULL;

    PRTE_ACQUIRE_THREAD(&(ring->lock));
    if (NULL != ring->addr[ring->head]) {
        p = (char *) ring->addr[ring->head];
        if (ring->tail == ring->size - 1) {
            ring->tail = 0;
        } else {
            ring->tail = ring->head + 1;
        }
    }
    ring->addr[ring->head] = (char *) ptr;
    if (ring->tail < 0) {
        ring->tail = ring->head;
    }
    if (ring->head == ring->size - 1) {
        ring->head = 0;
    } else {
        ring->head++;
    }
    PRTE_RELEASE_THREAD(&(ring->lock));
    return (void *) p;
}

void *prte_ring_buffer_pop(prte_ring_buffer_t *ring)
{
    char *p = NULL;

    PRTE_ACQUIRE_THREAD(&(ring->lock));
    if (-1 == ring->tail) {
        /* nothing has been put on the ring yet */
        p = NULL;
    } else {
        p = (char *) ring->addr[ring->tail];
        ring->addr[ring->tail] = NULL;
        if (ring->tail == ring->size - 1) {
            ring->tail = 0;
        } else {
            ring->tail++;
        }
        /* see if the ring is empty */
        if (ring->tail == ring->head) {
            ring->tail = -1;
        }
    }
    PRTE_RELEASE_THREAD(&(ring->lock));
    return (void *) p;
}

void *prte_ring_buffer_poke(prte_ring_buffer_t *ring, int i)
{
    char *p = NULL;
    int offset;

    PRTE_ACQUIRE_THREAD(&(ring->lock));
    if (ring->size <= i || -1 == ring->tail) {
        p = NULL;
    } else if (i < 0) {
        /* return the value at the head of the ring */
        if (ring->head == 0) {
            p = ring->addr[ring->size - 1];
        } else {
            p = ring->addr[ring->head - 1];
        }
    } else {
        /* calculate the offset of the tail in the ring */
        offset = ring->tail + i;
        /* correct for wrap-around */
        if (ring->size <= offset) {
            offset -= ring->size;
        }
        p = ring->addr[offset];
    }
    PRTE_RELEASE_THREAD(&(ring->lock));
    return (void *) p;
}
