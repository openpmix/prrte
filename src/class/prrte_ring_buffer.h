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
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file
 *
 */

#ifndef PRRTE_RING_BUFFER_H
#define PRRTE_RING_BUFFER_H

#include "prrte_config.h"

#include "src/threads/threads.h"
#include "src/class/prrte_object.h"
#include "src/util/output.h"

BEGIN_C_DECLS

/**
 * dynamic pointer ring
 */
struct prrte_ring_buffer_t {
    /** base class */
    prrte_object_t super;
    /** synchronization object */
    prrte_lock_t lock;
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
typedef struct prrte_ring_buffer_t prrte_ring_buffer_t;
/**
 * Class declaration
 */
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_ring_buffer_t);

/**
 * Initialize the ring buffer, defining its size.
 *
 * @param ring Pointer to a ring buffer (IN/OUT)
 * @param size The number of elements in the ring (IN)
 *
 * @return PRRTE_SUCCESS if all initializations were succesful. Otherwise,
 *  the error indicate what went wrong in the function.
 */
PRRTE_EXPORT int prrte_ring_buffer_init(prrte_ring_buffer_t* ring, int size);

/**
 * Push an item onto the ring buffer
 *
 * @param ring Pointer to ring (IN)
 * @param ptr Pointer value (IN)
 *
 * @return PRRTE_SUCCESS. Returns error if ring cannot take
 *  another entry
 */
PRRTE_EXPORT void* prrte_ring_buffer_push(prrte_ring_buffer_t *ring, void *ptr);


/**
 * Pop an item off of the ring. The oldest entry on the ring will be
 * returned. If nothing on the ring, NULL is returned.
 *
 * @param ring          Pointer to ring (IN)
 *
 * @return Error code.  NULL indicates an error.
 */

PRRTE_EXPORT void* prrte_ring_buffer_pop(prrte_ring_buffer_t *ring);

/*
 * Access an element of the ring, without removing it, indexed
 * starting at the tail - a value of -1 will return the element
 * at the head of the ring
 */
PRRTE_EXPORT void* prrte_ring_buffer_poke(prrte_ring_buffer_t *ring, int i);

END_C_DECLS

#endif /* PRRTE_RING_BUFFER_H */
