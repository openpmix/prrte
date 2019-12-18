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
 * Copyright (c) 2014-2015 Hewlett-Packard Development Company, LP.
 *                         All rights reserved.
 * Copyright (c) 2014-2015 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>
#include <stdlib.h>

#include "src/util/output.h"
#include "src/class/prrte_hash_table.h"
#include "constants.h"

/*
 * prrte_hash_table_t
 *
 * Sketch: [Contributed by David Linden of Hewlett-Packard]
 *
 * This has been found to be good for search and insert and
 * (seldom-)remove, all with probablistic O(1) time.  Having a good
 * distribution of the hash indices is important, so even if you know
 * the keys distribute well under a mask, that micro-optimization
 * isn't worth doing.
 *
 * One aspect is that the concept of buckets and elements is
 * unified. The buckets aka elements are in a single array, each
 * element having a valid flag.  The key hashes to a keyhash, the
 * keyhash determines the first index to probe.  Missing probes search
 * forward (wrapping) until the key is found or an invalid entry is
 * found.
 *
 * One parameter of the hash table is a maximum density, which must be
 * less than 1, expressed a numerator and denominator.  1/2 seems to
 * work well.  A density less than 1 ensures the search will stop
 * because searching will eventually find an invalid element.  At
 * maximum density, assuming random usage of the elements, the
 * expected search length is 1/(1-density); for a density of 1/2, this
 * is 2.
 *
 * I believe this blinded bucket/element scheme is actually more
 * storage-efficient than a bucket having a linear list of elements.
 * It is certainly better on the cache.
 *
 * Another parameter is the growth factor, another ratio, greater than
 * 1, expressed as a numerator and denominator.  2/1 seems to work
 * well.  When the hash table reaches maximum density, it is grown by
 * the growth factor (thus reducing the density).  Growing requires
 * rehashing and reinserting existing elements.  It turns out this
 * keeps insertion at O(1): multiplies the coefficient by
 * growth/(growth-1); for a growth of 2/1 this is 2.
 *
 * The key is hashed to a keyhash.  The keyhash determines the first
 * index to probe by using the remainder of the keyhash by the table's
 * 'capacity.'  The capacity is not a power of 2.  (Keys that vary
 * only in the high 32 bits of a 64 bit key would always colide with a
 * power-of-2 capacity.)  Rather, the capacity is arranged not to be a
 * multiple of 2, 3 or 5.  A potential capacity is rounded up to be (1
 * mod 30).
 *
 * Removing a key is the most involved operation.  It is necessary to
 * rehash any valid elements immediately after the removed element,
 * because some (perhaps all) of those elements would normally hash
 * lower if the removed key were never there.  This remains O(1); the
 * implementation just needs to be a little careful.
 *
 */

#define HASH_MULTIPLIER 31

/*
 * Define the structs that are opaque in the .h
 */

struct prrte_hash_element_t {
    int         valid;          /* whether this element is valid */
    union {                     /* the key, in its various forms */
        uint32_t        u32;
        uint64_t        u64;
        struct {
            const void * key;
            size_t      key_size;
        }       ptr;
    }           key;
    void *      value;          /* the value */
};
typedef struct prrte_hash_element_t prrte_hash_element_t;

struct prrte_hash_type_methods_t {
    /* Frees any storage associated with the element
     * The value is not owned by the hash table
     * The key,key_size of pointer keys is
     */
    void        (*elt_destructor)(prrte_hash_element_t * elt);
    /* Hash the key of the element -- for growing and adjusting-after-removal */
    uint64_t    (*hash_elt)(prrte_hash_element_t * elt);
};

/* interact with the class-like mechanism */

static void prrte_hash_table_construct(prrte_hash_table_t* ht);
static void prrte_hash_table_destruct(prrte_hash_table_t* ht);

PRRTE_CLASS_INSTANCE(
    prrte_hash_table_t,
    prrte_object_t,
    prrte_hash_table_construct,
    prrte_hash_table_destruct
);

static void
prrte_hash_table_construct(prrte_hash_table_t* ht)
{
  ht->ht_table = NULL;
  ht->ht_capacity = ht->ht_size = ht->ht_growth_trigger = 0;
  ht->ht_density_numer = ht->ht_density_denom = 0;
  ht->ht_growth_numer = ht->ht_growth_denom = 0;
  ht->ht_type_methods = NULL;
}

static void
prrte_hash_table_destruct(prrte_hash_table_t* ht)
{
    prrte_hash_table_remove_all(ht);
    free(ht->ht_table);
}

/*
 * Init, etc
 */

static size_t
prrte_hash_round_capacity_up(size_t capacity)
{
    /* round up to (1 mod 30) */
    return ((capacity+29)/30*30 + 1);
}

/* this could be the new init if people wanted a more general API */
/* (that's why it isn't static) */
int                             /* PRRTE_ return code */
prrte_hash_table_init2(prrte_hash_table_t* ht, size_t estimated_max_size,
                      int density_numer, int density_denom,
                      int growth_numer, int growth_denom)
{
    size_t est_capacity = estimated_max_size * density_denom / density_numer;
    size_t capacity = prrte_hash_round_capacity_up(est_capacity);
    ht->ht_table = (prrte_hash_element_t*) calloc(capacity, sizeof(prrte_hash_element_t));
    if (NULL == ht->ht_table) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    ht->ht_capacity       = capacity;
    ht->ht_density_numer  = density_numer;
    ht->ht_density_denom  = density_denom;
    ht->ht_growth_numer   = growth_numer;
    ht->ht_growth_denom   = growth_denom;
    ht->ht_growth_trigger = capacity * density_numer / density_denom;
    ht->ht_type_methods   = NULL;
    return PRRTE_SUCCESS;
}

int                             /* PRRTE_ return code */
prrte_hash_table_init(prrte_hash_table_t* ht, size_t table_size)
{
    /* default to density of 1/2 and growth of 2/1 */
    return prrte_hash_table_init2(ht, table_size, 1, 2, 2, 1);
}

int                             /* PRRTE_ return code */
prrte_hash_table_remove_all(prrte_hash_table_t* ht)
{
    size_t ii;
    for (ii = 0; ii < ht->ht_capacity; ii += 1) {
        prrte_hash_element_t * elt = &ht->ht_table[ii];
        if (elt->valid && ht->ht_type_methods && ht->ht_type_methods->elt_destructor) {
            ht->ht_type_methods->elt_destructor(elt);
        }
        elt->valid = 0;
        elt->value = NULL;
    }
    ht->ht_size = 0;
    /* the tests reuse the hash table for different types after removing all */
    /* so we should allow that by forgetting what type it used to be */
    ht->ht_type_methods = NULL;
    return PRRTE_SUCCESS;
}

static int                      /* PRRTE_ return code */
prrte_hash_grow(prrte_hash_table_t * ht)
{
    size_t jj, ii;
    prrte_hash_element_t* old_table;
    prrte_hash_element_t* new_table;
    size_t old_capacity;
    size_t new_capacity;

    old_table    = ht->ht_table;
    old_capacity = ht->ht_capacity;

    new_capacity = old_capacity * ht->ht_growth_numer / ht->ht_growth_denom;
    new_capacity = prrte_hash_round_capacity_up(new_capacity);

    new_table    = (prrte_hash_element_t*) calloc(new_capacity, sizeof(new_table[0]));
    if (NULL == new_table) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* for each element of the old table (indexed by jj), insert it
       into the new table (indexed by ii), using the hash_elt method
       to generically hash an element, then modulo the new capacity,
       and using struct-assignment to copy an old element into its
       place int he new table.  The hash table never owns the value,
       and in the case of ptr keys the old dlements will be blindly
       deleted, so we still own the ptr key storage, just in the new
       table now */
    for (jj = 0; jj < old_capacity; jj += 1) {
        prrte_hash_element_t * old_elt;
        prrte_hash_element_t * new_elt;
        old_elt =  &old_table[jj];
        if (old_elt->valid) {
            for (ii = (ht->ht_type_methods->hash_elt(old_elt)%new_capacity); ; ii += 1) {
                if (ii == new_capacity) { ii = 0; }
                new_elt = &new_table[ii];
                if (! new_elt->valid) {
                    *new_elt = *old_elt;
                    break;
                }
            }
        }
    }
    /* update with the new, free the old, return */
    ht->ht_table = new_table;
    ht->ht_capacity = new_capacity;
    ht->ht_growth_trigger = new_capacity * ht->ht_density_numer / ht->ht_density_denom;
    free(old_table);
    return PRRTE_SUCCESS;
}

/* one of the removal functions has determined which element should be
   removed.  With the help of the type methods this can be generic.
   The important thing is to rehash any valid elements immediately
   following the element-being-removed */
static int                      /* PRRTE_ return code */
prrte_hash_table_remove_elt_at(prrte_hash_table_t * ht, size_t ii)
{
    size_t jj, capacity = ht->ht_capacity;
    prrte_hash_element_t* elts = ht->ht_table;
    prrte_hash_element_t * elt;

    elt = &elts[ii];

    if (! elt->valid) {
        /* huh?  removing a not-valid element? */
        return PRRTE_ERROR;
    }

    elt->valid = 0;
    if (ht->ht_type_methods->elt_destructor) {
        ht->ht_type_methods->elt_destructor(elt);
    }

    /* need to possibly re-insert followers because of the now-gap */
    /* E.g., XYyAaCbz.  (where upper is ideal, lower is not)
     * remove A
     * leaving XYy.aCbz. and we need to reconsider aCbz
     * first a gets reinserted where it wants to be: XYya.Cbz.
     * next  C doesn't move:                         XYya.Cbz.
     * then  b gets put where it wants to be:        XYyabC.z.
     * then  z moves down a little:                  XYyabCz..
     * then  . means we're done
     */
    for (ii = ii+1; ; ii += 1) { /* scan immediately following elements */
        if (ii == capacity) { ii = 0; }
        elt = &elts[ii];
        if (! elt->valid) {
            break;              /* done */
        }
        /* rehash it and move it if necessary */
        for (jj = ht->ht_type_methods->hash_elt(elt)%capacity; ; jj += 1) {
            if (jj == capacity) { jj = 0; }
            if (jj == ii) {
                /* already in place, either ideal or best-for-now */
                break;
            } else if (! elts[jj].valid) {
                /* move it down, and invaildate where it came from */
                elts[jj] = elts[ii];
                elts[ii].valid = 0;
                break;
            } else {
                /* still need to find its place */
            }
        }
    }
    ht->ht_size -= 1;
    return PRRTE_SUCCESS;
}


/***************************************************************************/

static uint64_t
prrte_hash_hash_elt_uint32(prrte_hash_element_t * elt)
{
  return elt->key.u32;
}

static const struct prrte_hash_type_methods_t
prrte_hash_type_methods_uint32 = {
    NULL,
    prrte_hash_hash_elt_uint32
};

int                             /* PRRTE_ return code */
prrte_hash_table_get_value_uint32(prrte_hash_table_t* ht, uint32_t key, void * *value)
{
    size_t ii, capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_uint32:"
                    "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint32 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_get_value_uint32:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint32;
    for (ii = key%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.u32 == key) {
            *value = elt->value;
            return PRRTE_SUCCESS;
        } else {
            /* keey looking */
        }
    }

}

int                             /* PRRTE_ return code */
prrte_hash_table_set_value_uint32(prrte_hash_table_t * ht, uint32_t key, void * value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_set_value_uint32:"
                   "prrte_hash_table_init() has not been called");
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint32 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_set_value_uint32:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint32;
    for (ii = key%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            /* new entry */
            elt->key.u32 = key;
            elt->value = value;
            elt->valid = 1;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PRRTE_SUCCESS != (rc = prrte_hash_grow(ht))) {
                    return rc;
                }
            }
            return PRRTE_SUCCESS;
        } else if (elt->key.u32 == key) {
            /* replace existing element */
            elt->value = value;
            return PRRTE_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int
prrte_hash_table_remove_value_uint32(prrte_hash_table_t * ht, uint32_t key)
{
    size_t ii, capacity = ht->ht_capacity;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_uint32:"
                    "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint32 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_remove_value_uint32:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint32;
    for (ii = key%capacity; ; ii += 1) {
        prrte_hash_element_t * elt;
        if (ii == capacity) ii = 0;
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.u32 == key) {
            return prrte_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}


/***************************************************************************/


static uint64_t
prrte_hash_hash_elt_uint64(prrte_hash_element_t * elt)
{
  return elt->key.u64;
}

static const struct prrte_hash_type_methods_t
prrte_hash_type_methods_uint64 = {
    NULL,
    prrte_hash_hash_elt_uint64
};

int                             /* PRRTE_ return code */
prrte_hash_table_get_value_uint64(prrte_hash_table_t * ht, uint64_t key, void * *value)
{
    size_t ii;
    size_t capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_uint64:"
                   "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint64 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_get_value_uint64:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint64;
    for (ii = key%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.u64 == key) {
            *value = elt->value;
            return PRRTE_SUCCESS;
        } else {
            /* keep looking */
        }
    }

}

int                             /* PRRTE_ return code */
prrte_hash_table_set_value_uint64(prrte_hash_table_t * ht, uint64_t key, void * value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_set_value_uint64:"
                   "prrte_hash_table_init() has not been called");
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint64 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_set_value_uint64:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint64;
    for (ii = key%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            /* new entry */
            elt->key.u64 = key;
            elt->value = value;
            elt->valid = 1;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PRRTE_SUCCESS != (rc = prrte_hash_grow(ht))) {
                    return rc;
                }
            }
            return PRRTE_SUCCESS;
        } else if (elt->key.u64 == key) {
            elt->value = value;
            return PRRTE_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}


int                             /* PRRTE_ return code */
prrte_hash_table_remove_value_uint64(prrte_hash_table_t * ht, uint64_t key)
{
    size_t ii, capacity = ht->ht_capacity;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_uint64:"
                    "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_uint64 != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_remove_value_uint64:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_uint64;
    for (ii = key%capacity; ; ii += 1) {
        prrte_hash_element_t * elt;
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.u64 == key) {
            return prrte_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}


/***************************************************************************/

/* helper function used in several places */
static uint64_t
prrte_hash_hash_key_ptr(const void * key, size_t key_size)
{
    uint64_t hash;
    const unsigned char *scanner;
    size_t ii;

    hash = 0;
    scanner = (const unsigned char *)key;
    for (ii = 0; ii < key_size; ii += 1) {
        hash = HASH_MULTIPLIER*hash + *scanner++;
    }
    return hash;
}

/* ptr methods */

static void
prrte_hash_destruct_elt_ptr(prrte_hash_element_t * elt)
{
    elt->key.ptr.key_size = 0;
    void * key = (void *) elt->key.ptr.key; /* cast away const so we can free it */
    if (NULL != key) {
        elt->key.ptr.key = NULL;
        free(key);
    }
}

static uint64_t
prrte_hash_hash_elt_ptr(prrte_hash_element_t * elt)
{
    return prrte_hash_hash_key_ptr(elt->key.ptr.key, elt->key.ptr.key_size);
}

static const struct prrte_hash_type_methods_t
prrte_hash_type_methods_ptr = {
    prrte_hash_destruct_elt_ptr,
    prrte_hash_hash_elt_ptr
};

int                             /* PRRTE_ return code */
prrte_hash_table_get_value_ptr(prrte_hash_table_t * ht,
                              const void * key, size_t key_size,
                              void * *value)
{
    size_t ii, capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_ptr:"
                   "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_ptr != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_get_value_ptr:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_ptr;
    for (ii = prrte_hash_hash_key_ptr(key, key_size)%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.ptr.key_size == key_size &&
                   0 == memcmp(elt->key.ptr.key, key, key_size)) {
            *value = elt->value;
            return PRRTE_SUCCESS;
        } else {
            /* keep going */
        }
    }
}

int                             /* PRRTE_ return code */
prrte_hash_table_set_value_ptr(prrte_hash_table_t * ht,
                              const void * key, size_t key_size,
                              void * value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    prrte_hash_element_t * elt;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_set_value_ptr:"
                   "prrte_hash_table_init() has not been called");
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_ptr != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_set_value_ptr:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_ptr;
    for (ii = prrte_hash_hash_key_ptr(key, key_size)%capacity; ; ii += 1) {
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            /* new entry */
            void * key_local = malloc(key_size);
            memcpy(key_local, key, key_size);
            elt->key.ptr.key      = key_local;
            elt->key.ptr.key_size = key_size;
            elt->value = value;
            elt->valid = 1;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PRRTE_SUCCESS != (rc = prrte_hash_grow(ht))) {
                    return rc;
                }
            }
            return PRRTE_SUCCESS;
        } else if (elt->key.ptr.key_size == key_size &&
                   0 == memcmp(elt->key.ptr.key, key, key_size)) {
            /* replace existing value */
            elt->value = value;
            return PRRTE_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int                             /* PRRTE_ return code */
prrte_hash_table_remove_value_ptr(prrte_hash_table_t * ht,
                                 const void * key, size_t key_size)
{
    size_t ii, capacity = ht->ht_capacity;

#if PRRTE_ENABLE_DEBUG
    if(capacity == 0) {
        prrte_output(0, "prrte_hash_table_get_value_ptr:"
                    "prrte_hash_table_init() has not been called");
        return PRRTE_ERROR;
    }
    if (NULL != ht->ht_type_methods &&
        &prrte_hash_type_methods_ptr != ht->ht_type_methods) {
        prrte_output(0, "prrte_hash_table_remove_value_ptr:"
                    "hash table is for a different key type");
            return PRRTE_ERROR;
    }
#endif

    ht->ht_type_methods = &prrte_hash_type_methods_ptr;
    for (ii = prrte_hash_hash_key_ptr(key, key_size)%capacity; ; ii += 1) {
        prrte_hash_element_t * elt;
        if (ii == capacity) { ii = 0; }
        elt = &ht->ht_table[ii];
        if (! elt->valid) {
            return PRRTE_ERR_NOT_FOUND;
        } else if (elt->key.ptr.key_size == key_size &&
                   0 == memcmp(elt->key.ptr.key, key, key_size)) {
            return prrte_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}

/***************************************************************************/
/* Traversals */

static int                      /* PRRTE_ return code */
prrte_hash_table_get_next_elt(prrte_hash_table_t *ht,
                             prrte_hash_element_t * prev_elt, /* NULL means find first */
                             prrte_hash_element_t * *next_elt)
{
  prrte_hash_element_t* elts = ht->ht_table;
  size_t ii, capacity = ht->ht_capacity;

  for (ii = (NULL == prev_elt ? 0 : (prev_elt-elts)+1); ii < capacity; ii += 1) {
    prrte_hash_element_t * elt = &elts[ii];
    if (elt->valid) {
      *next_elt = elt;
      return PRRTE_SUCCESS;
    }
  }
  return PRRTE_ERROR;
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_first_key_uint32(prrte_hash_table_t * ht,
                                     uint32_t *key, void * *value,
                                     void * *node)
{
  return prrte_hash_table_get_next_key_uint32(ht, key, value, NULL, node);
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_next_key_uint32(prrte_hash_table_t * ht,
                                    uint32_t *key, void * *value,
                                    void * in_node, void * *out_node)
{
  prrte_hash_element_t * elt;
  if (PRRTE_SUCCESS == prrte_hash_table_get_next_elt(ht, (prrte_hash_element_t *) in_node, &elt)) {
    *key       = elt->key.u32;
    *value     = elt->value;
    *out_node  = elt;
    return PRRTE_SUCCESS;
  }
  return PRRTE_ERROR;
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_first_key_ptr(prrte_hash_table_t * ht,
                                  void * *key, size_t *key_size, void * *value,
                                  void * *node)
{
  return prrte_hash_table_get_next_key_ptr(ht, key, key_size, value, NULL, node);
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_next_key_ptr(prrte_hash_table_t * ht,
                                 void * *key, size_t *key_size, void * *value,
                                 void * in_node, void * *out_node)
{
  prrte_hash_element_t * elt;
  if (PRRTE_SUCCESS == prrte_hash_table_get_next_elt(ht, (prrte_hash_element_t *) in_node, &elt)) {
    *key       = (void *)elt->key.ptr.key;
    *key_size  = elt->key.ptr.key_size;
    *value     = elt->value;
    *out_node  = elt;
    return PRRTE_SUCCESS;
  }
  return PRRTE_ERROR;
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_first_key_uint64(prrte_hash_table_t * ht,
                                     uint64_t *key, void * *value,
                                     void * *node)
{
  return prrte_hash_table_get_next_key_uint64(ht, key, value, NULL, node);
}

int                             /* PRRTE_ return code */
prrte_hash_table_get_next_key_uint64(prrte_hash_table_t * ht,
                                    uint64_t *key, void * *value,
                                    void * in_node, void * *out_node)
{
  prrte_hash_element_t * elt;
  if (PRRTE_SUCCESS == prrte_hash_table_get_next_elt(ht, (prrte_hash_element_t *) in_node, &elt)) {
    *key       = elt->key.u64;
    *value     = elt->value;
    *out_node  = elt;
    return PRRTE_SUCCESS;
  }
  return PRRTE_ERROR;
}

/* there was/is no traversal for the ptr case; it would go here */
/* interact with the class-like mechanism */

static void prrte_proc_table_construct(prrte_proc_table_t* pt);
static void prrte_proc_table_destruct(prrte_proc_table_t* pt);

PRRTE_CLASS_INSTANCE(
    prrte_proc_table_t,
    prrte_hash_table_t,
    prrte_proc_table_construct,
    prrte_proc_table_destruct
);

static void
prrte_proc_table_construct(prrte_proc_table_t* pt)
{
  pt->pt_size = 0;
}

static void
prrte_proc_table_destruct(prrte_proc_table_t* pt)
{
}

/*
 * Init, etc
 */

int prrte_proc_table_init(prrte_proc_table_t* pt, size_t jobids, size_t vpids) {
    int rc;
    if (PRRTE_SUCCESS != (rc=prrte_hash_table_init(&pt->super, jobids))) {
        return rc;
    }
    pt->vpids_size = vpids;
    return PRRTE_SUCCESS;
}

int prrte_proc_table_remove_all(prrte_proc_table_t *pt) {
    int rc;
    prrte_hash_table_t * vpids;
    uint32_t jobid;
    void * node;

    rc = prrte_hash_table_get_first_key_uint32(&pt->super, &jobid, (void **)&vpids, &node);

    if (PRRTE_SUCCESS == rc) {
        do {
            if (NULL != vpids) {
                prrte_hash_table_remove_all(vpids);
                PRRTE_RELEASE(vpids);
            }
            rc = prrte_hash_table_get_next_key_uint32 (&pt->super, &jobid,
                                               (void **) &vpids, node, &node);
        } while (PRRTE_SUCCESS == rc);
    }

    return rc;
}

int prrte_proc_table_get_value(prrte_proc_table_t* pt, prrte_process_name_t key,
                              void** ptr) {
    int rc;
    prrte_hash_table_t * vpids;
    rc = prrte_hash_table_get_value_uint32(&pt->super, key.jobid, (void **)&vpids);
    if (rc != PRRTE_SUCCESS) {
        return rc;
    }
    rc = prrte_hash_table_get_value_uint32(vpids, key.vpid, ptr);
    return rc;
}

int prrte_proc_table_set_value(prrte_proc_table_t* pt, prrte_process_name_t key, void* value) {
    int rc;
    prrte_hash_table_t * vpids;
    rc = prrte_hash_table_get_value_uint32(&pt->super, key.jobid, (void **)&vpids);
    if (rc != PRRTE_SUCCESS) {
        vpids = PRRTE_NEW(prrte_hash_table_t);
        if (NULL == vpids) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        if (PRRTE_SUCCESS != (rc=prrte_hash_table_init(vpids, pt->vpids_size))) {
            PRRTE_RELEASE(vpids);
            return rc;
        }
        if (PRRTE_SUCCESS != (rc=prrte_hash_table_set_value_uint32(&pt->super, key.jobid, vpids))) {
            PRRTE_RELEASE(vpids);
            return rc;
        }
    }
    rc = prrte_hash_table_set_value_uint32(vpids, key.vpid, value);
    return rc;
}

int prrte_proc_table_remove_value(prrte_proc_table_t* pt, prrte_process_name_t key) {
    int rc;
    prrte_hash_table_t * vpids;
    if (PRRTE_SUCCESS != (rc=prrte_hash_table_get_value_uint32(&pt->super, key.jobid, (void **)&vpids))) {
        return rc;
    }
    if (PRRTE_SUCCESS == (rc=prrte_hash_table_remove_value_uint32(vpids, key.vpid))) {
        if (0 == vpids->ht_size) {
            prrte_hash_table_remove_value_uint32(&pt->super, key.jobid);
            PRRTE_RELEASE(vpids);
        }
    }
    return rc;
}

int prrte_proc_table_get_first_key(prrte_proc_table_t *pt, prrte_process_name_t *key,
                                  void **value, void **node1, void **node2) {
    int rc;
    uint32_t jobid, vpid;
    prrte_hash_table_t * vpids;
    if (PRRTE_SUCCESS != (rc=prrte_hash_table_get_first_key_uint32(&pt->super, &jobid, (void **)&vpids, node1))) {
        return rc;
    }
    rc = prrte_hash_table_get_first_key_uint32(vpids, &vpid, value, node2);
    if (PRRTE_SUCCESS == rc) {
        key->jobid = jobid;
        key->vpid = vpid;
    }
    return rc;
}

int prrte_proc_table_get_next_key(prrte_proc_table_t *pt, prrte_process_name_t *key,
                                 void **value, void *in_node1, void **out_node1,
                                 void *in_node2, void **out_node2) {
    int rc;
    uint32_t jobid = ((prrte_hash_element_t *)in_node1)->key.u32;
    uint32_t vpid;
    prrte_hash_table_t * vpids = ((prrte_hash_element_t *)in_node1)->value;

    rc = prrte_hash_table_get_next_key_uint32(vpids, &vpid, value, in_node2, out_node2);
    if (PRRTE_SUCCESS == rc) {
        key->jobid = jobid;
        key->vpid = vpid;
        *out_node1 = in_node1;
        return rc;
    }
    if (PRRTE_SUCCESS != (rc=prrte_hash_table_get_next_key_uint32(&pt->super, &jobid, (void **)&vpids, in_node1, out_node1))) {
        return rc;
    }
    rc = prrte_hash_table_get_first_key_uint32(vpids, &vpid, value, out_node2);
    if (PRRTE_SUCCESS == rc) {
        key->jobid = jobid;
        key->vpid = vpid;
    }
    return rc;
}
