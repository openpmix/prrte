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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/class/prte_lifo.h"

static void prte_lifo_construct(prte_lifo_t *lifo)
{
    PRTE_CONSTRUCT(&lifo->prte_lifo_ghost, prte_list_item_t);
    lifo->prte_lifo_ghost.prte_list_next = &lifo->prte_lifo_ghost;
    lifo->prte_lifo_head.data.item = (intptr_t) &lifo->prte_lifo_ghost;
    lifo->prte_lifo_head.data.counter = 0;
}

PRTE_CLASS_INSTANCE(prte_lifo_t, prte_object_t, prte_lifo_construct, NULL);
