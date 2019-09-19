/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2017 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
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
/** @file
 *
 * Utility functions to manage fortran <-> c opaque object
 * translation.  Note that since MPI defines fortran handles as
 * [signed] int's, we use int everywhere in here where you would
 * normally expect size_t.  There's some code that makes sure indices
 * don't go above FORTRAN_HANDLE_MAX (which is min(INT_MAX, fortran
 * INTEGER max)), just to be sure.
 */

#ifndef PRRTE_POINTER_ARRAY_H
#define PRRTE_POINTER_ARRAY_H

#include "prrte_config.h"

#include "src/threads/mutex.h"
#include "src/class/prrte_object.h"
#include "prefetch.h"

BEGIN_C_DECLS

/**
 * dynamic pointer array
 */
struct prrte_pointer_array_t {
    /** base class */
    prrte_object_t super;
    /** synchronization object */
    prrte_mutex_t lock;
    /** Index of lowest free element.  NOTE: This is only an
        optimization to know where to search for the first free slot.
        It does \em not necessarily imply indices all above this index
        are not taken! */
    int lowest_free;
    /** number of free elements in the list */
    int number_free;
    /** size of list, i.e. number of elements in addr */
    int size;
    /** maximum size of the array */
    int max_size;
    /** block size for each allocation */
    int block_size;
    /** pointer to an array of bits to speed up the research for an empty position. */
    uint64_t* free_bits;
    /** pointer to array of pointers */
    void **addr;
};
/**
 * Convenience typedef
 */
typedef struct prrte_pointer_array_t prrte_pointer_array_t;
/**
 * Class declaration
 */
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_pointer_array_t);

/**
 * Initialize the pointer array with an initial size of initial_allocation.
 * Set the maximum size of the array, as well as the size of the allocation
 * block for all subsequent growing operations. Remarque: The pointer array
 * has to be created bfore calling this function.
 *
 * @param array Pointer to pointer of an array (IN/OUT)
 * @param initial_allocation The number of elements in the initial array (IN)
 * @param max_size The maximum size of the array (IN)
 * @param block_size The size for all subsequent grows of the array (IN).
 *
 * @return PRRTE_SUCCESS if all initializations were succesfull. Otherwise,
 *  the error indicate what went wrong in the function.
 */
PRRTE_EXPORT int prrte_pointer_array_init( prrte_pointer_array_t* array,
                                           int initial_allocation,
                                           int max_size, int block_size );

/**
 * Add a pointer to the array (Grow the array, if need be)
 *
 * @param array Pointer to array (IN)
 * @param ptr Pointer value (IN)
 *
 * @return Index of inserted array element.  Return value of
 *  (-1) indicates an error.
 */
PRRTE_EXPORT int prrte_pointer_array_add(prrte_pointer_array_t *array, void *ptr);

/**
 * Set the value of an element in array
 *
 * @param array Pointer to array (IN)
 * @param index Index of element to be reset (IN)
 * @param value New value to be set at element index (IN)
 *
 * @return Error code.  (-1) indicates an error.
 */
PRRTE_EXPORT int prrte_pointer_array_set_item(prrte_pointer_array_t *array,
                                int index, void *value);

/**
 * Get the value of an element in array
 *
 * @param array          Pointer to array (IN)
 * @param element_index  Index of element to be returned (IN)
 *
 * @return Error code.  NULL indicates an error.
 */

static inline void *prrte_pointer_array_get_item(prrte_pointer_array_t *table,
                                                int element_index)
{
    void *p;

    if( PRRTE_UNLIKELY(0 > element_index || table->size <= element_index) ) {
        return NULL;
    }
    prrte_mutex_lock(&(table->lock));
    p = table->addr[element_index];
    prrte_mutex_unlock(&(table->lock));
    return p;
}


/**
 * Get the size of the pointer array
 *
 * @param array Pointer to array (IN)
 *
 * @returns size Size of the array
 *
 * Simple inline function to return the size of the array in order to
 * hide the member field from external users.
 */
static inline int prrte_pointer_array_get_size(prrte_pointer_array_t *array)
{
  return array->size;
}

/**
 * Set the size of the pointer array
 *
 * @param array Pointer to array (IN)
 *
 * @param size Desired size of the array
 *
 * Simple function to set the size of the array in order to
 * hide the member field from external users.
 */
PRRTE_EXPORT int prrte_pointer_array_set_size(prrte_pointer_array_t *array, int size);

/**
 * Test whether a certain element is already in use. If not yet
 * in use, reserve it.
 *
 * @param array Pointer to array (IN)
 * @param index Index of element to be tested (IN)
 * @param value New value to be set at element index (IN)
 *
 * @return true/false True if element could be reserved
 *                    False if element could not be reserved (e.g., in use).
 *
 * In contrary to array_set, this function does not allow to overwrite
 * a value, unless the previous value is NULL ( equiv. to free ).
 */
PRRTE_EXPORT bool prrte_pointer_array_test_and_set_item (prrte_pointer_array_t *table,
                                          int index,
                                          void *value);

/**
 * Empty the array.
 *
 * @param array Pointer to array (IN)
 *
 */
static inline void prrte_pointer_array_remove_all(prrte_pointer_array_t *array)
{
    int i;
    if( array->number_free == array->size )
        return;  /* nothing to do here this time (the array is already empty) */

    prrte_mutex_lock(&array->lock);
    array->lowest_free = 0;
    array->number_free = array->size;
    for(i = 0; i < array->size; i++) {
        array->addr[i] = NULL;
    }
    for(i = 0; i < (int)((array->size + 8*sizeof(uint64_t) - 1) / (8*sizeof(uint64_t))); i++) {
        array->free_bits[i] = 0;
    }
    prrte_mutex_unlock(&array->lock);
}

END_C_DECLS

#endif /* PRRTE_POINTER_ARRAY_H */
