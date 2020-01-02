/*
 * Copyright (c) 2017      Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/class/prrte_object.h"

#include "src/mca/reachable/reachable.h"
#include "src/mca/reachable/base/base.h"


static void prrte_reachable_construct(prrte_reachable_t *reachable)
{
    reachable->weights = NULL;
}


static void prrte_reachable_destruct(prrte_reachable_t * reachable)
{
    if (NULL != reachable->memory) {
        free(reachable->memory);
    }
}


prrte_reachable_t * prrte_reachable_allocate(unsigned int num_local,
                                             unsigned int num_remote)
{
    char *memory;
    unsigned int i;
    prrte_reachable_t *reachable = PRRTE_NEW(prrte_reachable_t);

    reachable->num_local = num_local;
    reachable->num_remote = num_remote;

    /* allocate all the pieces of the two dimensional array in one
       malloc, rather than a bunch of little allocations */
    memory = malloc(sizeof(int*) * num_local +
                    num_local * (sizeof(int) * num_remote));
    if (memory == NULL) return NULL;

    reachable->memory = (void*)memory;
    reachable->weights = (int**)reachable->memory;
    memory += (sizeof(int*) * num_local);

    for (i = 0; i < num_local; i++) {
        reachable->weights[i] = (int*)memory;
        memory += (sizeof(int) * num_remote);
    }

    return reachable;
}

PRRTE_CLASS_INSTANCE(
    prrte_reachable_t,
    prrte_object_t,
    prrte_reachable_construct,
    prrte_reachable_destruct
);
