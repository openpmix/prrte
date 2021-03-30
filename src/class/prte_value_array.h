/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_VALUE_ARRAY_H
#define PRTE_VALUE_ARRAY_H

#include "prte_config.h"

#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif /* HAVE_STRINGS_H */

#include "src/class/prte_object.h"
#if PRTE_ENABLE_DEBUG
#    include "src/util/output.h"
#endif
#include "constants.h"

BEGIN_C_DECLS

/*
 *  @file  Array of elements maintained by value.
 */

struct prte_value_array_t {
    prte_object_t super;
    unsigned char *array_items;
    size_t array_item_sizeof;
    size_t array_size;
    size_t array_alloc_size;
};
typedef struct prte_value_array_t prte_value_array_t;

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_value_array_t);

/**
 *  Initialize the array to hold items by value. This routine must
 *  be called prior to using the array.
 *
 *  @param   array       The array to initialize (IN).
 *  @param   item_size   The sizeof each array element (IN).
 *  @return  PRTE error code
 *
 * Note that there is no corresponding "finalize" function -- use
 * PRTE_DESTRUCT (for stack arrays) or PRTE_RELEASE (for heap arrays) to
 * delete it.
 */

static inline int prte_value_array_init(prte_value_array_t *array, size_t item_sizeof)
{
    array->array_item_sizeof = item_sizeof;
    array->array_alloc_size = 1;
    array->array_size = 0;
    array->array_items = (unsigned char *) realloc(array->array_items,
                                                   item_sizeof * array->array_alloc_size);
    return (NULL != array->array_items) ? PRTE_SUCCESS : PRTE_ERR_OUT_OF_RESOURCE;
}

/**
 *  Reserve space in the array for new elements, but do not change the size.
 *
 *  @param   array   The input array (IN).
 *  @param   size    The anticipated size of the array (IN).
 *  @return  PRTE error code.
 */

static inline int prte_value_array_reserve(prte_value_array_t *array, size_t size)
{
    if (size > array->array_alloc_size) {
        array->array_items = (unsigned char *) realloc(array->array_items,
                                                       array->array_item_sizeof * size);
        if (NULL == array->array_items) {
            array->array_size = 0;
            array->array_alloc_size = 0;
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        array->array_alloc_size = size;
    }
    return PRTE_SUCCESS;
}

/**
 *  Retreives the number of elements in the array.
 *
 *  @param   array   The input array (IN).
 *  @return  The number of elements currently in use.
 */

static inline size_t prte_value_array_get_size(prte_value_array_t *array)
{
    return array->array_size;
}

/**
 *  Set the number of elements in the array.
 *
 *  @param  array   The input array (IN).
 *  @param  size    The new array size.
 *
 *  @return  PRTE error code.
 *
 *  Note that resizing the array to a smaller size may not change
 *  the underlying memory allocated by the array. However, setting
 *  the size larger than the current allocation will grow it. In either
 *  case, if the routine is successful, prte_value_array_get_size() will
 *  return the new size.
 */

PRTE_EXPORT int prte_value_array_set_size(prte_value_array_t *array, size_t size);

/**
 *  Macro to retrieve an item from the array by value.
 *
 *  @param  array       The input array (IN).
 *  @param  item_type   The C datatype of the array item (IN).
 *  @param  item_index  The array index (IN).
 *
 *  @returns item       The requested item.
 *
 *  Note that this does not change the size of the array - this macro is
 *  strictly for performance - the user assumes the responsibility of
 *  ensuring the array index is valid (0 <= item index < array size).
 */

#define PRTE_VALUE_ARRAY_GET_ITEM(array, item_type, item_index) \
    ((item_type *) ((array)->array_items))[item_index]

/**
 *  Retrieve an item from the array by reference.
 *
 *  @param  array          The input array (IN).
 *  @param  item_index     The array index (IN).
 *
 *  @return ptr Pointer to the requested item.
 *
 *  Note that if the specified item_index is larger than the current
 *  array size, the array is grown to satisfy the request.
 */

static inline void *prte_value_array_get_item(prte_value_array_t *array, size_t item_index)
{
    if (item_index >= array->array_size
        && prte_value_array_set_size(array, item_index + 1) != PRTE_SUCCESS)
        return NULL;
    return array->array_items + (item_index * array->array_item_sizeof);
}

/**
 *  Macro to set an array element by value.
 *
 *  @param  array       The input array (IN).
 *  @param  item_type   The C datatype of the array item (IN).
 *  @param  item_index  The array index (IN).
 *  @param  item_value  The new value for the specified index (IN).
 *
 *  Note that this does not change the size of the array - this macro is
 *  strictly for performance - the user assumes the responsibility of
 *  ensuring the array index is valid (0 <= item index < array size).
 *
 * It is safe to free the item after returning from this call; it is
 * copied into the array by value.
 */

#define PRTE_VALUE_ARRAY_SET_ITEM(array, item_type, item_index, item_value) \
    (((item_type *) ((array)->array_items))[item_index] = item_value)

/**
 *  Set an array element by value.
 *
 *  @param   array       The input array (IN).
 *  @param   item_index  The array index (IN).
 *  @param   item_value  A pointer to the item, which is copied into
 *                       the array.
 *
 *  @return  PRTE error code.
 *
 * It is safe to free the item after returning from this call; it is
 * copied into the array by value.
 */

static inline int prte_value_array_set_item(prte_value_array_t *array, size_t item_index,
                                            const void *item)
{
    int rc;
    if (item_index >= array->array_size
        && (rc = prte_value_array_set_size(array, item_index + 1)) != PRTE_SUCCESS)
        return rc;
    memcpy(array->array_items + (item_index * array->array_item_sizeof), item,
           array->array_item_sizeof);
    return PRTE_SUCCESS;
}

/**
 *  Appends an item to the end of the array.
 *
 *  @param   array    The input array (IN).
 *  @param   item     A pointer to the item to append, which is copied
 *                    into the array.
 *
 *  @return  PRTE error code
 *
 * This will grow the array if it is not large enough to contain the
 * item.  It is safe to free the item after returning from this call;
 * it is copied by value into the array.
 */

static inline int prte_value_array_append_item(prte_value_array_t *array, const void *item)
{
    return prte_value_array_set_item(array, array->array_size, item);
}

/**
 *  Remove a specific item from the array.
 *
 *  @param   array       The input array (IN).
 *  @param   item_index  The index to remove, which must be less than
 *                       the current array size (IN).
 *
 *  @return  PRTE error code.
 *
 * All elements following this index are shifted down.
 */

static inline int prte_value_array_remove_item(prte_value_array_t *array, size_t item_index)
{
#if PRTE_ENABLE_DEBUG
    if (item_index >= array->array_size) {
        prte_output(0, "prte_value_array_remove_item: invalid index %lu\n",
                    (unsigned long) item_index);
        return PRTE_ERR_BAD_PARAM;
    }
#endif
    memmove(array->array_items + (array->array_item_sizeof * item_index),
            array->array_items + (array->array_item_sizeof * (item_index + 1)),
            array->array_item_sizeof * (array->array_size - item_index - 1));
    array->array_size--;
    return PRTE_SUCCESS;
}

/**
 * Get the base pointer of the underlying array.
 *
 * @param array The input array (IN).
 * @param array_type The C datatype of the array (IN).
 *
 * @returns ptr Pointer to the actual array.
 *
 * This function is helpful when you need to iterate through an
 * entire array; simply get the base value of the array and use native
 * C to iterate through it manually.  This can have better performance
 * than looping over PRTE_VALUE_ARRAY_GET_ITEM() and
 * PRTE_VALUE_ARRAY_SET_ITEM() because it will [potentially] reduce the
 * number of pointer dereferences.
 */

#define PRTE_VALUE_ARRAY_GET_BASE(array, item_type) ((item_type *) ((array)->array_items))

END_C_DECLS

#endif
