/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2014 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Voltaire All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "constants.h"

/*
 *  List classes
 */

static void prrte_list_item_construct(prrte_list_item_t*);
static void prrte_list_item_destruct(prrte_list_item_t*);

PRRTE_CLASS_INSTANCE(
    prrte_list_item_t,
    prrte_object_t,
    prrte_list_item_construct,
    prrte_list_item_destruct
);

static void prrte_list_construct(prrte_list_t*);
static void prrte_list_destruct(prrte_list_t*);

PRRTE_CLASS_INSTANCE(
    prrte_list_t,
    prrte_object_t,
    prrte_list_construct,
    prrte_list_destruct
);


/*
 *
 *      prrte_list_link_item_t interface
 *
 */

static void prrte_list_item_construct(prrte_list_item_t *item)
{
    item->prrte_list_next = item->prrte_list_prev = NULL;
    item->item_free = 1;
#if PRRTE_ENABLE_DEBUG
    item->prrte_list_item_refcount = 0;
    item->prrte_list_item_belong_to = NULL;
#endif
}

static void prrte_list_item_destruct(prrte_list_item_t *item)
{
#if PRRTE_ENABLE_DEBUG
    assert( 0 == item->prrte_list_item_refcount );
    assert( NULL == item->prrte_list_item_belong_to );
#endif  /* PRRTE_ENABLE_DEBUG */
}


/*
 *
 *      prrte_list_list_t interface
 *
 */

static void prrte_list_construct(prrte_list_t *list)
{
#if PRRTE_ENABLE_DEBUG
    /* These refcounts should never be used in assertions because they
       should never be removed from this list, added to another list,
       etc.  So set them to sentinel values. */

    PRRTE_CONSTRUCT( &(list->prrte_list_sentinel), prrte_list_item_t );
    list->prrte_list_sentinel.prrte_list_item_refcount  = 1;
    list->prrte_list_sentinel.prrte_list_item_belong_to = list;
#endif

    list->prrte_list_sentinel.prrte_list_next = &list->prrte_list_sentinel;
    list->prrte_list_sentinel.prrte_list_prev = &list->prrte_list_sentinel;
    list->prrte_list_length = 0;
}


/*
 * Reset all the pointers to be NULL -- do not actually destroy
 * anything.
 */
static void prrte_list_destruct(prrte_list_t *list)
{
    prrte_list_construct(list);
}


/*
 * Insert an item at a specific place in a list
 */
bool prrte_list_insert(prrte_list_t *list, prrte_list_item_t *item, long long idx)
{
    /* Adds item to list at index and retains item. */
    int     i;
    volatile prrte_list_item_t *ptr, *next;

    if ( idx >= (long long)list->prrte_list_length ) {
        return false;
    }

    if ( 0 == idx )
    {
        prrte_list_prepend(list, item);
    } else {
#if PRRTE_ENABLE_DEBUG
        /* Spot check: ensure that this item is previously on no
           lists */

        assert(0 == item->prrte_list_item_refcount);
#endif
        /* pointer to element 0 */
        ptr = list->prrte_list_sentinel.prrte_list_next;
        for ( i = 0; i < idx-1; i++ )
            ptr = ptr->prrte_list_next;

        next = ptr->prrte_list_next;
        item->prrte_list_next = next;
        item->prrte_list_prev = ptr;
        next->prrte_list_prev = item;
        ptr->prrte_list_next = item;

#if PRRTE_ENABLE_DEBUG
        /* Spot check: ensure this item is only on the list that we
           just insertted it into */

        prrte_atomic_add ( &(item->prrte_list_item_refcount), 1 );
        assert(1 == item->prrte_list_item_refcount);
        item->prrte_list_item_belong_to = list;
#endif
    }

    list->prrte_list_length++;
    return true;
}


static
void
prrte_list_transfer(prrte_list_item_t *pos, prrte_list_item_t *begin,
                   prrte_list_item_t *end)
{
    volatile prrte_list_item_t *tmp;

    if (pos != end) {
        /* remove [begin, end) */
        end->prrte_list_prev->prrte_list_next = pos;
        begin->prrte_list_prev->prrte_list_next = end;
        pos->prrte_list_prev->prrte_list_next = begin;

        /* splice into new position before pos */
        tmp = pos->prrte_list_prev;
        pos->prrte_list_prev = end->prrte_list_prev;
        end->prrte_list_prev = begin->prrte_list_prev;
        begin->prrte_list_prev = tmp;
#if PRRTE_ENABLE_DEBUG
        {
            volatile prrte_list_item_t* item = begin;
            while( pos != item ) {
                item->prrte_list_item_belong_to = pos->prrte_list_item_belong_to;
                item = item->prrte_list_next;
                assert(NULL != item);
            }
        }
#endif  /* PRRTE_ENABLE_DEBUG */
    }
}


void
prrte_list_join(prrte_list_t *thislist, prrte_list_item_t *pos,
               prrte_list_t *xlist)
{
    if (0 != prrte_list_get_size(xlist)) {
        prrte_list_transfer(pos, prrte_list_get_first(xlist),
                           prrte_list_get_end(xlist));

        /* fix the sizes */
        thislist->prrte_list_length += xlist->prrte_list_length;
        xlist->prrte_list_length = 0;
    }
}


void
prrte_list_splice(prrte_list_t *thislist, prrte_list_item_t *pos,
                 prrte_list_t *xlist, prrte_list_item_t *first,
                 prrte_list_item_t *last)
{
    size_t change = 0;
    prrte_list_item_t *tmp;

    if (first != last) {
        /* figure out how many things we are going to move (have to do
         * first, since last might be end and then we wouldn't be able
         * to run the loop)
         */
        for (tmp = first ; tmp != last ; tmp = prrte_list_get_next(tmp)) {
            change++;
        }

        prrte_list_transfer(pos, first, last);

        /* fix the sizes */
        thislist->prrte_list_length += change;
        xlist->prrte_list_length -= change;
    }
}


int prrte_list_sort(prrte_list_t* list, prrte_list_item_compare_fn_t compare)
{
    prrte_list_item_t* item;
    prrte_list_item_t** items;
    size_t i, index=0;

    if (0 == list->prrte_list_length) {
        return PRRTE_SUCCESS;
    }
    items = (prrte_list_item_t**)malloc(sizeof(prrte_list_item_t*) *
                                       list->prrte_list_length);

    if (NULL == items) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    while(NULL != (item = prrte_list_remove_first(list))) {
        items[index++] = item;
    }

    qsort(items, index, sizeof(prrte_list_item_t*),
          (int(*)(const void*,const void*))compare);
    for (i=0; i<index; i++) {
        prrte_list_append(list,items[i]);
    }
    free(items);
    return PRRTE_SUCCESS;
}
