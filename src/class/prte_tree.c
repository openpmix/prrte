/*
 * Copyright (c) 2011      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2012      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/class/prte_tree.h"
#include "constants.h"

/*
 *  List classes
 */

static void prte_tree_item_construct(prte_tree_item_t*);
static void prte_tree_item_destruct(prte_tree_item_t*);

PRTE_CLASS_INSTANCE(
                   prte_tree_item_t,
                   prte_object_t,
                   prte_tree_item_construct,
                   prte_tree_item_destruct
                   );

static void prte_tree_construct(prte_tree_t*);
static void prte_tree_destruct(prte_tree_t*);

PRTE_CLASS_INSTANCE(
                   prte_tree_t,
                   prte_object_t,
                   prte_tree_construct,
                   prte_tree_destruct
                   );


/*
 *
 *      prte_tree_item_t interface
 *
 */

static void prte_tree_item_construct(prte_tree_item_t *item)
{
    item->prte_tree_parent = NULL;
    item->prte_tree_num_ancestors = 0;
    item->prte_tree_sibling_rank = 0xdeadbeef;
    item->prte_tree_next_sibling = item->prte_tree_prev_sibling = NULL;
    item->prte_tree_num_children = 0;
    item->prte_tree_first_child = item->prte_tree_last_child = NULL;
#if PRTE_ENABLE_DEBUG
    item->prte_tree_item_refcount = 0;
    item->prte_tree_item_belong_to = NULL;
#endif
}

static void prte_tree_item_destruct(prte_tree_item_t *item)
{
#if PRTE_ENABLE_DEBUG
    assert( 0 == item->prte_tree_item_refcount );
    assert( NULL == item->prte_tree_item_belong_to );
#endif  /* PRTE_ENABLE_DEBUG */
}


/*
 *
 *      prte_tree_t interface
 *
 */

static void prte_tree_construct(prte_tree_t *tree)
{
    PRTE_CONSTRUCT( &(tree->prte_tree_sentinel), prte_tree_item_t );

#if PRTE_ENABLE_DEBUG
    /* These refcounts should never be used in assertions because they
       should never be removed from this list, added to another list,
       etc.  So set them to sentinel values. */

    tree->prte_tree_sentinel.prte_tree_item_refcount  = 1;
    tree->prte_tree_sentinel.prte_tree_item_belong_to = tree;
#endif
    tree->prte_tree_sentinel.prte_tree_container = tree;
    tree->prte_tree_sentinel.prte_tree_parent = &tree->prte_tree_sentinel;
    tree->prte_tree_sentinel.prte_tree_num_ancestors = (unsigned) -1;

    tree->prte_tree_sentinel.prte_tree_next_sibling =
        &tree->prte_tree_sentinel;
    tree->prte_tree_sentinel.prte_tree_prev_sibling =
        &tree->prte_tree_sentinel;

    tree->prte_tree_sentinel.prte_tree_first_child = &tree->prte_tree_sentinel;
    tree->prte_tree_sentinel.prte_tree_last_child = &tree->prte_tree_sentinel;

    tree->prte_tree_num_items = 0;
    tree->comp = NULL;
    tree->serialize = NULL;
    tree->deserialize = NULL;
    tree->get_key = NULL;
}

/*
 * Reset all the pointers to be NULL -- do not actually destroy
 * anything.
 */
static void prte_tree_destruct(prte_tree_t *tree)
{
    prte_tree_construct(tree);
}

/*
 * initialize tree container
 */
void prte_tree_init(prte_tree_t *tree, prte_tree_comp_fn_t comp,
                    prte_tree_item_serialize_fn_t serialize,
                    prte_tree_item_deserialize_fn_t deserialize,
                    prte_tree_get_key_fn_t get_key)
{
    tree->comp = comp;
    tree->serialize = serialize;
    tree->deserialize = deserialize;
    tree->get_key = get_key;
}

/*
 * count all the descendants from our level and below
 */
static int count_descendants(prte_tree_item_t* item)
{
    int current_count = 0;

    /* loop over all siblings for descendants to count */
    while (item) {
        current_count += count_descendants(prte_tree_get_first_child(item));
        current_count++; /* count ourselves */
        item = prte_tree_get_next_sibling(item);
    }
    return(current_count);
}

/*
 * get size of tree
 */
size_t prte_tree_get_size(prte_tree_t* tree)
{
#if PRTE_ENABLE_DEBUG
    /* not sure if we really want this running in devel, as it does
     * slow things down.  Wanted for development of splice / join to
     * make sure length was reset properly
     */
    size_t check_len = 0;
    prte_tree_item_t *root;

    if (!prte_tree_is_empty(tree)) {
        /* tree has entries so count up items */
        root = prte_tree_get_root(tree);
        check_len = count_descendants(root);
    }

    if (check_len != tree->prte_tree_num_items) {
        fprintf(stderr," Error :: prte_tree_get_size - prte_tree_num_items does not match actual tree length\n");
        fflush(stderr);
        abort();
    }
#endif

    return tree->prte_tree_num_items;
}

/*
 * add item to parent's child list
 */
void prte_tree_add_child(prte_tree_item_t *parent_item,
                         prte_tree_item_t *new_item)
{
#if PRTE_ENABLE_DEBUG
    /* Spot check: ensure that this item is previously on no lists */

    assert(0 == new_item->prte_tree_item_refcount);
    assert( NULL == new_item->prte_tree_item_belong_to );
#endif

    new_item->prte_tree_parent = parent_item;
    new_item->prte_tree_num_ancestors = parent_item->prte_tree_num_ancestors+1;
    if (parent_item->prte_tree_num_children) {
        /* append item to end of children and sibling lists */
        new_item->prte_tree_prev_sibling = parent_item->prte_tree_last_child;
        parent_item->prte_tree_last_child->prte_tree_next_sibling = new_item;
    } else {
        /* no children existing on parent */
        parent_item->prte_tree_first_child = new_item;
    }
    parent_item->prte_tree_last_child = new_item;
    parent_item->prte_tree_num_children++;
    new_item->prte_tree_container = parent_item->prte_tree_container;
    new_item->prte_tree_container->prte_tree_num_items++;

#if PRTE_ENABLE_DEBUG
    /* Spot check: ensure this item is only on the list that we just
       appended it to */

    PRTE_THREAD_ADD_FETCH32( &(new_item->prte_tree_item_refcount), 1 );
    assert(1 == new_item->prte_tree_item_refcount);
    new_item->prte_tree_item_belong_to = new_item->prte_tree_container;
#endif
}

/*
 * check to see if item is in tree
 */
#if PRTE_ENABLE_DEBUG
static bool item_in_tree(prte_tree_item_t *item, prte_tree_item_t *search_item)
{
    bool result = false;
    prte_tree_item_t *first_child;

    while (!result && item) {
        /* check for item match */
        result = (item == search_item) ? true : false;
        if (!result && (first_child = prte_tree_get_first_child(item))) {
            /* search descendants for match */
            result = item_in_tree(first_child, search_item);
        }
        if (!result) {
            /* didn't find match at our node or descending so check sibling */
            item = prte_tree_get_next_sibling(item);
        }
    }
    return(result);
}
#endif  /* PRTE_ENABLE_DEBUG */

/*
 * remove item and all items below it from tree and return it to the caller
 */
prte_tree_item_t *prte_tree_remove_subtree(prte_tree_item_t *item)
{
    prte_tree_item_t *parent_item = NULL;

#if PRTE_ENABLE_DEBUG
    /* validate that item does exist on tree */
    if (NULL != item->prte_tree_container) {
        /* we point to a container, check if we can find item in tree */
        if (!item_in_tree(prte_tree_get_root(item->prte_tree_container), item)) {
            return(NULL);
        }
    } else {
        return (NULL);
    }
#endif

    parent_item = item->prte_tree_parent;

    /* remove from parent */
    /* If item is the only child, set _first_child and _last_child to
       be item's _first_child and _last_child */
    if (parent_item->prte_tree_first_child == item &&
        parent_item->prte_tree_last_child == item) {
        parent_item->prte_tree_first_child = item->prte_tree_first_child;
        parent_item->prte_tree_last_child = item->prte_tree_last_child;
    } else {
        /* Otherwise, there are multiple children of this parent.  If
           this item is the first or last child of this parent, then
           ensure that the parent gets a valid first / last child:
           - If I have children, then my first/last child
           - If I have no children, then my immediate sibling */
        if (item->prte_tree_parent->prte_tree_first_child == item) {
            if (item->prte_tree_num_children > 0) {
                parent_item->prte_tree_first_child =
                    item->prte_tree_next_sibling;
            } else {
                parent_item->prte_tree_first_child =
                    prte_tree_get_next_sibling(item);
            }
        } else if (parent_item->prte_tree_last_child == item) {
            if (item->prte_tree_num_children > 0) {
                parent_item->prte_tree_last_child =
                    item->prte_tree_last_child;
            } else {
                parent_item->prte_tree_last_child =
                    prte_tree_get_prev_sibling(item);
            }
        }
    }
    item->prte_tree_parent->prte_tree_num_children--;

    /* remove from sibling pointers */
    if (NULL != item->prte_tree_prev_sibling) {
        item->prte_tree_prev_sibling->prte_tree_next_sibling=
            item->prte_tree_next_sibling;
    }
    if (NULL != item->prte_tree_next_sibling) {
        item->prte_tree_next_sibling->prte_tree_prev_sibling=
            item->prte_tree_prev_sibling;
    }
    item->prte_tree_prev_sibling = NULL;
    item->prte_tree_next_sibling = NULL;

    /* adjust items relating to container */
    item->prte_tree_container->prte_tree_num_items -= count_descendants(item);
    item->prte_tree_container = NULL;

    return(item);
}

int prte_tree_remove_item(prte_tree_t *tree,
                          prte_tree_item_t *item)
{
    prte_tree_item_t *parent_item = NULL, *child = NULL;

    parent_item = (prte_tree_item_t*)item->prte_tree_parent;

    /*
     * Point each of my children to my parent
     */
    for(child  = prte_tree_get_first_child(item);
        child != NULL;
        child  = prte_tree_get_next_sibling(child)) {
        child->prte_tree_parent = parent_item;
        child->prte_tree_num_ancestors--;

        parent_item->prte_tree_num_children++;
    }

    /*
     * My first child points to my 'prev' sibling
     */
    child  = prte_tree_get_first_child(item);
    if( NULL != child ) {
        child->prte_tree_prev_sibling = item->prte_tree_prev_sibling;
    }
    if( NULL != item->prte_tree_prev_sibling ) {
        (item->prte_tree_prev_sibling)->prte_tree_next_sibling = child;
    }

    child  = prte_tree_get_last_child(item);
    if( NULL != child ) {
        child->prte_tree_next_sibling = item->prte_tree_next_sibling;
    }
    if( NULL != item->prte_tree_next_sibling ) {
        (item->prte_tree_next_sibling)->prte_tree_prev_sibling = child;
    }

    /*
     * Remove me from my parent.  If I was the only child, then make
     * the first child be my first child, and make the last child be
     * my last child.
     */
    if( parent_item->prte_tree_first_child == item &&
        parent_item->prte_tree_last_child == item ) {
        parent_item->prte_tree_first_child = prte_tree_get_first_child(item);
        parent_item->prte_tree_last_child = prte_tree_get_last_child(item);
    } else {
        /* There were multiple children.  If I was the first or last,
           then ensure the parent gets a valid first or last child:
           - If I have children, then my first/last
           - If I have no childen, then my immediate sibling */
        if (parent_item->prte_tree_first_child == item) {
            if (item->prte_tree_num_children > 0) {
                parent_item->prte_tree_first_child =
                    item->prte_tree_first_child;
            } else {
                parent_item->prte_tree_first_child =
                    prte_tree_get_next_sibling(item);
            }
        } else if (parent_item->prte_tree_last_child == item) {
            if (item->prte_tree_num_children > 0) {
                parent_item->prte_tree_last_child =
                    item->prte_tree_last_child;
            } else {
                parent_item->prte_tree_last_child =
                    prte_tree_get_prev_sibling(item);
            }
        }
    }
    parent_item->prte_tree_num_children--;

    return PRTE_SUCCESS;
}

/* delimeter characters that mark items in a serialized stream */
static char *start_lvl = "[";
static char *end_lvl = "]";
static char *end_stream = "E";

/*
 * add item to prte buffer that represents all items of a sub-tree from the
 * item passed in on down.  We exit out of converting tree items once we've
 * done the last child of the tree_item and we are at depth 1.
 */
static int add_tree_item2buf(prte_tree_item_t *tree_item,
                             prte_buffer_t *buf,
                             prte_tree_item_serialize_fn_t fn,
                             int depth
                             )
{
    prte_tree_item_t *first_child;
    int rc;

    do {
        /* add start delim to buffer */
        if (PRTE_SUCCESS !=
            (rc = prte_dss.pack(buf, &start_lvl, 1, PRTE_STRING))){
            return(rc);
        }
        /* add item to prte buffer from class creator */
        fn(tree_item, buf);

        if ((first_child = prte_tree_get_first_child(tree_item))) {
            /* add items for our children */
            if (PRTE_SUCCESS !=
                (rc = add_tree_item2buf(first_child, buf, fn, depth+1))){
                return(rc);
            }
            if (PRTE_SUCCESS !=
                (rc = prte_dss.pack(buf, &end_lvl, 1, PRTE_STRING))){
                return(rc);
            }
        } else {
            /* end item entry */
            if (PRTE_SUCCESS !=
                (rc = prte_dss.pack(buf, &end_lvl, 1, PRTE_STRING))){
                return(rc);
            }
        }

        /* advance to next sibling, if none we'll drop out of
         * loop and return to our parent
         */
        tree_item = prte_tree_get_next_sibling(tree_item);
    } while (tree_item && 1 < depth);

    return(PRTE_SUCCESS);
}

/*
 * serialize tree data
 */
int prte_tree_serialize(prte_tree_item_t *start_item, prte_buffer_t *buffer)
{
    int rc;

    if (PRTE_SUCCESS !=
        (rc = add_tree_item2buf(start_item, buffer,
                                start_item->prte_tree_container->serialize,
                                1))){
        return(rc);
    }
    if (PRTE_SUCCESS !=
        (rc = prte_dss.pack(buffer, &end_stream, 1, PRTE_STRING))){
        return(rc);
    }
    return(PRTE_SUCCESS);
}

static int deserialize_add_tree_item(prte_buffer_t *data,
                                     prte_tree_item_t *parent_item,
                                     prte_tree_item_deserialize_fn_t deserialize,
                                     char **curr_delim,
                                     int depth)
{
    int idx = 1, rc;
    prte_tree_item_t *new_item = NULL;
    int level = 0; /* 0 - one up 1 - curr, 2 - one down */

    if (!*curr_delim) {
        if (PRTE_SUCCESS !=
            (rc = prte_dss.unpack(data, curr_delim, &idx, PRTE_STRING))) {
            return(rc);
        }
    }
    while(*curr_delim[0] != end_stream[0]) {
        if (*curr_delim[0] == start_lvl[0]) {
            level++;
        } else {
            level--;
        }

        switch (level) {
        case 0:
            if (1 < depth) {
                /* done with this level go up one level */
                return(PRTE_SUCCESS);
            }
            break;
        case 1:
            /* add found child at this level */
            deserialize(data, &new_item);
            prte_tree_add_child(parent_item, new_item);
            break;
        case 2:
            /* need to add child one level down */
            deserialize_add_tree_item(data, new_item, deserialize, curr_delim,
                                      depth+1);
            level--;
            break;
        }
        if (PRTE_SUCCESS !=
            (rc = prte_dss.unpack(data, curr_delim, &idx, PRTE_STRING))) {
            return(rc);
        }
    }
    return(PRTE_SUCCESS);
}

/*
 * deserialize tree data
 */
int prte_tree_deserialize(prte_buffer_t *serialized_data,
                          prte_tree_item_t *start_item)
{
    char * null = NULL;
    deserialize_add_tree_item(serialized_data,
                              start_item,
                              start_item->prte_tree_container->deserialize,
                              &null,
                              1);
    return PRTE_SUCCESS;
}

void * prte_tree_get_key(prte_tree_t *tree, prte_tree_item_t *item)
{
    return tree->get_key(item);
}

int prte_tree_dup(prte_tree_t *from, prte_tree_t *to)
{
    int ret;
    prte_buffer_t *buffer = NULL;

    prte_tree_init(to,
                   from->comp,
                   from->serialize,
                   from->deserialize,
                   from->get_key);

    buffer = PRTE_NEW(prte_buffer_t);

    prte_tree_serialize(prte_tree_get_root(from), buffer);
    ret = prte_tree_deserialize(buffer, prte_tree_get_root(to));

    PRTE_RELEASE(buffer);
    return ret;
}

int prte_tree_copy_subtree(prte_tree_t *from_tree, prte_tree_item_t *from_item,
                           prte_tree_t *to_tree,   prte_tree_item_t *to_parent)
{
    int ret;
    prte_buffer_t *buffer = NULL;

    buffer = PRTE_NEW(prte_buffer_t);

    prte_tree_serialize(from_item, buffer);
    ret = prte_tree_deserialize(buffer, to_parent);

    PRTE_RELEASE(buffer);
    return ret;
}

prte_tree_item_t *prte_tree_dup_item(prte_tree_t *base, prte_tree_item_t *from)
{
    prte_buffer_t *buffer = NULL;
    prte_tree_item_t *new_item = NULL;

    buffer = PRTE_NEW(prte_buffer_t);

    prte_tree_serialize(from, buffer);

    new_item = PRTE_NEW(prte_tree_item_t);
    prte_tree_deserialize(buffer, new_item);

    PRTE_RELEASE(buffer);
    return new_item;
}

int prte_tree_num_children(prte_tree_item_t *parent)
{
    prte_tree_item_t *child = NULL;
    int i = 0;

    for(child  = prte_tree_get_first_child(parent);
        child != NULL;
        child  = prte_tree_get_next_sibling(child) ) {
        ++i;
    }

    return i;
}

static int prte_tree_compare_subtrees(prte_tree_t *tree_left, prte_tree_t *tree_right,
                                      prte_tree_item_t *left, prte_tree_item_t *right)
{
    int ret;
    prte_tree_item_t *left_child = NULL, *right_child = NULL;

    /* Basecase */
    if( NULL == left && NULL == right ) {
        return 0; /* Match */
    }

    /* Compare: Depth */
    if( NULL == left && NULL != right ) {
        return -1;
    }
    else if( NULL != left && NULL == right ) {
        return 1;
    }

    /* Compare: Keys */
    if( 0 != tree_left->comp(right, prte_tree_get_key(tree_left, left)) ) {
        return -2;
    }

    /* Compare: Number of children */
    if( prte_tree_num_children(left) != prte_tree_num_children(right) ) {
        return 2;
    }

    /* Recursively compare all children */
    for(left_child  = prte_tree_get_first_child(left),        right_child  = prte_tree_get_first_child(right);
        left_child != NULL &&                                 right_child != NULL;
        left_child  = prte_tree_get_next_sibling(left_child), right_child  = prte_tree_get_next_sibling(right_child) ) {
        /* On first difference, return the result */
        if( 0 != (ret = prte_tree_compare_subtrees(tree_left, tree_right, left_child, right_child)) ) {
            return ret;
        }
    }

    return 0;
}

int prte_tree_compare(prte_tree_t *left, prte_tree_t *right)
{
    return prte_tree_compare_subtrees(left, right, prte_tree_get_root(left), prte_tree_get_root(right));
}

/*
 * search myself, descendants and siblings for item matching key
 */
static prte_tree_item_t *find_in_descendants(prte_tree_item_t* item, void *key)
{
    prte_tree_item_t *result = NULL, *first_child;

    while (!result && item) {
        /* check for item match */
        result = (item->prte_tree_container->comp(item, key) == 0) ?
            item : NULL;
        if (!result && (first_child = prte_tree_get_first_child(item))) {
            /* search descendants for match */
            result = find_in_descendants(first_child, key);
        }
        if (!result) {
            /* didn't find match at our node or descending so check sibling */
            item = prte_tree_get_next_sibling(item);
        }
    }
    return(result);
}

/*
 * return next tree item that matches key
 */
prte_tree_item_t *prte_tree_find_with(prte_tree_item_t *item, void *key)
{
    prte_tree_item_t *curr_item = item, *result = NULL;

    if (!prte_tree_is_empty(item->prte_tree_container)) {
        /* check my descendant for a match */
        result = find_in_descendants(prte_tree_get_first_child(item), key);

        if (!result) {
            /* check my siblings for match */
            if (NULL != (curr_item = prte_tree_get_next_sibling(curr_item))) {
                result = find_in_descendants(curr_item, key);
            }
        }

        /* check my ancestors (uncles) for match */
        curr_item = item;
        while (!result && curr_item && curr_item->prte_tree_num_ancestors > 0){
            curr_item = prte_tree_get_next_sibling(item->prte_tree_parent);
            while (NULL == curr_item &&
                   item->prte_tree_parent->prte_tree_num_ancestors > 0) {
                item = item->prte_tree_parent;
                curr_item = prte_tree_get_next_sibling(item->prte_tree_parent);
            }
            if (curr_item) {
                /* search ancestors descendants for match */
                result = find_in_descendants(curr_item, key);
            }
        }
    }

    return(result);
}
