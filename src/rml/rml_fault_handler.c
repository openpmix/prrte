/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
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

    /* Note which of these we did not already know about, BEFORE the repair
     * records them - see the errmgr hand-off below. */
    pmix_rank_t* incoming = (pmix_rank_t*) failed_ranks.array;
    size_t n_incoming = failed_ranks.size;
    bool* newly = NULL;
    if (0 < n_incoming) {
        newly = (bool*) calloc(n_incoming, sizeof(bool));
    }
    if (NULL != newly) {
        for (size_t i = 0; i < n_incoming; i++) {
            newly[i] = (PMIX_RANK_INVALID != incoming[i]) &&
                       !pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, incoming[i]);
        }
    }

    prte_rml_repair_routing_tree(&failed_ranks, global);

    /* Hand the departure to the errmgr.
     *
     * PRTE_PROC_STATE_COMM_FAILED is otherwise raised only by
     * prte_mca_oob_tcp_component_lost_connection, i.e. only on the daemon that
     * was holding the socket.  At the default radix that is always the HNP -
     * a ten-node DVM at radix 64 is flat, so the HNP is every daemon's parent -
     * and the DVM therefore looked correct.  Give the routing tree any depth
     * and it stops being true: an interior daemon detects the loss, the notice
     * walks up and correctly marks the rank failed everywhere including the
     * HNP, and the HNP - never having lost a socket - never runs the errmgr.
     * The procs that were on the dead node are never marked terminated, so the
     * job never completes and its tool waits forever.  A `prun` against a
     * radix-2 DVM whose job's node was killed hung indefinitely, where the same
     * DVM at radix 64 released it at once.
     *
     * Only ranks this daemon had not already recorded are reported, so the
     * daemon that detected the loss itself (and already raised COMM_FAILED from
     * the OOB) does not raise it twice when the HNP's global broadcast comes
     * back around, and a duplicate notice is a no-op.  The guard mirrors the
     * OOB's: while finalizing there is nobody left to tell. */
    if (NULL != newly) {
        if (!prte_finalizing) {
            for (size_t i = 0; i < n_incoming; i++) {
                pmix_proc_t dmn;
                if (!newly[i]) {
                    continue;
                }
                PMIX_LOAD_PROCID(&dmn, PRTE_PROC_MY_NAME->nspace, incoming[i]);
                PRTE_ACTIVATE_PROC_STATE(&dmn, PRTE_PROC_STATE_COMM_FAILED);
            }
        }
        free(newly);
    }

    /* repair takes a copy of what it needs, so the unpacked array is ours */
    PMIx_Data_array_destruct(&failed_ranks);
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

    // Update the reported list with any faults we know of. Note the reported
    // list is deliberately NOT padded out to our own length first:
    // update_ancestors fills an empty slot with "the previous ancestor's next
    // inheritor", which is meaningful for a hole in the middle of a real list
    // and nonsense for a slot invented past its end. A legitimately shorter
    // report is what the tail loop below is for.
    prte_rml_update_ancestors(&report);

    // If we match after updating their list, there's no new info for us
    bool different = report.size != prte_rml_base.ancestors.size;
    for(size_t i = 0; !different && i < report.size; i++){
        different = ((pmix_rank_t*)report.array)[i] !=
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }
    if(!different){
        PMIx_Data_array_destruct(&report);
        return;
    }

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

        // grow only when the next slot is past the end - "<=" also fired while
        // the array still had room and shrank it back under the entries we had
        // already recorded
        if(infer_i >= inferred.size) resize_ranks(&inferred, (infer_i+1)*1.5);
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
        PMIX_DATA_BUFFER_RELEASE(msg);
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
        PMIX_DESTRUCT(&ones);
        PMIX_DESTRUCT(&local_only);
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
        prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON_DIED, msg);
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

/* Bootstrap trigger: a daemon that has just come up announces itself to its
 * parent (one hop up its lifeline), NOT to the HNP. Its parent is precisely the
 * daemon that knows whether this rank was absent -- the global death broadcast
 * marked it everywhere -- so the parent can tell a genuine return from a first
 * boot and only escalate the former. Announcing to the parent instead of the
 * root keeps a returning daemon off the root's back: on a first boot every
 * parent simply drops the notice, so the root sees nothing (beyond its own few
 * direct children) and no daemon opens a socket to the root. The notice rides
 * the existing lifeline link, so it costs no new connection. */
void prte_rml_send_return_notice(void){
    pmix_data_buffer_t* msg = PMIx_Data_buffer_create();
    pmix_rank_t rank = PRTE_PROC_MY_NAME->rank;
    int ret = PMIx_Data_pack(NULL, msg, &rank, 1, PMIX_PROC_RANK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    /* announce our boot epoch so the HNP can confirm this is a strictly-newer
     * incarnation and propagate it in the revival for the stale-message guard */
    ret = PMIx_Data_pack(NULL, msg, &prte_rml_boot_epoch, 1, PMIX_UINT64);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    PRTE_RML_SEND(ret, PRTE_PROC_MY_PARENT->rank, msg,
                  PRTE_RML_TAG_DAEMON_RETURNED);
    if(PRTE_SUCCESS != ret){
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
    }
}

/* Handle a return announcement. A daemon sends this one hop to its parent; the
 * parent that finds the rank absent escalates one relayed message to the HNP,
 * and the HNP -- the single arbiter -- broadcasts the revival. A daemon that
 * does not have the rank marked absent (a first boot, a duplicate, or one it
 * already revived) drops the notice, so the whole exchange is idempotent and,
 * on the common first-boot path, never reaches the root at all. */
void prte_rml_recv_return_request(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    int cnt = 1;
    pmix_rank_t rank;
    int ret = PMIx_Data_unpack(NULL, buf, &rank, &cnt, PMIX_PROC_RANK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }
    cnt = 1;
    uint64_t epoch = 0;
    ret = PMIx_Data_unpack(NULL, buf, &epoch, &cnt, PMIX_UINT64);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* Idempotent filter: if this rank is not absent in our view, there is
     * nothing to revive -- drop it here rather than burden anyone upstream. */
    if(!pmix_bitmap_is_set_bit(&prte_rml_base.absent_dmns, rank)){
        return;
    }

    /* Not the arbiter: pass the notice one step toward the HNP. The message is
     * addressed to the HNP, so intermediate hops relay it without processing;
     * only the master's handler runs. This is O(1) per real return. */
    if(!PRTE_PROC_IS_MASTER){
        pmix_data_buffer_t* up = PMIx_Data_buffer_create();
        ret = PMIx_Data_pack(NULL, up, &rank, 1, PMIX_PROC_RANK);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(up);
            return;
        }
        ret = PMIx_Data_pack(NULL, up, &epoch, 1, PMIX_UINT64);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(up);
            return;
        }
        PRTE_RML_SEND(ret, PRTE_PROC_MY_HNP->rank, up,
                      PRTE_RML_TAG_DAEMON_RETURNED);
        if(PRTE_SUCCESS != ret){
            PRTE_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_RELEASE(up);
        }
        return;
    }

    /* Arbiter: accept the return only if it announces a strictly-newer
     * incarnation than the one we last recorded for this rank. A stale or
     * duplicate epoch (including the degenerate same-timestamp reboot) is
     * dropped, forcing the daemon to retry with a later epoch. Record the new
     * epoch as authoritative before broadcasting so the guard is armed. */
    if(0 != epoch && epoch <= prte_rml_get_epoch(rank)){
        return;
    }
    prte_rml_record_epoch(rank, epoch);

    PMIX_OUTPUT_VERBOSE((1, prte_rml_base.routed_output,
                         "%s routed:radix: daemon %s returned; broadcasting"
                         " revival", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_VPID_PRINT(rank)));

    /* Broadcast the revival. We do NOT revive our own tree here: the master
     * relays its own xcast back through the normal receive path, so we converge
     * via prte_rml_recv_revival_notice like everyone else -- and, crucially, on
     * the xcast's forward-first path. If we are the returned rank's parent we
     * are currently holding its orphaned children; reviving early would drop
     * them from our tree before the xcast forwards to them, stranding them.
     * Forward-first delivery reshapes our tree only after those children have
     * been handed the notice. The xcast travels the current tree, which routes
     * around the absent rank, so it need not (and will not) reach the returned
     * daemon itself -- that daemon already computed a healthy tree. */
    pmix_data_buffer_t* msg = PMIx_Data_buffer_create();
    ret = PMIx_Data_pack(NULL, msg, &rank, 1, PMIX_PROC_RANK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    ret = PMIx_Data_pack(NULL, msg, &epoch, 1, PMIX_UINT64);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(msg);
        return;
    }
    prte_grpcomm_xcast(PRTE_RML_TAG_DAEMON_REVIVED, msg);
    PMIX_DATA_BUFFER_RELEASE(msg);
}

/* All daemons: converge on a broadcast revival by re-inserting the returned
 * rank into the local routing tree. Idempotent via prte_rml_revive_routing_tree
 * (a no-op if the rank is not marked absent here). */
void prte_rml_recv_revival_notice(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    int cnt = 1;
    pmix_rank_t rank;
    int ret = PMIx_Data_unpack(NULL, buf, &rank, &cnt, PMIX_PROC_RANK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }
    cnt = 1;
    uint64_t epoch = 0;
    ret = PMIx_Data_unpack(NULL, buf, &epoch, &cnt, PMIX_UINT64);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* Record the returned incarnation's epoch as authoritative before rewiring,
     * so any late traffic from the old incarnation is dropped from here on. */
    prte_rml_record_epoch(rank, epoch);
    prte_rml_revive_routing_tree(rank);
}
