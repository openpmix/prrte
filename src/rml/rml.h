/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Runtime Messaging Layer (RML) Communication Interface
 *
 * The Runtime Messaging Layer (RML) provices basic point-to-point
 * communication between PRTE processes.  The system is available for
 * most architectures, with some exceptions (the Cray XT3/XT4, for example).
 */

#ifndef PRTE_RML_H_
#define PRTE_RML_H_

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/rml/rml_types.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS
/**
 * Send a buffer non-blocking message
 *
 * Send a buffer to the specified peer. The RML takes ownership of the buffer,
 * which may no longer be modified, read from, or reused in subsequent sends.
 *
 * @param[in] peer   Name of receiving process
 * @param[in] buffer Pointer to buffer to be sent
 * @param[in] tag    User defined tag for matching send/recv
 *
 * @retval PRTE_SUCCESS The message was successfully started
 * @retval PRTE_ERR_BAD_PARAM One of the parameters was invalid
 * @retval PRTE_ERR_ADDRESSEE_UNKNOWN Contact information for the
 *                    receiving process is not available
 * @retval PRTE_ERR_NODE_DOWN The receiving process is believed to have failed
 * @retval PRTE_ERROR  An unspecified error occurred
 */
PRTE_EXPORT int prte_rml_send_buffer_nb(pmix_rank_t rank,
                                        pmix_data_buffer_t *buffer,
                                        prte_rml_tag_t tag);

#define PRTE_RML_SEND(_r, r, b, t)                              \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-SEND(%s:%d): %s:%s:%d",        \
                            PMIX_RANK_PRINT(r), t,              \
                            __FILE__, __func__, __LINE__);      \
        (_r) = prte_rml_send_buffer_nb(r, b, t);                \
    } while(0)

/**
 * As prte_rml_send_buffer_nb, but tell the caller how the send ended.
 *
 * The return value of a plain send says only that the message was *queued*.
 * The next hop is resolved on a later event, so an unreachable addressee, a
 * peer that goes away mid-flight, or a connection that never completes are all
 * discovered after the call has returned - and reported nowhere the caller can
 * see.  A caller that is tracking whether the message arrived (a collective
 * waiting on the subtree beneath this hop) needs to be told.
 *
 * cbfunc runs on the progress thread and OWNS the buffer, exactly as the
 * default prte_rml_send_callback does - releasing it is the callback's job, and
 * chaining to prte_rml_send_callback is the way to keep the RML's own failure
 * reporting while adding to it.
 *
 * A message to *self* short-circuits into the local receive path and never
 * reaches the OOB, so cbfunc does not fire for one (the receive owns the
 * buffer).  There is nothing to report there - a self-send cannot fail - but a
 * caller counting callbacks must not count on one.
 */
PRTE_EXPORT int prte_rml_send_buffer_cb_nb(pmix_rank_t rank,
                                           pmix_data_buffer_t *buffer,
                                           prte_rml_tag_t tag,
                                           prte_rml_buffer_callback_fn_t cbfunc,
                                           void *cbdata);

#define PRTE_RML_SEND_CB(_r, r, b, t, cf, cd)                   \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-SEND-CB(%s:%d): %s:%s:%d",     \
                            PMIX_RANK_PRINT(r), t,              \
                            __FILE__, __func__, __LINE__);      \
        (_r) = prte_rml_send_buffer_cb_nb(r, b, t, cf, cd);     \
    } while(0)

/**
 * As prte_rml_send_buffer_cb_nb, but transmit a *shared* payload that the send
 * does not own.
 *
 * This is what a broadcast wants.  The ordinary send owns its buffer, so
 * sending identical bytes to every routing-tree child means building one
 * buffer per child - a full copy of the payload each, all of them made on the
 * progress thread before the first byte reaches the wire.  At the default
 * radix that is 64 copies of a launch message, 64 allocations of it, and 64x
 * its size resident on the sender at once.  A payload is packed once and
 * handed to every child.
 *
 * Ownership: the caller holds the reference PMIX_NEW gave it and must release
 * that reference once it has finished handing the payload out.  Each call that
 * returns PRTE_SUCCESS takes a reference of its own, dropped when that send
 * completes; a call that returns an error takes none.  So the buffer lives
 * until the last destination is done with it, whether that is a send or the
 * caller.
 *
 * The payload must not be modified once it has been handed to a send - several
 * sends read it concurrently.
 *
 * A message to *self* cannot share: the local receive path takes ownership of
 * the buffer it delivers.  Rather than making that a rule callers have to know,
 * a self-send here copies the payload and proceeds normally.
 *
 * cbfunc is called with a NULL buffer (see PRTE_RML_SEND_COMPLETE) because
 * there is nothing for it to dispose of.
 */
PRTE_EXPORT int prte_rml_send_payload_cb_nb(pmix_rank_t rank,
                                            prte_rml_payload_t *payload,
                                            prte_rml_tag_t tag,
                                            prte_rml_buffer_callback_fn_t cbfunc,
                                            void *cbdata);

#define PRTE_RML_SEND_PAYLOAD_CB(_r, r, p, t, cf, cd)               \
    do {                                                            \
        pmix_output_verbose(2, prte_rml_base.rml_output,            \
                            "RML-SEND-PAYLOAD-CB(%s:%d): %s:%s:%d", \
                            PMIX_RANK_PRINT(r), t,                  \
                            __FILE__, __func__, __LINE__);          \
        (_r) = prte_rml_send_payload_cb_nb(r, p, t, cf, cd);        \
    } while (0)

/**
 * As prte_rml_send_buffer_nb, but bypass the routing tree and deliver straight
 * to the named peer.
 *
 * The routed send picks the next hop toward its target, which at the default
 * radix funnels any non-child through the root.  That is right for control
 * traffic and wrong for a bandwidth-efficient collective, whose exchange
 * partners are deliberately not its tree neighbours: routing those exchanges
 * would push every byte through the one node the algorithm exists to keep out
 * of the path.
 *
 * Falls back to a routed send if the peer's contact information cannot be
 * obtained, so a caller never has to handle "no direct route" itself.
 *
 * A peer reached this way should be registered with
 * prte_rml_lateral_register() so that losing the link is not mistaken for a
 * routing-tree fault.
 */
PRTE_EXPORT int prte_rml_send_buffer_direct_nb(pmix_rank_t rank,
                                               pmix_data_buffer_t *buffer,
                                               prte_rml_tag_t tag);

#define PRTE_RML_SEND_DIRECT(_r, r, b, t)                       \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-SEND-DIRECT(%s:%d): %s:%s:%d", \
                            PMIX_RANK_PRINT(r), t,              \
                            __FILE__, __func__, __LINE__);      \
        (_r) = prte_rml_send_buffer_direct_nb(r, b, t);         \
    } while(0)

/**
 * Lateral links: peers we talk to directly that are not our routing-tree
 * neighbours.
 *
 * Registering one is not what creates the link - prte_rml_send_buffer_direct_nb
 * does that.  Registration is what tells the fault machinery how to read the
 * link's death.  A routing-tree connection dropping means the tree has changed
 * shape and must be repaired; a lateral link dropping means nothing of the
 * sort, and repairing the tree on the strength of it would abort every
 * in-flight collective across the DVM.
 *
 * A rank can be both: a collective's exchange partner may happen to be our
 * parent or one of our children.  Losing *that* link is a genuine tree fault,
 * so the registry is only consulted for peers that are purely lateral.
 */
PRTE_EXPORT void prte_rml_lateral_register(pmix_rank_t rank);
PRTE_EXPORT void prte_rml_lateral_deregister(pmix_rank_t rank);

/* Is this rank one we hold a lateral link to and NOT a routing-tree
 * neighbour?  False for anything in the tree, so a caller can use it as the
 * single test for "this loss is not the tree's business". */
PRTE_EXPORT bool prte_rml_is_lateral_only(pmix_rank_t rank);

/* Notified when a purely-lateral link is lost, so the collective that opened
 * it can end or re-plan.  The RML does nothing else about such a loss. */
typedef void (*prte_rml_lateral_lost_fn_t)(pmix_rank_t rank);
PRTE_EXPORT void prte_rml_lateral_set_lost_callback(prte_rml_lateral_lost_fn_t cbfunc);

/**
 * As prte_rml_send_buffer_nb, but attempts to deliver message even after daemon
 * failures
 */
PRTE_EXPORT int prte_rml_send_buffer_reliable_nb(pmix_rank_t rank,
                                                 pmix_data_buffer_t *buffer,
                                                 prte_rml_tag_t tag);

#define PRTE_RML_RELIABLE_SEND(_r, r, b, t)                       \
    do {                                                          \
        pmix_output_verbose(2, prte_rml_base.rml_output,          \
                            "RML-RELIABLE-SEND(%s:%d): %s:%s:%d", \
                            PMIX_RANK_PRINT(r), t,                \
                            __FILE__, __func__, __LINE__);        \
        (_r) = prte_rml_send_buffer_reliable_nb(r, b, t);         \
    } while(0)

/**
 * Purge the RML/OOB of contact info and pending messages
 * to/from a specified process. Used when a process aborts
 */
PRTE_EXPORT void prte_rml_purge(pmix_proc_t *peer);

#define PRTE_RML_PURGE(p)                                       \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-PURGE(%s): %s:%s:%d",          \
                            PRTE_NAME_PRINT(p),                 \
                            __FILE__, __func__, __LINE__);      \
        prte_rml_purge(p);                                      \
    } while(0)

/**
 * Receive a buffer non-blocking message
 *
 * @param[in]  peer    Peer process or PRTE_NAME_WILDCARD for wildcard receive
 * @param[in]  tag     User defined tag for matching send/recv
 * @param[in] persistent Boolean flag indicating whether or not this is a one-time recv
 * @param[in] cbfunc   Callback function on message comlpetion
 * @param[in] cbdata   User data to provide during completion callback
 */
PRTE_EXPORT void prte_rml_recv_buffer_nb(pmix_proc_t *peer, prte_rml_tag_t tag,
                                         bool persistent,
                                         prte_rml_buffer_callback_fn_t cbfunc,
                                         void *cbdata);

#define PRTE_RML_RECV(p, t, prs, c, cb)                         \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-RECV(%d): %s:%s:%d",           \
                            t, __FILE__, __func__, __LINE__);   \
        prte_rml_recv_buffer_nb(p, t, prs, c, cb);              \
    } while(0)


/**
 * Cancel a posted non-blocking receive
 *
 * Attempt to cancel a posted non-blocking receive.
 *
 * @param[in] peer    Peer process or PRTE_NAME_WILDCARD, exactly as passed
 *                    to the non-blocking receive call
 * @param[in] tag     Posted receive tag
 */
PRTE_EXPORT void prte_rml_recv_cancel(pmix_proc_t *peer, prte_rml_tag_t tag);

#define PRTE_RML_CANCEL(p, t)                                   \
    do {                                                        \
        pmix_output_verbose(2, prte_rml_base.rml_output,        \
                            "RML-CANCEL(%d): %s:%s:%d",         \
                            t, __FILE__, __func__, __LINE__);   \
        prte_rml_recv_cancel(p, t);                             \
    } while(0)

/**
 * Simulate this node's failure on TCP sockets, then raise SIGKILL
 */
PRTE_EXPORT void prte_rml_simulate_node_failure(void);

typedef struct {
    int rml_output;
    int routed_output;
    int max_retries;
    pmix_list_t posted_recvs;
    pmix_list_t unmatched_msgs;
    int radix;
    bool static_ports;

    // # daemons before failures
    pmix_rank_t n_dmns;
    // All faults known, globally confirmed or not
    pmix_bitmap_t failed_dmns;
    // All daemons globally confirmed to have failed - i.e. the subset of
    // failed_dmns whose departure the HNP has already broadcast. Its one
    // consumer is send_failures_notice, which subtracts it from failed_dmns
    // to work out which of this daemon's subtree failures a NEW parent has
    // yet to hear about. prte_rml_compute_routing_tree re-initializes it (the
    // subtraction is a bitmap XOR, which needs both operands the same width,
    // and a grow widens failed_dmns) but carries the marks across, because
    // nothing else in the daemon records that a departure has been broadcast.
    pmix_bitmap_t global_failed_dmns;
    // Ranks that have permanently departed the DVM (shrunk out, or lost to a
    // fault in a launched/elastic DVM). Unlike failed_dmns, this set is NOT
    // re-initialized by prte_rml_compute_routing_tree, so it survives the
    // recompute that a grow triggers and lets the rebuilt tree keep routing
    // around the hole (#2491).
    pmix_bitmap_t dead_dmns;
    // Ranks currently absent from a bootstrapped DVM because their node was
    // lost (e.g. powered off) but which may return with the same rank when the
    // node reboots. Like dead_dmns this persists across compute_routing_tree
    // recomputes and is restored into failed_dmns, so the tree routes around
    // the hole while the daemon is gone; unlike dead_dmns it is cleared when
    // the daemon returns (the "unheal" path). Only bootstrap faults populate
    // it -- a launched/elastic departure remains permanent in dead_dmns.
    pmix_bitmap_t absent_dmns;
    // Ranks that have come back (the unheal path cleared their absent mark)
    // and have not since been confirmed dead again. Constructed once, like the
    // two sets above. Its one job is to bound what may be *inferred*: a peer's
    // report of our lineage is a snapshot of that peer's view when it sent,
    // and a revival travels as its own xcast, so a notice that crossed with
    // one carries a lineage predating the return. Acting on it would bury a
    // daemon that is alive and talking to us, so a rank in this set may only
    // be declared dead by something that observed it directly - a lost socket,
    // or the HNP's arbitrated broadcast. Cleared again when such a death is
    // recorded, so a rank that returns and later really dies is not pinned
    // alive forever.
    pmix_bitmap_t revived_dmns;

    // Highest boot epoch (incarnation) known for each daemon rank, indexed by
    // rank; 0 means "not yet learned". A rebooted bootstrap daemon returns with
    // a strictly-greater epoch, so traffic stamped with an older epoch for a
    // rank is a stale incarnation and is dropped. Grown on demand as ranks
    // appear; see prte_rml_epoch_ok / prte_rml_record_epoch.
    uint64_t *peer_epochs;
    size_t peer_epochs_size;

    // Track all ancestors up to HNP, to simplify fault handling
    pmix_data_array_t ancestors;
    // My immediate ancestor
    pmix_rank_t lifeline;

    // Placement in routing tree, which changes based on faults
    prte_rml_routed_tree_node_t cur_node;

    // Array of children, indexed by fault-free subtree order
    // Some values may be PMIX_RANK_INVALID and should be ignored
    pmix_data_array_t children;
    // Count of children != PMIX_RANK_INVALID
    int n_children;

    // Ranks we hold a link to that is NOT part of the routing tree, opened by
    // prte_rml_send_buffer_direct_nb on behalf of a collective whose exchange
    // partners are chosen for bandwidth rather than for tree adjacency. This
    // set exists so a dropped connection can be read correctly: a tree
    // connection dropping means the tree changed shape and must be repaired,
    // a lateral one dropping means nothing about the tree at all. Repairing on
    // the strength of a lateral loss would end every in-flight collective in
    // the DVM. Deliberately NOT re-initialized by compute_routing_tree - a
    // grow does not dissolve a collective's exchange partners.
    pmix_bitmap_t lateral_links;
    // Told when a purely-lateral link is lost, so the collective that opened it
    // can end or re-plan. NULL until a collective registers interest.
    prte_rml_lateral_lost_fn_t lateral_lost_cb;
} prte_rml_base_t;

PRTE_EXPORT extern prte_rml_base_t prte_rml_base;

/* This process's boot epoch (incarnation) - a millisecond wall-clock timestamp
 * captured once at startup. A daemon that departs and reboots into the same
 * rank comes back with a strictly-greater epoch. Stamped into every outgoing
 * message's wire header as the origin's epoch. */
PRTE_EXPORT extern uint64_t prte_rml_boot_epoch;

/* Incarnation guard. prte_rml_epoch_ok records rank's epoch on first sight and
 * returns false only for traffic stamped with an epoch strictly older than the
 * one already known for that rank (a stale incarnation to be dropped); a newer
 * epoch passes but does not advance the table (the arbitrated revival does
 * that). prte_rml_record_epoch force-sets the authoritative epoch for a rank
 * (used by the revival path and the HNP's return validation);
 * prte_rml_get_epoch reads it (0 if unknown). */
PRTE_EXPORT bool prte_rml_epoch_ok(pmix_rank_t rank, uint64_t epoch);
PRTE_EXPORT void prte_rml_record_epoch(pmix_rank_t rank, uint64_t epoch);
PRTE_EXPORT uint64_t prte_rml_get_epoch(pmix_rank_t rank);

PRTE_EXPORT void prte_rml_register(void);
PRTE_EXPORT void prte_rml_close(void);
PRTE_EXPORT int prte_rml_open(void);
/* Render dead_dmns as a compact, range-collapsed list ("2,7:9") for the prted
 * command line, or NULL when nothing has departed. The caller frees it. The
 * partner routine loads such a list back into dead_dmns; it is what a daemon
 * launched into a DVM that has already lost ranks uses to build its FIRST
 * routing tree, since nothing it could receive would arrive in time. */
PRTE_EXPORT char *prte_rml_render_dead_dmns(void);
PRTE_EXPORT void prte_rml_load_dead_dmns(const char *spec);
/* common implementations */
PRTE_EXPORT void prte_rml_base_post_recv(int sd, short args, void *cbdata);
PRTE_EXPORT void prte_rml_base_process_msg(int fd, short flags, void *cbdata);
PRTE_EXPORT void prte_rml_send_callback(int status, pmix_proc_t *peer,
                                        pmix_data_buffer_t *buffer,
                                        prte_rml_tag_t tag, void *cbdata);
PRTE_EXPORT void prte_rml_compute_routing_tree(void);
PRTE_EXPORT void prte_rml_update_ancestors(pmix_data_array_t* ancestors);
/* Reconcile a peer's report of THIS daemon's ancestor list (root first, not
 * including us) against prte_rml_base.ancestors.  Both notices that reshape the
 * tree carry such a list - the adoption notice a parent sends down, and the
 * failure notice a child sends up - and both reconcile it here.
 *
 * `report` is normalized in place against our own failure knowledge; `inferred`
 * receives the ranks that must have died when the return is
 * PRTE_RML_ANCESTRY_INFERRED, and is left empty otherwise.  Both arrays belong
 * to the caller.  Nothing is left marked failed: the caller decides whether to
 * act on the inference by driving prte_rml_repair_routing_tree(). */
PRTE_EXPORT prte_rml_ancestry_t prte_rml_reconcile_ancestry(pmix_data_array_t* report,
                                                            pmix_data_array_t* inferred);
/* Repair the routing tree around a set of departed ranks and notify the fault
 * handlers.  `epoch` is the collective recovery epoch the HNP issued with the
 * global notice being applied; it is carried through to the handlers on the
 * status and is meaningful only when `global` is true - pass 0 on every local
 * repair, which does not move the epoch. */
PRTE_EXPORT void prte_rml_repair_routing_tree(pmix_data_array_t* failed_ranks,
                                              bool global, uint32_t epoch);
PRTE_EXPORT void prte_rml_revive_routing_tree(pmix_rank_t rank);
PRTE_EXPORT void prte_rml_fault_handler(const prte_rml_recovery_status_t* s);
PRTE_EXPORT int prte_rml_get_num_contributors(pmix_rank_t *dmns, size_t ndmns);
PRTE_EXPORT void prte_rml_recv_failures_notice(int status, pmix_proc_t *sender,
                                               pmix_data_buffer_t* buf,
                                               prte_rml_tag_t tag,
                                               void *cbdata);
PRTE_EXPORT void prte_rml_recv_adoption_notice(int status, pmix_proc_t *sender,
                                               pmix_data_buffer_t* buf,
                                               prte_rml_tag_t tag,
                                               void *cbdata);
/* A bootstrap daemon announces its (re)appearance to the HNP; the HNP
 * validates the rank is currently absent and broadcasts a revival. */
PRTE_EXPORT void prte_rml_send_return_notice(void);
PRTE_EXPORT void prte_rml_recv_return_request(int status, pmix_proc_t *sender,
                                              pmix_data_buffer_t* buf,
                                              prte_rml_tag_t tag,
                                              void *cbdata);
PRTE_EXPORT void prte_rml_recv_revival_notice(int status, pmix_proc_t *sender,
                                              pmix_data_buffer_t* buf,
                                              prte_rml_tag_t tag,
                                              void *cbdata);
PRTE_EXPORT int prte_rml_route_lost(pmix_rank_t route);
PRTE_EXPORT pmix_rank_t prte_rml_get_route(pmix_rank_t target);
PRTE_EXPORT int prte_rml_get_subtree_index(pmix_rank_t target);
PRTE_EXPORT bool prte_rml_is_node_up(pmix_rank_t node);

#define PRTE_RML_ACTIVATE_MESSAGE(m)                                           \
    do {                                                                       \
        /* setup the event */                                                  \
        prte_event_set(prte_event_base, &(m)->ev, -1, PRTE_EV_WRITE,           \
                       prte_rml_base_process_msg, (m));                        \
        prte_event_active(&(m)->ev, PRTE_EV_WRITE, 1);                         \
    } while (0);

#define PRTE_RML_POST_MESSAGE(p, t, s, b, l)                                                \
    do {                                                                                    \
        prte_rml_recv_t *_msg;                                                              \
        pmix_status_t _rc;                                                                  \
        pmix_byte_object_t _bo;                                                             \
        pmix_output_verbose(5, prte_rml_base.rml_output,                                    \
                            "%s Message posted at %s:%d for tag %d",                        \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__, (t));   \
        _msg = PMIX_NEW(prte_rml_recv_t);                                                   \
        PMIX_XFER_PROCID(&_msg->sender, (p));                                               \
        _msg->tag = (t);                                                                    \
        _msg->seq_num = (s);                                                                \
        _bo.bytes = (char *) (b);                                                           \
        _bo.size = (l);                                                                     \
        PMIX_DATA_BUFFER_CREATE(_msg->dbuf);                                                \
        _rc = PMIx_Data_load(_msg->dbuf, &_bo);                                             \
        if (PMIX_SUCCESS != _rc) {                                                          \
            PMIX_ERROR_LOG(_rc);                                                            \
        }                                                                                   \
        PRTE_RML_ACTIVATE_MESSAGE(_msg);                                                    \
    } while (0);

/* Complete a send: hand the message to its callback and dispose of the
 * send object. The callback owns the data buffer, so the send's reference
 * to it is dropped before release - otherwise the destructor would free a
 * buffer the callback has already released. Nobody else holds a reference
 * to the send object at this point, so the caller must not touch it after
 * completing it.
 *
 * A send carrying a *shared payload* is the exception: its buffer belongs to
 * the payload and other destinations may still be transmitting it, so the
 * callback is handed no buffer at all and the send's reference is dropped by
 * the destructor below. A callback that wants to inspect the payload must
 * therefore tolerate a NULL buffer - which the default one does, and which is
 * the honest statement of the situation: this callback has nothing to free.
 */
#define PRTE_RML_SEND_COMPLETE(m)                                                             \
    do {                                                                                      \
        prte_rml_send_t *_snd = (m);                                                          \
        pmix_data_buffer_t *_dbuf = (NULL == _snd->payload) ? _snd->dbuf : NULL;              \
        pmix_output_verbose(5, prte_rml_base.rml_output,                                      \
                            "%s-%s Send message complete at %s:%d",                           \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(_snd->dst)),\
                            __FILE__, __LINE__);                                              \
        _snd->dbuf = NULL;                                                                    \
            /* non-blocking buffer send */                                                    \
        _snd->cbfunc(_snd->status, &(_snd->dst),                                              \
                     _dbuf, _snd->tag, _snd->cbdata);                                         \
        PMIX_RELEASE(_snd);                                                                   \
    } while (0);


END_C_DECLS

#endif
