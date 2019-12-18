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
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "constants.h"
#include "src/class/prrte_ring_buffer.h"
#include "src/util/output.h"
#include "src/threads/threads.h"

static void prrte_ring_buffer_construct(prrte_ring_buffer_t *);
static void prrte_ring_buffer_destruct(prrte_ring_buffer_t *);

PRRTE_CLASS_INSTANCE(prrte_ring_buffer_t, prrte_object_t,
                   prrte_ring_buffer_construct,
                   prrte_ring_buffer_destruct);

/*
 * prrte_ring_buffer constructor
 */
static void prrte_ring_buffer_construct(prrte_ring_buffer_t *ring)
{
    PRRTE_CONSTRUCT_LOCK(&ring->lock);
    ring->in_use = false;
    ring->head = 0;
    ring->tail = -1;
    ring->size = 0;
    ring->addr = NULL;
}

/*
 * prrte_ring_buffer destructor
 */
static void prrte_ring_buffer_destruct(prrte_ring_buffer_t *ring)
{
    if( NULL != ring->addr) {
        free(ring->addr);
        ring->addr = NULL;
    }

    ring->size = 0;

    PRRTE_DESTRUCT_LOCK(&ring->lock);
}

/**
 * initialize a ring object
 */
int prrte_ring_buffer_init(prrte_ring_buffer_t* ring, int size)
{
    /* check for errors */
    if (NULL == ring) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Allocate and set the ring to NULL */
    ring->addr = (char **)calloc(size * sizeof(char*), 1);
    if (NULL == ring->addr) { /* out of memory */
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    ring->size = size;

    return PRRTE_SUCCESS;
}

void* prrte_ring_buffer_push(prrte_ring_buffer_t *ring, void *ptr)
{
    char *p=NULL;

    PRRTE_ACQUIRE_THREAD(&(ring->lock));
    if (NULL != ring->addr[ring->head]) {
        p = (char*)ring->addr[ring->head];
        if (ring->tail == ring->size - 1) {
            ring->tail = 0;
        } else {
            ring->tail = ring->head + 1;
        }
    }
    ring->addr[ring->head] = (char*)ptr;
    if (ring->tail < 0) {
        ring->tail = ring->head;
    }
    if (ring->head == ring->size - 1) {
        ring->head = 0;
    } else {
        ring->head++;
    }
    PRRTE_RELEASE_THREAD(&(ring->lock));
    return (void*)p;
}

void* prrte_ring_buffer_pop(prrte_ring_buffer_t *ring)
{
    char *p=NULL;

    PRRTE_ACQUIRE_THREAD(&(ring->lock));
    if (-1 == ring->tail) {
        /* nothing has been put on the ring yet */
        p = NULL;
    } else {
        p = (char*)ring->addr[ring->tail];
        ring->addr[ring->tail] = NULL;
        if (ring->tail == ring->size-1) {
            ring->tail = 0;
        } else {
            ring->tail++;
        }
        /* see if the ring is empty */
        if (ring->tail == ring->head) {
            ring->tail = -1;
        }
    }
    PRRTE_RELEASE_THREAD(&(ring->lock));
    return (void*)p;
}

 void* prrte_ring_buffer_poke(prrte_ring_buffer_t *ring, int i)
 {
    char *p=NULL;
    int offset;

    PRRTE_ACQUIRE_THREAD(&(ring->lock));
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
    PRRTE_RELEASE_THREAD(&(ring->lock));
    return (void*)p;
}
