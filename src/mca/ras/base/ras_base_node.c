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
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/dss/dss.h"
#include "src/util/argv.h"
#include "src/util/if.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ras/base/ras_private.h"

/*
 * Add the specified node definitions to the global data store
 * NOTE: this removes all items from the list!
 */
int prrte_ras_base_node_insert(prrte_list_t* nodes, prrte_job_t *jdata)
{
    prrte_list_item_t* item;
    prrte_std_cntr_t num_nodes;
    int rc, i;
    prrte_node_t *node, *hnp_node, *nptr;
    char *ptr;
    bool hnp_alone = true, skiphnp = false;
    prrte_attribute_t *kv;
    char **alias=NULL, **nalias;
    prrte_proc_t *daemon;
    prrte_job_t *djob;

    /* get the number of nodes */
    num_nodes = (prrte_std_cntr_t)prrte_list_get_size(nodes);
    if (0 == num_nodes) {
        return PRRTE_SUCCESS;  /* nothing to do */
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                         "%s ras:base:node_insert inserting %ld nodes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (long)num_nodes));

    /* mark the job as being a large-cluster sim if that was requested */
    if (1 < prrte_ras_base.multiplier) {
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_MULTI_DAEMON_SIM,
                           PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);
    }

    /* set the size of the global array - this helps minimize time
     * spent doing realloc's
     */
    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_size(prrte_node_pool, num_nodes * prrte_ras_base.multiplier))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* if we are not launching, get the daemon job */
    djob = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);

    /* get the hnp node's info */
    hnp_node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, 0);

    if (prrte_ras_base.launch_orted_on_hn) {
        if (NULL != hnp_node) {
            PRRTE_LIST_FOREACH(node, nodes, prrte_node_t) {
                if (prrte_check_host_is_local(node->name)) {
                    prrte_hnp_is_allocated = true;
                    break;
                }
            }
            if (prrte_hnp_is_allocated && !(PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping) &
                PRRTE_MAPPING_NO_USE_LOCAL)) {
                if (NULL != hnp_node->name) {
                    free(hnp_node->name);
                }
                hnp_node->name = strdup("prte");
                skiphnp = true;
                PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_NO_USE_LOCAL);
            }
        }
    }

    /* cycle through the list */
    while (NULL != (item = prrte_list_remove_first(nodes))) {
        node = (prrte_node_t*)item;

        /* the HNP had to already enter its node on the array - that entry is in the
         * first position since it is the first one entered. We need to check to see
         * if this node is the same as the HNP's node so we don't double-enter it
         */
        if (!skiphnp && NULL != hnp_node && prrte_check_host_is_local(node->name)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                                 "%s ras:base:node_insert updating HNP [%s] info to %ld slots",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 node->name,
                                 (long)node->slots));

            /* flag that hnp has been allocated */
            prrte_hnp_is_allocated = true;
            /* update the total slots in the job */
            prrte_ras_base.total_slots_alloc += node->slots;
            /* copy the allocation data to that node's info */
            hnp_node->slots = node->slots;
            hnp_node->slots_max = node->slots_max;
            /* copy across any attributes */
            PRRTE_LIST_FOREACH(kv, &node->attributes, prrte_attribute_t) {
                prrte_set_attribute(&node->attributes, kv->key, PRRTE_ATTR_LOCAL, &kv->data, kv->type);
            }
            if (prrte_managed_allocation || PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
                /* the slots are always treated as sacred
                 * in managed allocations
                 */
                PRRTE_FLAG_SET(hnp_node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
            } else {
                PRRTE_FLAG_UNSET(hnp_node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
            }
            /* use the local name for our node - don't trust what
             * we got from an RM. If requested, store the resolved
             * nodename info
             */
            if (prrte_show_resolved_nodenames) {
                /* if the node name is different, store it as an alias */
                if (0 != strcmp(node->name, hnp_node->name)) {
                    /* get any current list of aliases */
                    ptr = NULL;
                    prrte_get_attribute(&hnp_node->attributes, PRRTE_NODE_ALIAS, (void**)&ptr, PRRTE_STRING);
                    if (NULL != ptr) {
                        alias = prrte_argv_split(ptr, ',');
                        free(ptr);
                    }
                    /* add to list of aliases for this node - only add if unique */
                    prrte_argv_append_unique_nosize(&alias, node->name, false);
                }
                if (prrte_get_attribute(&node->attributes, PRRTE_NODE_ALIAS, (void**)&ptr, PRRTE_STRING)) {
                    nalias = prrte_argv_split(ptr, ',');
                    /* now copy over any aliases that are unique */
                    for (i=0; NULL != nalias[i]; i++) {
                        prrte_argv_append_unique_nosize(&alias, nalias[i], false);
                    }
                    prrte_argv_free(nalias);
                }
                /* and store the result */
                if (0 < prrte_argv_count(alias)) {
                    ptr = prrte_argv_join(alias, ',');
                    prrte_set_attribute(&hnp_node->attributes, PRRTE_NODE_ALIAS, PRRTE_ATTR_LOCAL, ptr, PRRTE_STRING);
                    free(ptr);
                }
                prrte_argv_free(alias);
            }
            /* don't keep duplicate copy */
            PRRTE_RELEASE(node);
            /* create copies, if required */
            for (i=1; i < prrte_ras_base.multiplier; i++) {
                prrte_dss.copy((void**)&node, hnp_node, PRRTE_NODE);
                PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_DAEMON_LAUNCHED);
                node->index = prrte_pointer_array_add(prrte_node_pool, node);
            }
        } else {
            /* insert the object onto the prrte_nodes global array */
            PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                                 "%s ras:base:node_insert node %s slots %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 (NULL == node->name) ? "NULL" : node->name,
                                 node->slots));
            if (prrte_managed_allocation) {
                /* the slots are always treated as sacred
                 * in managed allocations
                 */
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
            }
            /* insert it into the array */
            node->index = prrte_pointer_array_add(prrte_node_pool, (void*)node);
            if (PRRTE_SUCCESS > (rc = node->index)) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            if (prrte_do_not_launch) {
                /* create a daemon for this node since we won't be launching
                 * and the mapper needs to see a daemon - this is used solely
                 * for testing the mappers */
                daemon = PRRTE_NEW(prrte_proc_t);
                daemon->name.jobid = PRRTE_PROC_MY_NAME->jobid;
                daemon->name.vpid = node->index;
                daemon->state = PRRTE_PROC_STATE_RUNNING;
                PRRTE_RETAIN(node);
                daemon->node = node;
                prrte_pointer_array_set_item(djob->procs, daemon->name.vpid, daemon);
                djob->num_procs++;
                PRRTE_RETAIN(daemon);
                node->daemon = daemon;
            }
            /* update the total slots in the job */
            prrte_ras_base.total_slots_alloc += node->slots;
            /* check if we have fqdn names in the allocation */
            if (NULL != strchr(node->name, '.')) {
                prrte_have_fqdn_allocation = true;
            }
            /* indicate the HNP is not alone */
            hnp_alone = false;
            for (i=1; i < prrte_ras_base.multiplier; i++) {
                prrte_dss.copy((void**)&nptr, node, PRRTE_NODE);
                nptr->index = prrte_pointer_array_add(prrte_node_pool, nptr);
            }
       }
    }

    /* if we didn't find any fqdn names in the allocation, then
     * ensure we don't have any domain info in the node record
     * for the hnp
     */
    if (NULL != hnp_node && !prrte_have_fqdn_allocation && !hnp_alone) {
        if (NULL != (ptr = strchr(hnp_node->name, '.'))) {
            *ptr = '\0';
        }
    }

    return PRRTE_SUCCESS;
}
