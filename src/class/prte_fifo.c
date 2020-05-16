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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/class/prte_fifo.h"

static void prte_fifo_construct (prte_fifo_t *fifo)
{
    PRTE_CONSTRUCT(&fifo->prte_fifo_ghost, prte_list_item_t);

    fifo->prte_fifo_ghost.prte_list_next = &fifo->prte_fifo_ghost;

    /** used to protect against ABA problems when not using a 128-bit compare-and-set */
    fifo->prte_fifo_ghost.item_free = 0;

    fifo->prte_fifo_head.data.counter = 0;
    fifo->prte_fifo_head.data.item = (intptr_t) &fifo->prte_fifo_ghost;

    fifo->prte_fifo_tail.data.counter = 0;
    fifo->prte_fifo_tail.data.item = (intptr_t) &fifo->prte_fifo_ghost;
}

PRTE_CLASS_INSTANCE(prte_fifo_t, prte_object_t, prte_fifo_construct, NULL);
