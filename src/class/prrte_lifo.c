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
 * Copyright (c) 2014      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/class/prrte_lifo.h"

static void prrte_lifo_construct (prrte_lifo_t *lifo)
{
    PRRTE_CONSTRUCT(&lifo->prrte_lifo_ghost, prrte_list_item_t);
    lifo->prrte_lifo_ghost.prrte_list_next = &lifo->prrte_lifo_ghost;
    lifo->prrte_lifo_head.data.item = (intptr_t) &lifo->prrte_lifo_ghost;
    lifo->prrte_lifo_head.data.counter = 0;
}

PRRTE_CLASS_INSTANCE(prrte_lifo_t, prrte_object_t, prrte_lifo_construct, NULL);
