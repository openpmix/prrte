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
#include "src/mca/state/state.h"

#include "src/rml/radix.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"


//Yoink some helpers from routed_radix.c
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
static void shrink_ranks(pmix_data_array_t* arr){
    size_t size = arr->size;
    for(size_t idx = 1; idx <= size; idx++){
        if(PMIX_RANK_INVALID != ((pmix_rank_t*)arr->array)[arr->size - idx]){
            break;
        }
        size--;
    }
    resize_ranks(arr, size);
}

void prte_rml_recv_failures_notice(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,sender,tag,cbdata);

    int cnt = 1;

    bool global;
    int ret = PMIx_Data_unpack(NULL, buf, &global, &cnt, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    pmix_data_array_t failed_ranks = PMIX_DATA_ARRAY_STATIC_INIT;
    ret = PMIx_Data_unpack(NULL, buf, &failed_ranks, &cnt, PMIX_DATA_ARRAY);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_array_destruct(&failed_ranks);
        return;
    }

    prte_rml_repair_routing_tree(&failed_ranks, global);
}

void prte_rml_recv_adoption_notice(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,sender,tag,cbdata);

    int cnt = 1;
    pmix_data_array_t report = PMIX_DATA_ARRAY_STATIC_INIT;
    int ret = PMIx_Data_unpack(NULL, buf, &report, &cnt, PMIX_DATA_ARRAY);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_array_destruct(&report);
        return;
    }

    // We can't just assume the rank sending us an adoption notice is actually
    // our new parent. They might have less information than us, or this message
    // may just be old. Instead, we use their reported ancestry list to infer
    // any relevant faults that must have happened and use that information to
    // update our state.

    // Update the reported list with any faults we know of
    if(report.size < prte_rml_base.ancestors.size){
        resize_ranks(&report, prte_rml_base.ancestors.size);
    }
    prte_rml_update_ancestors(&report);

    // If we match after updating their list, there's no new info for us
    bool different = report.size != prte_rml_base.ancestors.size;
    for(size_t i = 0; !different && i < report.size; i++){
        different = ((pmix_rank_t*)report.array)[i] !=
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }
    if(!different) return;

    if(report.size > prte_rml_base.ancestors.size){
        // This should never happen -- it implies there is some extra failure
        // that could lead to our depth increasing, which is an invariate
        // violation. Depth should only ever decrease.
        PRTE_ERROR_LOG( PRTE_ERR_UNRECOVERABLE );
        PMIX_OUTPUT_VERBOSE((
            0, prte_rml_base.routed_output,
            "%s routed:radix: incompatible routing tree state from %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)
        ));
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_array_destruct(&report);
        return;
    }

    // Operate on a copy of our ancestors
    pmix_data_array_t ancestors = PMIX_DATA_ARRAY_STATIC_INIT;
    resize_ranks(&ancestors, prte_rml_base.ancestors.size);
    for(size_t i = 0; i < ancestors.size; i++){
        ((pmix_rank_t*)ancestors.array)[i] =
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }

    // Build an array of inferred faults
    pmix_data_array_t inferred = PMIX_DATA_ARRAY_STATIC_INIT;
    resize_ranks(&inferred, 1);
    size_t infer_i = 0;

    for(size_t i = 0; i < report.size && i < ancestors.size; i++){
        pmix_rank_t ancestor = ((pmix_rank_t*)ancestors.array)[i];
        if(ancestor == ((pmix_rank_t*)report.array)[i]) continue;

        if(infer_i >= inferred.size) resize_ranks(&inferred, (infer_i+1)*1.5);
        ((pmix_rank_t*)inferred.array)[infer_i++] = ancestor;

        pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, ancestor);
        prte_rml_update_ancestors(&ancestors);

        i--;
    }
    while(ancestors.size > report.size){
        pmix_rank_t ancestor = ((pmix_rank_t*)ancestors.array)[report.size];

        if(infer_i <= inferred.size) resize_ranks(&inferred, (infer_i+1)*1.5);
        ((pmix_rank_t*)inferred.array)[infer_i++] = ancestor;

        pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, ancestor);
        prte_rml_update_ancestors(&ancestors);
    }

    // Undo setting the failed bit for inferred failures, so we can do the full
    // error handling process for them.
    shrink_ranks(&inferred);
    for(size_t i = 0; i < inferred.size; i++){
        pmix_bitmap_clear_bit(
            &prte_rml_base.failed_dmns, ((pmix_rank_t*)inferred.array)[i]
        );
    }

    // If the arrays are still different, one/both of us are in an invalid state
    different = report.size != ancestors.size;
    for(size_t i = 0; !different && i < report.size; i++){
        different = ((pmix_rank_t*)report.array)[i] !=
            ((pmix_rank_t*)ancestors.array)[i];
    }
    PMIx_Data_array_destruct(&report);
    PMIx_Data_array_destruct(&ancestors);
    if(different){
        PRTE_ERROR_LOG( PRTE_ERR_UNRECOVERABLE );
        PMIX_OUTPUT_VERBOSE((
            0, prte_rml_base.routed_output,
            "%s routed:radix: incompatible routing tree state from %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)
        ));
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_array_destruct(&inferred);
        return;
    }

    // Finally, do a full repair on the inferred faults
    if(inferred.size > 0){
        prte_rml_repair_routing_tree(&inferred, /* global = */ false);
    }
    PMIx_Data_array_destruct(&inferred);
}

static void send_adoption_notices(const prte_rml_recovery_status_t* status){
    if(!status->children_changed && !status->promoted) return;

    // Build array of (my view of) their new ancestors
    pmix_data_array_t arr = PMIX_DATA_ARRAY_STATIC_INIT;
    resize_ranks(&arr, prte_rml_base.ancestors.size+1);
    for(size_t i = 0; i < prte_rml_base.ancestors.size; i++){
        ((pmix_rank_t*)arr.array)[i] =
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }
    ((pmix_rank_t*)arr.array)[arr.size-1] = PRTE_PROC_MY_NAME->rank;

    // Pack into a buffer
    pmix_data_buffer_t* base_msg = PMIx_Data_buffer_create();
    int ret = PMIx_Data_pack(NULL, base_msg, &arr, 1, PMIX_DATA_ARRAY);
    PMIx_Data_array_destruct(&arr);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(base_msg);
        return;
    }

    pmix_rank_t* prev_children = (pmix_rank_t*)status->prev_children.array;
    pmix_rank_t* children = (pmix_rank_t*)prte_rml_base.children.array;
    for(size_t i = 0; i < prte_rml_base.children.size; i++){
        if(PMIX_RANK_INVALID == children[i]) continue;
        if(!status->promoted && prev_children[i] == children[i]) continue;

        pmix_data_buffer_t* msg = PMIx_Data_buffer_create();
        ret = PMIx_Data_copy_payload(msg, base_msg);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(msg);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            break;
        }

        PRTE_RML_SEND(ret, children[i], msg, PRTE_RML_TAG_DAEMON_ADOPTED);
        if(PRTE_SUCCESS != ret){
            PRTE_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(msg);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            break;
        }
    }

    PMIX_DATA_BUFFER_RELEASE(base_msg);
}

static void send_failures_notice(const prte_rml_recovery_status_t* status){
    pmix_data_buffer_t* msg = PMIx_Data_buffer_create();

    bool global = PRTE_PROC_IS_MASTER;
    int ret = PMIx_Data_pack(NULL, msg, &global, 1, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    // Build array of failures to pass up to my parent
    pmix_data_array_t arr = PMIX_DATA_ARRAY_STATIC_INIT;
    if(status->parent_changed){
        // TODO: Include current ancestor list, to ensure new parent understands
        // that they are my new parent.

        // New parent might not be aware of old failures, report all non-global
        // failures in my subtree
        pmix_bitmap_t local_only;
        PMIX_CONSTRUCT(&local_only, pmix_bitmap_t);
        pmix_bitmap_copy(&local_only, &prte_rml_base.failed_dmns);
        pmix_bitmap_bitwise_xor_inplace(
            &local_only, &prte_rml_base.global_failed_dmns
        );

        // Turns out there's no invert or find_and_clear...
        pmix_bitmap_t ones;
        PMIX_CONSTRUCT(&ones, pmix_bitmap_t);
        pmix_bitmap_init(&ones, pmix_bitmap_size(&local_only));
        pmix_bitmap_set_all_bits(&ones);
        pmix_bitmap_bitwise_xor_inplace(&local_only, &ones);

        size_t size =
            pmix_bitmap_num_unset_bits(&local_only, prte_rml_base.n_dmns);
        resize_ranks(&arr, size);
        size_t idx = 0;
        for(size_t i = 0; i < size; i++){
            int int_rank;
            pmix_bitmap_find_and_set_first_unset_bit(&local_only, &int_rank);
            pmix_rank_t rank = (pmix_rank_t) int_rank;

            if(radix_subtree_contains(&prte_rml_base.cur_node, rank)){
                ((pmix_rank_t*)arr.array)[idx++] = rank;
            }
        }
    } else {
        // Parent is unchanged, just report current failures in my subtree
        resize_ranks(&arr, status->failed_ranks.size);
        size_t idx = 0;
        for(size_t i = 0; i < arr.size; i++){
            pmix_rank_t rank = ((pmix_rank_t*)status->failed_ranks.array)[i];

            if(radix_subtree_contains(&prte_rml_base.cur_node, rank)){
                ((pmix_rank_t*)arr.array)[idx++] = rank;
            }
        }
    }
    shrink_ranks(&arr);

    ret = PMIx_Data_pack(NULL, msg, &arr, 1, PMIX_DATA_ARRAY);
    PMIx_Data_array_destruct(&arr);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }

    // HNP broadcasts new failure information down
    if(PRTE_PROC_IS_MASTER){
        prte_grpcomm.xcast(PRTE_RML_TAG_DAEMON_DIED, msg);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }

    // All others send new failure information up a level
    PRTE_RML_SEND(ret, prte_rml_base.lifeline, msg, PRTE_RML_TAG_DAEMON_DIED);
    if(PRTE_SUCCESS != ret){
        PRTE_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
}

void prte_rml_fault_handler(const prte_rml_recovery_status_t* status){
    // RML does all handling during the local scope callback
    if(PRTE_RML_FAULT_SCOPE_GLOBAL == status->scope) return;

    for(size_t i = 0; i < status->failed_ranks.size; i++){
        pmix_rank_t rank = ((pmix_rank_t*)status->failed_ranks.array)[i];
        pmix_proc_t proc;
        PMIX_LOAD_PROCID(&proc, PRTE_PROC_MY_NAME->nspace, rank);
        PRTE_RML_PURGE(&proc);
    }

    send_adoption_notices(status);
    send_failures_notice(status);
}
