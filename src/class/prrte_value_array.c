/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/class/prrte_value_array.h"


static void prrte_value_array_construct(prrte_value_array_t* array)
{
    array->array_items = NULL;
    array->array_size = 0;
    array->array_item_sizeof = 0;
    array->array_alloc_size = 0;
}

static void prrte_value_array_destruct(prrte_value_array_t* array)
{
    if (NULL != array->array_items)
        free(array->array_items);
}

PRRTE_CLASS_INSTANCE(
    prrte_value_array_t,
    prrte_object_t,
    prrte_value_array_construct,
    prrte_value_array_destruct
);


int prrte_value_array_set_size(prrte_value_array_t* array, size_t size)
{
#if PRRTE_ENABLE_DEBUG
    if(array->array_item_sizeof == 0) {
        prrte_output(0, "prrte_value_array_set_size: item size must be initialized");
        return PRRTE_ERR_BAD_PARAM;
    }
#endif

    if(size > array->array_alloc_size) {
        while(array->array_alloc_size < size)
            array->array_alloc_size <<= 1;
        array->array_items = (unsigned char *)realloc(array->array_items,
            array->array_alloc_size * array->array_item_sizeof);
        if (NULL == array->array_items)
            return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    array->array_size = size;
    return PRRTE_SUCCESS;
}

