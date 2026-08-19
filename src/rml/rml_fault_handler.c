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

#include "src/grpcomm/grpcomm.h"
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

/* Do two rank arrays hold the same sequence? */
static bool ranks_differ(const pmix_data_array_t* a, const pmix_data_array_t* b)
{
    if (a->size != b->size) {
        return true;
    }
    for (size_t i = 0; i < a->size; i++) {
        if (((pmix_rank_t*)a->array)[i] != ((pmix_rank_t*)b->array)[i]) {
            return true;
        }
    }
    return false;
}

/* Record one inferred ancestor death, marking the rank failed so that the next
 * pass of prte_rml_update_ancestors walks past it. Returns false, and sets
 * *why, when this rank must not be inferred dead at all:
 *
 *  - An out-of-range rank. PMIX_RANK_INVALID is the value an ancestor slot
 *    holds when the walk found no living inheritor at all, and setting a bit
 *    at that index would ask the bitmap to grow to UINT32_MAX bits.
 *
 *  - The root. Rank 0 is every daemon's first ancestor and has no inheritor to
 *    be routed around, which is why update_ancestors starts its walk at index
 *    1; losing the HNP is fatal by construction, not something to recover from.
 *    A well-formed report can never ask for it -- every ancestor list begins
 *    with 0, so index 0 never differs -- but a malformed one that stripped down
 *    to nothing would fall into the tail loop and, because update_ancestors
 *    never revisits index 0, hypothesise the root's death over and over: the
 *    unit test for this hangs rather than failing.
 *
 *  - A rank we already believe dead. Nothing can be *inferred* about one --
 *    update_ancestors would have walked past it before we ever compared -- so
 *    seeing it here means the walk is not converging. This is what bounds the
 *    walk: every accepted inference marks a living rank dead, so there can be
 *    at most one per daemon.
 *
 *  - A rank that has RETURNED. A report is a snapshot of the sender's view at
 *    the moment it sent, and a revival travels as its own xcast, so a notice
 *    that crossed with one carries a lineage predating the return. Burying a
 *    daemon that is alive and talking to us is much worse than converging a
 *    beat later on a death somebody actually observed, so the whole
 *    reconciliation is abandoned rather than the one rank skipped.
 */
static bool record_inference(pmix_data_array_t* inferred, size_t* n,
                             pmix_rank_t ancestor, prte_rml_ancestry_t* why)
{
    if (ancestor >= prte_rml_base.n_dmns || 0 == ancestor) {
        *why = PRTE_RML_ANCESTRY_INCONSISTENT;
        return false;
    }
    if (pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, ancestor)) {
        *why = PRTE_RML_ANCESTRY_INCONSISTENT;
        return false;
    }
    if (pmix_bitmap_is_set_bit(&prte_rml_base.revived_dmns, ancestor)) {
        *why = PRTE_RML_ANCESTRY_STALE;
        return false;
    }

    // grow only when the next slot is past the end - "<=" also fired while
    // the array still had room and shrank it back under the entries we had
    // already recorded
    if (*n >= inferred->size) {
        resize_ranks(inferred, (*n+1)*3/2);
    }
    ((pmix_rank_t*)inferred->array)[(*n)++] = ancestor;
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, ancestor);
    return true;
}

prte_rml_ancestry_t prte_rml_reconcile_ancestry(pmix_data_array_t* report,
                                                pmix_data_array_t* inferred)
{
    // Apply what WE already know to the peer's list first: a difference our own
    // failure knowledge accounts for is not new information.
    //
    // Note what is deliberately NOT done here: the report is not first padded
    // out to our own length. update_ancestors fills an empty slot with "the
    // previous ancestor's next inheritor", which is meaningful for a hole in
    // the middle of a real list and nonsense for a slot invented past its end -
    // padding a report of [0] (all the HNP sends when it adopts a grandchild
    // directly) produced [0,2] at radix 2, rank 2 being the root's OTHER child
    // and no ancestor of ours at all. That reconciled against nothing and took
    // the DVM down with a FORCED_EXIT, in exactly the race the notice exists to
    // settle. A legitimately shorter report is what the tail loop below is for.
    prte_rml_update_ancestors(report);

    if (!ranks_differ(report, &prte_rml_base.ancestors)) {
        return PRTE_RML_ANCESTRY_AGREED;
    }

    if (report->size > prte_rml_base.ancestors.size) {
        // The peer places us deeper than we place ourselves, and our own
        // failure knowledge did not account for it. An ancestor list only ever
        // shortens through failures, so no set of deaths reconciles this.
        return PRTE_RML_ANCESTRY_INCONSISTENT;
    }

    // Operate on a copy of our ancestors, walking it toward the report one
    // inferred death at a time. record_inference is what bounds the walk: it
    // refuses a rank that is already failed, so every pass that is allowed to
    // proceed marks one more living rank dead.
    pmix_data_array_t ancestors = PMIX_DATA_ARRAY_STATIC_INIT;
    resize_ranks(&ancestors, prte_rml_base.ancestors.size);
    for (size_t i = 0; i < ancestors.size; i++) {
        ((pmix_rank_t*)ancestors.array)[i] =
            ((pmix_rank_t*)prte_rml_base.ancestors.array)[i];
    }

    resize_ranks(inferred, 1);
    size_t infer_i = 0;
    prte_rml_ancestry_t verdict = PRTE_RML_ANCESTRY_INFERRED;

    for (size_t i = 0; i < report->size && i < ancestors.size; i++) {
        pmix_rank_t ancestor = ((pmix_rank_t*)ancestors.array)[i];
        if (ancestor == ((pmix_rank_t*)report->array)[i]) {
            continue;
        }

        if (!record_inference(inferred, &infer_i, ancestor, &verdict)) {
            break;
        }
        prte_rml_update_ancestors(&ancestors);

        i--;
    }
    while (PRTE_RML_ANCESTRY_INFERRED == verdict &&
           ancestors.size > report->size) {
        pmix_rank_t ancestor = ((pmix_rank_t*)ancestors.array)[report->size];

        if (!record_inference(inferred, &infer_i, ancestor, &verdict)) {
            break;
        }
        prte_rml_update_ancestors(&ancestors);
    }

    // Undo setting the failed bit for inferred failures, so the caller can do
    // the full error handling process for them.
    shrink_ranks(inferred);
    for (size_t i = 0; i < inferred->size; i++) {
        pmix_bitmap_clear_bit(
            &prte_rml_base.failed_dmns, ((pmix_rank_t*)inferred->array)[i]
        );
    }

    // If the arrays are still different, one/both of us are in an invalid state
    if (PRTE_RML_ANCESTRY_INFERRED == verdict &&
        ranks_differ(report, &ancestors)) {
        verdict = PRTE_RML_ANCESTRY_INCONSISTENT;
    }
    PMIx_Data_array_destruct(&ancestors);

    if (PRTE_RML_ANCESTRY_INFERRED != verdict) {
        // Nothing may be acted on, so hand back no inferences at all
        resize_ranks(inferred, 0);
    } else if (0 == inferred->size) {
        // The lists differ but no death explains it
        verdict = PRTE_RML_ANCESTRY_INCONSISTENT;
    }
    return verdict;
}

/* Hand a set of departed daemon ranks to the errmgr, skipping any this daemon
 * had already recorded.
 *
 * PRTE_PROC_STATE_COMM_FAILED is what makes a daemon act on a departure rather
 * than merely route around it - on the HNP it is what sweeps the dead node's
 * procs to TERM_WO_SYNC so their job can complete.  It is otherwise raised only
 * by prte_mca_oob_tcp_component_lost_connection, i.e. only on the daemon that
 * was holding the socket.  At the default radix that is always the HNP - a
 * ten-node DVM at radix 64 is flat, so the HNP is every daemon's parent - and
 * the DVM therefore looked correct.  Give the routing tree any depth and it
 * stops being true: an interior daemon detects the loss, the notice walks up
 * and correctly marks the rank failed everywhere including the HNP, and the HNP
 * - never having lost a socket - never runs the errmgr.  The procs that were on
 * the dead node are never marked terminated, so the job never completes and its
 * tool waits forever.  A `prun` against a radix-2 DVM whose job's node was
 * killed hung indefinitely, where the same DVM at radix 64 released it at once.
 *
 * Every path in this file that learns of a departure reports it here, whether
 * it was told (a failure notice) or deduced it (a peer's lineage that only an
 * unrecorded death explains).  Those must not diverge: the routing tree's
 * failed_dmns set is the sole record of what we already know, so a path that
 * repairs the tree without reporting does not merely stay quiet - it makes
 * every later notice for that rank look like a duplicate and suppresses the
 * report for good.
 *
 * Which is also why this MUST be called before the repair that records the
 * ranks, and why it can be: the state activation is thread-shifted, so the
 * errmgr cannot run ahead of the repair no matter what order the two calls
 * appear in, whereas the failed_dmns test is only meaningful while the repair
 * has yet to set the bits.
 *
 * For an inferred death that also depends on prte_rml_reconcile_ancestry
 * having undone the marks it sets while walking - it does, deliberately, "so
 * the caller can do the full error handling process", and
 * test_reconcile_ancestry pins it.  A bit left behind there would not merely
 * skip one report; it would make the rank look known to every path forever.
 */
static void report_new_departures(const pmix_data_array_t* failed)
{
    /* mirrors the OOB's guard: while finalizing there is nobody left to tell */
    if (prte_finalizing) {
        return;
    }
    pmix_rank_t* ranks = (pmix_rank_t*) failed->array;
    for (size_t i = 0; i < failed->size; i++) {
        pmix_proc_t dmn;
        /* PMIX_RANK_INVALID is the padding an ancestor walk leaves behind, not
         * a departure */
        if (PMIX_RANK_INVALID == ranks[i]) {
            continue;
        }
        if (pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns, ranks[i])) {
            continue;
        }
        PMIX_LOAD_PROCID(&dmn, PRTE_PROC_MY_NAME->nspace, ranks[i]);
        PRTE_ACTIVATE_PROC_STATE(&dmn, PRTE_PROC_STATE_COMM_FAILED);
    }
}

/* A child's report of its own lineage, riding its failure notice (see
 * send_failures_notice). Its last entry is the parent it sent to, which is what
 * makes it checkable: strip that entry and what remains is the child's view of
 * OUR ancestor list, directly comparable with our own.
 *
 * Consumes nothing and repairs only on a clean inference. Where the downward
 * (adoption) path treats an irreconcilable report as fatal, this one only logs:
 * that report is how a daemon learns its own lineage, whereas this one is an
 * accelerant. Everything it can tell us also arrives by a route that does not
 * depend on a peer's honesty - our own lost socket, or the HNP's arbitrated
 * broadcast - so a report we cannot place is a race to drop, not a reason to
 * end the DVM.
 */
static void reconcile_child_ancestry(pmix_data_array_t* report,
                                     pmix_proc_t* sender)
{
    /* A repair here would emit adoption and failure notices to daemons that are
     * already on their way out, where they can be misread as faults - the same
     * reason prte_rml_route_lost stops short of one while a teardown is under
     * way. */
    if (prte_finalizing || prte_prteds_term_ordered ||
        prte_abnormal_term_ordered || prte_dvm_leaving) {
        return;
    }

    if (0 == report->size) {
        /* only the HNP has an empty ancestor list, and the HNP never sends
         * this leg of the message */
        return;
    }

    pmix_rank_t* ranks = (pmix_rank_t*) report->array;
    if (ranks[report->size-1] != PRTE_PROC_MY_NAME->rank) {
        /* Stale: the sender has re-homed since it sent, or we have. Its list
         * names some other daemon as its parent, so it says nothing about OUR
         * lineage and must not be reconciled against it. */
        PMIX_OUTPUT_VERBOSE((
            1, prte_rml_base.routed_output,
            "%s routed:radix: failure notice from %s reports parent %s, not us"
            " - ignoring its ancestry",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender),
            PRTE_VPID_PRINT(ranks[report->size-1])
        ));
        return;
    }
    resize_ranks(report, report->size-1);

    pmix_data_array_t inferred = PMIX_DATA_ARRAY_STATIC_INIT;
    prte_rml_ancestry_t verdict = prte_rml_reconcile_ancestry(report, &inferred);

    switch (verdict) {
    case PRTE_RML_ANCESTRY_AGREED:
        /* The ordinary outcome, and worth a trace of its own: it is the only
         * evidence that the lineage travelled and was checked at all, which is
         * the half of this a test can pin down. Whether it ever carries news
         * depends on who wins a race with a socket error. */
        PMIX_OUTPUT_VERBOSE((
            2, prte_rml_base.routed_output,
            "%s routed:radix: lineage reported by %s agrees with ours",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)
        ));
        break;

    case PRTE_RML_ANCESTRY_INFERRED:
        PMIX_OUTPUT_VERBOSE((
            1, prte_rml_base.routed_output,
            "%s routed:radix: lineage reported by %s implies %lu ancestor"
            " death(s) we had not recorded", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
            PRTE_NAME_PRINT(sender), (unsigned long) inferred.size
        ));
        report_new_departures(&inferred);
        prte_rml_repair_routing_tree(&inferred, /* global = */ false, /* epoch = */ 0);
        break;

    case PRTE_RML_ANCESTRY_STALE:
    case PRTE_RML_ANCESTRY_INCONSISTENT:
        PMIX_OUTPUT_VERBOSE((
            1, prte_rml_base.routed_output,
            "%s routed:radix: lineage reported by %s cannot be reconciled"
            " - ignoring it", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
            PRTE_NAME_PRINT(sender)
        ));
        break;
    }
    PMIx_Data_array_destruct(&inferred);
}

void prte_rml_recv_failures_notice(
    int status, pmix_proc_t* sender, pmix_data_buffer_t* buf,
    prte_rml_tag_t tag, void* cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);

    int cnt = 1;

    bool global;
    int ret = PMIx_Data_unpack(NULL, buf, &global, &cnt, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    /* A global notice carries the collective recovery epoch it moves the DVM
     * to - see the epoch member of prte_rml_recovery_status_t.  The upward leg
     * does not: only the HNP issues an epoch, and only its broadcast applies
     * one. */
    uint32_t epoch = 0;
    if(global){
        cnt = 1;
        ret = PMIx_Data_unpack(NULL, buf, &epoch, &cnt, PMIX_UINT32);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            return;
        }
    }

    cnt = 1;
    pmix_data_array_t failed_ranks = PMIX_DATA_ARRAY_STATIC_INIT;
    ret = PMIx_Data_unpack(NULL, buf, &failed_ranks, &cnt, PMIX_DATA_ARRAY);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_array_destruct(&failed_ranks);
        return;
    }

    /* Hand the departure to the errmgr before the repair records it - only
     * ranks we had not already recorded are reported, so the daemon that
     * detected the loss itself (and already raised COMM_FAILED from the OOB)
     * does not raise it twice when the HNP's global broadcast comes back
     * around, and a duplicate notice is a no-op. */
    report_new_departures(&failed_ranks);

    prte_rml_repair_routing_tree(&failed_ranks, global, epoch);

    /* repair takes a copy of what it needs, so the unpacked array is ours */
    PMIx_Data_array_destruct(&failed_ranks);

    /* The sender's own lineage rides the upward leg only. Reconcile it AFTER
     * the repair above: the ranks it just reported may be exactly what explains
     * the difference between our two views, in which case there is nothing left
     * to infer. */
    if (!global) {
        pmix_data_array_t report = PMIX_DATA_ARRAY_STATIC_INIT;
        cnt = 1;
        ret = PMIx_Data_unpack(NULL, buf, &report, &cnt, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != ret) {
            /* Advisory payload: losing it costs us the early convergence, not
             * the DVM, and the failures above have already been applied. */
            PMIX_ERROR_LOG(ret);
        } else {
            reconcile_child_ancestry(&report, sender);
        }
        PMIx_Data_array_destruct(&report);
    }
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
    pmix_data_array_t inferred = PMIX_DATA_ARRAY_STATIC_INIT;
    prte_rml_ancestry_t verdict = prte_rml_reconcile_ancestry(&report, &inferred);
    PMIx_Data_array_destruct(&report);

    switch (verdict) {
    case PRTE_RML_ANCESTRY_AGREED:
        break;

    case PRTE_RML_ANCESTRY_INFERRED:
        // Finally, report the inferred deaths and do a full repair on them
        report_new_departures(&inferred);
        prte_rml_repair_routing_tree(&inferred, /* global = */ false, /* epoch = */ 0);
        break;

    case PRTE_RML_ANCESTRY_STALE:
        // Our would-be parent's view predates a return we have already applied.
        // It gets the correction the same way we did, from the revival xcast.
        PMIX_OUTPUT_VERBOSE((
            1, prte_rml_base.routed_output,
            "%s routed:radix: ignoring pre-revival adoption notice from %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)
        ));
        break;

    case PRTE_RML_ANCESTRY_INCONSISTENT:
        // This one is fatal in this direction and not in the other: an adoption
        // notice is how we learn our own lineage, so a report we cannot place
        // means we no longer know where we are in the tree.
        PRTE_ERROR_LOG( PRTE_ERR_UNRECOVERABLE );
        PMIX_OUTPUT_VERBOSE((
            0, prte_rml_base.routed_output,
            "%s routed:radix: incompatible routing tree state from %s",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)
        ));
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        break;
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

    /* Only the broadcast carries an epoch, and issuing it here - once per
     * notice actually emitted - is what makes the value the same everywhere
     * and strictly increasing.  Every daemon adopts it rather than counting
     * the notices it has received, so a daemon that missed one (a daemon
     * launched into a DVM that has already recovered has missed all of them)
     * is corrected by the next notice or by the WIREUP broadcast. */
    if(global){
        uint32_t epoch = prte_grpcomm_issue_epoch();
        ret = PMIx_Data_pack(NULL, msg, &epoch, 1, PMIX_UINT32);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_RELEASE(msg);
            return;
        }
    }

    // Build array of failures to pass up to my parent
    pmix_data_array_t arr = PMIX_DATA_ARRAY_STATIC_INIT;
    if(status->parent_changed){
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

    // Tell our parent the lineage we now believe in - our ancestor list, whose
    // last entry is the parent we are sending to. This is the mirror of what an
    // adoption notice carries the other way, and the receiver reconciles it the
    // same way, but the two directions were not symmetric until now: a notice
    // travelling UP reported facts about the sender's subtree and nothing about
    // why it was talking to this daemon at all. A parent that has not yet
    // detected the death that re-homed us learns nothing from it, and keeps
    // routing our traffic through a dead ancestor until one of its own sends
    // times out. It cannot even reconstruct the death from the failure array:
    // that array is filtered to the sender's own subtree, and an ancestor is by
    // definition not in it, so the common re-homing case sends an EMPTY array.
    //
    // The list rides only this leg. The HNP's copy of this message is a global
    // broadcast to daemons whose lineage has nothing to do with ours, which is
    // why `global` gates it - and why `global` unpacks first, so the same tag
    // can carry both shapes.
    if (!global) {
        ret = PMIx_Data_pack(NULL, msg, &prte_rml_base.ancestors, 1,
                             PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_RELEASE(msg);
            return;
        }
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
