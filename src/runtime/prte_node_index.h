/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 *
 * An index of nodes by every name they answer to.
 *
 * Finding a node by name means walking a list or the node pool and asking
 * prte_node_match() or prte_nptr_match() of each entry. That is the right
 * answer for one lookup and the wrong one for a lookup per line of input: a
 * hostfile, or a list of hosts brought into the DVM, looks every entry up
 * against the entries before it, and the cost grows with the square of the
 * input. This index answers the same two questions from a hash table, so a
 * pass over N entries costs N lookups rather than N walks.
 *
 * It gives the answers the walks give, including which node comes back when
 * more than one matches: each node is added with its position - its order
 * on the list being walked, or its index in the node pool - and a lookup
 * returns the matching node with the lowest position, which is the one the
 * walk would have reached first.
 *
 * The index borrows its nodes; it takes no reference on them. It describes
 * the names a node had when it was added, so a caller that gives a node
 * another name adds it (prte_node_index_add_alias), and one that takes a
 * node off its list removes it (prte_node_index_remove) before the node can
 * be released or renamed. It is built for one pass and destructed after it;
 * nothing keeps a global index in step with the pool.
 */

#ifndef PRTE_RUNTIME_NODE_INDEX_H
#define PRTE_RUNTIME_NODE_INDEX_H

#include "prte_config.h"

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

typedef struct {
    pmix_hash_table_t table; /* name -> the nodes answering to it */
    int next;                /* position the next node from a list takes */
} prte_node_index_t;

PRTE_EXPORT void prte_node_index_construct(prte_node_index_t *idx);
PRTE_EXPORT void prte_node_index_destruct(prte_node_index_t *idx);

/* Add a node at a position - its index in the node pool, or its place on a
 * list. A node with no name is not added: neither match answers to one. */
PRTE_EXPORT int prte_node_index_add(prte_node_index_t *idx, prte_node_t *node, int pos);

/* Add every node on a list, at its place on the list, and every node in the
 * pool, at its pool index. */
PRTE_EXPORT int prte_node_index_add_list(prte_node_index_t *idx, pmix_list_t *nodes);
PRTE_EXPORT int prte_node_index_add_pool(prte_node_index_t *idx);

/* Add a node at the next place on the list it is being appended to. */
PRTE_EXPORT int prte_node_index_append(prte_node_index_t *idx, prte_node_t *node);

/* Record another name for a node already in the index. */
PRTE_EXPORT int prte_node_index_add_alias(prte_node_index_t *idx, prte_node_t *node,
                                          const char *alias);

/* Take a node out of the index, under every name it now has. */
PRTE_EXPORT void prte_node_index_remove(prte_node_index_t *idx, prte_node_t *node);

/* The node prte_node_match() would find for this name, or NULL. */
PRTE_EXPORT prte_node_t *prte_node_index_match_name(prte_node_index_t *idx, const char *name);

/* The first node prte_nptr_match() would accept for this one, or NULL. */
PRTE_EXPORT prte_node_t *prte_node_index_match_node(prte_node_index_t *idx, prte_node_t *node);

END_C_DECLS

#endif
