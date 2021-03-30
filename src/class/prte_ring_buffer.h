/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
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
/** @file
 *
 */

#ifndef PRTE_RING_BUFFER_H
#define PRTE_RING_BUFFER_H

#include "prte_config.h"

#include "src/class/prte_object.h"
#include "src/threads/threads.h"
#include "src/util/output.h"

BEGIN_C_DECLS

/**
 * dynamic pointer ring
 */
struct prte_ring_buffer_t {
    /** base class */
    prte_object_t super;
    /** synchronization object */
    prte_lock_t lock;
    bool in_use;
    /* head/tail indices */
    int head;
    int tail;
    /** size of list, i.e. number of elements in addr */
    int size;
    /** pointer to ring */
    char **addr;
};
/**
 * Convenience typedef
 */
typedef struct prte_ring_buffer_t prte_ring_buffer_t;
/**
 * Class declaration
 */
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_ring_buffer_t);

/**
 * Initialize the ring buffer, defining its size.
 *
 * @param ring Pointer to a ring buffer (IN/OUT)
 * @param size The number of elements in the ring (IN)
 *
 * @return PRTE_SUCCESS if all initializations were succesful. Otherwise,
 *  the error indicate what went wrong in the function.
 */
PRTE_EXPORT int prte_ring_buffer_init(prte_ring_buffer_t *ring, int size);

/**
 * Push an item onto the ring buffer
 *
 * @param ring Pointer to ring (IN)
 * @param ptr Pointer value (IN)
 *
 * @return PRTE_SUCCESS. Returns error if ring cannot take
 *  another entry
 */
PRTE_EXPORT void *prte_ring_buffer_push(prte_ring_buffer_t *ring, void *ptr);

/**
 * Pop an item off of the ring. The oldest entry on the ring will be
 * returned. If nothing on the ring, NULL is returned.
 *
 * @param ring          Pointer to ring (IN)
 *
 * @return Error code.  NULL indicates an error.
 */

PRTE_EXPORT void *prte_ring_buffer_pop(prte_ring_buffer_t *ring);

/*
 * Access an element of the ring, without removing it, indexed
 * starting at the tail - a value of -1 will return the element
 * at the head of the ring
 */
PRTE_EXPORT void *prte_ring_buffer_poke(prte_ring_buffer_t *ring, int i);

END_C_DECLS

#endif /* PRTE_RING_BUFFER_H */
