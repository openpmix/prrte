/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>
#include <sys/time.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "grpcomm_internal.h"
#include "src/grpcomm/grpcomm.h"

#define XCAST prte_grpcomm_globals.xcast_ops

/* The initiating op created in xcast_nb() is consumed by begin_xcast() (it is
 * packed and relayed to the master, then discarded); the op the master actually
 * tracks and completes is a fresh one built on receipt.  A completion callback
 * therefore cannot simply ride on the initiating op.  Instead, because the
 * master assigns op-ids for and relays every xcast — including its own — through
 * itself, we queue one entry per master-originated broadcast and pop it (FIFO)
 * when the master receives that broadcast back to build its tracked op.  The
 * entry is enqueued in begin_xcast(), immediately before the broadcast is sent
 * (and unwound if that send fails), so the queue tracks exactly the stream of
 * broadcasts that were actually emitted, in emission order.  Enqueue and the
 * op-id stamping done on receipt both run on the single progress thread and the
 * send-to-self is delivered in order, so the FIFO stays aligned with the
 * master's own broadcasts.  One entry is enqueued for every master-originated
 * xcast (NULL callback included) to keep that alignment; cbfunc is a local
 * function pointer, so it is only meaningful for broadcasts the master itself
 * originates.  The queue lives in XCAST (XCAST.pending_completions), constructed
 * with the xcast-ops object, so it is a properly initialized list rather than a
 * static one that append would corrupt. */
typedef struct {
    pmix_list_item_t super;
    prte_grpcomm_xcast_complete_fn_t cbfunc;
    void *cbdata;
} pending_completion_t;
PMIX_CLASS_INSTANCE(pending_completion_t, pmix_list_item_t, NULL, NULL);

/* An op's identity folded into a single pointer-sized value, and back.
 *
 * PRTE_RML_SEND_CB carries one void* to its completion callback, and the
 * identity it has to carry is now a pair: an op id is unique only within its
 * topology, so the id alone would find the wrong op - or the right id in the
 * wrong tree - as soon as both trees are in use. Folding is safe because an
 * op id is a counter that will not approach the top three bits of a pointer
 * in any run that could also exhaust memory. */
#define SIG_TO_CBDATA(sig) \
    ((void *)(intptr_t)(((sig).op_id * PRTE_GRPCOMM_TOPO_COUNT) + (sig).topology))
#define SIG_FROM_CBDATA(sig, cbd)                                             \
    do {                                                                      \
        size_t _v = (size_t)(intptr_t)(cbd);                                  \
        (sig).topology =                                                      \
            (prte_grpcomm_topology_t)(_v % PRTE_GRPCOMM_TOPO_COUNT);          \
        (sig).op_id = _v / PRTE_GRPCOMM_TOPO_COUNT;                           \
    } while(0)

/* internal signature used to uniquely track a particular xcast
 *
 * The op id is unique within its topology, not across all of them - two trees
 * numbering from one counter would each raise out-of-order on the other's
 * traffic. So the topology is part of the identity rather than a property of
 * the op, and it travels with the signature. */
typedef struct {
    prte_grpcomm_topology_t topology;
    size_t op_id;    // controller's assigned ID, unique within the topology
} signature_t;

/* the reliability state of the tree a signature names */
#define TREE_OF(sig) (XCAST.tree[(sig).topology])

/* internal component object for tracking ongoing operations */
typedef struct {
    pmix_list_item_t super;
    signature_t sig;
    prte_event_t ev;
    // Only locally process the msg being xcast once
    bool processed;
    // If we are promoted, we must wait until our parent replays this op to
    // replay it to our children. This is because our completion information
    // for older ops is invalid when our subtree grows
    bool replay_pending_parent;
    /* Who last spoke to us about this op - the daemon that forwarded it, or
     * that re-polled us for an ack.  It is who we answer, in preference to
     * whoever our own derivation currently calls our parent: see send_ack_to. */
    pmix_rank_t upstream;
    /* Held because the daemon that forwarded it had seen departures we had
     * not, so our own derivation of this tree is behind the one it was sent
     * under.  Cleared when the news arrives - see the parking commentary in
     * prte_grpcomm_xcast_recv. */
    bool awaiting_news;
    // # children at time of (re)start
    size_t nexpected;
    // # children confirmed completed
    size_t nreported;
    // track which acks are valid by order of faults reported from HNP
    pmix_rank_t ack_id_up;
    pmix_rank_t ack_id_down;
    // hold onto the user's message until completion is confirmed
    pmix_byte_object_t msg;
    bool msg_compressed;
    // tag for the underlying user message
    prte_rml_tag_t msg_tag;
    // optional completion callback, fired on the master when the whole DVM has
    // received this op (see prte_grpcomm_xcast_nb).  NULL when unused.
    prte_grpcomm_xcast_complete_fn_t cbfunc;
    void *cbdata;
} op_t;
PMIX_CLASS_DECLARATION(op_t);

// event handler for prte_grpcomm_xcast to safely access global data
//   void* = a built op_t*
static void begin_xcast(int, short, void*);
// Returns NULL if not found
static op_t* find_op(signature_t *sig);
// Returns op after constructing & inserting it into our tracking list
static op_t* insert_forwarded_op(signature_t *sig);
// Standard forward to all children,
static void forward_op(op_t *op);
// Forward to specific destination
static void forward_op_to(op_t *op, pmix_rank_t dest);
// Pack the forward once, ready to be sent to any number of children.  NULL on
// failure, already reported
static prte_rml_payload_t* build_forward_payload(op_t *op);
// Send an already-packed forward to one destination; the payload is shared,
// so this takes no ownership of it
static void forward_payload_to(op_t *op, prte_rml_payload_t *payload,
                               pmix_rank_t dest);
// Send-completion callback: a message to a child that never arrives means that
// subtree's ack is never coming, so stop expecting it
static void forward_lost(int status, pmix_proc_t *peer,
                         pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata);
// Locally process the message being broadcast, if not already done
static void process_msg(op_t *op);
// Ack that myself and my full subtree have received this message
static void send_ack_to(signature_t* sig, pmix_rank_t ack_id, pmix_rank_t up);
// Request an ack after a failure without resending full user message
static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id);
// Remove local tracking and ack to parent
static void finish_op(op_t *op);
// Finish every op whose subtree has reported. Ops are held in op-id order
// and finish_op is what enforces that they retire in it
static void drive_completions(void);
// Give up on this op's exchange and get the payload the tree way
static void tree_whole_forward(op_t *op);
/* Who this daemon relays to, and who relays to it, in a given tree - see the
 * definitions for why the two trees answer differently. */
static void topo_children(prte_grpcomm_topology_t topo, pmix_rank_t **children,
                          size_t *n, bool *owned);
static size_t topo_n_children(prte_grpcomm_topology_t topo);
static pmix_rank_t topo_parent(prte_grpcomm_topology_t topo);
/* Is an edge of this tree sent direct rather than routed?  Registers the peer
 * as a lateral link when it is one - see the definition. */
static bool edge_is_direct(prte_grpcomm_topology_t topo, pmix_rank_t dest);


// Pack the xcast message forwarded to our children.  Takes no destination:
// the forward is identical for all of them, which is what lets one packed
// buffer be shared by every send (see tree_whole_forward)
static int pack_forward_msg(pmix_data_buffer_t *buffer, op_t *op);
// Pack the initiating relay from an originator to the controller
static int pack_relay_msg(pmix_data_buffer_t *buffer, op_t *op);
// (un)pack components
static int pack_sig     (pmix_data_buffer_t* buffer, signature_t* sig);
static int unpack_sig   (pmix_data_buffer_t* buffer, signature_t* sig);
static int pack_ack_id  (pmix_data_buffer_t* buffer, pmix_rank_t* ack_id);
static int unpack_ack_id(pmix_data_buffer_t* buffer, pmix_rank_t* ack_id);
static int pack_msg     (pmix_data_buffer_t* buffer, op_t* op);
static int unpack_msg   (pmix_data_buffer_t* buffer, op_t* op);
static int pack_bool    (pmix_data_buffer_t* buffer, bool* boolean);
static int unpack_bool  (pmix_data_buffer_t* buffer, bool* boolean);

int prte_grpcomm_xcast(prte_rml_tag_t tag, pmix_data_buffer_t *msg){
    return prte_grpcomm_xcast_nb(tag, msg, NULL, NULL);
}

/* Which tree a collective's release travels.
 *
 * Chosen by tag, not by size, and that is deliberate: the operations differ
 * in what they carry, and a byte threshold cannot express that while a tag
 * can. A fence release is the whole modex, which is what makes the release
 * fanout dominate a collecting fence; a group release is small, and a low
 * radix buys nothing where the r*M*beta term is nil and costs depth. So the
 * fence's release is the one that moves when the low-radix tree is turned on
 * and the group's is not, until somebody measures a reason otherwise.
 *
 * This is the single seam every release goes through, which is why the choice
 * lives here rather than at each of the four call sites. */
prte_grpcomm_topology_t prte_grpcomm_release_topology(prte_rml_tag_t tag)
{
    if (prte_grpcomm_globals.low_radix_release &&
        PRTE_RML_TAG_FENCE_RELEASE == tag) {
        return PRTE_GRPCOMM_TOPO_RELEASE;
    }
    return PRTE_GRPCOMM_TOPO_ROUTING;
}

int prte_grpcomm_release_bcast_select(prte_rml_tag_t tag,
                                      pmix_data_buffer_t *msg)
{
    return prte_grpcomm_xcast_topo(tag, msg,
                                   prte_grpcomm_release_topology(tag),
                                   NULL, NULL);
}

int prte_grpcomm_xcast_nb(prte_rml_tag_t tag, pmix_data_buffer_t *msg,
                                 prte_grpcomm_xcast_complete_fn_t cbfunc,
                                 void *cbdata){
    return prte_grpcomm_xcast_topo(tag, msg, PRTE_GRPCOMM_TOPO_ROUTING,
                                   cbfunc, cbdata);
}

int prte_grpcomm_xcast_topo(prte_rml_tag_t tag, pmix_data_buffer_t *msg,
                            prte_grpcomm_topology_t topology,
                            prte_grpcomm_xcast_complete_fn_t cbfunc,
                            void *cbdata){
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast: with %d bytes on tree %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) msg->bytes_used, (int) topology));

    op_t* op = PMIX_NEW(op_t);
    op->sig.topology = topology;
    op->msg_tag = tag;
    /* stash the completion callback on the initiating op.  It is not fired from
     * this op (which is discarded after begin_xcast relays it); begin_xcast
     * copies it into the pending-completion FIFO once the broadcast is actually
     * sent, and finish_op fires it from the op the master builds on receipt. */
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    /* Make a (possibly compressed) copy of this message in a new op - this is
     * non-destructive, so our caller is still responsible for releasing any
     * memory in the buffer they gave us.
     *
     * The payload is compressed exactly once, here at the originator, and the
     * compressed bytes are what every hop forwards; only the local delivery in
     * process_msg() inflates.  So the sender pays one deflate and each daemon
     * pays one inflate, off the forwarding path.
     *
     * Whether that is worth doing is not a property of the payload's size
     * alone.  Write M for the payload in bytes, rho for the fraction the
     * compressor leaves (so 0.5 means it halved it), B for the bandwidth of one
     * link in bytes/sec, d for the depth of the routing tree and k for its
     * radix - the number of children a daemon forwards to.  Shrinking the
     * payload saves (1 - rho) * M / B on every link the broadcast crosses, and
     * the critical path holds d * k of them, because each of the d levels
     * serialises k full copies onto one outbound link.  Deflate is linear in M
     * above a few KB, so writing its cost as M / R for a compressor rate R in
     * bytes/sec makes M cancel from both sides and leaves
     *
     *     compress iff   R  >  B / (d * k * (1 - rho))
     *
     * a comparison of two rates, not a size.  A size threshold cannot express
     * that; it buys only protection from the fixed cost of starting the
     * compressor and from the poor ratios of tiny inputs, which is worth having
     * but is the compressor's own business - every pcompress component already
     * declines an input below its configured limit, and declines any result
     * that is not actually smaller.  So there is nothing for this layer to add:
     * hand the payload over and let the component judge it.
     *
     * At the scales this runtime is built for the answer is a clear yes.  A
     * 10000-node DVM at 128 processes per node has d = 3 and k = 64 (the
     * default radix), so d * k = 192, and its fence release carries the
     * aggregated modex of 1.28M processes - hundreds of megabytes.  Measured on
     * a modex-shaped corpus, zstd reaches rho = 0.63 at R = 434 MB/s where
     * zlib manages 0.65 at 103 MB/s.  Broadcasting 256 MB over 10 Gb/s
     * (B = 1.25 GB/s): 39 s raw against 20 s compressed, for well under a
     * second of deflate.  Compression halves it.
     *
     * The one thing to keep in mind is that this deflate runs BEFORE the
     * thread-shift below, so it is serial on the caller's thread - and every
     * caller is already the progress thread.  At 256 MB the HNP stalls for the
     * duration, servicing no RML message and no PMIx connection while it works.
     * The wire time it buys back is larger, so the trade is right, but the cost
     * lands as one stall rather than as evenly spread bandwidth. */
    {
        struct timeval t0, t1;
        char timing[32];

        timing[0] = '\0';
        if (prte_grpcomm_globals.enable_timing) {
            gettimeofday(&t0, NULL);
        }

        op->msg_compressed = (bool) PMIx_Data_compress(
            (uint8_t*) msg->base_ptr, msg->bytes_used,
            (uint8_t**) &op->msg.bytes, &op->msg.size
        );

        if (prte_grpcomm_globals.enable_timing) {
            gettimeofday(&t1, NULL);
            snprintf(timing, sizeof(timing), " in %ld us",
                     (long) ((t1.tv_sec - t0.tv_sec) * 1000000L
                             + (t1.tv_usec - t0.tv_usec)));
        }

        /* What the compressor decided and what it bought - raw size, on-wire
         * size, and the ratio - for every broadcast, so the line is a complete
         * census of what a DVM broadcasts rather than only of what it
         * compressed.  The deflate time is appended only when timing is
         * enabled; it is the term that has to be weighed against the wire time
         * saved on every link of the tree. */
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:xcast: tag %u raw %lu wire %lu "
                             "ratio %.4f %s%s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned) tag,
                             (unsigned long) msg->bytes_used,
                             (unsigned long) (op->msg_compressed ? op->msg.size
                                                                 : msg->bytes_used),
                             (0 == msg->bytes_used) ? 1.0
                                 : (double) (op->msg_compressed ? op->msg.size
                                                                : msg->bytes_used)
                                   / (double) msg->bytes_used,
                             op->msg_compressed ? "compressed" : "uncompressed",
                             timing));
    }

    if(!op->msg_compressed){
        pmix_data_buffer_t msg_copy;
        PMIx_Data_buffer_construct(&msg_copy);

        int rc = PMIx_Data_copy_payload(&msg_copy, msg);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Data_buffer_destruct(&msg_copy);
            PMIX_RELEASE(op);
            /* this entry point answers in PRTE codes - callers log it with
             * PRTE_ERROR_LOG and at least one runs it back through
             * prte_pmix_convert_rc(), which would mistranslate a PMIx status */
            return prte_pmix_convert_status(rc);
        }

        PMIx_Data_unload(&msg_copy, &op->msg);
        PMIx_Data_buffer_destruct(&msg_copy);
    }

    /* must push this into the event library to ensure we can
     * access framework-global data safely */

    prte_event_set(prte_event_base, &op->ev, -1, PRTE_EV_WRITE, begin_xcast, op);
    PMIX_POST_OBJECT(op);
    prte_event_active(&op->ev, PRTE_EV_WRITE, 1);

    return PRTE_SUCCESS;
}

void prte_grpcomm_xcast_recv(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) buffer->bytes_used));

    signature_t sig;
    uint32_t sender_version;
    if(PMIX_SUCCESS != unpack_sig(buffer, &sig)) return;
    {
        int32_t cnt = 1;
        if (PMIX_SUCCESS != PMIx_Data_unpack(NULL, buffer, &sender_version,
                                             &cnt, PMIX_UINT32)) {
            return;
        }
    }

    /* Ignore messages from old parents - but "parent" is a question about the
     * tree this broadcast is travelling, which is why the signature has to be
     * read first. A release arriving over the low-radix tree comes from that
     * tree's parent, and on a routing tree of a different radix that daemon is
     * a sibling or a cousin rather than a parent. Screening on the routing
     * parent would discard every one of them. */
    if (!PRTE_PROC_IS_MASTER &&
        PRTE_GRPCOMM_TOPO_ROUTING == sig.topology &&
        sender->rank != topo_parent(sig.topology)) {
        /* An old parent's forward, on the tree where "old" is a question this
         * daemon can answer.  The repair notice travels this tree, so by the
         * time a new parent relays anything we have already processed the
         * notice that made it our parent - the screen can only ever be
         * rejecting a genuinely superseded message.
         *
         * On a derived tree that reasoning does not hold and the screen is
         * actively wrong: every daemon recomputes its own shape when the
         * notice reaches it, and a daemon that has repaired can replay to a
         * child that has not.  The child then reads a forward from its new
         * parent as coming from a stranger and discards it, and since the
         * whole point of the replay is to reach daemons whose relay died, it
         * discards the one message that would have released it.  That was
         * exactly the failure: a fence released on the low-radix tree hung
         * three daemons behind a killed relay, each of them having received
         * and silently dropped the repair.
         *
         * Accepting it costs nothing.  A duplicate forward is already
         * idempotent - op_id decides, and an op we hold or have completed is
         * recognized either way - and the ack goes back to the sender rather
         * than to our own idea of a parent. */
        return;
    }

    /* An op-id of zero is an originator relaying to the controller rather than
     * a forward down the tree; the controller stamps the real id below. The
     * two carry the same fields in the same order - pack_relay_msg and
     * pack_forward_msg are deliberately identical - so nothing downstream has
     * to tell them apart. */

    // A daemon that has never seen any xcast (op_id_inited == 0) yet is being
    // handed an op is a *late joiner*: a daemon grown into a running DVM, or one
    // whose node rebooted and returned (the bootstrap unheal path), or simply a
    // bootstrap daemon that booted after the first broadcast. It cannot have the
    // ops that preceded this one, so it must adopt them as already complete
    // rather than enforce ordering it can never satisfy -- finish_op would
    // otherwise raise PRTE_ERR_OUT_OF_ORDER_MSG on the first op above 1 and the
    // daemon would force-exit. Setting op_id_completed_at_promotion below the
    // completed mark makes those prior ops assume-complete exactly as a
    // promotion does. The master assigns op-ids and is never a late joiner, so
    // it is excluded. This is safe because a daemon that has *ever* participated
    // has op_id_inited > 0, so a genuine ordering violation mid-stream is still
    // caught.
    bool late_joiner = !PRTE_PROC_IS_MASTER && (0 == TREE_OF(sig).op_id_inited);

    if(PRTE_PROC_IS_MASTER){
        if(sig.op_id){
            // If I'm HNP, I expect sender has not assigned a global ID
            PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
            return;
        }
        sig.op_id = ++TREE_OF(sig).op_id_inited;
        /* Absolute microseconds, so a broadcast's coverage can be measured
         * across daemons rather than only at one.  Every daemon prints the
         * same stamp when it processes the payload (process_msg), and the
         * span from this line to the last of those is how long the broadcast
         * took to reach the whole DVM - which is the quantity a change to the
         * fanout tree is about, and the one an end-to-end fence cannot
         * resolve because the rollup's noise is larger than the effect.
         *
         * It is only comparable where the clocks are: fine on a container
         * swarm sharing one kernel, meaningless across a real cluster without
         * a synchronized clock. */
        if (prte_grpcomm_globals.enable_timing) {
            struct timeval tnow;
            gettimeofday(&tnow, NULL);
            /* No payload tag here on purpose: `tag` at this point is the
             * tag the xcast message ARRIVED on (PRTE_RML_TAG_XCAST), not the
             * one the payload will be delivered at - that is inside the
             * message and is not unpacked until below.  The processed lines
             * carry the real one, and (tree, op_id) pairs the two. */
            pmix_output(0, "%s grpcomm:xcast:timing started op_id %lu "
                        "tree %d at %ld.%06ld",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (unsigned long) sig.op_id,
                        (int) sig.topology, (long) tnow.tv_sec,
                        (long) tnow.tv_usec);
        }
    }
    if(!sig.op_id){
        PRTE_ERROR_LOG( PRTE_ERR_NOT_INITIALIZED );
        return;
    }
    if(sig.op_id > TREE_OF(sig).op_id_inited){
        TREE_OF(sig).op_id_inited = sig.op_id;
    }
    if(late_joiner && sig.op_id > 1){
        // Catch up to just below this op: ops 1..op_id-1 are taken as complete,
        // and this op is processed in order as our first.
        TREE_OF(sig).op_id_completed = sig.op_id - 1;
        TREE_OF(sig).op_id_completed_at_promotion = sig.op_id - 1;
    }

    // If we marked our subtree as completed, but then were promoted, our
    // subtree is now larger and may not have actually completed everywhere.
    // But ops complete in order, so if we have completed anything since our
    // promotion, we know our new subtree has also completed all the older ops
    bool assume_incomplete =
        sig.op_id <= TREE_OF(sig).op_id_completed_at_promotion
        && TREE_OF(sig).op_id_completed == TREE_OF(sig).op_id_completed_at_promotion;

    // If we're certain our subtree has already completed this, we can just ack
    bool complete = !assume_incomplete &&
        sig.op_id <= TREE_OF(sig).op_id_completed;

    pmix_rank_t ack_id;
    if(PMIX_SUCCESS != unpack_ack_id(buffer, &ack_id)) return;

    if(complete) {
        send_ack_to(&sig, ack_id, sender->rank);
        return;
    }

    op_t* op = find_op(&sig);
    if(NULL == op){
        op = insert_forwarded_op(&sig);
        /* If we are the master and this is one of our own broadcasts, attach the
         * completion callback queued for it in begin_xcast (FIFO).  Remote-origin
         * broadcasts queue nothing, so they never consume an entry.  This has to
         * happen before the unpack below can abandon the op: the FIFO is
         * positional, so an entry left on it once its broadcast has been dropped
         * would be handed to the next broadcast the master makes. */
        if(PRTE_PROC_IS_MASTER && NULL != sender &&
           sender->rank == PRTE_PROC_MY_NAME->rank &&
           !pmix_list_is_empty(&XCAST.pending_completions)){
            pending_completion_t* pc =
                (pending_completion_t*) pmix_list_remove_first(&XCAST.pending_completions);
            op->cbfunc = pc->cbfunc;
            op->cbdata = pc->cbdata;
            PMIX_RELEASE(pc);
        }
        if(PMIX_SUCCESS != unpack_msg(buffer, op)){
            pmix_list_remove_item(&XCAST.ops, &op->super);
            PMIX_RELEASE(op);
            return;
        }
    }

    op->ack_id_up = ack_id;
    op->upstream = sender->rank;

    /* Park a forward that was derived from news we have not heard.
     *
     * On a derived tree the shape is a function of the live set, so a daemon
     * that has recorded more departures than we have computed this forward
     * from a set ours does not yet include - and everything we would do with
     * it next depends on our own derivation.  Above all the child count: a
     * daemon that has just been promoted into a dead relay's slot still reads
     * itself as a leaf until the news reaches it, so it would answer the
     * repair by retiring the operation as complete, having forwarded it
     * nowhere.  The subtree the repair existed to reach is then stranded with
     * nobody holding the payload, which is exactly how this failed: five of
     * seven daemons released, and the two below the promoted one waiting for
     * a broadcast that had already been declared delivered.
     *
     * So take the payload and deliver it locally - that part does not depend
     * on the tree at all, and it is what releases this daemon's own clients -
     * but do not derive anything until the news arrives.  nexpected is left
     * at its constructed SIZE_MAX, so the operation cannot satisfy op_ready()
     * and retire while parked; release_tree_fault() picks it up when the
     * departure notice lands, and forwards it under the shape both ends then
     * agree on.
     *
     * Two things have to be true before we hold anything, and the second is
     * what keeps this from being a hazard of its own.  The sender must be
     * ahead of us, and the forward must have come from a daemon our own
     * derivation does not call our parent - concrete local evidence that the
     * two views disagree, rather than an inference from a number.  In the
     * ordinary case a forward always arrives from our parent, so ordinary
     * traffic can never be parked however the versions happen to sit; and the
     * case this exists for always fails that test, because a daemon promoted
     * into a dead relay's slot is by definition being addressed by a daemon
     * that is not its parent yet.
     *
     * That matters because the version is a count of events learned, not an
     * agreed number.  It is monotone, which is all the comparison needs, but
     * daemons can legitimately hold different counts for a while - a grown
     * daemon is seeded from the departed set it was launched with, and a
     * revival moves the count on the daemons that have processed it first.
     * Requiring the second signal means such a skew costs nothing.
     *
     * The routing tree is exempt because it does not have the problem: its
     * repair notice travels the routing tree itself, so a daemon has always
     * processed the notice that gave it a new parent before anything that
     * parent relays afterwards can arrive. */
    if (PRTE_GRPCOMM_TOPO_ROUTING != sig.topology &&
        sender_version > prte_rml_tree_version() &&
        sender->rank != topo_parent(sig.topology)) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:xcast holding op_id %lu on tree %d: "
                             "%s is at tree version %u, we are at %u",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned long) sig.op_id, (int) sig.topology,
                             PRTE_NAME_PRINT(sender),
                             (unsigned) sender_version,
                             (unsigned) prte_rml_tree_version()));
        op->awaiting_news = true;
        process_msg(op);
        return;
    }
    op->awaiting_news = false;

    if(assume_incomplete){
        op->processed = true;
        op->replay_pending_parent = true;
    }

    if(op->replay_pending_parent){
        forward_op(op);
        if(op->processed){
            /* Nothing further to do locally. Settling up may release the op,
             * so it must be the last thing that touches it - reading
             * op->processed again afterwards, as this used to, is a read of
             * freed memory that happened to find the value it wanted. */
            drive_completions();
            return;
        }
    }
    if(op->processed) return;

    PMIX_OUTPUT_VERBOSE((
        1, prte_grpcomm_globals.output,
        "%s grpcomm:xcast:recv: new xcast of tag %u with op_id %lu",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) op->msg_tag,
        (unsigned long) sig.op_id
    ));

    // We need to process (invoke the user msg's callback, generally) and
    // forward to our children.
    // For most xcasts, it's best to forward first to maintain message ordering,
    // but xcasts that modify how we send messages should be processed first.
    // DAEMON_DIED is processed first because a death *grows* our child set
    // (orphans promote to us), and we must repair before forwarding so the
    // promoted grandchildren receive it. DAEMON_REVIVED is the opposite: a
    // return *shrinks* our child set (the returned rank reclaims its orphans),
    // so it must stay on the forward-first path -- forwarding to our current
    // children before the reshape is what delivers the notice to the very
    // children that are about to re-home. Do not move it into this set.
    bool process_first = PRTE_RML_TAG_WIREUP == op->msg_tag ||
                         PRTE_RML_TAG_DAEMON_DIED == op->msg_tag;
    if(process_first){
        process_msg(op);
        forward_op(op);
    } else {
        forward_op(op);
        process_msg(op);
    }
    /* Settle up last, and never touch the op again: this may finish it, and it
     * may also finish an op behind it that was waiting on this one's payload.
     * Under tree_whole it is what the "no children, so ack immediately" call
     * here always was. */
    drive_completions();
}

void prte_grpcomm_xcast_ack(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);

    int ret = PMIX_SUCCESS;

    signature_t sig;
    pmix_rank_t ack_id;
    bool is_request;

    if(PMIX_SUCCESS == ret) ret = unpack_sig(buffer, &sig);
    if(PMIX_SUCCESS == ret) ret = unpack_ack_id(buffer, &ack_id);
    if(PMIX_SUCCESS == ret) ret = unpack_bool(buffer, &is_request);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        return;
    }

    op_t* op = find_op(&sig);

    if(is_request){
        if (PRTE_GRPCOMM_TOPO_ROUTING == sig.topology &&
            sender->rank != topo_parent(sig.topology)) {
            // Old message, or one from a daemon that is our parent in some
            // other tree than the one this op travels. Only asked of the
            // routing tree, for the reason given at the same screen in
            // prte_grpcomm_xcast_recv: on a derived tree the two ends repair
            // independently, so "not my parent" may only mean "I have not
            // heard yet", and refusing the poll strands the op.
            return;
        }
        if(NULL != op){
            // We'll send with the new id once we're done, to whoever asked
            op->ack_id_up = ack_id;
            op->upstream = sender->rank;
        } else if(sig.op_id <= TREE_OF(sig).op_id_completed){
            // We've finished this one, ack now
            send_ack_to(&sig, ack_id, sender->rank);
        } else {
            // We haven't seen this xcast before
            PRTE_ERROR_LOG( PRTE_ERR_OUT_OF_ORDER_MSG );
        }
    } else {
        if(NULL == op || op->ack_id_down != ack_id) return;
        op->nreported++;
        if(op->nreported == op->nexpected){
            /* The subtree has reported. */
            drive_completions();
        } else if(op->nreported > op->nexpected){
            PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
        }
    }
}

/* Repair an op travelling a tree other than the routing one.
 *
 * The routing tree is repaired incrementally and the RML hands us a
 * description of what moved - who was promoted, which children changed, what
 * the previous set was.  None of that describes a derived tree, which is not
 * repaired at all: after a death every daemon simply computes a different
 * answer from a live set they all hold in step.  There is no "previous
 * children" to diff against, and inventing one would mean caching a second
 * tree only so that a fault could compare it.
 *
 * So do not diff.  Every daemon still holding this op forwards it to whoever
 * its children are now, and the asymmetry the design turns on does the rest:
 * a duplicate is harmless - the op id decides, so a daemon that holds this op
 * or has already completed it recognizes it either way - while a release that
 * never arrives hangs a fence for good.  Replaying too much costs a message on
 * a path that has just lost a daemon; replaying too little costs the job.
 *
 * Note that nobody waits.  The routing tree makes a *promoted* daemon defer
 * its replay until its own parent has replayed, because its subtree may have
 * grown to include daemons that never saw this op, and sending them op N+1
 * before op N breaks the ordering finish_op enforces.  Here that deferral is
 * both unnecessary and harmful.  Unnecessary because an op a child is missing
 * is by definition still in flight - the root cannot retire one until every
 * daemon has acked it - so it is held by some daemon that is replaying it in
 * this same pass, and the list is walked in op order.  Harmful because a
 * daemon deferring to a parent that has already replayed waits for a second
 * replay that is never coming, and strands its whole subtree.  That was
 * measured rather than reasoned: relaxing only the parent screen took the
 * count from four daemons released to five - the one that received the replay
 * directly - and left the two below it waiting on it. */
static void release_tree_fault(op_t* op)
{
    size_t n = topo_n_children(op->sig.topology);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast repairing %sop_id %lu on tree %d: "
                         "%d children now, parent %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         op->awaiting_news ? "held " : "",
                         (unsigned long) op->sig.op_id,
                         (int) op->sig.topology, (int) n,
                         PRTE_VPID_PRINT(topo_parent(op->sig.topology))));

    /* Note what is deliberately NOT done here: the ack id we hold is not
     * invalidated.  The block above does that for the routing tree, so that a
     * new parent is not answered under a round it never opened - but it is
     * only safe where somebody is going to ask again, and the empty-subtree
     * case below answers immediately and then retires the op.  An ack under
     * the last id we were actually given is at worst ignored; one under
     * PMIX_RANK_INVALID is ignored for certain, and there is no second
     * chance.  Every op that survives this call is about to be forwarded, and
     * the forward it draws in reply carries a fresh id with it. */
    if (0 == n) {
        /* nothing below us on this tree any more - our subtree is complete
         * by virtue of being empty, which is what the routing path does with
         * the same discovery */
        finish_op(op);
        return;
    }

    op->nexpected = n;
    op->nreported = 0;
    /* we cannot tell a surviving child's ack from a dead one's, so nothing
     * counted before this point can be trusted - start a fresh round */
    op->ack_id_down++;
    op->replay_pending_parent = false;
    /* the news we were held for */
    op->awaiting_news = false;

    tree_whole_forward(op);
}


/* A lateral link died while we were relying on it.
 *
 * This is the hole that opens the moment a derived tree's edges stop being
 * routing edges.  An undeliverable forward is already covered - the send
 * completes with an error and forward_lost drops the expectation - but a link
 * that drops AFTER the forward landed produces no send completion at all, and
 * the op then waits on an ack that is never coming.
 *
 * Note what this is not allowed to conclude.  prte_rml_route_lost() reaches
 * here precisely because it has decided the loss is NOT a routing-tree fault,
 * and it deliberately does not diagnose the peer as dead: the socket may
 * simply have dropped.  So this must not fail anything or mark anyone dead.
 * Re-deriving and re-forwarding is the whole response, and it is safe for both
 * readings - if the peer is alive the direct send re-opens the connection, and
 * if it really died the global notice arrives later and the repair runs again
 * on a live set that no longer contains it. */
void prte_grpcomm_xcast_lateral_lost(pmix_rank_t rank)
{
    op_t *op, *next_op;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast lateral link to %s lost -"
                         " replaying any derived-tree ops that used it",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_VPID_PRINT(rank)));

    PMIX_LIST_FOREACH_SAFE(op, next_op, &XCAST.ops, op_t){
        if (PRTE_GRPCOMM_TOPO_ROUTING == op->sig.topology) {
            continue;
        }
        release_tree_fault(op);
    }
}

void prte_grpcomm_xcast_fault_handler(
    const prte_rml_recovery_status_t* status
) {
    // We must do all xcast handling in the local scope, since reliable xcasts
    // is how we get the global scope notifications in the first place
    if(status->scope != PRTE_RML_FAULT_SCOPE_LOCAL) return;

    /* Our subtree may have grown, and what we hold about it is then stale:
     * an op we completed and released was completed by the daemons that were
     * below us *then*, which says nothing about one that has just arrived.
     * Marking the completions as suspect is what makes a later replay of such
     * an op get re-inserted and forwarded on (assume_incomplete, in the recv
     * path) rather than answered with a bare ack that ends the cascade one
     * daemon short of the one still waiting.
     *
     * On the routing tree we are told when that happened: the RML repairs it
     * incrementally and reports the promotion.  On a derived tree nobody
     * reports anything - every daemon simply recomputes a different answer
     * from the new live set - so there is no promotion to be told about, and
     * the honest reading of any death is that our subtree there may have
     * grown.  Do not be tempted to narrow this by comparing child sets: the
     * daemon that needs the replay is not our child but somewhere below one,
     * and our own children may be identical while the tree beneath them is
     * not. */
    for (int t = 0; t < PRTE_GRPCOMM_TOPO_COUNT; t++) {
        if (PRTE_GRPCOMM_TOPO_ROUTING == t && !status->promoted) {
            continue;
        }
        XCAST.tree[t].op_id_completed_at_promotion =
            XCAST.tree[t].op_id_completed;
    }
    if(status->parent_changed || status->promoted || status->demoted){
        // Avoid confusing new parent by accidentally acking with
        // the valid ack id. They'll tell us what id to use.
        op_t* op;
        PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
            op->ack_id_up = PMIX_RANK_INVALID;
        }
    }
    /* Every tree this daemon relays on has just changed shape, not only the
     * routing one - the release tree is derived from the live daemon set, so
     * a death reshapes it too, and the ops travelling it need the same
     * treatment on their own terms.  They cannot take the branch below: it
     * reads the routing tree's child count, diffs against the routing tree's
     * previous children, and forwards to them.  Applied to an op that is not
     * travelling the routing tree, every one of those is the wrong tree. */
    {
        op_t* op;
        op_t* next_op;
        PMIX_LIST_FOREACH_SAFE(op, next_op, &XCAST.ops, op_t){
            if (PRTE_GRPCOMM_TOPO_ROUTING == op->sig.topology) {
                continue;
            }
            release_tree_fault(op);
        }
    }

    if(status->children_changed || status->promoted || status->demoted){
        const pmix_rank_t* prev_children =
            (const pmix_rank_t*) status->prev_children.array;
        const pmix_rank_t* children =
            (const pmix_rank_t*) prte_rml_base.children.array;

        op_t* op;
        op_t* next_op;
        PMIX_LIST_FOREACH_SAFE(op, next_op, &XCAST.ops, op_t){
            if (PRTE_GRPCOMM_TOPO_ROUTING != op->sig.topology) {
                continue;
            }
            if(0 == prte_rml_base.n_children){
                /* Under tree_whole both tests are constants - every op holds
                 * its payload and nothing can be blocked - so this is the
                 * unconditional finish it has always been. */
                finish_op(op);
                continue;
            }

            op->nexpected = prte_rml_base.n_children;

            // If this op is currently pending replay, so are all after it.
            if(op->replay_pending_parent) break;

            // If any children have reported back, we have no way of knowing if
            // it was the surviving children or a failed child. So we will need
            // to start a new ack round.
            // If promoted/demoted, avoid late ack arrivals from old children
            // causing confusion by also starting a new round.
            bool new_ack_round =
                op->nreported > 0 || status->promoted || status->demoted;
            if(new_ack_round) op->ack_id_down++;
            op->nreported = 0;

            // If promoted, we can't begin replays yet b/c we don't know if our
            // new children have completed the same ops we have. We could end up
            // sending op N+1 when they've never seen op N. So wait for our
            // parent to replay ops to ensure correct ordering.
            if(status->promoted){
                op->replay_pending_parent = true;
                continue;
            }

            for(size_t i = 0; i < prte_rml_base.children.size; i++){
                if(PMIX_RANK_INVALID == children[i]){
                    continue;
                } else if(children[i] != prev_children[i] || status->demoted){
                    // When demoted, we don't know if an old child that followed
                    // us believed for some short window to have had a different
                    // parent, which could have caused them to discard the
                    // original forward. So always replay the op to every child
                    // when demoted.
                    forward_op_to(op, children[i]);
                } else if(new_ack_round){
                    request_ack(children[i], &op->sig, op->ack_id_down);
                }
            }
        }
    }

    drive_completions();
}

/* Discard a broadcast that was never emitted, telling its caller.
 *
 * A completion callback is normally fired by finish_op, from the op the
 * master builds when this broadcast is relayed back to it - which never
 * happens if the broadcast dies here.  A caller that is waiting on that
 * callback would then wait forever: the collective shrink never emits its
 * completion event, and filem's chunk pump never restarts a read.  Neither
 * failure announces itself, so say so here instead.  The callback carries no
 * status, so it means only "stop waiting"; the error itself has already been
 * logged by the caller of this function.
 */
static void abandon_xcast(op_t *op)
{
    if (PRTE_PROC_IS_MASTER && NULL != op->cbfunc) {
        op->cbfunc(op->cbdata);
    }
    PMIX_RELEASE(op);
}

static void begin_xcast(int sd, short args, void* cbdata){
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    op_t* op = (op_t*) cbdata;
    PMIX_ACQUIRE_OBJECT(op);


    // setup the payload
    pmix_data_buffer_t *xcast_msg = PMIx_Data_buffer_create();
    /* the packers answer in PMIx statuses - PRTE_RML_RELIABLE_SEND below
     * answers in PRTE codes, so the two failures below are logged through
     * different decoders on purpose */
    int rc = pack_relay_msg(xcast_msg, op);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        abandon_xcast(op);
        return;
    }

    /* Record the completion callback for this broadcast now that it is about to
     * go out.  We enqueue one entry per master-originated broadcast, in send
     * order, so the master can pop it (FIFO) when this broadcast is relayed back
     * to it and it builds the op it tracks (see prte_grpcomm_xcast_recv).
     * Enqueuing here — immediately before the send, and unwinding on failure —
     * rather than in xcast_nb keeps the FIFO aligned with exactly the broadcasts
     * that were actually emitted, even if a send is abandoned.  This is a general
     * facility: any master-originated broadcast enqueues an entry (NULL callback
     * included) so the FIFO stays aligned, and finish_op fires the callback only
     * when one was actually registered. */
    pending_completion_t *pc = NULL;
    if (PRTE_PROC_IS_MASTER) {
        pc = PMIX_NEW(pending_completion_t);
        pc->cbfunc = op->cbfunc;
        pc->cbdata = op->cbdata;
        pmix_list_append(&XCAST.pending_completions, &pc->super);
    }

    // send it to the HNP (could be myself) for relay
    PRTE_RML_RELIABLE_SEND(
        rc, PRTE_PROC_MY_HNP->rank, xcast_msg, PRTE_RML_TAG_XCAST
    );
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        if (NULL != pc) {
            pmix_list_remove_item(&XCAST.pending_completions, &pc->super);
            PMIX_RELEASE(pc);
        }
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        abandon_xcast(op);
        return;
    }

    /* the initiating op has now been packed and relayed to the master; it is
     * not the op we track and complete (that one is built fresh on receipt),
     * so discard it here */
    PMIX_RELEASE(op);
}

static void send_ack_msg(
    signature_t* sig, pmix_rank_t ack_id, bool is_request, pmix_rank_t dest
) {
    pmix_data_buffer_t* msg = PMIx_Data_buffer_create();
    int ret = PMIX_SUCCESS;
    if(PMIX_SUCCESS == ret) ret = pack_sig(msg, sig);
    if(PMIX_SUCCESS == ret) ret = pack_ack_id(msg, &ack_id);
    if(PMIX_SUCCESS == ret) ret = pack_bool(msg, &is_request);
    if(PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIx_Data_buffer_release(msg);
        return;
    }

    /* An ack retraces an edge of the same tree, so it is sent the same way -
     * routed for the routing tree, direct for a derived one.  Acks are a few
     * bytes, so this is about hops rather than bandwidth: routing one on a
     * derived tree sends it through the controller and back out, which is two
     * hops and a wakeup at the busiest daemon in the DVM for a message that
     * had one hop to travel. */
    bool direct = edge_is_direct(sig->topology, dest);
    if(is_request){
        /* A re-poll aimed at a child we cannot reach says exactly what an
         * undeliverable forward says - no ack is ever coming from that subtree
         * - so it is tracked the same way.  The upward direction is not: a
         * failure there is our lifeline, which is the fault machinery's
         * business and not this op's. */
        if (direct) {
            PRTE_RML_SEND_DIRECT_CB(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK,
                                    forward_lost, SIG_TO_CBDATA(*sig));
        } else {
            PRTE_RML_SEND_CB(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK,
                             forward_lost, SIG_TO_CBDATA(*sig));
        }
    } else if (direct) {
        PRTE_RML_SEND_DIRECT(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK);
    } else {
        PRTE_RML_SEND(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK);
    }
    /* the RML answers in PRTE codes, unlike the packers above */
    if(PRTE_SUCCESS != ret){
        PRTE_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
    }
}

/* Answer whoever asked.
 *
 * An ack is a reply to a particular forward or poll, so the daemon waiting for
 * it is the one that sent that message - not whoever our own derivation calls
 * our parent right now.  On the routing tree the two are the same, because the
 * tree is repaired by a notice travelling that same tree and a daemon's view
 * therefore changes before anything it relays afterwards can arrive.  A
 * derived tree has no such ordering: every daemon recomputes it independently
 * when the death notice reaches it, so a repaired parent can replay to a child
 * that has not yet heard, and the two disagree for as long as that takes.
 *
 * Answering the sender needs neither side to be up to date.  Answering the
 * wrong daemon costs nothing - it holds no op under that ack id and drops the
 * message - while failing to answer the right one hangs the broadcast, so
 * where the two rules differ this is the safe direction. */
static void send_ack_to(signature_t* sig, pmix_rank_t ack_id, pmix_rank_t up){
    if(PRTE_PROC_IS_MASTER) return;
    if(PMIX_RANK_INVALID == up) return;
    send_ack_msg(sig, ack_id, false, up);
}


static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id){
    send_ack_msg(sig, ack_id, true, from);
}

/* Is this op ready to be settled up - do we hold the payload, and has our
 * whole subtree reported? */
static bool op_ready(const op_t* op){
    return op->nreported >= op->nexpected;
}

static void drive_completions(void){
    bool progress = true;
    while(progress){
        op_t* op;
        op_t* next;
        progress = false;
        PMIX_LIST_FOREACH_SAFE(op, next, &XCAST.ops, op_t){
            if(!op_ready(op)) continue;
            finish_op(op);
            /* finish_op unlinked and released op, so restart the walk
             * rather than trust the cursor */
            progress = true;
            break;
        }
    }
}


static void finish_op(op_t* op) {
    send_ack_to(&op->sig, op->ack_id_up,
                PMIX_RANK_INVALID != op->upstream
                    ? op->upstream : topo_parent(op->sig.topology));
    pmix_list_remove_item(&XCAST.ops, &op->super);
    if(op->sig.op_id > TREE_OF(op->sig).op_id_completed_at_promotion){
        if(op->sig.op_id != TREE_OF(op->sig).op_id_completed+1){
            PRTE_ERROR_LOG( PRTE_ERR_OUT_OF_ORDER_MSG );
        } else {
            TREE_OF(op->sig).op_id_completed++;
        }
    }
    process_msg(op); // If not already processed, process before releasing
    /* on the master, a completed op means every daemon in the DVM has received
     * this broadcast; fire the caller's completion callback if one was
     * registered.  This is the hook the collective DVM-shrink path uses to run
     * its single routing-tree repair and emit its completion event.  It fires
     * only here (PRTE_PROC_IS_MASTER): a non-master's finish_op means only its
     * own subtree completed, not the whole DVM. */
    if (PRTE_PROC_IS_MASTER && NULL != op->cbfunc) {
        op->cbfunc(op->cbdata);
    }
    PMIX_RELEASE(op);
}

#define DIRECT_XCAST_PACK(buf, ptr, type)                              \
    {                                                                  \
        int rc = PMIx_Data_pack(NULL, buf, ptr, 1, type);              \
        if(PMIX_SUCCESS != rc){                                        \
            PMIX_ERROR_LOG(rc);                                        \
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT); \
            return rc;                                                 \
        }                                                              \
    }
#define DIRECT_XCAST_UNPACK(buf, ptr, type)                            \
    {                                                                  \
        int _count = 1;                                                \
        int rc = PMIx_Data_unpack(NULL, buf, ptr, &_count, type);      \
        if (PMIX_SUCCESS != rc) {                                      \
            PMIX_ERROR_LOG(rc);                                        \
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT); \
            return rc;                                                 \
        }                                                              \
    }

static int pack_sig(pmix_data_buffer_t* buffer, signature_t* sig){
    /* The topology leads, because the op id that follows is only meaningful
     * within it - the reader has to know which tree's sequence it is reading
     * before the number means anything. */
    uint8_t topo = (uint8_t) sig->topology;
    DIRECT_XCAST_PACK(buffer, &topo,          PMIX_UINT8);
    DIRECT_XCAST_PACK(buffer, &sig->op_id,    PMIX_SIZE);
    return PMIX_SUCCESS;
}
static int pack_ack_id(pmix_data_buffer_t* buffer, pmix_rank_t* ack_id){
    DIRECT_XCAST_PACK(buffer, ack_id, PMIX_PROC_RANK);
    return PMIX_SUCCESS;
}
static int pack_msg(pmix_data_buffer_t* buffer, op_t* op){
    DIRECT_XCAST_PACK(buffer, &op->msg_tag,        PRTE_RML_TAG);
    DIRECT_XCAST_PACK(buffer, &op->msg_compressed, PMIX_BOOL);
    DIRECT_XCAST_PACK(buffer, &op->msg,            PMIX_BYTE_OBJECT);
    return PMIX_SUCCESS;
}
static int pack_bool(pmix_data_buffer_t* buffer, bool* boolean){
    DIRECT_XCAST_PACK(buffer, boolean, PMIX_BOOL);
    return PMIX_SUCCESS;
}

static int unpack_sig(pmix_data_buffer_t* buffer, signature_t* sig){
    uint8_t topo = 0;
    DIRECT_XCAST_UNPACK(buffer, &topo, PMIX_UINT8);
    /* Screen it before it indexes anything: this value selects an array
     * element and a tree to walk, and a value no release ever assigned would
     * do both out of bounds. */
    if(PRTE_GRPCOMM_TOPO_COUNT <= topo){
        PRTE_ERROR_LOG( PRTE_ERR_BAD_PARAM );
        return PRTE_ERR_BAD_PARAM;
    }
    sig->topology = (prte_grpcomm_topology_t) topo;
    DIRECT_XCAST_UNPACK(buffer, &sig->op_id, PMIX_SIZE);
    return PMIX_SUCCESS;
}
static int unpack_ack_id(pmix_data_buffer_t* buffer, pmix_rank_t* ack_id){
    DIRECT_XCAST_UNPACK(buffer, ack_id, PMIX_PROC_RANK);
    return PMIX_SUCCESS;
}
static int unpack_msg(pmix_data_buffer_t* buffer, op_t* op){
    DIRECT_XCAST_UNPACK(buffer, &op->msg_tag,        PRTE_RML_TAG);
    DIRECT_XCAST_UNPACK(buffer, &op->msg_compressed, PMIX_BOOL);
    DIRECT_XCAST_UNPACK(buffer, &op->msg,            PMIX_BYTE_OBJECT);
    return PMIX_SUCCESS;
}
static int unpack_bool(pmix_data_buffer_t* buffer, bool* boolean){
    DIRECT_XCAST_UNPACK(buffer, boolean, PMIX_BOOL);
    return PMIX_SUCCESS;
}

/* The relay from an originator to the controller.  A point-to-point send; the
 * receiver tells it from a forward by the op-id, which is zero here and
 * assigned by the controller. */
static int pack_relay_msg(pmix_data_buffer_t* buffer, op_t* op){
    uint32_t version = prte_rml_tree_version();
    int rc = pack_sig(buffer, &op->sig);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buffer, &version, 1, PMIX_UINT32);
    }
    if(PMIX_SUCCESS == rc) rc = pack_ack_id(buffer, &op->ack_id_down);
    if(PMIX_SUCCESS == rc) rc = pack_msg(buffer, op);
    return rc;
}

static int pack_forward_msg(pmix_data_buffer_t* buffer, op_t* op){
    uint32_t version = prte_rml_tree_version();
    int rc = pack_sig(buffer, &op->sig);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buffer, &version, 1, PMIX_UINT32);
    }
    if(PMIX_SUCCESS == rc) rc = pack_ack_id(buffer, &op->ack_id_down);
    if(PMIX_SUCCESS == rc) rc = pack_msg(buffer, op);
    return rc;
}

static op_t* find_op(signature_t* sig){
    op_t* op = NULL;
    PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
        if(sig->op_id == op->sig.op_id) return op;
    }
    return NULL;
}

static op_t* insert_forwarded_op(signature_t* sig) {
    op_t* op = PMIX_NEW(op_t);
    op->sig = *sig;

    if(sig->op_id == TREE_OF(*sig).op_id_inited){
        pmix_list_append(&XCAST.ops, &op->super);
    } else {
        op_t* next_op = NULL;
        PMIX_LIST_FOREACH(next_op, &XCAST.ops, op_t){
            if(next_op->sig.op_id > sig->op_id) break;
        }
        pmix_list_insert_pos(&XCAST.ops, &next_op->super, &op->super);
    }

    op_t* prev = (op_t*) pmix_list_get_prev(op);
    bool dup = prev != (op_t*) &XCAST.ops.pmix_list_sentinel
        && prev->sig.op_id == op->sig.op_id;
    if(dup){
        PRTE_ERROR_LOG(PRTE_ERR_DUPLICATE_MSG);
        pmix_list_remove_item(&XCAST.ops, &op->super);
        PMIX_RELEASE(op);
        return prev;
    }
    return op;
}

/* MOVEMENT: send the whole payload to each routing-tree child.
 *
 * Right for a small message, where the cost is the depth of the tree and a
 * high radix makes that 1 or 2 hops.  Wrong for a large one: a node with r
 * children serializes r full copies of the payload on its outbound link, at
 * every level, so the bandwidth term is d*r*M*beta - which is the entire cost
 * of broadcasting a launch message or a preload chunk at scale.
 *
 * The forward is byte-for-byte identical for every child - it carries the
 * op-id, the ack-id and the payload, none of which depend on the destination -
 * so it is packed once here and shared by every send.  Packing it per child
 * cost a full copy of the payload per child, and all of those copies were made
 * on the progress thread inside this one event callback, before any of them
 * could reach the wire: at the default radix, 64 copies of a launch message
 * made and held before the first byte moved.
 *
 * That is why pack_forward_msg takes no destination.  If a forward ever does
 * need to differ per child, this is the loop that has to go back to packing
 * inside it - the sharing is not an optimization the RML can make on its own. */
/* Who this daemon relays to, and who relays to it, in a given tree.
 *
 * The routing tree's answer is already computed and cached - it is consulted
 * on every message and repaired incrementally, so it has to be. The release
 * tree's is derived on demand: it is consulted once per broadcast, and
 * recomputing is cheaper than keeping a second cache correct across every
 * repair. Both answers come from state every daemon holds in step, so no two
 * daemons can disagree about either shape.
 *
 * The caller frees *children when it was allocated, which topo_children says
 * by setting *owned. The routing tree hands back its cached array. */
static void topo_children(prte_grpcomm_topology_t topo, pmix_rank_t **children,
                          size_t *n, bool *owned)
{
    *owned = false;
    if (PRTE_GRPCOMM_TOPO_RELEASE == topo) {
        pmix_rank_t parent;
        if (PRTE_SUCCESS != prte_rml_release_tree(PRTE_PROC_MY_NAME->rank,
                                                  &parent, children, n)) {
            *children = NULL;
            *n = 0;
            return;
        }
        *owned = (NULL != *children);
        return;
    }
    *children = (pmix_rank_t *) prte_rml_base.children.array;
    *n = prte_rml_base.children.size;
}

/* How many children this daemon actually has in a tree - the count a
 * completion is measured against, and a statement about that tree alone. */
static size_t topo_n_children(prte_grpcomm_topology_t topo)
{
    pmix_rank_t *children;
    size_t i, n, live = 0;
    bool owned;

    if (PRTE_GRPCOMM_TOPO_ROUTING == topo) {
        return (size_t) prte_rml_base.n_children;
    }
    topo_children(topo, &children, &n, &owned);
    for (i = 0; i < n; i++) {
        if (PMIX_RANK_INVALID != children[i]) {
            live++;
        }
    }
    if (owned) { free(children); }
    return live;
}

static pmix_rank_t topo_parent(prte_grpcomm_topology_t topo)
{
    if (PRTE_GRPCOMM_TOPO_RELEASE == topo) {
        pmix_rank_t parent = PMIX_RANK_INVALID, *kids = NULL;
        size_t n = 0;
        if (PRTE_SUCCESS != prte_rml_release_tree(PRTE_PROC_MY_NAME->rank,
                                                  &parent, &kids, &n)) {
            return PMIX_RANK_INVALID;
        }
        if (NULL != kids) {
            free(kids);
        }
        return parent;
    }
    return PRTE_PROC_MY_PARENT->rank;
}

static void tree_whole_forward(op_t* op){
    pmix_rank_t* children;
    prte_rml_payload_t* payload;
    size_t i, nchildren;
    bool any = false, owned = false;

    topo_children(op->sig.topology, &children, &nchildren, &owned);

    for (i = 0; i < nchildren; i++) {
        if (PMIX_RANK_INVALID != children[i]) {
            any = true;
            break;
        }
    }
    if (!any) {
        if (owned) { free(children); }
        return;
    }

    payload = build_forward_payload(op);
    if (NULL == payload) {
        if (owned) { free(children); }
        return;
    }

    for (i = 0; i < nchildren; i++) {
        if (PMIX_RANK_INVALID == children[i]) {
            continue;
        }
        forward_payload_to(op, payload, children[i]);
    }

    /* every child that accepted the payload holds its own reference now; drop
     * ours, so the buffer dies with the last send rather than with this loop */
    PMIX_RELEASE(payload);
    if (owned) { free(children); }
}

/* Fault injection: hold a forward back so the op is still travelling its tree
 * when a daemon is killed underneath it.  See grpcomm_internal.h for why this
 * exists and why it refuses the routing tree. */
typedef struct {
    pmix_event_t ev;
    op_t *op;
} xcast_delay_caddy_t;

static bool forward_should_delay(prte_grpcomm_topology_t topo)
{
    if (0 >= prte_grpcomm_globals.xcast_delay_ms) {
        return false;
    }
    if (PRTE_GRPCOMM_TOPO_ROUTING == topo) {
        return false;
    }
    if (0 > prte_grpcomm_globals.xcast_delay_vpid) {
        return true;
    }
    return ((pmix_rank_t) prte_grpcomm_globals.xcast_delay_vpid
            == PRTE_PROC_MY_NAME->rank);
}

static void forward_delay_fire(int sd, short args, void *cbdata)
{
    xcast_delay_caddy_t *dc = (xcast_delay_caddy_t *) cbdata;
    op_t *op;
    bool live = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* The op may have retired while we sat on it - the fault handler retires
     * one whose subtree has emptied - and forwarding a payload out of a
     * retired op would put a broadcast on the tree that nothing is waiting
     * for.  We hold a reference, so the object is ours to read either way;
     * what has to be checked is whether it is still the live op. */
    PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
        if (op == dc->op) {
            live = true;
            break;
        }
    }
    if (live) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:xcast forwarding the held-back op",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        tree_whole_forward(dc->op);
    }
    PMIX_RELEASE(dc->op);
    free(dc);
}

static void forward_op(op_t* op){
    /* The daemon job object can be gone by the time a broadcast is being
     * forwarded - teardown retires it while the last xcasts (the halt, the
     * job-end notifications) are still moving - so this cannot be an
     * unchecked dereference. Its absence says nothing about whether to
     * forward; only the do-not-launch attribute does, and without the job
     * object nobody set it. */
    prte_job_t* daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if(NULL != daemons &&
       prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH,
                          NULL, PMIX_BOOL)){
        return;
    }

    /* A daemon reports its subtree complete once it holds the payload and
     * all its children have reported. */
    op->replay_pending_parent = false;
    op->nexpected = topo_n_children(op->sig.topology);
    op->nreported = 0;

    if (forward_should_delay(op->sig.topology)) {
        xcast_delay_caddy_t *dc = (xcast_delay_caddy_t *) calloc(1, sizeof(*dc));
        struct timeval tv;

        if (NULL != dc) {
            /* nexpected is already set, so the op cannot retire while we hold
             * the payload: it is waiting on children that have not been sent
             * to yet, which is exactly the state being manufactured */
            PMIX_RETAIN(op);
            dc->op = op;
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                 "%s grpcomm:xcast holding its forward back %d ms",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 prte_grpcomm_globals.xcast_delay_ms));
            tv.tv_sec = prte_grpcomm_globals.xcast_delay_ms / 1000;
            tv.tv_usec = (prte_grpcomm_globals.xcast_delay_ms % 1000) * 1000;
            prte_event_evtimer_set(prte_event_base, &dc->ev, forward_delay_fire, dc);
            PMIX_POST_OBJECT(dc);
            prte_event_evtimer_add(&dc->ev, &tv);
            return;
        }
    }

    tree_whole_forward(op);
}

/* A forward that never arrives is an ack that never comes.
 *
 * PRTE_RML_SEND reports only that the message was *queued*: the OOB resolves
 * the next hop on a later event, and an unreachable one is discovered there
 * (prte_oob_base_send_nb sets PRTE_ERR_ADDRESSEE_UNKNOWN and completes the
 * send through this callback).  So the rc check in forward_op_to cannot see
 * it, and without this the op waits forever on a subtree it never reached.
 *
 * That is not a hypothetical.  The routing tree holds daemons that have not
 * yet reported home - setup_virtual_machine recomputes it at launch
 * *initiation*, and must, because plm/ssh tree-spawn is driven from it - so
 * every broadcast issued during a DVM grow forwards to a child there is as yet
 * no way to reach.  errmgr/dvm deliberately swallows that send failure (it is
 * not a daemon death), which is right for the daemon and left the op stuck:
 * the master's op_id_completed stopped advancing, every later broadcast tripped
 * PRTE_ERR_OUT_OF_ORDER_MSG in finish_op, and anything waiting on the
 * completion callback - the elastic shrink campaign - waited forever (#2617).
 *
 * Dropping the expectation is the answer rather than failing the broadcast,
 * because a daemon that joins after op N was never going to see op N anyway:
 * the late-joiner catch-up in insert_forwarded_op has it adopt ops 1..N-1 as
 * complete when it arrives.  This only makes the sender agree with that.  A
 * child that genuinely died is a separate story and still takes the fault
 * handler's path, which recomputes nexpected and starts a fresh ack round. */
static void forward_lost(int status, pmix_proc_t *peer,
                         pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata)
{
    signature_t sig;
    op_t *op;

    SIG_FROM_CBDATA(sig, cbdata);

    /* keep the RML's own reporting: what a failed send means for the *daemon*
     * is the errmgr's call, and unchanged.  We only decide what it means for
     * this op. */
    prte_rml_send_callback(status, peer, buffer, tag, NULL);

    if (PRTE_SUCCESS == status) {
        return;
    }

    op = find_op(&sig);
    if (NULL == op) {
        /* the op finished (or was abandoned) while this send was in flight */
        return;
    }

    PMIX_OUTPUT_VERBOSE((
        1, prte_grpcomm_globals.output,
        "%s grpcomm:xcast op %lu never reached %s (%s) - dropping its subtree "
        "from the ack rollup",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned long) sig.op_id,
        (NULL == peer) ? "someone" : PRTE_NAME_PRINT(peer),
        PRTE_ERROR_NAME(status)
    ));

    if (0 < op->nexpected) {
        op->nexpected--;
    }
    drive_completions();
}

/* Pack the forward once.  Returns NULL having already reported the failure -
 * a forward we cannot build is not something any caller can carry on past. */
static prte_rml_payload_t* build_forward_payload(op_t* op){
    pmix_data_buffer_t* xcast_msg = PMIx_Data_buffer_create();
    prte_rml_payload_t* payload;

    /* pack_forward_msg answers in PMIx statuses */
    int rc = pack_forward_msg(xcast_msg, op);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return NULL;
    }

    payload = PMIX_NEW(prte_rml_payload_t);
    payload->dbuf = xcast_msg;
    return payload;
}

/* An edge of a derived tree is sent DIRECT, and the peer is registered.
 *
 * The routed send picks the next hop the *routing* tree would take, so an edge
 * that is not also a routing edge gets relayed - and at a high routing radix
 * the relay is the controller itself.  The bytes then cross the very link the
 * second tree exists to keep them off, the controller handles them twice, and
 * the fanout is not reduced at all: measured on eight daemons at routing radix
 * 64 with release radix 2, seven of nine release edges went back through rank
 * 0.  A direct send is what makes the tree mean anything, and the design said
 * so from the start ("A radix-3 release edge is a sibling edge in a radix-64
 * routing tree, so this needs direct sends to non-children").
 *
 * Registering the peer is a separate obligation and it is about faults, not
 * delivery: losing a lateral link means the tree has NOT changed shape, and
 * repairing on the strength of it would end every in-flight collective in the
 * DVM.  prte_rml_is_lateral_only() is the test the fault path uses, and it
 * answers false for anything that is also a tree neighbour, so registering a
 * peer that happens to be our parent or child costs nothing.
 *
 * The routing tree keeps the routed send unchanged: there every edge IS the
 * route. */
static bool edge_is_direct(prte_grpcomm_topology_t topo, pmix_rank_t dest)
{
    if (PRTE_GRPCOMM_TOPO_ROUTING == topo) {
        return false;
    }
    if (dest != prte_rml_get_route(dest)) {
        /* not reachable in one routing hop, so this is a genuine lateral edge
         * and the fault machinery has to be told about the link */
        prte_rml_lateral_register(dest);
    }
    return true;
}

static void forward_payload_to(op_t* op, prte_rml_payload_t* payload,
                               pmix_rank_t dest){
    int rc;

    PMIX_OUTPUT_VERBOSE((
        5, prte_grpcomm_globals.output,
        "%s grpcomm:send_relay sending relay msg of %d bytes to %s",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) payload->dbuf->bytes_used,
        PRTE_VPID_PRINT(dest)
    ));

    /* The op-id is carried by value rather than by pointer: the send outlives
     * the op on precisely the paths that matter, so the callback has to be
     * able to look the op up and find it gone. */
    if (edge_is_direct(op->sig.topology, dest)) {
        PRTE_RML_SEND_PAYLOAD_DIRECT_CB(rc, dest, payload, PRTE_RML_TAG_XCAST,
                                        forward_lost, SIG_TO_CBDATA(op->sig));
    } else {
        PRTE_RML_SEND_PAYLOAD_CB(rc, dest, payload, PRTE_RML_TAG_XCAST,
                                 forward_lost, SIG_TO_CBDATA(op->sig));
    }
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        /* a refused send took no reference, so there is nothing to unwind -
         * the payload is still the caller's to release */
        return;
    }
}

static void forward_op_to(op_t* op, pmix_rank_t dest){
    prte_rml_payload_t* payload = build_forward_payload(op);

    if (NULL == payload) {
        return;
    }
    forward_payload_to(op, payload, dest);
    PMIX_RELEASE(payload);
}

static void process_wireup(pmix_data_buffer_t *msg){
    if(PRTE_PROC_IS_MASTER) return;

    int ret = prte_util_decode_nidmap(msg);
    if(PMIX_SUCCESS != ret){
       PMIX_ERROR_LOG(ret);
       PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
       return;
    }

    /* the jobs already running in this DVM, which we may never have heard of
     * if we only just joined it. Must be read here, after the nidmap (which
     * is what binds each daemon to its node) and before the per-daemon
     * records below, because that loop runs to the end of the buffer. */
    ret = prte_util_decode_job_catchup(msg);
    if(PRTE_SUCCESS != ret){
       PRTE_ERROR_LOG(ret);
       PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
       return;
    }

    /* the collective recovery epoch the DVM has reached.  This is how a daemon
     * that joined an already-recovered DVM learns it: the failure notices that
     * moved the epoch were broadcast before this daemon was routable, so they
     * never arrived, and its contributions would be dropped as stale forever
     * after (see the epoch member of prte_rml_recovery_status_t).  Adopting is
     * by highest value seen, so a daemon already at or past this one is
     * unaffected and no in-flight notice can be undone by a late wireup. */
    /* Whether we joined a DVM that was already running collectives - see the
     * matching note where this is packed, and prte_grpcomm_fence_gen_baseline().
     * Only the first wireup is honoured, so a daemon already in the DVM keeps
     * the answer it was given when it arrived. */
    bool grown = false;
    int gcnt = 1;
    ret = PMIx_Data_unpack(NULL, msg, &grown, &gcnt, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
       PMIX_ERROR_LOG(ret);
       PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
       return;
    }
    prte_grpcomm_fence_note_join(grown);

    uint32_t epoch = 0;
    int ecnt = 1;
    ret = PMIx_Data_unpack(NULL, msg, &epoch, &ecnt, PMIX_UINT32);
    if(PMIX_SUCCESS != ret){
       PMIX_ERROR_LOG(ret);
       PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
       return;
    }
    prte_grpcomm_advance_epoch(epoch);

    pmix_value_t val = PMIX_VALUE_STATIC_INIT;
    pmix_value_t sval = PMIX_VALUE_STATIC_INIT;
    pmix_proc_t dmn;
    int cnt = 1;
    do {
        PMIx_Value_destruct(&val);
        PMIx_Value_destruct(&sval);
        ret = PMIx_Data_unpack(NULL, msg, &dmn, &cnt, PMIX_PROC);
        if(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == ret) return;
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }

        PMIx_Value_construct(&val);
        val.type = PMIX_STRING;

        ret = PMIx_Data_unpack(NULL, msg, &val.data.string, &cnt, PMIX_STRING);
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }

        /* that node's PMIx SERVER rendezvous URI - for tools asking where a
         * given node's server is, never for reaching the daemon (that is the
         * PROC_URI above). It is a per-record field, so it MUST be unpacked
         * here, before any of the skips below: a `continue` that left it in
         * the buffer would make the next iteration read this string as a
         * pmix_proc_t. May be NULL if that daemon reported none. */
        PMIx_Value_construct(&sval);
        sval.type = PMIX_STRING;

        ret = PMIx_Data_unpack(NULL, msg, &sval.data.string, &cnt, PMIX_STRING);
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }

        /* store it for every daemon but ourselves - PMIx already holds our
         * own server's URI, and unlike the PROC_URI skips below we have no
         * other source for the HNP's or our parent's */
        if(!PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_NAME) && NULL != sval.data.string){
            ret = PMIx_Store_internal(&dmn, PMIX_SERVER_URI, &sval);
            if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }
        }

        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_HNP)) continue;
        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_NAME)) continue;
        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_PARENT)) continue;

        ret = PMIx_Store_internal(&dmn, PMIX_PROC_URI, &val);
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }
    } while(PMIX_SUCCESS == ret);

    /* Reaching here means the loop broke on a real unpack failure - a wireup
     * we cannot read leaves this daemon with no route to anyone, so it ends
     * the job. The clean end of the record stream is the
     * READ_PAST_END_OF_BUFFER return inside the loop, which is why that one
     * is a return and not a break. */
    if(val.type != PMIX_UNDEF) PMIx_Value_destruct(&val);
    if(sval.type != PMIX_UNDEF) PMIx_Value_destruct(&sval);
    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    return;
}

static void process_msg(op_t* op){
    int ret = PMIX_SUCCESS;

    if(op->processed) return;
    op->processed = true;

    /* The other end of the coverage measurement started in
     * prte_grpcomm_xcast_recv - this is the moment this daemon actually has
     * the payload, which for a fence release is the moment its clients are
     * about to be let go. */
    if (prte_grpcomm_globals.enable_timing) {
        struct timeval tnow;
        gettimeofday(&tnow, NULL);
        pmix_output(0, "%s grpcomm:xcast:timing processed op_id %lu tag %u "
                    "tree %d at %ld.%06ld",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    (unsigned long) op->sig.op_id, (unsigned) op->msg_tag,
                    (int) op->sig.topology, (long) tnow.tv_sec,
                    (long) tnow.tv_usec);
    }

    pmix_data_buffer_t *msg = PMIx_Data_buffer_create();
    if(op->msg_compressed){
        pmix_byte_object_t decomp_msg = PMIX_BYTE_OBJECT_STATIC_INIT;
        bool success = PMIx_Data_decompress(
            (uint8_t* ) op->msg.bytes, op->msg.size,
            (uint8_t**) &decomp_msg.bytes, &decomp_msg.size
        );
        if(!success){
            prte_show_help("help-prte-runtime.txt", "failed-to-uncompress",
                           true, prte_process_info.nodename);
            PMIX_BYTE_OBJECT_DESTRUCT(&decomp_msg);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIx_Data_buffer_release(msg);
            return;
        }
        ret = PMIx_Data_load(msg, &decomp_msg);
    } else {
        ret = PMIx_Data_embed(msg, &op->msg);
    }
    if(PMIX_SUCCESS != ret){
        /* this ends the DVM, so it must say why - the decompress failure
         * above has its own show_help, and this path had nothing at all */
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
        return;
    }

    if(PRTE_RML_TAG_WIREUP == op->msg_tag){
        process_wireup(msg);
    } else {
        /* pass the relay buffer to myself for processing - don't inject it into
         * the RML system via send as that will compete with the relay messages
         * down in the OOB. Instead, pass it directly to the RML message
         * processor */
        PRTE_RML_POST_MESSAGE(
            PRTE_PROC_MY_NAME, op->msg_tag, 1, msg->base_ptr, msg->bytes_used
        );
        msg->base_ptr = NULL;
        msg->bytes_used = 0;
    }

    PMIx_Data_buffer_release(msg);
    return;
}

static void op_con(op_t* p)
{
    /* Every field, because PMIX_NEW mallocs without zeroing - a member left
     * out here is heap garbage that gets packed onto the wire. The topology
     * was exactly that until the swarm refused the first broadcast of the
     * run: TOPO_ROUTING is zero, but nothing was setting it to zero. */
    p->sig.op_id = 0;
    p->sig.topology = PRTE_GRPCOMM_TOPO_ROUTING;


    p->processed = false;
    p->replay_pending_parent = false;
    p->upstream = PMIX_RANK_INVALID;
    p->awaiting_news = false;

    p->nreported = 0;
    /* "not yet computed" - forward_op() sets the real count. Deliberately the
     * largest value rather than zero, so an op that has not been forwarded can
     * never satisfy op_ready() and retire without having been sent anywhere */
    p->nexpected = SIZE_MAX;
    p->ack_id_up = 0;
    p->ack_id_down = 0;

    PMIx_Byte_object_construct(&p->msg);
    p->msg_compressed = false;
    p->msg_tag = PRTE_RML_TAG_INVALID;

    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void op_des(op_t* p)
{
    PMIX_BYTE_OBJECT_DESTRUCT(&p->msg);
}
PMIX_CLASS_INSTANCE(op_t, pmix_list_item_t, op_con, op_des);


static void xcast_con(prte_grpcomm_xcast_t* p)
{
    PMIX_CONSTRUCT(&p->ops, pmix_list_t);
    PMIX_CONSTRUCT(&p->pending_completions, pmix_list_t);
    for(int t = 0; t < PRTE_GRPCOMM_TOPO_COUNT; t++){
        p->tree[t].op_id_completed = 0;
        p->tree[t].op_id_completed_at_promotion = 0;
        p->tree[t].op_id_inited = 0;
    }
}
static void xcast_des(prte_grpcomm_xcast_t* p)
{
    PMIX_LIST_DESTRUCT(&p->ops);
    PMIX_LIST_DESTRUCT(&p->pending_completions);
}
PMIX_CLASS_INSTANCE(prte_grpcomm_xcast_t, pmix_object_t, xcast_con, xcast_des);

