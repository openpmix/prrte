/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/proc_info.h"

#include "src/mca/ras/base/base.h"

/* Normalize a node name to the short form, storing the FQDN as rawname
 * and as an alias so both forms resolve via prte_quickmatch / prte_nptr_match.
 * No-op when keep_fqdn_hostnames is set or the name is an IP address. */
static void normalize_node(prte_node_t *node)
{
    char *ptr, *fqdn;

    if (prte_keep_fqdn_hostnames || pmix_net_isaddr(node->name)) {
        return;
    }
    ptr = strchr(node->name, '.');
    if (NULL == ptr) {
        return;
    }
    /* copy the full FQDN, then truncate node->name in place to the short name */
    fqdn = strdup(node->name);
    if (NULL == fqdn) {
        /* leave the name whole rather than truncate it with nowhere to keep
         * the full form - a short name with no rawname and no alias is a node
         * the allocation's own spelling can no longer reach */
        return;
    }
    *ptr = '\0';
    if (NULL == node->rawname) {
        node->rawname = fqdn;
    } else {
        PMIx_Argv_append_unique_nosize(&node->aliases, fqdn);
        free(fqdn);
    }
    PMIx_Argv_append_unique_nosize(&node->aliases, node->rawname);
}

/*
 * Add the specified node definitions to the global data store
 *
 * NOTE: this removes all items from the list - but only when it succeeds.
 * An error return leaves whatever it had not reached still on the list, so
 * the caller must PMIX_LIST_DESTRUCT it rather than PMIX_DESTRUCT it.  The
 * node in hand when the error was hit is released here; nothing else can,
 * since it is already off the list and not yet in the pool.
 */
int prte_ras_base_node_insert(pmix_list_t *nodes, prte_job_t *jdata)
{
    pmix_list_item_t *item;
    int32_t num_nodes;
    int rc, i;
    prte_node_t *node, *hnp_node, *nptr;
    bool skiphnp = false, found;
    prte_attribute_t *kv, *kv2;
    prte_proc_t *daemon;
    prte_job_t *djob;
    pmix_status_t prc;

    /* get the number of nodes */
    num_nodes = (int32_t) pmix_list_get_size(nodes);
    if (0 == num_nodes) {
        return PRTE_SUCCESS; /* nothing to do */
    }

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:base:node_insert inserting %ld nodes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (long) num_nodes));

    /* mark the job as being a large-cluster sim if that was requested */
    if (1 < prte_ras_base.multiplier && NULL != jdata) {
        prte_set_attribute(&jdata->attributes, PRTE_JOB_MULTI_DAEMON_SIM,
                           PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
    }

    /* set the size of the global array - this helps minimize time
     * spent doing realloc's
     */
    rc = pmix_pointer_array_set_size(prte_node_pool,
                                     num_nodes * prte_ras_base.multiplier);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* if we are not launching, get the daemon job */
    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* get the hnp node's info */
    hnp_node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);

    if (prte_ras_base.launch_orted_on_hn) {
        if (NULL != hnp_node) {
            PMIX_LIST_FOREACH(node, nodes, prte_node_t)
            {
                if (prte_check_host_is_local(node->name)) {
                    prte_hnp_is_allocated = true;
                    break;
                }
            }
            if (prte_hnp_is_allocated &&
                !(PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping) & PRTE_MAPPING_NO_USE_LOCAL)) {
                if (NULL != hnp_node->name) {
                    free(hnp_node->name);
                }
                hnp_node->name = strdup("prte");
                skiphnp = true;
                PRTE_SET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping, PRTE_MAPPING_NO_USE_LOCAL);
                PRTE_FLAG_SET(hnp_node, PRTE_NODE_NON_USABLE); // leave this node out of mapping operations
            }
        }
    }

    /* cycle through the list */
    while (NULL != (item = pmix_list_remove_first(nodes))) {
        node = (prte_node_t *) item;

        /* the HNP had to already enter its node on the array - that entry is in the
         * first position since it is the first one entered. We need to check to see
         * if this node is the same as the HNP's node so we don't double-enter it
         */
        if (!skiphnp && NULL != hnp_node && prte_check_host_is_local(node->name)) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:node_insert updating HNP [%s] info to %ld slots",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name,
                                 (long) node->slots));

            /* flag that hnp has been allocated */
            prte_hnp_is_allocated = true;
            /* update the total slots in the job */
            prte_ras_base.total_slots_alloc += node->slots;
            /* copy the allocation data to that node's info */
            hnp_node->slots_max = node->slots_max;
            if (prte_get_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS, NULL, PMIX_BOOL)) {
                hnp_node->slots += node->slots;
                if (0 > hnp_node->slots) {
                    hnp_node->slots = 0;
                } else if (hnp_node->slots > hnp_node->slots_max && hnp_node->slots_max > 0) {
                    hnp_node->slots = hnp_node->slots_max;
                }
            } else {
                hnp_node->slots = node->slots;
            }
            /* copy across any attributes. Note that prte_set_attribute()
             * expects a raw C value pointer, NOT a pmix_value_t, so the
             * attribute is duplicated and spliced in directly rather than
             * being round-tripped through set_attribute. */
            PMIX_LIST_FOREACH(kv, &node->attributes, prte_attribute_t)
            {
                /* build the replacement before dropping what it replaces:
                 * removing first and then failing to copy leaves the node
                 * with neither, and these carry the per-node launch settings
                 * (username, port), so losing one silently launches the
                 * daemon differently */
                kv2 = PMIX_NEW(prte_attribute_t);
                kv2->key = kv->key;
                kv2->local = kv->local;
                prc = PMIx_Value_xfer(&kv2->data, &kv->data);
                if (PMIX_SUCCESS != prc) {
                    PMIX_ERROR_LOG(prc);
                    PMIX_RELEASE(kv2);
                    continue;
                }
                prte_remove_attribute(&hnp_node->attributes, kv->key);
                pmix_list_append(&hnp_node->attributes, &kv2->super);
            }
            /* the incoming node carries the authority for its own slot count:
             * whoever supplied it says whether the number is a given (an RM
             * allocation, an explicit "slots=" in a hostfile) or a placeholder
             * to be computed from the node's topology later */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                PRTE_FLAG_SET(hnp_node, PRTE_NODE_FLAG_SLOTS_GIVEN);
            } else {
                PRTE_FLAG_UNSET(hnp_node, PRTE_NODE_FLAG_SLOTS_GIVEN);
            }
            /* if the node name is different, store it as an alias */
            if (0 != strcmp(hnp_node->name, node->name)) {
                PMIx_Argv_append_unique_nosize(&hnp_node->aliases, node->name);
            }
             // transfer across any new aliases
            if (NULL != node->aliases) {
                for (i=0; NULL != node->aliases[i]; i++) {
                    PMIx_Argv_append_unique_nosize(&hnp_node->aliases, node->aliases[i]);
                }
            }
            if (NULL != node->rawname) {
                if (NULL != hnp_node->rawname) {
                    free(hnp_node->rawname);
                }
                hnp_node->rawname = strdup(node->rawname);
            }
            /* don't keep duplicate copy */
            PMIX_RELEASE(node);
            /* create copies, if required */
            for (i = 1; i < prte_ras_base.multiplier; i++) {
                rc = prte_node_copy(&node, hnp_node);
                if (PRTE_SUCCESS != rc) {
                    return rc;
                }
                PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
                node->index = pmix_pointer_array_add(prte_node_pool, node);
            }
        } else {
            /* insert the object into the prte_nodes global array */
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:node_insert node %s slots %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name, node->slots));
            // see if we already have this node
            found = false;
            for (i=0; i < prte_node_pool->size; i++) {
                nptr = (prte_node_t*)pmix_pointer_array_get_item(prte_node_pool, i);
                if (NULL == nptr) {
                    continue;
                }
                if (prte_nptr_match(nptr, node)) {
                    found = true;
                    /* The incoming entry is discarded in favor of the one
                     * already in the pool, so anything the caller marked on
                     * it has to be carried across. A dynamic addition marks
                     * its nodes PRTE_NODE_STATE_ADDED precisely so the DVM
                     * extension will launch a daemon on them; dropping that
                     * for a node the pool already knows about makes the grow
                     * a no-op. That is the normal case when re-growing a node
                     * that was previously shrunk out of the DVM: the shrink
                     * removes its daemon, but the pool entry survives. */
                    if (PRTE_NODE_STATE_ADDED == node->state) {
                        nptr->state = PRTE_NODE_STATE_ADDED;
                    }
                    if (prte_get_attribute(&node->attributes, PRTE_NODE_ADD_SLOTS, NULL, PMIX_BOOL)) {
                        nptr->slots += node->slots;
                        if (0 > nptr->slots) {
                            nptr->slots = 0;
                        } else if (nptr->slots > nptr->slots_max && nptr->slots_max > 0) {
                            nptr->slots = nptr->slots_max;
                        }
                    }
                    break;
                }
            }
            if (found) {
                /* the pool entry is authoritative - drop the duplicate the
                 * caller handed us. Its slots must NOT be added to the
                 * running total: that node is already counted, and adding
                 * it again inflates total_slots_alloc on every re-grow of a
                 * node the pool already knows about. The multiplier copies
                 * likewise belong to the original insertion, not to this
                 * rediscovery of it. */
                PMIX_RELEASE(node);
                continue;
            }
            /* PRTE_NODE_FLAG_SLOTS_GIVEN arrives already set by whoever
             * supplied the node and is carried into the pool untouched */
            /* detect FQDN before normalizing, then normalize to short name */
            if (!pmix_net_isaddr(node->name) && NULL != strchr(node->name, '.')) {
                prte_have_fqdn_allocation = true;
            }
            normalize_node(node);
            /* Insert it into the array.
             *
             * A component may pre-assign the pool slot by setting node->index
             * before handing us the node; ras/bootstrap does exactly that, so
             * a node lands at the slot matching its canonical DVM rank (read
             * from the same config file the daemons read to compute their own
             * rank). Appending to the lowest free slot would reproduce that
             * only by accident of the order the config happens to list nodes
             * in - and would silently overwrite the pre-assignment, making the
             * correspondence unenforced. A slot that is already taken means a
             * malformed config; fall back to an append rather than clobber
             * another node. */
            if (0 <= node->index &&
                NULL == pmix_pointer_array_get_item(prte_node_pool, node->index)) {
                rc = pmix_pointer_array_set_item(prte_node_pool, node->index,
                                                 (void *) node);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    /* it is off the caller's list and not in the pool, so
                     * this is the only reference left to it */
                    PMIX_RELEASE(node);
                    return rc;
                }
            } else {
                node->index = pmix_pointer_array_add(prte_node_pool, (void *) node);
                if (PRTE_SUCCESS > (rc = node->index)) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_RELEASE(node);
                    return rc;
                }
            }
            if (NULL != djob &&
                prte_get_attribute(&djob->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) &&
                NULL == node->daemon) {
                /* create a daemon for this node since we won't be launching
                 * and the mapper needs to see a daemon - this is used solely
                 * for testing the mappers */
                daemon = PMIX_NEW(prte_proc_t);
                PMIX_LOAD_PROCID(&daemon->name, PRTE_PROC_MY_NAME->nspace, node->index);
                daemon->state = PRTE_PROC_STATE_RUNNING;
                /* the node backpointer is borrowed, not retained */
                daemon->node = node;
                pmix_pointer_array_set_item(djob->procs, daemon->name.rank, daemon);
                djob->num_procs++;
                PMIX_RETAIN(daemon);
                node->daemon = daemon;
            }
            /* update the total slots in the job */
            prte_ras_base.total_slots_alloc += node->slots;
            /* duplicate the node if requested */
            for (i = 1; i < prte_ras_base.multiplier; i++) {
                rc = prte_node_copy(&nptr, node);
                if (PRTE_SUCCESS != rc) {
                    return rc;
                }
                nptr->index = pmix_pointer_array_add(prte_node_pool, nptr);
            }
        }
    }

    return PRTE_SUCCESS;
}
