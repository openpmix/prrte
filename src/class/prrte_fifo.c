/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2018 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/class/prrte_fifo.h"

static void prrte_fifo_construct (prrte_fifo_t *fifo)
{
    PRRTE_CONSTRUCT(&fifo->prrte_fifo_ghost, prrte_list_item_t);

    fifo->prrte_fifo_ghost.prrte_list_next = &fifo->prrte_fifo_ghost;

    /** used to protect against ABA problems when not using a 128-bit compare-and-set */
    fifo->prrte_fifo_ghost.item_free = 0;

    fifo->prrte_fifo_head.data.counter = 0;
    fifo->prrte_fifo_head.data.item = (intptr_t) &fifo->prrte_fifo_ghost;

    fifo->prrte_fifo_tail.data.counter = 0;
    fifo->prrte_fifo_tail.data.item = (intptr_t) &fifo->prrte_fifo_ghost;
}

PRRTE_CLASS_INSTANCE(prrte_fifo_t, prrte_object_t, prrte_fifo_construct, NULL);
