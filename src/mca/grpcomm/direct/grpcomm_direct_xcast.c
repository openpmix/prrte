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

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

#define XCAST prte_mca_grpcomm_direct_component.xcast_ops

/* internal signature used to uniquely track a particular xcast */
typedef struct {
    size_t global_op_id;    // HNP's assigned collective ID, globally unique
    size_t initiator_op_id; // Initiator's locally-assigned ID
    pmix_rank_t initiator;
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
} op_t;
PMIX_CLASS_DECLARATION(op_t);

// event handler for prte_grpcomm_direct_xcast to safely access global data
//   void* = a built op_t*
static void begin_xcast(int, short, void*);
// Forward op to the HNP to initiate
static void launch_op(op_t* op);
// Returns NULL if not found. Fuzzy search, allowing global_op_id=0 to match
// any other global_op_id. Handles updating the stored global id and moving from
// pending_ops to ops based on the search signature
static op_t* find_op(signature_t *sig);
// Returns op after constructing & inserting it into our tracking list
static op_t* insert_forwarded_op(signature_t *sig);
// Standard forward to all children,
static void forward_op(op_t *op);
// Forward to specific destination
static void forward_op_to(op_t *op, pmix_rank_t dest);
// Locally process the message being broadcast
static void process_msg(op_t *op);
// Ack that myself and my full subtree have processed this message
static void send_ack(signature_t* sig, pmix_rank_t ack_id);
// Request an ack after a failure without resending full user message
static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id);
// Remove local tracking and ack to parent
static void finish_op(op_t *op);

// Pack the full xcast message to be forwarded to our children or HNP
static int pack_forward_msg(pmix_data_buffer_t *buffer, op_t *op);
// (un)pack components - listed in correct order.
static int pack_sig     (pmix_data_buffer_t* buffer, signature_t* sig);
static int unpack_sig   (pmix_data_buffer_t* buffer, signature_t* sig);
static int pack_ack_id  (pmix_data_buffer_t* buffer, pmix_rank_t* ack_id);
static int unpack_ack_id(pmix_data_buffer_t* buffer, pmix_rank_t* ack_id);
static int pack_msg     (pmix_data_buffer_t* buffer, op_t* op);
static int unpack_msg   (pmix_data_buffer_t* buffer, op_t* op);

int prte_grpcomm_direct_xcast(prte_rml_tag_t tag, pmix_data_buffer_t *msg){
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) msg->bytes_used));

    op_t* op = PMIX_NEW(op_t);
    op->msg_tag = tag;
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

void prte_grpcomm_direct_xcast_recv(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);
    if(!PRTE_PROC_IS_MASTER && sender->rank != PRTE_PROC_MY_PARENT->rank){
        // Ignore messages from old parents
        return;
    }
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int) buffer->bytes_used));

    signature_t sig;
    if(PMIX_SUCCESS != unpack_sig(buffer, &sig)) return;

    pmix_rank_t ack_id;
    if(PMIX_SUCCESS != unpack_ack_id(buffer, &ack_id)) return;

    PMIX_OUTPUT_VERBOSE((
        1, prte_grpcomm_base_framework.framework_output,
        "%s grpcomm:direct:xcast:recv: %lu, initiated by %s with id %lu",
        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sig.global_op_id,
        PRTE_VPID_PRINT(sig.initiator), sig.initiator_op_id
    ));

    if(!sig.global_op_id && !PRTE_PROC_IS_MASTER){
        // If I'm not HNP, I expect HNP has assigned a global ID
        PRTE_ERROR_LOG( PRTE_ERR_NOT_INITIALIZED );
        return;
    } else if(sig.global_op_id && PRTE_PROC_IS_MASTER){
        // If I'm HNP, I expect sender has not assigned a global ID, so if this
        // has one they have likely repeated an init after seeing the xcast
        // start, which shouldn't happen
        PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
        return;
    }

    // If we marked our subtree as completed, but then were promoted, our
    // subtree is now larger and may not have actually completed everywhere
    bool assume_incomplete = sig.global_op_id &&
        sig.global_op_id <= XCAST.op_id_completed_at_promotion;
    // But ops complete in order, so if we have completed anything since our
    // promotion, we know our new subtree has also completed all the older ops
    assume_incomplete = assume_incomplete &&
        XCAST.op_id_completed == XCAST.op_id_completed_at_promotion;

    // If we're certain our subtree has already completed this, we can just ack
    bool complete = !assume_incomplete && sig.global_op_id &&
        sig.global_op_id <= XCAST.op_id_completed;
    if(complete) {
        send_ack(&sig, ack_id);
        return;
    }

    op_t* op = find_op(&sig);
    if(NULL == op){
        op = insert_forwarded_op(&sig);
        if(PMIX_SUCCESS != unpack_msg(buffer, op)){
            pmix_list_remove_item(&XCAST.ops, &op->super);
            PMIX_RELEASE(op);
            return;
        }
    }

    if(!op->sig.global_op_id){
        op->sig.global_op_id = ++XCAST.op_id_global;
        if(PRTE_PROC_MY_NAME->rank == op->sig.initiator){
            pmix_list_remove_item(&XCAST.pending_ops, &op->super);
            pmix_list_append(&XCAST.ops, &op->super);
        }
    } else if(PRTE_PROC_IS_MASTER && op->processed) {
        // Sender confirming op wasn't lost after a failure
        PMIX_OUTPUT_VERBOSE((
            1, prte_grpcomm_base_framework.framework_output,
            "%s grpcomm:direct:xcast:recv: safely ignoring duplicate init",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)
        ));
        return;
    }

    if(assume_incomplete){
        op->processed = true;
        op->replay_pending_parent = true;
    }

    op->ack_id_up = ack_id;
    if(op->replay_pending_parent) forward_op(op);
    if(op->processed) return;

    // We need to process (invoke the user msg's callback, generally) and
    // forward to our children.
    // For most xcasts, it's best to forward first to maintain message ordering,
    // but xcasts that modify how we send messages should be processed first
    bool process_first = PRTE_RML_TAG_WIREUP == op->msg_tag ||
                         PRTE_RML_TAG_DAEMON_DIED == op->msg_tag;
    if(process_first){
        process_msg(op);
        forward_op(op);
    } else {
        forward_op(op);
        process_msg(op);
    }
}

void prte_grpcomm_direct_xcast_ack(
    int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
    prte_rml_tag_t tag, void *cbdata
) {
    PRTE_HIDE_UNUSED_PARAMS(status,tag,cbdata);

    signature_t sig;
    int ret = unpack_sig(buffer, &sig);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    pmix_rank_t ack_id;
    ret = unpack_ack_id(buffer, &ack_id);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        return;
    }

    bool is_request;
    int cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &is_request, &cnt, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
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
        } else if(sig.global_op_id <= XCAST.op_id_completed){
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
            finish_op(op);
        } else if(op->nreported > op->nexpected){
            PRTE_ERROR_LOG( PRTE_ERR_DUPLICATE_MSG );
        }
    }
}

void prte_grpcomm_direct_xcast_fault_handler(
    const prte_rml_recovery_status_t* status
) {
    // We must do all xcast handling in the local scope, since reliable xcasts
    // is how we get the global scope notifications in the first place
    if(status->scope != PRTE_RML_FAULT_SCOPE_LOCAL) return;

    if(status->promoted){
        XCAST.op_id_completed_at_promotion =
            XCAST.op_id_completed;
    }
    if(status->ancestors_changed){
        // Anything still pending may have been lost, so relaunch.
        // Launching is idempotent as long as the op hasn't already
        // completed when we repeat the launch, so relaunching ops
        // not yet seen is always safe.
        op_t* op;
        PMIX_LIST_FOREACH(op, &XCAST.pending_ops, op_t){
            launch_op(op);
        }
    }
    if(status->parent_changed || status->promoted){
        // Avoid confusing new parent by accidentally acking with
        // the valid ack id. They'll tell us what id to use.
        op_t* op;
        PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
            op->ack_id_up = PMIX_RANK_INVALID;
        }
    }
    if(status->children_changed || status->promoted){
        const pmix_rank_t* prev_children =
            (const pmix_rank_t*) status->prev_children.array;
        const pmix_rank_t* children =
            (const pmix_rank_t*) prte_rml_base.children.array;

        op_t* op;
        PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
            op->nexpected = prte_rml_base.n_children;

            // If this op is currently pending replay, so are all after it.
            if(op->replay_pending_parent) break;

            // If any children have reported back, we have no way of knowing if
            // it was the surviving children or a failed child. So we will need
            // to start a new ack round.
            // If promoted, avoid late ack arrivals from old children causing
            // confusion by also starting a new round.
            bool new_ack_round = op->nreported > 0 || status->promoted;
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
                } else if(children[i] != prev_children[i]){
                    forward_op_to(op, children[i]);
                } else if(new_ack_round){
                    request_ack(children[i], &op->sig, op->ack_id_down);
                }
            }
        }
    }
}

static void begin_xcast(int sd, short args, void* cbdata){
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    op_t* op = (op_t*) cbdata;
    PMIX_ACQUIRE_OBJECT(&op);

    op->sig.initiator = PRTE_PROC_MY_NAME->rank;
    op->sig.initiator_op_id = ++XCAST.op_id_local;
    op->processed = false;

    pmix_list_append(&XCAST.pending_ops, &op->super);
    launch_op(op);
}

static void launch_op(op_t* op){
    /* setup the payload */
    pmix_data_buffer_t *xcast_msg = PMIx_Data_buffer_create();
    int rc = pack_forward_msg(xcast_msg, op);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return;
    }

    /* send it to the HNP (could be myself) for relay */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, xcast_msg, PRTE_RML_TAG_XCAST);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return;
    }
}

static void send_ack_msg(
    signature_t* sig, pmix_rank_t ack_id, bool is_request, pmix_rank_t dest
) {
    pmix_data_buffer_t* msg = PMIx_Data_buffer_create();
    int ret = pack_sig(msg, sig);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
        return;
    }
    ret = pack_ack_id(msg, &ack_id);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
        return;
    }

    ret = PMIx_Data_pack(NULL, msg, &is_request, 1, PMIX_BOOL);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
        return;
    }

    PRTE_RML_SEND(ret, dest, msg, PRTE_RML_TAG_XCAST_ACK);
    if(PMIX_SUCCESS != ret){
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIx_Data_buffer_release(msg);
        return;
    }
}

static void send_ack(signature_t* sig, pmix_rank_t ack_id){
    if(PRTE_PROC_IS_MASTER) return;
    send_ack_msg(sig, ack_id, false, PRTE_PROC_MY_PARENT->rank);
}

static void request_ack(pmix_rank_t from, signature_t* sig, pmix_rank_t ack_id){
    send_ack_msg(sig, ack_id, true, from);
}

static void finish_op(op_t* op) {
    send_ack(&op->sig, op->ack_id_up);
    pmix_list_remove_item(&XCAST.ops, &op->super);
    if(op->sig.global_op_id > XCAST.op_id_completed_at_promotion &&
        op->sig.global_op_id != XCAST.op_id_completed+1){
        PRTE_ERROR_LOG( PRTE_ERR_OUT_OF_ORDER_MSG );
    } else {
        XCAST.op_id_completed++;
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
    DIRECT_XCAST_PACK(buffer, &sig->global_op_id,    PMIX_SIZE);
    DIRECT_XCAST_PACK(buffer, &sig->initiator_op_id, PMIX_SIZE);
    DIRECT_XCAST_PACK(buffer, &sig->initiator,       PMIX_PROC_RANK);
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

static int unpack_sig(pmix_data_buffer_t* buffer, signature_t* sig){
    DIRECT_XCAST_UNPACK(buffer, &sig->global_op_id,    PMIX_SIZE);
    DIRECT_XCAST_UNPACK(buffer, &sig->initiator_op_id, PMIX_SIZE);
    DIRECT_XCAST_UNPACK(buffer, &sig->initiator,       PMIX_PROC_RANK);
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

static int pack_forward_msg(pmix_data_buffer_t* buffer, op_t* op){
    int rc = pack_sig(buffer, &op->sig);
    if(PMIX_SUCCESS == rc) rc = pack_ack_id(buffer, &op->ack_id_down);
    if(PMIX_SUCCESS == rc) rc = pack_msg(buffer, op);
    return rc;
}

static op_t* find_op(signature_t* sig){
    op_t* op = NULL;
    if(sig->initiator == PRTE_PROC_MY_NAME->rank){
        bool found = false;
        PMIX_LIST_FOREACH(op, &XCAST.pending_ops, op_t){
            found = sig->initiator_op_id == op->sig.initiator_op_id;
            if(found) break;
        }
        // Move this to the right list
        if(found && sig->global_op_id){
            op->sig.global_op_id = sig->global_op_id;
            pmix_list_remove_item(&XCAST.pending_ops, &op->super);
            pmix_list_append(&XCAST.ops, &op->super);
        }
        if(found) return op;
    }

    PMIX_LIST_FOREACH(op, &XCAST.ops, op_t){
        if(sig->initiator != op->sig.initiator) continue;
        if(sig->initiator_op_id != op->sig.initiator_op_id) continue;
        if(!sig->global_op_id || sig->global_op_id == op->sig.global_op_id){
            return op;
        }

        // initiator and initiator_op_id match, but both have global_op_id
        // assigned and different, which should never happen
        // If you see this, there's an issue with PRRTE's failure recovery logic
        //   (or something is messing up memory with invalid accesses)
        PRTE_ERROR_LOG(PRTE_ERR_DUPLICATE_MSG);
    }
    return NULL;
}

static op_t* insert_forwarded_op(signature_t* sig) {
    op_t* op = PMIX_NEW(op_t);
    op->sig = *sig;

    if(!sig->global_op_id){
        // Only possible on HNP, this op will be assigned the next global ID, so
        // put it at the end
        pmix_list_append(&XCAST.ops, &op->super);
    } else {
        op_t* next_op = NULL;
        PMIX_LIST_FOREACH(next_op, &XCAST.ops, op_t){
            if(next_op->sig.global_op_id > sig->global_op_id) break;
            if(next_op->sig.global_op_id == sig->global_op_id) {
                // Should not happen
                PRTE_ERROR_LOG(PRTE_ERR_DUPLICATE_MSG);
            }
        }
        pmix_list_insert_pos(&XCAST.ops, &next_op->super, &op->super);
    }
    return op;
}

static void forward_op(op_t* op){
    prte_job_t* daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    bool skip = prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH,
                                   NULL, PMIX_BOOL);
    if(skip) return;

    op->replay_pending_parent = false;
    op->nexpected = prte_rml_base.n_children;
    op->nreported = 0;

    /* send the message to each of our children */
    pmix_rank_t* children = (pmix_rank_t*) prte_rml_base.children.array;
    for(size_t i = 0; i < prte_rml_base.children.size; i++){
        if(children[i] == PMIX_RANK_INVALID) continue;
        forward_op_to(op, children[i]);
    }

    // No children, ack immediately
    if(op->nexpected == 0) send_ack(&op->sig, op->ack_id_up);

    return;
}

static void forward_op_to(op_t* op, pmix_rank_t dest){
    pmix_data_buffer_t* xcast_msg = PMIx_Data_buffer_create();

    int rc = pack_forward_msg(xcast_msg, op);
    if(PMIX_SUCCESS != rc){
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(xcast_msg);
        return;
    }

    PMIX_OUTPUT_VERBOSE((
        5, prte_grpcomm_base_framework.framework_output,
        "%s grpcomm:direct:send_relay sending relay msg of %d bytes to %s",
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
    pmix_proc_t dmn;
    int cnt = 1;
    do {
        PMIx_Value_destruct(&val);
        ret = PMIx_Data_unpack(NULL, msg, &dmn, &cnt, PMIX_PROC);
        if(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == ret) return;
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }

        PMIx_Value_construct(&val);
        val.type = PMIX_STRING;

        ret = PMIx_Data_unpack(NULL, msg, &val.data.string, &cnt, PMIX_STRING);
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }

        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_HNP)) continue;
        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_NAME)) continue;
        if(PMIX_CHECK_PROCID(&dmn, PRTE_PROC_MY_PARENT)) continue;

        ret = PMIx_Store_internal(&dmn, PMIX_PROC_URI, &val);
        if(PMIX_SUCCESS != ret){ PMIX_ERROR_LOG(ret); break; }
    } while(PMIX_SUCCESS == ret);

    if(val.type != PMIX_UNDEF) PMIx_Value_destruct(&val);
    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    return;
}

static void process_msg(op_t* op){
    int ret = op->processed ? PRTE_ERR_DUPLICATE_MSG : PMIX_SUCCESS;
    if(PRTE_SUCCESS != ret){
        PRTE_ERROR_LOG( ret );
        return;
    }
    op->processed = true;

    pmix_data_buffer_t *msg = PMIx_Data_buffer_create();
    if(op->msg_compressed){
        pmix_byte_object_t decomp_msg = PMIX_BYTE_OBJECT_STATIC_INIT;
        bool success = PMIx_Data_decompress(
            (uint8_t* ) op->msg.bytes, op->msg.size,
            (uint8_t**) &decomp_msg.bytes, &decomp_msg.size
        );
        if(!success){
            pmix_show_help("help-prte-runtime.txt", "failed-to-uncompress",
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
        // TODO: Would be best to set the sender to op->sig.initiator, but do
        // any current xcast recv handlers rely on current behavior?
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
    p->sig.initiator = -1;
    p->sig.initiator_op_id = 0;
    p->sig.global_op_id = 0;

    p->processed = false;
    p->replay_pending_parent = false;

    p->nreported = 0;
    p->nexpected = -1;
    p->ack_id_up = 0;
    p->ack_id_down = 0;

    PMIx_Byte_object_construct(&p->msg);
    p->msg_compressed = false;
    p->msg_tag = PRTE_RML_TAG_INVALID;
}
static void op_des(op_t* p)
{
    PMIX_BYTE_OBJECT_DESTRUCT(&p->msg);
}
PMIX_CLASS_INSTANCE(op_t, pmix_list_item_t, op_con, op_des);


static void xcast_con(prte_grpcomm_xcast_t* p)
{
    PMIX_CONSTRUCT(&p->ops, pmix_list_t);
    PMIX_CONSTRUCT(&p->pending_ops, pmix_list_t);
    p->op_id_completed = 0;
    p->op_id_completed_at_promotion = 0;
    p->op_id_local = 0;
    p->op_id_global = 0;
}
static void xcast_des(prte_grpcomm_xcast_t* p)
{
    PMIX_LIST_DESTRUCT(&p->ops);
    PMIX_LIST_DESTRUCT(&p->pending_ops);
}
PMIX_CLASS_INSTANCE(prte_grpcomm_xcast_t, pmix_object_t, xcast_con, xcast_des);

