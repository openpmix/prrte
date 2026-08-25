/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * Contains the typedefs for the use of the rml
 */

#ifndef PRTE_RML_TYPES_H_
#define PRTE_RML_TYPES_H_

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <limits.h>
#ifdef HAVE_SYS_UIO_H
/* for struct iovec */
#    include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#    include <net/uio.h>
#endif

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/**
 * Message matching tag
 *
 * Message matching tag.  Unlike MPI, there is no wildcard receive,
 * all messages must match exactly. Tag values less than
 * PRTE_RML_TAG_DYNAMIC are reserved and may only be referenced using
 * a defined constant.
 */
typedef uint32_t prte_rml_tag_t;


/**
 * Function prototype for callback from non-blocking buffer send and receive
 *
 * Function prototype for callback from non-blocking buffer send and
 * receive. On send, the buffer will be the same pointer passed to
 * send_buffer_nb. On receive, the buffer will be allocated and owned
 * by the RML, not the process receiving the callback.
 *
 * @note The parameter in/out parameters are relative to the user's callback
 * function.
 *
 * @param[in] status  Completion status
 * @param[in] peer    Name of peer process
 * @param[in] buffer  Message buffer
 * @param[in] tag     User defined tag for matching send/recv
 * @param[in] cbdata  User data passed to send_buffer_nb() or recv_buffer_nb()
 */
typedef void (*prte_rml_buffer_callback_fn_t)(int status, pmix_proc_t *peer,
                                              pmix_data_buffer_t *buffer,
                                              prte_rml_tag_t tag,
                                              void *cbdata);

/* Convenience def for readability */
#define PRTE_RML_PERSISTENT     true
#define PRTE_RML_NON_PERSISTENT false

/**
 * Constant tag values for well-known services
 */

#define PRTE_RML_TAG PMIX_UINT32

#define PRTE_RML_TAG_INVALID              0
#define PRTE_RML_TAG_DAEMON               1
#define PRTE_RML_TAG_IOF_HNP              2
#define PRTE_RML_TAG_IOF_PROXY            3
#define PRTE_RML_TAG_XCAST_BARRIER        4
#define PRTE_RML_TAG_PLM                  5
#define PRTE_RML_TAG_LAUNCH_RESP          6
#define PRTE_RML_TAG_ERRMGR               7
#define PRTE_RML_TAG_WIREUP               8
#define PRTE_RML_TAG_RML_INFO_UPDATE      9
#define PRTE_RML_TAG_PRTED_CALLBACK       10
#define PRTE_RML_TAG_ROLLUP               11
#define PRTE_RML_TAG_REPORT_REMOTE_LAUNCH 12

#define PRTE_RML_TAG_DAEMON_DIED    13
#define PRTE_RML_TAG_DAEMON_ADOPTED 14

/* The launch message, split off PRTE_RML_TAG_DAEMON so that what a broadcast
 * IS can be read from its tag.
 *
 * It is delivered to the same handler as PRTE_RML_TAG_DAEMON and carries the
 * same command byte - nothing about the receiving side changes. The tag exists
 * to make the message's purpose declarable at the point it is sent, because
 * that is what lets grpcomm choose a data movement by what the message is
 * rather than by guessing from how big it happens to be. PRTE_RML_TAG_DAEMON
 * was the only overloaded broadcast tag, and this was the only large thing
 * on it. */
#define PRTE_RML_TAG_DAEMON_LAUNCH 18

/* The part of the launch message that is addressed to one daemon: the
 * bindings of the procs that daemon is about to fork.  Sent point to point
 * while the rest of the message is broadcast, so the cpusets cost the tree
 * their own size once instead of once per daemon they reach.  See
 * prte_odls_base_send_cpuset_slices(). */
#define PRTE_RML_TAG_LAUNCH_SLICE 19

#define PRTE_RML_TAG_XCAST         15
#define PRTE_RML_TAG_XCAST_ACK     16
/* The bulk broadcast's allgather phase, and the request to abandon it.  Kept
 * off PRTE_RML_TAG_XCAST because these travel over lateral links between
 * daemons that are not tree neighbours, while everything on the XCAST tag is
 * from a parent and is discarded if it is not. */

/* For FileM Base */
#define PRTE_RML_TAG_FILEM_BASE      21
#define PRTE_RML_TAG_FILEM_BASE_RESP 22

/* For FileM RSH Component */
#define PRTE_RML_TAG_FILEM_RSH 23

#define PRTE_RML_TAG_TCONN_RESP      24

/* support data store/lookup */
#define PRTE_RML_TAG_DATA_SERVER 27
#define PRTE_RML_TAG_DATA_CLIENT 28
/* ...and the same request, but for THIS daemon's own store rather than the
 * DVM's.  A PMIX_RANGE_LOCAL item never leaves the daemon that relayed it,
 * so a request about one must not be handed to an external data server -
 * which is what the tag tells the receive, since by then the range is
 * buried in the payload.  See src/runtime/data_server/AGENTS.md. */
#define PRTE_RML_TAG_DATA_SERVER_LOCAL 84

/* collectives */
#define PRTE_RML_TAG_FENCE_RELEASE     31
#define PRTE_RML_TAG_FENCE             33
/* A fence's lateral allgather.  Separate from PRTE_RML_TAG_FENCE because
 * these arrive from exchange partners rather than from a routing-tree child,
 * and the rollup receiver rejects anything that is not in one of its
 * subtrees. */

/* debugger release */
#define PRTE_RML_TAG_DEBUGGER_RELEASE 37

/* bootstrap */
#define PRTE_RML_TAG_BOOTSTRAP 38

/* report a missed msg */
#define PRTE_RML_TAG_MISSED_MSG 39

/* sensor data */
#define PRTE_RML_TAG_SENSOR_DATA 49

/* direct modex support */
#define PRTE_RML_TAG_DIRECT_MODEX      50
#define PRTE_RML_TAG_DIRECT_MODEX_RESP 51

/* error notifications */
#define PRTE_RML_TAG_NOTIFICATION         59

/* stacktrace for debug */
#define PRTE_RML_TAG_STACK_TRACE          60

/* memory profile */
#define PRTE_RML_TAG_MEMPROFILE           61

/* warmup connection - simply establishes the connection */
#define PRTE_RML_TAG_WARMUP_CONNECTION    63

/* node regex report */
#define PRTE_RML_TAG_NODE_REGEX_REPORT    64

/* pmix log requests */
#define PRTE_RML_TAG_LOGGING              65
#define PRTE_RML_TAG_LOGGING_RESP         66

/* scheduler requests */
#define PRTE_RML_TAG_SCHED                72
#define PRTE_RML_TAG_SCHED_RESP           73

/* group construct */
#define PRTE_RML_TAG_GROUP                74
#define PRTE_RML_TAG_GROUP_RELEASE        75

// monitor
#define PRTE_RML_TAG_MONITOR_REQUEST      76
#define PRTE_RML_TAG_MONITOR_RESP         77

/* reliable messaging */
#define PRTE_RML_TAG_RELM_STATE           78
#define PRTE_RML_TAG_RELM_LINK            79

/* daemon return / revival (the bootstrap "unheal" path): a rebooted daemon
 * announces itself to the HNP with DAEMON_RETURNED; the HNP broadcasts
 * DAEMON_REVIVED so every daemon re-inserts the returned rank into its tree */
#define PRTE_RML_TAG_DAEMON_RETURNED      80
#define PRTE_RML_TAG_DAEMON_REVIVED       81

/* a daemon's rendered show_help text, on its way to the HNP - see
 * src/util/prte_show_help.c for why a prted cannot emit its own */
#define PRTE_RML_TAG_SHOW_HELP            82

/* the membership of a PMIx_Connect assemblage, on its way to the DVM master,
 * which is where it is held - see src/prted/pmix/pmix_server_connect.c */
#define PRTE_RML_TAG_CONNECTED            83

#define PRTE_RML_TAG_MAX                 100

#define PRTE_RML_TAG_NTOH(t) ntohl(t)
#define PRTE_RML_TAG_HTON(t) htonl(t)

/*** length of the tag. change this when type of prte_rml_tag_t is changed ***/
/*** max valu in unit32_t is 0xFFFF_FFFF when converted to char this is 8  **
#define PRTE_RML_TAG_T_CHAR_LEN   8
#define PRTE_RML_TAG_T_SPRINT    "%8x" */

/* ******************************************************************** */

/*
 * RML proxy commands
 */
typedef uint8_t prte_rml_cmd_flag_t;
#define PRTE_RML_CMD        PMIX_UINT8
#define PRTE_RML_UPDATE_CMD 1

typedef struct {
    pmix_object_t super;
    pmix_proc_t name;
    pmix_data_buffer_t data;
    bool active;
} prte_rml_recv_cb_t;
PMIX_CLASS_DECLARATION(prte_rml_recv_cb_t);

/* A payload that several sends transmit without any of them owning it.
 *
 * The ordinary send owns its buffer: exactly one prte_rml_send_t points at a
 * pmix_data_buffer_t and disposes of it when the send completes.  That is the
 * right rule for point-to-point traffic and the wrong one for a broadcast,
 * where the same bytes go to every routing-tree child.  Building one buffer
 * per child costs a full copy of the payload per child, all of it on the
 * progress thread and all of it ahead of the first byte reaching the wire -
 * at the default radix that is 64 identical copies of a launch message the
 * daemon then sends 64 times.
 *
 * A payload is a reference-counted wrapper around one such buffer.  Each send
 * that accepts it takes its own reference and drops it on completion; the
 * originator drops the reference it got from PMIX_NEW once it has handed the
 * payload to everyone it means to.  The buffer dies with the last reference,
 * whichever send that turns out to be.
 *
 * Nothing about the transport had to change to allow this: the OOB reads
 * base_ptr/bytes_used and keeps all of its per-destination progress
 * (sdptr/sdbytes/hdr_sent) in the send object, so several sends can read one
 * buffer concurrently.  The one rule is the obvious one - a shared payload is
 * immutable from the moment it is first handed to a send.
 */
typedef struct {
    pmix_object_t super;
    pmix_data_buffer_t *dbuf;
} prte_rml_payload_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rml_payload_t);

/* structure to send RML messages - used internally */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t dst; // targeted recipient
    pmix_proc_t origin;
    int status;         // returned status on send
    prte_rml_tag_t tag; // targeted tag
    int retries;        // #times we have tried to send it

    /* user's send callback functions and data */
    prte_rml_buffer_callback_fn_t cbfunc;
    void *cbdata;

    /* data buffer */
    pmix_data_buffer_t *dbuf;
    /* When non-NULL, dbuf is this payload's buffer and is NOT owned by this
     * send: the send holds a reference to the payload instead, dropped in the
     * destructor, and completion hands the callback no buffer to dispose of.
     * NULL for every ordinary send, which owns its dbuf outright. */
    prte_rml_payload_t *payload;
    /* msg seq number */
    uint32_t seq_num;
    /* boot epoch (incarnation) of the origin. Defaults to this process's own
     * epoch when a message originates here; a relayed message carries the
     * original sender's epoch, copied from the received wire header. Lets a
     * hop drop traffic from a stale incarnation of a rebooted bootstrap rank. */
    uint64_t epoch;
    /* Send straight to dst rather than to the next hop the routing tree would
     * pick.  A bandwidth-efficient collective exchanges with partners that are
     * not its tree neighbours, and routing those exchanges through the tree
     * would funnel them via the root - which is exactly the bottleneck such a
     * collective exists to avoid.  Only ever set for a peer whose contact info
     * we can obtain; the send falls back to the routed path if we cannot. */
    bool direct;
} prte_rml_send_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rml_send_t);

/* define an object for transferring send requests to the event lib */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_rml_send_t send;
} prte_rml_send_request_t;
PMIX_CLASS_DECLARATION(prte_rml_send_request_t);

/* structure to recv RML messages - used internally */
typedef struct {
    pmix_list_item_t super;
    prte_event_t ev;
    pmix_proc_t sender;      // sender
    prte_rml_tag_t tag;      // targeted tag
    uint32_t seq_num;        // sequence number
    pmix_data_buffer_t *dbuf; // the recvd data
} prte_rml_recv_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rml_recv_t);

typedef struct {
    pmix_list_item_t super;
    bool buffer_data;
    pmix_proc_t peer;
    prte_rml_tag_t tag;
    bool persistent;
    prte_rml_buffer_callback_fn_t cbfunc;
    void *cbdata;
} prte_rml_posted_recv_t;
PMIX_CLASS_DECLARATION(prte_rml_posted_recv_t);

/* define an object for transferring recv requests to the list of posted recvs */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    bool cancel;
    prte_rml_posted_recv_t *post;
} prte_rml_recv_request_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rml_recv_request_t);

/* struct for traversing the routing tree - used internally */
typedef struct {
    pmix_rank_t rank;
    pmix_rank_t depth; // depth of this rank in tree
    pmix_rank_t width; // width of this rank's layer in tree
    pmix_rank_t count; // max count of radix tree of this width
    pmix_rank_t base;  // rank the node was originally constructed as
                       //   note: used for going back down the tree after going
                       //   up, not necessarily related to failures
} prte_rml_routed_tree_node_t;

/* Outcome of reconciling a peer's report of THIS daemon's ancestor list
 * against our own view of it - see prte_rml_reconcile_ancestry(). */
typedef enum {
    /* The two views agree once our own failure knowledge has been applied to
     * the report: the peer told us nothing we did not already know. */
    PRTE_RML_ANCESTRY_AGREED,
    /* The peer knows of ancestor deaths we do not.  The inferred array holds
     * the ranks that must have failed for its view to be the correct one. */
    PRTE_RML_ANCESTRY_INFERRED,
    /* Reconciling would require declaring a rank dead that has RETURNED, so
     * the report necessarily predates the return and says nothing current.
     * A race to be ignored, not an error. */
    PRTE_RML_ANCESTRY_STALE,
    /* No set of ancestor deaths reconciles the two views. */
    PRTE_RML_ANCESTRY_INCONSISTENT
} prte_rml_ancestry_t;

/* struct for passing information about a daemon fault to MCA components */
typedef struct {
    pmix_object_t super;

    // Faults are handled by the RML as they are discovered, but they are also
    // passed up the tree to be broadcast by the HNP.Handlers will be called
    // twice for all faults - once when the fault is first detected and once
    // when it is globally xcast.
    //
    // Global scope notifications will occur in the same order on all ranks, but
    // RML recovery occurs prior to the local scope notifications - so global
    // notifications will always report no changes to the tree. Components are
    // therefore responsible for saving any necessary data between the local and
    // global scope recoveries.
    enum {
        PRTE_RML_FAULT_SCOPE_LOCAL,
        PRTE_RML_FAULT_SCOPE_GLOBAL
    } scope;

    pmix_data_array_t failed_ranks;

    // The collective recovery epoch this event moves the DVM to, and the
    // reason it is carried here rather than counted by each handler: it is
    // issued by the HNP when it originates the global notice, so every daemon
    // adopts the same number no matter how many notices it has seen. A daemon
    // that missed one - which a daemon launched into an already-recovered DVM
    // necessarily has - is corrected by the next one that reaches it, and by
    // the WIREUP broadcast it receives on joining.
    //
    // Only meaningful when scope is GLOBAL; zero on the local pass, which
    // does not move the epoch.
    uint32_t epoch;

    // Ancestor deaths could lead to this rank substituting for a hole further
    // up the tree. If we've been promoted, our subtree has grown. At most, one
    // of our old children can be the same. Their position in the children list
    // may be different.
    //
    // It is important to note that, depending on how failure information ends
    // up propagating, that single child may believe they had a different
    // parent for a short time between. So, if a component chooses to discard
    // messages from old parents, some messages to that child may be lost.
    //
    // This means appropriate handling should almost always treat all children
    // as new if we have been promoted.
    //
    // Similar reasoning applies to our parent.
    bool promoted;

    // The mirror of promoted, set by the unheal path: a daemon that had
    // departed returned, re-inserting itself above us, so our ancestor list
    // grew and our subtree shrank. As with promotion, handlers should treat
    // the re-homing children as new, because a child that briefly routed
    // through the grandparent must discard that lineage.
    bool demoted;

    bool ancestors_changed;
    pmix_data_array_t prev_ancestors; // pmix_rank_t

    bool parent_changed;
    pmix_rank_t prev_parent;

    bool children_changed;
    pmix_data_array_t prev_children; // pmix_rank_t
} prte_rml_recovery_status_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rml_recovery_status_t);

END_C_DECLS

#endif /* RML_TYPES */
