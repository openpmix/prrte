/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_node_index.h"
#include "src/util/proc_info.h"

/* A name, and every node that answers to it - as its name or as an alias,
 * which prte_node_match() tells apart - with the position each was added
 * at. Entries keep the order they were added in; a lookup still takes the
 * lowest position, so a node added out of order is found correctly too. */
typedef struct {
    prte_node_t *node;
    int pos;
    bool alias;
} prte_node_index_entry_t;

typedef struct {
    size_t n;
    size_t size;
    prte_node_index_entry_t *e;
} prte_node_index_bucket_t;

void prte_node_index_construct(prte_node_index_t *idx)
{
    PMIX_CONSTRUCT(&idx->table, pmix_hash_table_t);
    pmix_hash_table_init(&idx->table, 1024);
    idx->next = 0;
}

void prte_node_index_destruct(prte_node_index_t *idx)
{
    void *key, *value, *in_node, *out_node;
    size_t keysize;
    int rc;

    rc = pmix_hash_table_get_first_key_ptr(&idx->table, &key, &keysize, &value, &in_node);
    while (PMIX_SUCCESS == rc) {
        prte_node_index_bucket_t *b = (prte_node_index_bucket_t *) value;
        free(b->e);
        free(b);
        rc = pmix_hash_table_get_next_key_ptr(&idx->table, &key, &keysize, &value, in_node,
                                              &out_node);
        in_node = out_node;
    }
    PMIX_DESTRUCT(&idx->table);
}

static prte_node_index_bucket_t *get_bucket(prte_node_index_t *idx, const char *name)
{
    void *value = NULL;

    if (PMIX_SUCCESS != pmix_hash_table_get_value_ptr(&idx->table, name, strlen(name), &value)) {
        return NULL;
    }
    return (prte_node_index_bucket_t *) value;
}

static int add_key(prte_node_index_t *idx, const char *name, prte_node_t *node, int pos,
                   bool alias)
{
    prte_node_index_bucket_t *b;
    prte_node_index_entry_t *grown;

    b = get_bucket(idx, name);
    if (NULL == b) {
        b = (prte_node_index_bucket_t *) calloc(1, sizeof(*b));
        if (NULL == b) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        if (PMIX_SUCCESS != pmix_hash_table_set_value_ptr(&idx->table, name, strlen(name), b)) {
            free(b);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (b->n == b->size) {
        /* nearly every name belongs to one node */
        grown = (prte_node_index_entry_t *) realloc(b->e, (b->size + 1) * sizeof(*grown));
        if (NULL == grown) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        b->e = grown;
        b->size++;
    }
    b->e[b->n].node = node;
    b->e[b->n].pos = pos;
    b->e[b->n].alias = alias;
    b->n++;
    return PRTE_SUCCESS;
}

int prte_node_index_add(prte_node_index_t *idx, prte_node_t *node, int pos)
{
    int i, rc;

    /* neither walk answers for a node with no name, aliases or not */
    if (NULL == node || NULL == node->name) {
        return PRTE_SUCCESS;
    }
    rc = add_key(idx, node->name, node, pos, false);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    for (i = 0; NULL != node->aliases && NULL != node->aliases[i]; i++) {
        rc = add_key(idx, node->aliases[i], node, pos, true);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    if (idx->next <= pos) {
        idx->next = pos + 1;
    }
    return PRTE_SUCCESS;
}

int prte_node_index_add_list(prte_node_index_t *idx, pmix_list_t *nodes)
{
    prte_node_t *node;
    int rc;

    PMIX_LIST_FOREACH(node, nodes, prte_node_t) {
        rc = prte_node_index_add(idx, node, idx->next++);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    return PRTE_SUCCESS;
}

int prte_node_index_add_pool(prte_node_index_t *idx)
{
    prte_node_t *node;
    int i, rc;

    if (NULL == prte_node_pool) {
        return PRTE_SUCCESS;
    }
    for (i = 0; i < prte_node_pool->size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
        if (NULL == node) {
            continue;
        }
        rc = prte_node_index_add(idx, node, i);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    return PRTE_SUCCESS;
}

int prte_node_index_append(prte_node_index_t *idx, prte_node_t *node)
{
    return prte_node_index_add(idx, node, idx->next++);
}

int prte_node_index_add_alias(prte_node_index_t *idx, prte_node_t *node, const char *alias)
{
    prte_node_index_bucket_t *b;
    size_t k;

    if (NULL == node->name || NULL == alias) {
        return PRTE_SUCCESS;
    }
    /* the node keeps the position it was added at */
    b = get_bucket(idx, node->name);
    for (k = 0; NULL != b && k < b->n; k++) {
        if (b->e[k].node == node && !b->e[k].alias) {
            return add_key(idx, alias, node, b->e[k].pos, true);
        }
    }
    /* not in the index - nothing to add it under */
    return PRTE_SUCCESS;
}

static void remove_key(prte_node_index_t *idx, const char *name, prte_node_t *node)
{
    prte_node_index_bucket_t *b = get_bucket(idx, name);
    size_t k, j;

    if (NULL == b) {
        return;
    }
    for (k = 0, j = 0; k < b->n; k++) {
        if (b->e[k].node != node) {
            b->e[j++] = b->e[k];
        }
    }
    b->n = j;
}

void prte_node_index_remove(prte_node_index_t *idx, prte_node_t *node)
{
    int i;

    if (NULL == node || NULL == node->name) {
        return;
    }
    remove_key(idx, node->name, node);
    for (i = 0; NULL != node->aliases && NULL != node->aliases[i]; i++) {
        remove_key(idx, node->aliases[i], node);
    }
}

/* The lowest-placed entry under this name, optionally only those of one
 * kind (want_alias: -1 either, 0 names only, 1 aliases only), improving on
 * the best found so far. */
static void best_of(prte_node_index_t *idx, const char *name, int want_alias,
                    prte_node_t **best, int *bestpos)
{
    prte_node_index_bucket_t *b;
    size_t k;

    if (NULL == name || NULL == (b = get_bucket(idx, name))) {
        return;
    }
    for (k = 0; k < b->n; k++) {
        if (0 <= want_alias && b->e[k].alias != (1 == want_alias)) {
            continue;
        }
        if (NULL == *best || b->e[k].pos < *bestpos) {
            *best = b->e[k].node;
            *bestpos = b->e[k].pos;
        }
    }
}

prte_node_t *prte_node_index_match_name(prte_node_index_t *idx, const char *name)
{
    prte_node_t *best = NULL;
    int bestpos = 0;
    const char *nm;

    if (NULL == name) {
        return NULL;
    }
    /* as prte_node_match(): a name that refers to this host is compared
     * with the node names as our own nodename, while the aliases are
     * compared with the name as given */
    nm = prte_check_host_is_local(name) ? prte_process_info.nodename : name;
    best_of(idx, nm, 0, &best, &bestpos);
    best_of(idx, name, 1, &best, &bestpos);
    return best;
}

prte_node_t *prte_node_index_match_node(prte_node_index_t *idx, prte_node_t *node)
{
    prte_node_t *best = NULL;
    int bestpos = 0, i;

    if (NULL == node || NULL == node->name) {
        return NULL;
    }
    /* as prte_nptr_match(): two nodes match when any name or alias of one
     * is any name or alias of the other */
    best_of(idx, node->name, -1, &best, &bestpos);
    for (i = 0; NULL != node->aliases && NULL != node->aliases[i]; i++) {
        best_of(idx, node->aliases[i], -1, &best, &bestpos);
    }
    return best;
}
