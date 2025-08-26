/*
 * Copyright (c) 2007-2011 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <stddef.h>

#include "src/class/pmix_bitmap.h"
#include "src/util/pmix_output.h"

#include "src/rml/radix.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/filem/filem.h"


static void resize_ranks(pmix_data_array_t* arr, size_t size){
    if(size == arr->size) return;
    pmix_data_array_t old_arr = *arr;

    PMIx_Data_array_init(arr, PMIX_PROC_RANK);
    PMIx_Data_array_construct(arr, size, PMIX_PROC_RANK);

    size_t min_size = arr->size < old_arr.size ? arr->size : old_arr.size;
    for(size_t i = 0; i < min_size; i++){
        // Copy as much old data as fits
        ((pmix_rank_t*)arr->array)[i] = ((pmix_rank_t*)old_arr.array)[i];
    }
    for(size_t i = min_size; i < arr->size; i++){
        // Fill any new data with invalids
        ((pmix_rank_t*)arr->array)[i] = PMIX_RANK_INVALID;
    }

    PMIx_Data_array_destruct(&old_arr);
}

// Shrink array to minimum size while maintaining valid entries at the same idx
static void shrink_ranks(pmix_data_array_t* arr){
    size_t size = arr->size;
    for(size_t idx = 1; idx <= arr->size; idx++){
        if(PMIX_RANK_INVALID != ((pmix_rank_t*)arr->array)[arr->size - idx]){
            break;
        }
        size--;
    }
    resize_ranks(arr, size);
}

pmix_rank_t prte_rml_get_route(pmix_rank_t target){
    pmix_rank_t ret;

    if (PRTE_PROC_MY_NAME->rank == target) {
        ret = target;
    } else if(!radix_subtree_contains(&prte_rml_base.cur_node, target)){
        ret = PRTE_PROC_MY_PARENT->rank;
    } else {
        pmix_rank_t idx = radix_subtree_index(&prte_rml_base.cur_node, target);
        if(idx >= prte_rml_base.children.size){
            // this is a failed rank that we can't get any closer to
            ret = PMIX_RANK_INVALID;
        } else {
            ret = ((pmix_rank_t*)prte_rml_base.children.array)[idx];
        }
    }

    PMIX_OUTPUT_VERBOSE((1, prte_rml_base.routed_output,
                         "%s routed_radix_get(%s) --> %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_VPID_PRINT(target),
                         PRTE_VPID_PRINT(ret)));

    return ret;
}

// Update list of ancestors after failures
void prte_rml_update_ancestors(pmix_data_array_t* ancestors_arr){
    pmix_rank_t* ancestors = (pmix_rank_t*) ancestors_arr->array;

    radix_node_t prev_anc = radix_node(0);
    for(size_t i = 1; i < ancestors_arr->size; i++){
        radix_node_t anc = radix_node(ancestors[i]);

        if(PMIX_RANK_INVALID == anc.rank){
            // If the previous ancestor was promoted up past this depth, this
            // ancestor is prev ancestor's next inheritor.
            anc = prev_anc;
            radix_to_next_living(&anc);
        } else if(!radix_is_living(&anc)){
            // Otherwise replace with this ancestor's next inheritor if dead
            radix_to_next_living(&anc);
        }

        if(anc.rank == PRTE_PROC_MY_NAME->rank){
            // I'm next in line, so I've been promoted and have fewer ancestors
            for(size_t j = i; j < ancestors_arr->size; j++){
                ancestors[j] = PMIX_RANK_INVALID;
            }
            break;
        }
        if(anc.rank == ancestors[i]) {
            //No change to this ancestor
            prev_anc = anc;
            continue;
        }

        // Update ancestor to the new rank
        ancestors[i] = anc.rank;
        prev_anc = anc;

        // If this ancestor was promoted up my tree, mark anything along its
        // path as invalid
        for(size_t j = i+1; j < ancestors_arr->size; j++){
            radix_node_t virt_anc = radix_at_depth(&prte_rml_base.cur_node, j);
            if(!radix_subtree_contains(&virt_anc, anc.rank)){
                // This ancestor came from a higher depth's other subtree
                break;
            } else if(ancestors[j] == anc.rank){
                ancestors[j] = PMIX_RANK_INVALID;
                break;
            }
            ancestors[j] = PMIX_RANK_INVALID;
        }
    }

    // Shrink out any now invalid ancestors at the end of the array
    shrink_ranks(ancestors_arr);

    if(ancestors_arr == &prte_rml_base.ancestors){
        // Update lifeline/parent only if we're updating my actual ancestors
        ancestors = (pmix_rank_t*) ancestors_arr->array;
        pmix_rank_t lifeline = ancestors_arr->size > 0 ?
            ancestors[ancestors_arr->size-1] : PMIX_RANK_INVALID;
        PRTE_PROC_MY_PARENT->rank = prte_rml_base.lifeline = lifeline;
    }
}

// See if we need to promote ourselves after changing the ancestry list
static void handle_promotion(void){
    pmix_rank_t depth = prte_rml_base.ancestors.size;
    if(depth == prte_rml_base.cur_node.depth) return;

    // Make sure we can fit up to our max # children
    resize_ranks(&prte_rml_base.children, prte_rml_base.radix);
    pmix_rank_t* children = prte_rml_base.children.array;

    radix_to_depth(&prte_rml_base.cur_node, depth);
    radix_node_t old_subtree = radix_at_depth(&prte_rml_base.cur_node, depth+1);

    size_t idx = 0;
    radix_node_t iter;
    RADIX_CHILD_FOREACH(prte_rml_base.cur_node, iter){
        if(iter.rank == old_subtree.rank){
            radix_node_t me = radix_node(PRTE_PROC_MY_NAME->rank);
            children[idx++] =
                radix_rooted_get_next_living(&old_subtree, &me).rank;
            // No children after my subtree, or I wouldn't have been promoted
            break;
        } else {
            children[idx++] = iter.rank;
        }
    }
    for(; idx < prte_rml_base.children.size; idx++){
        children[idx] = PMIX_RANK_INVALID;
    }
}

// Replace failed children after promotion or failures
static void update_descendants(void){
    pmix_rank_t* children = (pmix_rank_t*)prte_rml_base.children.array;
    size_t size = prte_rml_base.children.size;

    prte_rml_base.n_children = 0;
    for(size_t i = 0; i < size; i++){
        if(PMIX_RANK_INVALID == children[i]) continue;
        if(pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, children[i])){
            radix_node_t child = radix_node(children[i]);
            children[i] = radix_rooted_get_next_living(&child, &child).rank;
            if(PMIX_RANK_INVALID == children[i]) continue;
        }
        prte_rml_base.n_children++;
    }
    shrink_ranks(&prte_rml_base.children);
    return;
}

void prte_rml_repair_routing_tree(pmix_data_array_t* failed_ranks, bool global){
    if(global){
        // Make sure these are given local notice first, but mark as globally
        // failed just before, to avoid redundant failure notices up the tree
        pmix_rank_t* ranks = (pmix_rank_t*) failed_ranks->array;
        for(size_t i = 0; i < failed_ranks->size; i++){
            pmix_bitmap_set_bit(&prte_rml_base.global_failed_dmns, ranks[i]);
        }
        prte_rml_repair_routing_tree(failed_ranks, false);
    }

    prte_rml_recovery_status_t status;
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    if(global) status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;

    resize_ranks(&status.failed_ranks, failed_ranks->size);
    size_t j = 0;
    for(size_t i = 0; i < failed_ranks->size; i++){
        pmix_rank_t r = ((pmix_rank_t*)failed_ranks->array)[i];
        if(!global){
            if(pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, r)){
                // Don't notify twice for the same rank
                continue;
            }
            pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, r);
        }
        ((pmix_rank_t*)status.failed_ranks.array)[j++] = r;
    }
    shrink_ranks(&status.failed_ranks);

    //If no new information, just return
    if(status.failed_ranks.size == 0) return;

    if(!global){
        // Skip this work for global, since it will have already been done
        // in the local update
        prte_rml_update_ancestors(&prte_rml_base.ancestors);
        handle_promotion();
        update_descendants();
    }

    if(status.prev_ancestors.size != prte_rml_base.ancestors.size){
        status.ancestors_changed = true;
    } else {
        for(size_t i = 0; i < status.prev_ancestors.size; i++){
            pmix_rank_t prev = ((pmix_rank_t*)status.prev_ancestors.array)[i];
            pmix_rank_t cur = ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
            if(prev != cur){
                status.ancestors_changed = true;
                break;
            }
        }
    }

    status.parent_changed = status.prev_parent != prte_rml_base.lifeline;

    if(status.prev_children.size != prte_rml_base.children.size){
        status.children_changed = true;
        // A convenience for fault handlers that will be using this status,
        // so they can always safely iterate up to children.size
        if(status.prev_children.size < prte_rml_base.children.size){
            resize_ranks(&status.prev_children, prte_rml_base.children.size);
        }
    } else {
        for(size_t i = 0; i < status.prev_children.size; i++){
            pmix_rank_t prev = ((pmix_rank_t*)status.prev_children.array)[i];
            pmix_rank_t cur = ((pmix_rank_t*)prte_rml_base.children.array)[i];
            if(prev != cur){
                status.children_changed = true;
                break;
            }
        }
    }

    // Notify components
    const prte_rml_recovery_status_t* s = &status;
    prte_rml_fault_handler(s);
    // TODO: Should this fn become the central point responsible for setting
    // failed procs to PRTE_PROC_STATE_COMM_FAILED?
    // TODO: Any reason to add a 'fault_handler' function to the errmgr MCA?
    prte_grpcomm.fault_handler(s);
    prte_filem  .fault_handler(s);
    // TODO: Fault handlers for less operation-oriented components like iof.
    // Should be easier to manage reliable P2P message replay after implementing
    // global fault acknowledgement w/ consistent ordering. Probably something
    // like marking messages w/ an 'epoch' that is just the global fault count
    // so we ignore any old messages that just took longer to arrive.
    // Some details to consider w.r.t ensuring correct ordering, but we can
    // probably just make a generic PRTE_RML_SEND_RELIABLY macro to handle all
    // that. Maybe offload the actual code to the oob? A new MCA seems like too
    // much

    PMIX_DESTRUCT(&status);
}

int prte_rml_route_lost(pmix_rank_t route){
    if(prte_finalizing){
        /* see if it is one of our children - if so, remove it */
        pmix_rank_t* children = (pmix_rank_t*)prte_rml_base.children.array;
        size_t size = prte_rml_base.children.size;

        pmix_rank_t idx = radix_subtree_index(&prte_rml_base.cur_node, route);
        if(idx < size && children[idx] == route){
            PMIX_OUTPUT_VERBOSE((3, prte_rml_base.routed_output,
                                 "%s routed:radix: finalizing, connection to"
                                 " child daemon %s lost",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_VPID_PRINT(route)));
            children[idx] = PMIX_RANK_INVALID;
            prte_rml_base.n_children--;
        }
        return PRTE_SUCCESS;
    }

    /* if we lose the connection to the HNP and we are NOT already in finalize,
     * tell the OOB to abort.
     * NOTE: we cannot call abort from here as the OOB needs to first release a
     * thread-lock - otherwise, we will hang!!
     */
    if(route == PRTE_PROC_MY_HNP->rank){
        PMIX_OUTPUT_VERBOSE((2, prte_rml_base.routed_output,
                             "%s routed:radix: Connection to hnp %s lost",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(PRTE_PROC_MY_HNP)));
        return PRTE_ERR_FATAL;
    }

    pmix_data_array_t failed_ranks = PMIX_DATA_ARRAY_STATIC_INIT;
    resize_ranks(&failed_ranks, 1);
    ((pmix_rank_t*)failed_ranks.array)[0] = route;

    prte_rml_repair_routing_tree(&failed_ranks, /* global = */ false);

    PMIx_Data_array_destruct(&failed_ranks);
    return PRTE_SUCCESS;
}

void prte_rml_compute_routing_tree(void){
    // Save our state prior to any daemon failures
    prte_rml_base.n_dmns = prte_process_info.num_daemons;

    // TODO: Should we save this info when DVM is resized?
    pmix_bitmap_init(&prte_rml_base.failed_dmns, prte_rml_base.n_dmns);
    pmix_bitmap_init(&prte_rml_base.global_failed_dmns, prte_rml_base.n_dmns);

    prte_rml_base.cur_node = radix_node(PRTE_PROC_MY_NAME->rank);

    // Build array of ancestors
    size_t n_ancestors = prte_rml_base.cur_node.depth;
    resize_ranks(&prte_rml_base.ancestors, n_ancestors);
    pmix_rank_t* ancestors = (pmix_rank_t*) prte_rml_base.ancestors.array;
    for(size_t i = 0; i < n_ancestors; i++){
        ancestors[i] = radix_at_depth(&prte_rml_base.cur_node, i).rank;
    }
    prte_rml_base.lifeline =
        n_ancestors == 0 ? PMIX_RANK_INVALID : ancestors[n_ancestors-1];
    PRTE_PROC_MY_PARENT->rank = prte_rml_base.lifeline;

    // Build array of children
    resize_ranks(&prte_rml_base.children, prte_rml_base.radix);
    pmix_rank_t* children = (pmix_rank_t*) prte_rml_base.children.array;
    radix_node_t child;
    int child_index = 0;
    RADIX_CHILD_FOREACH(prte_rml_base.cur_node, child){
        children[child_index++] = child.rank;
    }
    shrink_ranks(&prte_rml_base.children);
    prte_rml_base.n_children = prte_rml_base.children.size;

    // Print verbose output
    if (1 > pmix_output_get_verbosity(prte_rml_base.routed_output)) return;

    pmix_output(
        0, "%s: parent %s num_children %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
        PRTE_VPID_PRINT(PRTE_PROC_MY_PARENT->rank), prte_rml_base.n_children
    );
    prte_job_t* dmns = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    for(size_t i = 0; i < prte_rml_base.children.size; i++){
        pmix_rank_t child_rank = ((pmix_rank_t*) prte_rml_base.children.array)[i];

        prte_proc_t* d =
            (prte_proc_t*) pmix_pointer_array_get_item(dmns->procs, child_rank);
        bool has_name = NULL!=d && NULL!=d->node && NULL!=d->node->name;
        char* node_name = has_name ? d->node->name : "";
        pmix_output(
            0, "%s: \tchild %s%s%s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
            PRTE_VPID_PRINT(child_rank), has_name ? " node " : "", node_name
        );

        if(5 > pmix_output_get_verbosity(prte_rml_base.routed_output)) continue;
        radix_node_t node = radix_node(child_rank);
        pmix_rank_t r;
        RADIX_SUBTREE_FOREACH(node, r) {
            pmix_output(
                0, "%s: \t\trelation %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                PRTE_VPID_PRINT(r)
            );
        }
    }
}

int prte_rml_get_num_contributors(pmix_rank_t *dmns, size_t ndmns){
    pmix_bitmap_t contributors;
    PMIX_CONSTRUCT(&contributors, pmix_bitmap_t);
    pmix_bitmap_init(&contributors, prte_rml_base.children.size);
    for(size_t i = 0; i < ndmns; i++){
        if(pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, dmns[i])){
            continue;
        }
        pmix_rank_t child =
            radix_subtree_index(&prte_rml_base.cur_node, dmns[i]);
        if(PMIX_RANK_INVALID != child){
            pmix_bitmap_set_bit(&contributors, child);
        }
    }
    int n_contributors =
        pmix_bitmap_num_set_bits(&contributors, prte_rml_base.children.size);
    PMIX_DESTRUCT(&contributors);
    return n_contributors;
}
