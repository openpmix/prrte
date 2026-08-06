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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

/* internal signature used to uniquely track a particular xcast */
typedef struct {
    size_t op_id;    // HNP's assigned collective ID, globally unique
} signature_t;

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
    // How this broadcast's bytes physically travel. Chosen by the originator
    // and carried on the wire, so every daemon moves the payload the same way.
    // A broadcast has a single originator, so unlike an allgather there is
    // nothing to agree on - whoever started it decides, and says so.
    uint32_t movement;
    // Do we hold the WHOLE payload? tree_whole answers yes the moment the op
    // arrives; a scatter+allgather cannot answer yes until its exchange has
    // closed. Local delivery and the ack to our parent both gate on this
    // rather than on "the op exists", which is the same statement only for
    // tree_whole.
    bool payload_complete;
    // Scatter+allgather state, NULL under tree_whole and NULL on the
    // controller's own op until it chunks the payload.
    struct bulk_state_t *bulk;
} op_t;
PMIX_CLASS_DECLARATION(op_t);

/* Scatter+allgather state for one broadcast.
 *
 * The participant list is NOT derived locally. Every daemon has to agree on
 * which exchange position is which rank, and deriving that from the
 * failed-daemon set would have daemons disagree in the window around a fault -
 * the exchange would then deadlock, each waiting on a partner the other does
 * not believe exists. So the controller stamps the list into the scatter
 * message and everyone reads it. At four bytes a rank that is 16 KB for a
 * 4096-daemon DVM, carried once alongside a payload that is large by
 * definition. This rule is load-bearing: do not replace it with a locally
 * computed set. */
typedef struct bulk_state_t {
    pmix_rank_t *parts;         /* participant ranks; position i is parts[i] */
    size_t nparts;
    size_t mypos;               /* our position, or SIZE_MAX if we are not one */
    size_t total;               /* bytes in the whole (possibly compressed) payload */
    pmix_byte_object_t *chunks; /* [nparts], indexed by POSITION */
    bool *held;                 /* [nparts]; a real chunk may be zero-length, so
                                 * presence cannot be read off the pointer */
    size_t nheld;
    size_t step;                /* next Bruck step to run */
} bulk_state_t;

/* What a message on PRTE_RML_TAG_XCAST_BULK is. */
#define BULK_KIND_CHUNKS  0u  /* blocks from an exchange partner */
#define BULK_KIND_REPLAY  1u  /* "I cannot finish - send it the tree way" */

/* Chunks that arrived before the scatter that would have created their op.
 * Held whole rather than decoded, because until the op exists we do not know
 * the participant list the positions in them are relative to. */
typedef struct {
    pmix_list_item_t super;
    size_t op_id;
    pmix_data_buffer_t buf;
} early_chunks_t;
PMIX_CLASS_DECLARATION(early_chunks_t);

/* How a broadcast's payload reaches the daemons below us.
 *
 * Everything around this - op-id sequencing, the ACK rollup, the
 * process_first ordering set, late-joiner catch-up, the promotion replay hold
 * - is identical whichever movement is in use, and is the hard part. Keep it
 * that way: a new movement supplies data transport, not a second copy of the
 * reliability machinery. */
typedef struct {
    uint32_t id;                    /* stamped on the wire */
    const char *name;
    void (*forward)(op_t *op);      /* emit the payload to our subtree */
} bcast_movement_t;

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
// Locally process the message being broadcast, if not already done
static void process_msg(op_t *op);
// Ack that myself and my full subtree have received this message
static void send_ack(signature_t* sig, pmix_rank_t ack_id);
// Request an ack after a failure without resending full user message
static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id);
// Remove local tracking and ack to parent
static void finish_op(op_t *op);
// Finish every op that is now both complete and next in op-id order
static void drive_completions(void);
// Give up on this op's exchange and get the payload the tree way
static void bulk_degrade(op_t *op);
// The movements themselves
static void tree_whole_forward(op_t *op);
static void scatter_allgather_forward(op_t *op);
// How this broadcast will travel - decided by its originator, and only there
static uint32_t select_movement(prte_rml_tag_t tag, size_t nbytes);
// Is an op ahead of this one in op-id order still waiting on its payload?
static bool op_blocked(const op_t *op);

// The bulk movement's state, transport and reassembly
static void bulk_free(op_t *op);
static void bulk_progress(op_t *op);
static bool bulk_payload_ready(op_t *op);
static int bulk_absorb_chunks(pmix_data_buffer_t *buffer, op_t *op);
static void bulk_purge_early(void);
static int bulk_attach(op_t *op, pmix_rank_t *parts, size_t nparts, size_t total);
static int bulk_store(bulk_state_t *b, size_t pos, const pmix_byte_object_t *src);
static void bulk_drain_early(op_t *op);
static int unpack_scatter_msg(pmix_data_buffer_t *buffer, op_t *op);

// Pack the xcast message forwarded to one of our children
static int pack_forward_msg(pmix_data_buffer_t *buffer, op_t *op,
                            pmix_rank_t dest);
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
static int pack_movement  (pmix_data_buffer_t* buffer, uint32_t* movement);
static int unpack_movement(pmix_data_buffer_t* buffer, uint32_t* movement);

int prte_grpcomm_xcast(prte_rml_tag_t tag, pmix_data_buffer_t *msg){
    return prte_grpcomm_xcast_nb(tag, msg, NULL, NULL);
}

int prte_grpcomm_xcast_nb(prte_rml_tag_t tag, pmix_data_buffer_t *msg,
                                 prte_grpcomm_xcast_complete_fn_t cbfunc,
                                 void *cbdata){
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) msg->bytes_used));

    op_t* op = PMIX_NEW(op_t);
    op->msg_tag = tag;
    /* stash the completion callback on the initiating op.  It is not fired from
     * this op (which is discarded after begin_xcast relays it); begin_xcast
     * copies it into the pending-completion FIFO once the broadcast is actually
     * sent, and finish_op fires it from the op the master builds on receipt. */
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    /* Make a (possibly compressed) copy of this message in a new op - this is
     * non-destructive, so our caller is still responsible for releasing any
     * memory in the buffer they gave us
     */
    op->msg_compressed = (bool) PMIx_Data_compress(
        (uint8_t*) msg->base_ptr, msg->bytes_used,
        (uint8_t**) &op->msg.bytes, &op->msg.size
    );
    if(!op->msg_compressed){
        pmix_data_buffer_t msg_copy;
        PMIx_Data_buffer_construct(&msg_copy);

        int rc = PMIx_Data_copy_payload(&msg_copy, msg);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Data_buffer_destruct(&msg_copy);
            PMIX_RELEASE(op);
            return rc;
        }

        PMIx_Data_unload(&msg_copy, &op->msg);
        PMIx_Data_buffer_destruct(&msg_copy);
    }

    /* must push this into the event library to ensure we can
     * access framework-global data safely */

    prte_event_set(prte_event_base, &op->ev, -1, PRTE_EV_WRITE, begin_xcast, op);
    PMIX_POST_OBJECT(&op);
    prte_event_active(&op->ev, PRTE_EV_WRITE, 1);

    return PMIX_SUCCESS;
}

void prte_grpcomm_xcast_recv(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);
    if(!PRTE_PROC_IS_MASTER && sender->rank != PRTE_PROC_MY_PARENT->rank){
        // Ignore messages from old parents
        return;
    }
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) buffer->bytes_used));

    signature_t sig;
    if(PMIX_SUCCESS != unpack_sig(buffer, &sig)) return;

    /* An op-id of zero is an originator relaying to the controller, not a
     * forward down the tree. The distinction decides how the payload that
     * follows is laid out: the relay always carries it whole, because that hop
     * is an ordinary point-to-point send even when the broadcast is going to
     * be scattered afterwards. Capture it before the controller stamps an id
     * on the signature below. */
    bool initiating = (0 == sig.op_id);

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
    bool late_joiner = !PRTE_PROC_IS_MASTER && (0 == XCAST.op_id_inited);

    if(PRTE_PROC_IS_MASTER){
        if(sig.op_id){
            // If I'm HNP, I expect sender has not assigned a global ID
            PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
            return;
        }
        sig.op_id = ++XCAST.op_id_inited;
    }
    if(!sig.op_id){
        PRTE_ERROR_LOG( PRTE_ERR_NOT_INITIALIZED );
        return;
    }
    if(sig.op_id > XCAST.op_id_inited){
        XCAST.op_id_inited = sig.op_id;
    }
    if(late_joiner && sig.op_id > 1){
        // Catch up to just below this op: ops 1..op_id-1 are taken as complete,
        // and this op is processed in order as our first.
        XCAST.op_id_completed = sig.op_id - 1;
        XCAST.op_id_completed_at_promotion = sig.op_id - 1;
    }

    // If we marked our subtree as completed, but then were promoted, our
    // subtree is now larger and may not have actually completed everywhere.
    // But ops complete in order, so if we have completed anything since our
    // promotion, we know our new subtree has also completed all the older ops
    bool assume_incomplete =
        sig.op_id <= XCAST.op_id_completed_at_promotion
        && XCAST.op_id_completed == XCAST.op_id_completed_at_promotion;

    // If we're certain our subtree has already completed this, we can just ack
    bool complete = !assume_incomplete &&
        sig.op_id <= XCAST.op_id_completed;

    pmix_rank_t ack_id;
    if(PMIX_SUCCESS != unpack_ack_id(buffer, &ack_id)) return;

    /* The movement sits ahead of the payload on the wire, so it is read here
     * rather than beside the unpack that consumes the payload - reading it
     * later would take the movement's bytes as the start of the message. */
    uint32_t movement;
    if(PMIX_SUCCESS != unpack_movement(buffer, &movement)) return;

    if(complete) {
        send_ack(&sig, ack_id);
        return;
    }

    /* Is the payload that follows a partition of the message, or the whole of
     * it? Both ends answer this the same way: the sender packs the scatter
     * form only for a tree forward of a still-bulk op, which is exactly this
     * condition. A degraded op has had its movement flipped to tree_whole
     * before it was packed, so a replay reads as whole here - which is what
     * makes the fallback work at all. */
    bool scattered = (PRTE_GRPCOMM_BCAST_SCATTER_ALLGATHER == movement) &&
                     !initiating;

    op_t* op = find_op(&sig);
    if(NULL != op && !op->payload_complete && !scattered){
        /* A degraded replay for an op we are still assembling piecemeal: it
         * carries the whole payload, so take it and abandon the exchange. The
         * usual path below does not re-read the payload of an op it already
         * has, which for every other kind of replay is right - there is
         * nothing new in it. */
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:xcast:recv: taking whole replay of "
                             "op %lu", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned long) sig.op_id));
        bulk_free(op);
        op->movement = movement;
        PMIX_BYTE_OBJECT_DESTRUCT(&op->msg);
        if(PMIX_SUCCESS != unpack_msg(buffer, op)){
            pmix_list_remove_item(&XCAST.ops, &op->super);
            PMIX_RELEASE(op);
            return;
        }
        op->payload_complete = true;
    }
    if(NULL == op){
        op = insert_forwarded_op(&sig);
        op->movement = movement;
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
        int urc = scattered ? unpack_scatter_msg(buffer, op)
                            : unpack_msg(buffer, op);
        if(PMIX_SUCCESS != urc){
            pmix_list_remove_item(&XCAST.ops, &op->super);
            PMIX_RELEASE(op);
            return;
        }
        /* A scatter hands us a fraction of the message; the exchange has to
         * close before anything may be delivered from it. */
        if(scattered) op->payload_complete = false;
    }

    op->ack_id_up = ack_id;
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
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), op->msg_tag, sig.op_id
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
        if(sender->rank != PRTE_PROC_MY_PARENT->rank){
            // Old message
            return;
        }
        if(NULL != op){
            // We'll send with the new id once we're done
            op->ack_id_up = ack_id;
        } else if(sig.op_id <= XCAST.op_id_completed){
            // We've finished this one, ack now
            send_ack(&sig, ack_id);
        } else {
            // We haven't seen this xcast before
            PRTE_ERROR_LOG( PRTE_ERR_OUT_OF_ORDER_MSG );
        }
    } else {
        if(NULL == op || op->ack_id_down != ack_id) return;
        op->nreported++;
        if(op->nreported == op->nexpected){
            /* The subtree has reported. Whether that finishes the op also
             * depends on whether we hold the payload, which under
             * scatter+allgather routinely lags our children - their subtrees
             * can assemble before ours does. */
            drive_completions();
        } else if(op->nreported > op->nexpected){
            PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
        }
    }
}

void prte_grpcomm_xcast_bulk_recv(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);

    signature_t sig;
    uint8_t kind = BULK_KIND_CHUNKS;
    int cnt = 1;
    int rc;
    op_t* op;

    rc = PMIx_Data_unpack(NULL, buffer, &sig.op_id, &cnt, PMIX_SIZE);
    if(PMIX_SUCCESS == rc){
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &kind, &cnt, PMIX_UINT8);
    }
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        return;
    }

    op = find_op(&sig);

    if(BULK_KIND_REPLAY == kind){
        /* A participant cannot finish its exchange and wants the payload the
         * tree way. Only the controller is certain to hold all of it. A
         * request for an op we no longer have is not an error: it finished
         * here before the request arrived, which means the payload is already
         * on its way down as somebody else's replay. */
        if(!PRTE_PROC_IS_MASTER){
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return;
        }
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:xcast:bulk %s cannot finish op %lu",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (NULL == sender) ? "someone"
                                              : PRTE_VPID_PRINT(sender->rank),
                             (unsigned long) sig.op_id));
        if(NULL != op) bulk_degrade(op);
        drive_completions();
        return;
    }

    if(NULL != op && NULL == op->bulk){
        /* The op gave up on its exchange and is taking the payload down the
         * tree instead, so these blocks are answers to a question no longer
         * being asked. */
        return;
    }
    if(NULL == op){
        /* Either the scatter that creates this op has not reached us yet - the
         * two phases travel different routes, so a partner nearer the root can
         * be a step ahead - or the op is over. Park what we cannot yet place;
         * the positions in it are relative to a participant list we have not
         * been given. */
        if(sig.op_id <= XCAST.op_id_completed) return;
        early_chunks_t* e = PMIX_NEW(early_chunks_t);
        e->op_id = sig.op_id;
        if(PMIX_SUCCESS != PMIx_Data_copy_payload(&e->buf, buffer)){
            PMIX_RELEASE(e);
            return;
        }
        pmix_list_append(&XCAST.early_chunks, &e->super);
        return;
    }

    if(PMIX_SUCCESS != bulk_absorb_chunks(buffer, op)){
        bulk_degrade(op);
        drive_completions();
        return;
    }

    /* Whatever just landed may have unblocked the next step, and the step
     * after that if this message was itself late. */
    bulk_progress(op);
    if(bulk_payload_ready(op)){
        process_msg(op);
    }
    drive_completions();
}

void prte_grpcomm_xcast_lateral_lost(pmix_rank_t rank)
{
    op_t* op;
    op_t* next_op;
    bool touched = false;

    /* The RML has told us a link we opened for an exchange has dropped, and
     * deliberately said nothing about whether the peer is dead - that is the
     * tree's business, not ours. Either way the exchange it was carrying
     * cannot close, so any op that named this rank as a participant falls back
     * to the tree. */
    PMIX_LIST_FOREACH_SAFE(op, next_op, &XCAST.ops, op_t){
        if(NULL == op->bulk) continue;
        for(size_t i = 0; i < op->bulk->nparts; i++){
            if(op->bulk->parts[i] != rank) continue;
            bulk_degrade(op);
            touched = true;
            break;
        }
    }
    if(touched) drive_completions();
}

void prte_grpcomm_xcast_fault_handler(
    const prte_rml_recovery_status_t* status
) {
    // We must do all xcast handling in the local scope, since reliable xcasts
    // is how we get the global scope notifications in the first place
    if(status->scope != PRTE_RML_FAULT_SCOPE_LOCAL) return;

    if(status->promoted){
        XCAST.op_id_completed_at_promotion =
            XCAST.op_id_completed;
    }
    if(status->parent_changed || status->promoted || status->demoted){
        // Avoid confusing new parent by accidentally acking with
        // the valid ack id. They'll tell us what id to use.
        op_t* op;
        PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
            op->ack_id_up = PMIX_RANK_INVALID;
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
            if(0 == prte_rml_base.n_children){
                /* Under tree_whole both tests are constants - every op holds
                 * its payload and nothing can be blocked - so this is the
                 * unconditional finish it has always been. */
                if(op->payload_complete && !op_blocked(op)) finish_op(op);
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

    /* Any exchange still running has lost a participant, and no rearrangement
     * of the survivors can produce the block that participant owed. Give up on
     * it and let the payload come down the repaired tree instead. Done after
     * the tree work above so the replays it sends go to the new children. */
    {
        op_t* op;
        op_t* next_op;
        PMIX_LIST_FOREACH_SAFE(op, next_op, &XCAST.ops, op_t){
            if(NULL != op->bulk) bulk_degrade(op);
        }
    }
    drive_completions();
}

static void begin_xcast(int sd, short args, void* cbdata){
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    op_t* op = (op_t*) cbdata;
    PMIX_ACQUIRE_OBJECT(&op);

    /* Pick the data movement here, on the daemon that originated this
     * broadcast, and from the size actually going on the wire - the compressed
     * size, since that is what would be scattered. Every other daemon reads
     * the choice off the wire, so there is nothing to agree on and no way for
     * two of them to disagree about a broadcast in flight.
     *
     * Done here rather than in the entry point because the selection reads the
     * DVM's size out of prte_rml_base, which belongs to the progress thread -
     * and this handler is the first point that is running on it. */
    op->movement = select_movement(op->msg_tag, op->msg.size);

    // setup the payload
    pmix_data_buffer_t *xcast_msg = PMIx_Data_buffer_create();
    int rc = pack_relay_msg(xcast_msg, op);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        PMIX_RELEASE(op);
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
        PMIX_RELEASE(op);
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

    PRTE_RML_SEND(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
    }
}

static void send_ack(signature_t* sig, pmix_rank_t ack_id){
    if(PRTE_PROC_IS_MASTER) return;
    send_ack_msg(sig, ack_id, false, PRTE_PROC_MY_PARENT->rank);
}

static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id){
    send_ack_msg(sig, ack_id, true, from);
}

/* Is this op ready to be settled up - do we hold the payload, and has our
 * whole subtree reported? */
static bool op_ready(const op_t* op){
    return op->payload_complete && op->nreported >= op->nexpected;
}

/* Is an earlier op still waiting on its payload?
 *
 * A daemon completes ops in op-id order, and finish_op enforces that with a
 * hard error. Mixed movement makes it possible to break it honestly for the
 * first time: a bulk op N is still assembling over lateral links while a tiny
 * op N+1 behind it arrives and completes down the tree in one hop. So op N+1
 * waits, and drive_completions releases it when N settles.
 *
 * The test is deliberately "is an earlier op still short of its payload"
 * rather than "am I next in sequence". Only a bulk op can answer yes to that,
 * so a DVM using tree_whole throughout takes exactly the path it always did,
 * including the existing out-of-order diagnostic - this hold cannot mask it.
 * XCAST.ops is kept in op-id order, so the walk stops at ourselves. */
static bool op_blocked(const op_t* op){
    op_t* prev;
    PMIX_LIST_FOREACH(prev, &XCAST.ops, op_t){
        if(prev->sig.op_id >= op->sig.op_id) break;
        if(!prev->payload_complete) return true;
    }
    return false;
}

static void drive_completions(void){
    bool progress = true;
    while(progress){
        op_t* op;
        op_t* next;
        progress = false;
        PMIX_LIST_FOREACH_SAFE(op, next, &XCAST.ops, op_t){
            if(!op_ready(op) || op_blocked(op)) continue;
            finish_op(op);
            /* finish_op unlinked and released op, and may have unblocked ops
             * behind it, so restart the walk rather than trust the cursor */
            progress = true;
            break;
        }
    }
    bulk_purge_early();
}


static void finish_op(op_t* op) {
    send_ack(&op->sig, op->ack_id_up);
    pmix_list_remove_item(&XCAST.ops, &op->super);
    if(op->sig.op_id > XCAST.op_id_completed_at_promotion){
        if(op->sig.op_id != XCAST.op_id_completed+1){
            PRTE_ERROR_LOG( PRTE_ERR_OUT_OF_ORDER_MSG );
        } else {
            XCAST.op_id_completed++;
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
static int pack_movement(pmix_data_buffer_t* buffer, uint32_t* movement){
    DIRECT_XCAST_PACK(buffer, movement, PMIX_UINT32);
    return PMIX_SUCCESS;
}

static int unpack_movement(pmix_data_buffer_t* buffer, uint32_t* movement){
    DIRECT_XCAST_UNPACK(buffer, movement, PMIX_UINT32);
    return PMIX_SUCCESS;
}

static int pack_bool(pmix_data_buffer_t* buffer, bool* boolean){
    DIRECT_XCAST_PACK(buffer, boolean, PMIX_BOOL);
    return PMIX_SUCCESS;
}

static int unpack_sig(pmix_data_buffer_t* buffer, signature_t* sig){
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

/* The scatter's body: the participant list every daemon reads its position out
 * of, the size of the whole payload so the seams can be recomputed locally,
 * and the chunks this particular destination's subtree owns. */
static int pack_scatter_msg(pmix_data_buffer_t* buffer, op_t* op,
                            pmix_rank_t dest){
    bulk_state_t* b = op->bulk;
    size_t nchunks = 0;

    DIRECT_XCAST_PACK(buffer, &op->msg_tag,        PRTE_RML_TAG);
    DIRECT_XCAST_PACK(buffer, &op->msg_compressed, PMIX_BOOL);
    DIRECT_XCAST_PACK(buffer, &b->total,           PMIX_SIZE);
    DIRECT_XCAST_PACK(buffer, &b->nparts,          PMIX_SIZE);
    for(size_t i = 0; i < b->nparts; i++){
        DIRECT_XCAST_PACK(buffer, &b->parts[i], PMIX_PROC_RANK);
    }

    /* Which chunks belong below this child: the ones whose owner routes
     * through it. get_subtree_index answers with the child slot that contains
     * a rank, so comparing the owner's slot to the destination's is the whole
     * partition - no separate map to keep in step with the tree. */
    int dest_idx = prte_rml_get_subtree_index(dest);
    for(size_t i = 0; i < b->nparts; i++){
        if(!b->held[i]) continue;
        if(prte_rml_get_subtree_index(b->parts[i]) != dest_idx) continue;
        nchunks++;
    }
    DIRECT_XCAST_PACK(buffer, &nchunks, PMIX_SIZE);
    for(size_t i = 0; i < b->nparts; i++){
        if(!b->held[i]) continue;
        if(prte_rml_get_subtree_index(b->parts[i]) != dest_idx) continue;
        DIRECT_XCAST_PACK(buffer, &i,          PMIX_SIZE);
        DIRECT_XCAST_PACK(buffer, &b->chunks[i], PMIX_BYTE_OBJECT);
    }
    return PMIX_SUCCESS;
}

/* Read a scatter body.  Attaches the exchange state, because the participant
 * list arriving here is the only place it comes from. */
static int unpack_scatter_msg(pmix_data_buffer_t* buffer, op_t* op){
    size_t total = 0, nparts = 0, nchunks = 0;
    pmix_rank_t* parts;
    /* not "rc": DIRECT_XCAST_UNPACK opens a block declaring one of its own,
     * and -Wshadow is an error here */
    int ret;

    DIRECT_XCAST_UNPACK(buffer, &op->msg_tag,        PRTE_RML_TAG);
    DIRECT_XCAST_UNPACK(buffer, &op->msg_compressed, PMIX_BOOL);
    DIRECT_XCAST_UNPACK(buffer, &total,              PMIX_SIZE);
    DIRECT_XCAST_UNPACK(buffer, &nparts,             PMIX_SIZE);
    if(0 == nparts || nparts > (size_t) INT32_MAX){
        /* A truncated or corrupt message unpacks to this, and the allocation
         * below would otherwise be sized from it. */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    parts = (pmix_rank_t*) malloc(nparts * sizeof(pmix_rank_t));
    if(NULL == parts) return PRTE_ERR_OUT_OF_RESOURCE;
    for(size_t i = 0; i < nparts; i++){
        int _cnt = 1;
        ret = PMIx_Data_unpack(NULL, buffer, &parts[i], &_cnt, PMIX_PROC_RANK);
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            free(parts);
            return ret;
        }
    }
    /* bulk_attach owns parts from here, on success and failure alike */
    ret = bulk_attach(op, parts, nparts, total);
    if(PRTE_SUCCESS != ret){
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    DIRECT_XCAST_UNPACK(buffer, &nchunks, PMIX_SIZE);
    for(size_t i = 0; i < nchunks; i++){
        pmix_byte_object_t chunk = PMIX_BYTE_OBJECT_STATIC_INIT;
        size_t pos = 0;
        int _cnt = 1;

        ret = PMIx_Data_unpack(NULL, buffer, &pos, &_cnt, PMIX_SIZE);
        if(PMIX_SUCCESS == ret){
            _cnt = 1;
            ret = PMIx_Data_unpack(NULL, buffer, &chunk, &_cnt, PMIX_BYTE_OBJECT);
        }
        if(PMIX_SUCCESS != ret){
            PMIX_ERROR_LOG(ret);
            return ret;
        }
        ret = bulk_store(op->bulk, pos, &chunk);
        PMIX_BYTE_OBJECT_DESTRUCT(&chunk);
        if(PRTE_SUCCESS != ret){
            PRTE_ERROR_LOG(ret);
            return ret;
        }
    }
    return PMIX_SUCCESS;
}

/* The relay from an originator to the controller.  Always carries the whole
 * payload however the broadcast is going to travel afterwards: this hop is a
 * point-to-point send, not the scatter. The receiver tells the two apart by
 * the op-id, which is zero here and assigned by the controller. */
static int pack_relay_msg(pmix_data_buffer_t* buffer, op_t* op){
    int rc = pack_sig(buffer, &op->sig);
    if(PMIX_SUCCESS == rc) rc = pack_ack_id(buffer, &op->ack_id_down);
    if(PMIX_SUCCESS == rc) rc = pack_movement(buffer, &op->movement);
    if(PMIX_SUCCESS == rc) rc = pack_msg(buffer, op);
    return rc;
}

static int pack_forward_msg(pmix_data_buffer_t* buffer, op_t* op,
                            pmix_rank_t dest){
    int rc = pack_sig(buffer, &op->sig);
    if(PMIX_SUCCESS == rc) rc = pack_ack_id(buffer, &op->ack_id_down);
    /* The movement rides with the payload rather than being re-derived on each
     * daemon: whoever originated the broadcast decided how it travels, and a
     * receiver that re-decided could disagree and misparse. Packed unguarded -
     * every daemon in a DVM runs the same build, so the bytes are never
     * conditional even when behaviour is. */
    if(PMIX_SUCCESS == rc) rc = pack_movement(buffer, &op->movement);
    if(PMIX_SUCCESS != rc) return rc;

    /* What follows the movement depends on it: a scatter carries a partition
     * of the payload plus the map to reassemble it, everything else carries
     * the payload. A degraded replay of a bulk op has already had its movement
     * flipped to tree_whole and its exchange state freed, so it packs whole
     * here - which is the entire point of the degrade. */
    if(NULL != op->bulk){
        return pack_scatter_msg(buffer, op, dest);
    }
    return pack_msg(buffer, op);
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

    if(sig->op_id == XCAST.op_id_inited){
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

/* ---------------------------------------------------------------------- */
/* MOVEMENT: scatter + allgather.
 *
 * The tree movement's bandwidth term is d*r*M*beta - a node with r children
 * puts r full copies of the payload on its outbound link, at every level.
 * This one moves about M bytes per daemon instead: the controller scatters
 * chunk i to participant i down the routing tree, and then every participant
 * reassembles the whole by an allgather across lateral links.
 *
 * The scatter *is* the forward, so it inherits the framing untouched - the ACK
 * rollup, the process_first set, the replay machinery. The allgather is the
 * only place grpcomm talks to a daemon that is not a routing-tree neighbour,
 * and it has its own tag for that reason. */

static void early_con(early_chunks_t* p){
    p->op_id = 0;
    PMIx_Data_buffer_construct(&p->buf);
}
static void early_des(early_chunks_t* p){
    PMIx_Data_buffer_destruct(&p->buf);
}
PMIX_CLASS_INSTANCE(early_chunks_t, pmix_list_item_t, early_con, early_des);

static void bulk_free(op_t* op){
    bulk_state_t* b = op->bulk;

    if(NULL == b) return;
    if(NULL != b->chunks){
        for(size_t i = 0; i < b->nparts; i++){
            PMIX_BYTE_OBJECT_DESTRUCT(&b->chunks[i]);
        }
        free(b->chunks);
    }
    if(NULL != b->held) free(b->held);
    if(NULL != b->parts) free(b->parts);
    free(b);
    op->bulk = NULL;
}

/* Attach exchange state carrying this participant list.  Takes ownership of
 * `parts` on every path, success or not, so a caller never has to unwind it. */
static int bulk_attach(op_t* op, pmix_rank_t* parts, size_t nparts, size_t total){
    bulk_state_t* b;

    bulk_free(op);
    b = (bulk_state_t*) calloc(1, sizeof(bulk_state_t));
    if(NULL == b){
        free(parts);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* nparts is never zero in practice - a scatter with no participants would
     * not have been sent - but calloc(0) may hand back NULL, which the checks
     * below cannot tell from failure */
    b->chunks = (pmix_byte_object_t*) calloc(nparts ? nparts : 1,
                                             sizeof(pmix_byte_object_t));
    b->held = (bool*) calloc(nparts ? nparts : 1, sizeof(bool));
    if(NULL == b->chunks || NULL == b->held){
        if(NULL != b->chunks) free(b->chunks);
        if(NULL != b->held) free(b->held);
        free(b);
        free(parts);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    b->parts = parts;
    b->nparts = nparts;
    b->total = total;
    b->mypos = SIZE_MAX;
    for(size_t i = 0; i < nparts; i++){
        if(parts[i] == PRTE_PROC_MY_NAME->rank){
            b->mypos = i;
            break;
        }
    }
    op->bulk = b;
    op->payload_complete = false;
    return PRTE_SUCCESS;
}

/* Take a copy of one chunk into its position slot.  Idempotent: a chunk that
 * arrives twice - which a replayed forward or an overlapping exchange step can
 * produce - is dropped rather than counted again, because nheld is what
 * decides completion. */
static int bulk_store(bulk_state_t* b, size_t pos, const pmix_byte_object_t* src){
    if(pos >= b->nparts || b->held[pos]) return PRTE_SUCCESS;

    if(0 == src->size){
        /* a real but empty chunk - a payload shorter than the participant
         * count yields several. Mark it held without a zero-length malloc,
         * whose return is not portably distinguishable from failure. */
        b->chunks[pos].bytes = NULL;
        b->chunks[pos].size = 0;
    } else {
        b->chunks[pos].bytes = (char*) malloc(src->size);
        if(NULL == b->chunks[pos].bytes) return PRTE_ERR_OUT_OF_RESOURCE;
        memcpy(b->chunks[pos].bytes, src->bytes, src->size);
        b->chunks[pos].size = src->size;
    }
    b->held[pos] = true;
    b->nheld++;
    return PRTE_SUCCESS;
}

/* Splice the chunks back into one payload, in position order. */
static int bulk_assemble(op_t* op){
    bulk_state_t* b = op->bulk;
    char* buf;
    size_t off, len;

    if(0 == b->total){
        PMIX_BYTE_OBJECT_DESTRUCT(&op->msg);
        return PRTE_SUCCESS;
    }
    buf = (char*) malloc(b->total);
    if(NULL == buf) return PRTE_ERR_OUT_OF_RESOURCE;

    for(size_t i = 0; i < b->nparts; i++){
        if(PRTE_SUCCESS != prte_grpcomm_chunk_bounds(b->total, b->nparts, i,
                                                     &off, &len)){
            free(buf);
            return PRTE_ERR_BAD_PARAM;
        }
        if(len != b->chunks[i].size){
            /* The seams disagree, so some participant split a different total
             * or a different participant count. Refuse rather than deliver a
             * payload stitched from mismatched pieces: the caller would see a
             * corrupt message with nothing to point at. */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            free(buf);
            return PRTE_ERR_BAD_PARAM;
        }
        if(0 < len) memcpy(buf + off, b->chunks[i].bytes, len);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&op->msg);
    op->msg.bytes = buf;
    op->msg.size = b->total;
    return PRTE_SUCCESS;
}

/* Do we hold every block the exchange step about to run has to send?  Slot j
 * holds participant (mypos + j)'s block - Bruck leaves them rotated, so the
 * mapping goes through bruck_owner rather than being assumed. */
static bool bulk_have_blocks(const bulk_state_t* b, size_t nblocks){
    for(size_t j = 0; j < nblocks; j++){
        size_t owner = prte_grpcomm_bruck_owner(b->mypos, b->nparts, j);
        if(SIZE_MAX == owner || !b->held[owner]) return false;
    }
    return true;
}

/* If the payload is now whole, assemble it.  Does NOT deliver or complete the
 * op: callers are in the middle of the framing's own sequencing and several of
 * them still touch the op afterwards. */
static bool bulk_payload_ready(op_t* op){
    bulk_state_t* b = op->bulk;

    if(NULL == b || op->payload_complete) return false;
    if(b->nheld < b->nparts) return false;

    if(PRTE_SUCCESS != bulk_assemble(op)){
        /* Nothing local can fix a bad reassembly, and the controller still
         * holds the original, so ask for it whole. */
        bulk_degrade(op);
        return false;
    }
    op->payload_complete = true;
    return true;
}

/* Returns non-success rather than degrading here: the caller is looping over
 * op->bulk, and degrading frees it. */
static int bulk_send_step(op_t* op, size_t send_to, size_t nblocks){
    bulk_state_t* b = op->bulk;
    pmix_rank_t dest = b->parts[send_to];
    pmix_data_buffer_t* buf = PMIx_Data_buffer_create();
    uint8_t kind = BULK_KIND_CHUNKS;
    int rc;

    rc = PMIx_Data_pack(NULL, buf, &op->sig.op_id, 1, PMIX_SIZE);
    if(PMIX_SUCCESS == rc) rc = PMIx_Data_pack(NULL, buf, &kind, 1, PMIX_UINT8);
    if(PMIX_SUCCESS == rc) rc = PMIx_Data_pack(NULL, buf, &nblocks, 1, PMIX_SIZE);
    for(size_t j = 0; PMIX_SUCCESS == rc && j < nblocks; j++){
        size_t owner = prte_grpcomm_bruck_owner(b->mypos, b->nparts, j);
        rc = PMIx_Data_pack(NULL, buf, &owner, 1, PMIX_SIZE);
        if(PMIX_SUCCESS == rc){
            rc = PMIx_Data_pack(NULL, buf, &b->chunks[owner], 1, PMIX_BYTE_OBJECT);
        }
    }
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* Tell the RML this peer is not a tree neighbour before the send opens the
     * connection, so a drop between the two cannot be read as a lifeline loss.
     * The link is deliberately never deregistered: the socket outlives the
     * collective, and a registration withdrawn while the connection is still
     * open would make a later drop look like a routing-tree fault. Retiring
     * both together is the idle-teardown work, which is not done yet. */
    prte_rml_lateral_register(dest);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast:bulk sending %d blocks to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nblocks,
                         PRTE_VPID_PRINT(dest)));

    PRTE_RML_SEND_DIRECT(rc, dest, buf, PRTE_RML_TAG_XCAST_BULK);
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    return PMIX_SUCCESS;
}

/* Run every exchange step we now have the blocks for.  Steps can arrive out of
 * order, so this is a loop over "can I run the next one" rather than a step
 * driven directly by whichever message just landed. */
static void bulk_progress(op_t* op){
    bulk_state_t* b = op->bulk;
    size_t nsteps;

    if(NULL == b || SIZE_MAX == b->mypos) return;
    nsteps = prte_grpcomm_bruck_nsteps(b->nparts);
    while(b->step < nsteps){
        size_t send_to, recv_from, nblocks;
        if(PRTE_SUCCESS != prte_grpcomm_bruck_step(b->mypos, b->nparts, b->step,
                                                   &send_to, &recv_from,
                                                   &nblocks)){
            break;
        }
        if(!bulk_have_blocks(b, nblocks)) break;
        if(PMIX_SUCCESS != bulk_send_step(op, send_to, nblocks)){
            /* Degrade outside the loop: it frees the very state `b` points
             * at, so stepping the counter afterwards would be a write to
             * freed memory. */
            bulk_degrade(op);
            return;
        }
        b->step++;
    }
}

/* Ask the controller to abandon the exchange and replay this op whole.  Only
 * the controller is certain to hold the entire payload, so the request goes
 * straight there rather than to our parent, which under this movement may hold
 * no more of it than we do. */
static void bulk_request_replay(op_t* op){
    pmix_data_buffer_t* buf;
    uint8_t kind = BULK_KIND_REPLAY;
    int rc;

    buf = PMIx_Data_buffer_create();
    rc = PMIx_Data_pack(NULL, buf, &op->sig.op_id, 1, PMIX_SIZE);
    if(PMIX_SUCCESS == rc) rc = PMIx_Data_pack(NULL, buf, &kind, 1, PMIX_UINT8);
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_XCAST_BULK);
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

/* Give up on this op's exchange.
 *
 * A participant lost mid-scatter or mid-allgather cannot be recovered by the
 * exchange itself - the block it owed is simply gone - and inventing a second
 * recovery mechanism to sit beside the framing's replay would be one more
 * thing to get wrong. So the movement flips to tree_whole and the payload is
 * re-sent whole by someone who has it. Any daemon holding the whole payload
 * can heal its own subtree; one that does not asks the controller, which
 * always can. Receivers holding partial chunks have not delivered anything yet
 * (process_msg gates on payload_complete), so they simply take the whole
 * payload instead. Correctness over speed during recovery. */
static void bulk_degrade(op_t* op){
    if(NULL == op->bulk) return;
    if(PRTE_GRPCOMM_BCAST_TREE_WHOLE == op->movement){
        /* already degraded - a second fault, or several partners reporting the
         * same one, must not restart the replay */
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:xcast:bulk op %lu falling back to the tree",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (unsigned long) op->sig.op_id));

    op->movement = PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    bool have_all = op->payload_complete;
    /* Drop the exchange state on BOTH paths, not just the one that re-sends.
     * pack_forward_msg decides the body's shape from op->bulk and the receiver
     * decides how to read it from the movement on the wire, so an op left with
     * a tree_whole movement and exchange state still attached would pack a
     * scatter body under a whole-payload header - and the next replay, or the
     * next fault's replay to a changed child, would desynchronize the buffer.
     * The two must never be able to disagree. Freeing here is also what makes
     * a second call to this function a no-op. */
    bulk_free(op);
    if(have_all){
        forward_op(op);
    } else {
        bulk_request_replay(op);
    }
}

/* Every daemon currently in the routing tree, in rank order.  Computed once,
 * by the controller, and then carried on the wire - see bulk_state_t. */
static pmix_rank_t* bulk_participants(size_t* nparts){
    pmix_rank_t* parts;
    size_t n = 0;

    *nparts = 0;
    if(0 >= prte_rml_base.n_dmns) return NULL;

    parts = (pmix_rank_t*) malloc(prte_rml_base.n_dmns * sizeof(pmix_rank_t));
    if(NULL == parts) return NULL;
    for(pmix_rank_t r = 0; r < (pmix_rank_t) prte_rml_base.n_dmns; r++){
        if(prte_rml_is_node_up(r)) parts[n++] = r;
    }
    *nparts = n;
    return parts;
}

/* Read a run of (position, chunk) pairs into an op's exchange state. */
static int bulk_absorb_chunks(pmix_data_buffer_t* buffer, op_t* op){
    size_t nblocks = 0;
    int cnt = 1;
    int rc;

    rc = PMIx_Data_unpack(NULL, buffer, &nblocks, &cnt, PMIX_SIZE);
    if(PMIX_SUCCESS != rc){
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    for(size_t i = 0; i < nblocks; i++){
        pmix_byte_object_t chunk = PMIX_BYTE_OBJECT_STATIC_INIT;
        size_t pos = 0;

        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &pos, &cnt, PMIX_SIZE);
        if(PMIX_SUCCESS == rc){
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, buffer, &chunk, &cnt, PMIX_BYTE_OBJECT);
        }
        if(PMIX_SUCCESS != rc){
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        rc = bulk_store(op->bulk, pos, &chunk);
        PMIX_BYTE_OBJECT_DESTRUCT(&chunk);
        if(PRTE_SUCCESS != rc){
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* Forget parked chunks for ops that are over.  An op that degraded to the tree
 * will never read the blocks its partners had already sent it, and one that
 * completed has no use for them either. */
static void bulk_purge_early(void){
    early_chunks_t* e;
    early_chunks_t* next;

    PMIX_LIST_FOREACH_SAFE(e, next, &XCAST.early_chunks, early_chunks_t){
        if(e->op_id <= XCAST.op_id_completed){
            pmix_list_remove_item(&XCAST.early_chunks, &e->super);
            PMIX_RELEASE(e);
        }
    }
}

/* Take in any exchange chunks that arrived before this op's scatter did. */
static void bulk_drain_early(op_t* op){
    early_chunks_t* e;
    early_chunks_t* next;

    PMIX_LIST_FOREACH_SAFE(e, next, &XCAST.early_chunks, early_chunks_t){
        if(e->op_id != op->sig.op_id) continue;
        pmix_list_remove_item(&XCAST.early_chunks, &e->super);
        if(PMIX_SUCCESS != bulk_absorb_chunks(&e->buf, op)){
            PMIX_RELEASE(e);
            bulk_degrade(op);
            return;
        }
        PMIX_RELEASE(e);
    }
}

/* MOVEMENT: scatter the payload down the tree, then allgather across.
 *
 * Two entries into this: the controller, which holds the whole payload and has
 * to chunk it first, and every other daemon, which was handed its subtree's
 * chunks by its parent and passes on the ones that are not its own. */
static void scatter_allgather_forward(op_t* op){
    pmix_rank_t* children = (pmix_rank_t*) prte_rml_base.children.array;

    if(NULL == op->bulk){
        /* We are the controller: nobody has chunked this yet. */
        size_t nparts = 0;
        pmix_rank_t* parts = bulk_participants(&nparts);
        pmix_byte_object_t chunk;
        size_t off, len;

        if(NULL == parts || 2 > nparts){
            /* Not enough of a DVM to exchange across. Not an error - a
             * single-daemon DVM is a legitimate thing to broadcast in. */
            if(NULL != parts) free(parts);
            op->movement = PRTE_GRPCOMM_BCAST_TREE_WHOLE;
            tree_whole_forward(op);
            return;
        }
        if(PRTE_SUCCESS != bulk_attach(op, parts, nparts, op->msg.size)){
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            op->movement = PRTE_GRPCOMM_BCAST_TREE_WHOLE;
            tree_whole_forward(op);
            return;
        }
        /* The controller holds every chunk, so it is complete from the start
         * and never assembles anything - but it still runs the exchange, since
         * its partners are waiting on the blocks it owes them.
         *
         * Say so BEFORE chunking rather than after: bulk_attach cleared the
         * flag, and if the chunking below fails, bulk_degrade has to see that
         * we hold the whole payload and re-send it down the tree. Read as
         * still-incomplete it would instead ask the controller for a replay -
         * and we are the controller, so the request would come straight back
         * to an op already marked degraded and be dropped, stranding the
         * broadcast with no diagnostic. */
        op->payload_complete = true;
        for(size_t i = 0; i < nparts; i++){
            if(PRTE_SUCCESS != prte_grpcomm_chunk_bounds(op->bulk->total, nparts,
                                                         i, &off, &len)){
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                bulk_degrade(op);
                return;
            }
            chunk.bytes = op->msg.bytes + off;
            chunk.size = len;
            if(PRTE_SUCCESS != bulk_store(op->bulk, i, &chunk)){
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                bulk_degrade(op);
                return;
            }
        }
    }

    /* Pass each child the chunks its own subtree owns.  This is the forward,
     * so it carries the framing with it exactly as tree_whole's does. */
    for(size_t i = 0; i < prte_rml_base.children.size; i++){
        if(children[i] == PMIX_RANK_INVALID) continue;
        forward_op_to(op, children[i]);
    }

    if(SIZE_MAX == op->bulk->mypos){
        /* The controller named a participant set we are not in - we grew into
         * the DVM after it was computed. We can relay to our subtree but we
         * cannot take part in the exchange, so we need the payload the other
         * way. */
        bulk_degrade(op);
        return;
    }

    bulk_drain_early(op);
    bulk_progress(op);
    bulk_payload_ready(op);
}

/* MOVEMENT: send the whole payload to each routing-tree child.
 *
 * Right for a small message, where the cost is the depth of the tree and a
 * high radix makes that 1 or 2 hops.  Wrong for a large one: a node with r
 * children serializes r full copies of the payload on its outbound link, at
 * every level, so the bandwidth term is d*r*M*beta - which is the entire cost
 * of broadcasting a launch message or a preload chunk at scale. */
static void tree_whole_forward(op_t* op){
    pmix_rank_t* children = (pmix_rank_t*) prte_rml_base.children.array;
    for(size_t i = 0; i < prte_rml_base.children.size; i++){
        if(children[i] == PMIX_RANK_INVALID) continue;
        forward_op_to(op, children[i]);
    }
}

static const bcast_movement_t bcast_movements[] = {
    { .id = PRTE_GRPCOMM_BCAST_TREE_WHOLE,
      .name = "tree_whole",
      .forward = tree_whole_forward },
    { .id = PRTE_GRPCOMM_BCAST_SCATTER_ALLGATHER,
      .name = "scatter_allgather",
      .forward = scatter_allgather_forward },
};

/* May this tag's payload travel laterally?
 *
 * A payload whose delivery order is a correctness invariant may not: the
 * exchange completes out of order with respect to the tree, which is exactly
 * what the process_first set and the forward-before-process rule exist to
 * prevent. That constraint and the size split agree rather than conflict -
 * these are all tiny messages, which is why keeping them on the tree costs
 * nothing.
 *
 * PRTE_RML_TAG_WIREUP is the one exclusion that will look wrong later: it is
 * large, and it would otherwise be the best possible candidate. But it is in
 * the process_first set because it changes the child set, so it has to be
 * processed before it is forwarded - and a scatter cannot be processed until
 * after the exchange. Do not "optimise" it back in. */
static bool bulk_tag_allowed(prte_rml_tag_t tag){
    return PRTE_RML_TAG_WIREUP != tag &&
           PRTE_RML_TAG_DAEMON_DIED != tag &&
           PRTE_RML_TAG_DAEMON_REVIVED != tag;
}

/* Which broadcasts are worth scattering, by what they ARE.
 *
 * This is a table rather than a threshold because PRRTE does not carry
 * arbitrary traffic - it carries a known, small set of things, and which of
 * them a message is IS the operation. The launch message is the one large
 * broadcast PRRTE makes routinely, and since it was split onto its own tag
 * there is nothing left to infer.
 *
 * PRTE_RML_TAG_FILEM_BASE is deliberately NOT here despite being the other
 * "large" entry in the design's operation table. Its chunks are capped at
 * PRTE_FILEM_RAW_CHUNK_MAX, 16 KB, which is small enough that the answer
 * depends on the DVM size rather than being clear: at a few hundred daemons
 * the tree's r*M*beta fanout dominates and the exchange wins, at ten daemons
 * the exchange's extra log2(N) latency steps cost more than the fanout saves.
 * A knob-free table has no business containing a guess, so if FILEM is to be
 * scattered, raise the chunk size or measure it first.
 *
 * Adding a tag here is the intended way to opt a new bulk payload in. That is
 * not a cost to be designed around - it is the declaration that keeps the
 * choice reasoned. */
static bool bulk_tag_prefers_bulk(prte_rml_tag_t tag){
    return PRTE_RML_TAG_DAEMON_LAUNCH == tag;
}

/* Choose how this broadcast will travel.  Runs on the originator only, and its
 * answer is stamped on the wire; nothing downstream re-decides. */
static uint32_t select_movement(prte_rml_tag_t tag, size_t nbytes){
    size_t ndmns = (0 < prte_rml_base.n_dmns) ? (size_t) prte_rml_base.n_dmns : 0;

    if(!bulk_tag_allowed(tag)) return PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    /* Below two daemons there is no exchange to run, whatever was asked for. */
    if(2 > ndmns) return PRTE_GRPCOMM_BCAST_TREE_WHOLE;

    switch(prte_grpcomm_globals.bcast_select){
    case PRTE_GRPCOMM_BCAST_SELECT_TREE:
        return PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    case PRTE_GRPCOMM_BCAST_SELECT_BULK:
        return PRTE_GRPCOMM_BCAST_SCATTER_ALLGATHER;
    case PRTE_GRPCOMM_BCAST_SELECT_SIZE:
        /* The escape hatch, not the production path: it holds a number
         * nobody has measured. Kept because a programming model may push
         * something unexpectedly large through a tag nobody classified. */
        if(nbytes >= prte_grpcomm_globals.bcast_bulk_min_bytes &&
           ndmns >= prte_grpcomm_globals.bcast_bulk_min_daemons){
            return PRTE_GRPCOMM_BCAST_SCATTER_ALLGATHER;
        }
        return PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    case PRTE_GRPCOMM_BCAST_SELECT_TAG:
    default:
        return bulk_tag_prefers_bulk(tag) ? PRTE_GRPCOMM_BCAST_SCATTER_ALLGATHER
                                          : PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    }
}

static const bcast_movement_t* movement_by_id(uint32_t id){
    for(size_t i = 0; i < sizeof(bcast_movements)/sizeof(bcast_movements[0]); i++){
        if(bcast_movements[i].id == id) return &bcast_movements[i];
    }
    return NULL;
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

    /* The ACK rollup is subtree-shaped whichever movement carries the bytes:
     * a daemon reports its subtree complete once it holds the payload and all
     * its children have reported. So this accounting is framing, not movement,
     * and stays here. */
    op->replay_pending_parent = false;
    op->nexpected = prte_rml_base.n_children;
    op->nreported = 0;

    const bcast_movement_t* mv = movement_by_id(op->movement);
    if(NULL == mv){
        /* Unknown movement: the originator stamped something this build does
         * not implement. Refuse rather than guess - guessing would deliver a
         * misparsed payload. Every daemon in a DVM runs the same build, so
         * this is a bug, not a version skew. */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }
    mv->forward(op);
}

static void forward_op_to(op_t* op, pmix_rank_t dest){
    pmix_data_buffer_t* xcast_msg = PMIx_Data_buffer_create();

    int rc = pack_forward_msg(xcast_msg, op, dest);
    if(PMIX_SUCCESS != rc){
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return;
    }

    PMIX_OUTPUT_VERBOSE((
        5, prte_grpcomm_globals.output,
        "%s grpcomm:send_relay sending relay msg of %d bytes to %s",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) xcast_msg->bytes_used,
        PRTE_VPID_PRINT(dest)
    ));

    PRTE_RML_SEND(rc, dest, xcast_msg, PRTE_RML_TAG_XCAST);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return;
    }
}

static void process_wireup(pmix_data_buffer_t *msg){
    if(PRTE_PROC_IS_MASTER) return;

    int ret = prte_util_decode_nidmap(msg);
    if(PMIX_SUCCESS != ret){
       PMIX_ERROR_LOG(ret);
       PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
       return;
    }

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

    if(val.type != PMIX_UNDEF) PMIx_Value_destruct(&val);
    if(sval.type != PMIX_UNDEF) PMIx_Value_destruct(&sval);
    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    return;
}

static void process_msg(op_t* op){
    int ret = PMIX_SUCCESS;

    if(op->processed) return;
    /* Under tree_whole this is always true by the time the op exists, so this
     * gate costs that path nothing. Under scatter+allgather the op exists long
     * before the payload does, and delivering a partially-assembled message
     * would hand the rest of PRRTE a truncated buffer. Guarding here rather
     * than at each call site means every existing caller - the forward-first
     * and process-first branches of the recv, and finish_op - is correct
     * without knowing which movement is in play; the bulk receive calls back
     * in once the exchange closes. */
    if(!op->payload_complete) return;
    op->processed = true;

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
    p->sig.op_id = 0;

    p->processed = false;
    p->replay_pending_parent = false;

    p->nreported = 0;
    p->nexpected = -1;
    p->ack_id_up = 0;
    p->ack_id_down = 0;

    PMIx_Byte_object_construct(&p->msg);
    p->msg_compressed = false;
    p->msg_tag = PRTE_RML_TAG_INVALID;

    p->cbfunc = NULL;
    p->cbdata = NULL;
    /* the tree carries every broadcast until a movement is selected for it */
    p->movement = PRTE_GRPCOMM_BCAST_TREE_WHOLE;
    /* tree_whole holds the whole payload as soon as it holds the op; a
     * scatter overrides this when it takes the op over */
    p->payload_complete = true;
    p->bulk = NULL;
}
static void op_des(op_t* p)
{
    PMIX_BYTE_OBJECT_DESTRUCT(&p->msg);
    bulk_free(p);
}
PMIX_CLASS_INSTANCE(op_t, pmix_list_item_t, op_con, op_des);


static void xcast_con(prte_grpcomm_xcast_t* p)
{
    PMIX_CONSTRUCT(&p->ops, pmix_list_t);
    PMIX_CONSTRUCT(&p->pending_completions, pmix_list_t);
    PMIX_CONSTRUCT(&p->early_chunks, pmix_list_t);
    p->op_id_completed = 0;
    p->op_id_completed_at_promotion = 0;
    p->op_id_inited = 0;
}
static void xcast_des(prte_grpcomm_xcast_t* p)
{
    PMIX_LIST_DESTRUCT(&p->ops);
    PMIX_LIST_DESTRUCT(&p->pending_completions);
    PMIX_LIST_DESTRUCT(&p->early_chunks);
}
PMIX_CLASS_INSTANCE(prte_grpcomm_xcast_t, pmix_object_t, xcast_con, xcast_des);

