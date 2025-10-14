/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Internal-only rml interface for interacting with radix tree nodes
 */

#ifndef PRTE_RML_RADIX_H_
#define PRTE_RML_RADIX_H_

#include "math.h"
#include "constants.h"

#include "src/rml/rml.h"

#define RADIX_SUBTREE_FOREACH(root, child_rank) \
    for(                                        \
        child_rank = root.rank;                 \
        child_rank < prte_rml_base.n_dmns;      \
        child_rank += root.width                \
    )

#define RADIX_CHILD_FOREACH(root, child)    \
    for(                                    \
        child = radix_child(&root, 0);      \
        PMIX_RANK_INVALID != child.rank;    \
        child = radix_right_sibling(&child) \
    )

#define RADIX_NODE_STATIC_INIT         \
    {                                  \
        .rank = PMIX_RANK_INVALID,     \
        .depth = 0,                    \
        .width = 1,                    \
        .count = 1,                    \
        .base_rank = PMIX_RANK_INVALID \
    }

typedef prte_rml_routed_tree_node_t radix_node_t;

/* Obtain a radix node configured for this rank */
static radix_node_t radix_node(const pmix_rank_t rank);

/**
 * Raise the node's rank up the tree, to represent its parent
 *
 * No change if the node's rank is 0
 */
static void radix_to_parent(radix_node_t* node);

/* As radix_to_parent, but return a new node instead of updating in place */
static radix_node_t radix_parent(const radix_node_t* node);

/**
 * Get the nth child of this node, where subsequent children have a higher rank
 *
 * Returned node's rank may be PMIX_RANK_INVALID, indicating no such child
 */
static radix_node_t radix_child(const radix_node_t* node, pmix_rank_t n);

/**
 * Given the nth child of a node, get the n+1th child
 *
 * If the returned node's rank is PMIX_RANK_INVALID, no such sibling
 */
static radix_node_t radix_right_sibling(const radix_node_t* node);

/**
 * Raise/lower the node to a given depth in the tree.
 *
 * Fails if depth > this node's base_rank's depth
 *  (meaning rank will be PMIX_RANK_INVALID)
 */
static void radix_to_depth(radix_node_t* node, const pmix_rank_t depth);
static radix_node_t radix_at_depth(
    const radix_node_t* node, const pmix_rank_t depth
);

/* Returns node to base_rank after a depth change */
static void radix_to_base(radix_node_t* node);

/**
 * Set to the next node in the tree, via a depth-first, right-first traversal
 *
 * Updates base_rank in addition to the rest
 */
static void radix_to_next(radix_node_t* node);

static bool radix_is_living(radix_node_t* node);

/* As radix_to_next, but continues until a living rank is found */
static void radix_to_next_living(radix_node_t* node);

/* Returns true if root is a direct ancestor of rank */
static bool radix_subtree_contains(
    const radix_node_t* root, const pmix_rank_t rank
);

/**
 * Returns index of node's child subtrees that contains n, or PMIX_RANK_INVALID
 */
static pmix_rank_t radix_subtree_index(
    const radix_node_t* node, const pmix_rank_t n
);

/* As radix_to_next_living, but only allows nodes in the subtree of root */
static void radix_rooted_to_next_living(
    const radix_node_t* root, radix_node_t* node
);
static radix_node_t radix_rooted_get_next_living(
    const radix_node_t* root, const radix_node_t* node
);

/* Debug helper */
__prte_attribute_unused__
static bool radix_node_is_valid(const radix_node_t* node){
    if(node->base >= (pmix_rank_t) prte_rml_base.n_dmns) return false;

    bool rank_invalid = false;
    rank_invalid  = node->rank >= prte_rml_base.n_dmns;
    rank_invalid |= node->rank > node->base;
    rank_invalid |= node->rank >= node->count;
    if(rank_invalid) return false;

    if(node->depth > prte_rml_base.n_dmns) return false;
    if(node->width == 0) return false;
    if(node->count == 0) return false;
    if(node->count/prte_rml_base.radix > prte_rml_base.n_dmns) return false;
    if(node->count < node->width) return false;

    pmix_rank_t layer_offset = node->count - node->width;
    pmix_rank_t exp_rank =
        (node->base - layer_offset)%node->width + layer_offset;
    if(node->rank != exp_rank) return false;

    pmix_rank_t exp_count =
        (node->width*prte_rml_base.radix-1)/(prte_rml_base.radix-1);
    if(node->count != exp_count) return false;

    pmix_rank_t depth = node->depth;
    pmix_rank_t width = node->width;
    while(width >= (pmix_rank_t) prte_rml_base.radix && depth > 0){
        if(width % prte_rml_base.radix != 0) return false;
        width = width / prte_rml_base.radix;
        depth--;
    }
    if(depth != 0 || width != 1) return false;

    return true;
}

/* Helper: update rank based on base_rank and current depth/width/count */
static inline void radix_update_rank(radix_node_t* node){
    node->rank = PMIX_RANK_INVALID;
    if(node->base >= prte_rml_base.n_dmns) return;
    if(node->width == 0 || node->count < node->width) return;

    pmix_rank_t layer_offset = node->count - node->width;
    if(node->base < layer_offset) return;

    node->rank = (node->base - layer_offset)%node->width + layer_offset;
    if(node->rank > node->base) node->rank = PMIX_RANK_INVALID;
}

/* Helper: increment depth and update width/count without changing rank */
static inline void radix_incr_depth(radix_node_t* node){
    if(node->depth > prte_rml_base.n_dmns+1 ||
        node->width/prte_rml_base.radix > prte_rml_base.n_dmns
    ) {
        // Avoid overflows leading to undefined behaviour
        node->depth = PMIX_RANK_INVALID;
        node->width = PMIX_RANK_INVALID;
        node->count = PMIX_RANK_INVALID;
    }
    node->depth++;
    node->width *= prte_rml_base.radix;
    node->count += node->width;
}
/* Helper: decrement depth and update width/count without changing rank */
static inline void radix_decr_depth(radix_node_t* node){
    if(node->depth == 0 || !PMIX_RANK_IS_VALID(node->depth)){
        // Avoid underflows leading to undefined behaviour
        node->depth = PMIX_RANK_INVALID;
        node->width = PMIX_RANK_INVALID;
        node->count = PMIX_RANK_INVALID;
    }
    node->depth--;
    node->count -= node->width;
    node->width /= prte_rml_base.radix;
}

__prte_attribute_unused__
static radix_node_t radix_node(const pmix_rank_t rank){
    radix_node_t node = {.base = rank};
    radix_to_base(&node);
    return node;
}

static void radix_to_parent(radix_node_t* node){
    radix_decr_depth(node);
    radix_update_rank(node);
}
__prte_attribute_unused__
static radix_node_t radix_parent(const radix_node_t* node){
    radix_node_t parent = *node;
    radix_to_parent(&parent);
    parent.base = parent.rank;
    return parent;
}

__prte_attribute_unused__
static radix_node_t radix_child(const radix_node_t* node, pmix_rank_t idx){
    radix_node_t child = *node;
    radix_incr_depth(&child);
    if(idx >= (pmix_rank_t) prte_rml_base.radix) child.rank = PMIX_RANK_INVALID;
    if(node->rank >= prte_rml_base.n_dmns){
        child.base = PMIX_RANK_INVALID;
    } else {
        child.base = node->rank + node->width * (idx+1);
    }
    radix_update_rank(&child);
    return child;
}

__prte_attribute_unused__
static radix_node_t radix_right_sibling(const radix_node_t* node){
    radix_node_t sibling = *node;
    if(node->rank >= prte_rml_base.n_dmns){
        sibling.base = PMIX_RANK_INVALID;
    } else {
        sibling.base = node->rank + node->width/prte_rml_base.radix;
        if(sibling.base >= node->count) sibling.base = PMIX_RANK_INVALID;
    }
    radix_update_rank(&sibling);
    return sibling;
}

static void radix_to_depth(radix_node_t* node, const pmix_rank_t depth){
    double width = pow(prte_rml_base.radix, depth);
    double count = (width*prte_rml_base.radix-1)/(prte_rml_base.radix-1);
    if(count/prte_rml_base.radix > prte_rml_base.n_dmns){
        node->rank  = PMIX_RANK_INVALID;
        node->depth = PMIX_RANK_INVALID;
        node->width = PMIX_RANK_INVALID;
        node->count = PMIX_RANK_INVALID;
        return;
    }

    node->depth = depth;
    node->width = width;
    node->count = count;
    radix_update_rank(node);
}

__prte_attribute_unused__
static radix_node_t radix_at_depth(
    const radix_node_t* node, const pmix_rank_t depth
) {
    radix_node_t ret = *node;
    radix_to_depth(&ret, depth);
    return ret;
}

static void radix_to_base(radix_node_t* node){
    if(node->base >= prte_rml_base.n_dmns){
        node->base = node->rank = PMIX_RANK_INVALID;
        return;
    }

    double radix = prte_rml_base.radix;
    node->depth = log(node->base*(radix-1) + 1) / log(radix);
    node->width = pow(prte_rml_base.radix, node->depth);
    node->count = (node->width*prte_rml_base.radix-1)/(prte_rml_base.radix-1);

    // Floating point logic and integer truncation could mean we're slightly off
    // with our chosen depth
    while(node->count <= node->base) radix_incr_depth(node);
    while(node->count-node->width > node->base) radix_decr_depth(node);
    node->rank = node->base;
}

static void radix_to_next(radix_node_t* node){
    if(node->rank >= prte_rml_base.n_dmns){
        node->rank = PMIX_RANK_INVALID;
    } else if(node->rank+node->width < prte_rml_base.n_dmns){
        // Node has at least one child
        pmix_rank_t child = node->rank + node->width*prte_rml_base.radix;
        while(child > prte_rml_base.n_dmns) child -= node->width;
        radix_incr_depth(node);
        node->rank = child;
    } else {
        // Node has no children, work back up the tree until we find a
        // valid left-sibling node
        do {
            radix_decr_depth(node);
            node->rank -= node->width;
        } while(node->rank < node->count);
        radix_incr_depth(node);
        if(node->rank == 0 || node->rank >= prte_rml_base.n_dmns){
            node->rank = PMIX_RANK_INVALID;
        }
    }
    node->base = node->rank;
}

static bool radix_is_living(radix_node_t* node){
    return node->rank < prte_rml_base.n_dmns &&
        !pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, node->rank);
}

static void radix_to_next_living(radix_node_t* node){
    do {
        if(node->rank >= prte_rml_base.n_dmns){
            node->rank = node->base = PMIX_RANK_INVALID;
            return;
        }
        radix_to_next(node);
    } while(!radix_is_living(node));
}


static bool radix_subtree_contains(
    const radix_node_t* root, const pmix_rank_t rank
) {
    if(rank >= prte_rml_base.n_dmns) return false;
    if(rank < root->rank) return false;
    pmix_rank_t layer_offset = root->count - root->width;
    return root->rank == (rank-layer_offset)%root->width + layer_offset;
}

__prte_attribute_unused__
static pmix_rank_t radix_subtree_index(
    const radix_node_t* node, const pmix_rank_t n
) {
    if(n == node->rank || !radix_subtree_contains(node, n)){
        return PMIX_RANK_INVALID;
    }
    pmix_rank_t child_width = node->width*prte_rml_base.radix;
    // Return simplified from:
    // child_rank = (n - node->count)%child_width + node->count
    // child_index = ( child_rank - node->count ) / node->width
    return ((n - node->count)%child_width) / node->width;
}

static void radix_rooted_to_next_living(
    const radix_node_t* root, radix_node_t* node
) {
    if(root == node){
        radix_node_t tmp = *root;
        radix_rooted_to_next_living(&tmp, node);
        return;
    }
    radix_to_next_living(node);
    if(!radix_subtree_contains(root, node->rank)){
        node->rank = node->base = PMIX_RANK_INVALID;
    }
}

__prte_attribute_unused__
static radix_node_t radix_rooted_get_next_living(
    const radix_node_t* root, const radix_node_t* node
) {
    radix_node_t ret = *node;
    radix_rooted_to_next_living(root, &ret);
    return ret;
}

#endif
