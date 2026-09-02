/* -*- C -*-
 *
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PRTE_GRPCOMM_INTERNAL_H
#define PRTE_GRPCOMM_INTERNAL_H

#include "prte_config.h"

#include "src/class/pmix_bitmap.h"

#include "src/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/* Which tree a broadcast travels.
 *
 * There is more than one: the routing tree, at a high radix, which everything
 * has always used; and the release tree, at a low one, which a collective's
 * fan-out will use because the two want opposite radices - fanout is free on
 * the way up a rollup and is the entire cost on the way down a release. See
 * docs/plans/scalable_collectives/two-radix-release.rst.
 *
 * The topology is part of a broadcast's identity, not a routing hint. Two
 * trees cannot share reliability accounting: a completion count is a
 * statement about one tree's shape, and acks rolled up tree A say nothing
 * about coverage of a message that travelled tree B. So each topology gets
 * its own op-id sequence, its own completion state, and its own idea of who
 * its parent and children are - and a message names which it belongs to.
 *
 * Keep PRTE_GRPCOMM_TOPO_ROUTING at zero: it is the value a zeroed structure
 * or an unstamped message lands on, and it is the one that has always been
 * meant. */
typedef enum {
    PRTE_GRPCOMM_TOPO_ROUTING = 0,
    PRTE_GRPCOMM_TOPO_RELEASE,
    PRTE_GRPCOMM_TOPO_COUNT
} prte_grpcomm_topology_t;

/* The per-tree reliability state.  One of these per topology: the sequence is
 * what orders ops within a tree, and two trees ordering against one counter
 * would have each raising out-of-order on the other's traffic. */
typedef struct {
    // ID of the last known completed (in our subtree) operation
    size_t op_id_completed;
    // op_id_completed when we were last promoted
    // (our subtree grew, so we can't assume completion in our new subtree)
    size_t op_id_completed_at_promotion;
    // ID of the last known initiated operation
    size_t op_id_inited;
} prte_grpcomm_tree_state_t;

/* Tracks ongoing xcast operations to ensure all messages are delivered exactly
 * once to all daemons even in the presence of daemon failures */
typedef struct {
    pmix_object_t super;
    // list of ongoing operations, defined in grpcomm_xcast.c
    pmix_list_t ops;
    // FIFO of completion callbacks for master-originated broadcasts awaiting
    // relay back to the master (see grpcomm_xcast.c)
    pmix_list_t pending_completions;
    // reliability state, one set per tree
    prte_grpcomm_tree_state_t tree[PRTE_GRPCOMM_TOPO_COUNT];
} prte_grpcomm_xcast_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_xcast_t);

/*
 * grpcomm's own global state.  This was two structs - the MCA framework's
 * "base" and the component's own - until grpcomm stopped being a framework;
 * they were always one thing.
 */
typedef struct {
    // verbosity stream, from the grpcomm_base_verbose MCA parameter (the
    // parameter keeps its old name so existing debug recipes still work)
    int output;
    // The group context-id pool.  A construct asking for
    // PMIX_GROUP_ASSIGN_CONTEXT_ID gets this value, and it *decrements* -
    // it counts down from UINT32_MAX so DVM-assigned context ids cannot
    // collide with ids assigned from the bottom of the range elsewhere.
    uint32_t context_id;
    prte_grpcomm_xcast_t xcast_ops;
    // track ongoing fence operations - list of prte_grpcomm_fence_t
    pmix_list_t fence_ops;
    // track ongoiong group operations - list of prte_grpcomm_group_t
    pmix_list_t group_ops;
    // A short memory of group operations we have already released - list of
    // prte_grpcomm_group_memo_t, capped at PRTE_GRPCOMM_GROUP_MEMO_MAX. A
    // contribution can arrive after the release that retired its tracker has
    // already been processed here; without this, get_tracker() would take it
    // as the first contribution to a brand-new operation and build a tracker
    // that nothing will ever complete or delete.
    pmix_list_t completed_group_ops;
    // How many fences over each signature this daemon has released - list of
    // prte_grpcomm_fence_memo_t, capped at PRTE_GRPCOMM_FENCE_MEMO_MAX. This
    // is what tells a straggler from the next round: a fence signature is
    // only its participant list, so without a per-round number on the wire a
    // contribution that outlived the release which ended its fence is
    // indistinguishable from the first contribution to the next fence over
    // the same procs. Unlike completed_group_ops this is a counter rather
    // than a memo of "already done", because a fence has no local client
    // whose arrival could forget the entry - see the commentary in
    // grpcomm_fence.c.
    pmix_list_t fence_generations;
    // The collective recovery epoch for this daemon. A daemon failure
    // invalidates every in-flight rollup, because how many contributions each
    // daemon expects is derived from the routing tree. Recovery is a
    // simultaneous restart across the DVM, and this counter is what tells one
    // round from the next, so a contribution still in flight from before the
    // failure can be recognized as stale. Shared by fence and group: one
    // failure, one restart, one epoch.
    uint32_t recovery_epoch;
    // Measure and report how long the collectives spend in the operations
    // worth timing - today, the deflate an xcast performs on its payload.
    // Off by default: it is a measurement aid, not production instrumentation,
    // and the clock reads it needs sit directly in the broadcast path.
    // Set with the grpcomm_enable_timing MCA parameter.
    bool enable_timing;
    // Fault injection: hold this daemon's own fence contribution back by
    // delay_ms before sending it, on the daemon whose vpid is delay_vpid.
    //
    // This exists to make a race reachable that no test can otherwise
    // provoke: abort_fence_op() ends a fence while contributions are still
    // climbing the tree, and what arrives afterwards must be recognized as
    // belonging to a round that is over rather than absorbed into the next
    // one. Without a way to hold a contribution back, the window is a timing
    // accident nobody can arrange.
    //
    // It ships, and is deliberately not compiled out of an optimized build:
    // a race hook that only exists under PRTE_ENABLE_DEBUG cannot be used to
    // reproduce a race on the build that shows it. Off unless asked for -
    // delay_ms 0 costs one compare per fence.
    int fence_delay_ms;
    int fence_delay_vpid;
    // Fault injection, the other half: hold this daemon's own *processing* of
    // a fence release back, without holding up the forward to its children.
    //
    // That is the shape of the one window the round number has to cover and
    // grpcomm_fence_delay_ms cannot reach. xcast forwards a release before
    // processing it, so a child is released while its parent still has the
    // previous round open; the child's clients start the next round and their
    // contribution arrives at a parent that has not caught up. Delaying the
    // parent's processing widens that gap from microseconds to whatever is
    // asked for, which is what makes it testable at all.
    //
    // Ships for the same reason its sibling does.
    int release_delay_ms;
    int release_delay_vpid;
    // Fault injection, the third: hold this daemon's *forward* of a broadcast
    // back, so the op stays in flight on the tree for as long as is asked
    // for.
    //
    // Its two siblings both act on a release that has already been forwarded,
    // which is the right shape for the round-number window and the wrong one
    // for the fault path: repairing a tree only means anything while an op is
    // still travelling it, and on eight daemons a release is over in
    // microseconds. Widening that to seconds is what makes it possible to
    // kill a daemon in the middle of a broadcast on purpose rather than by
    // luck.
    //
    // Deliberately confined to the trees that are NOT the routing tree. The
    // routing tree carries the daemon command channel - the halt, the wireup,
    // the launch - and a knob that stalls those does not inject a fault, it
    // wedges the DVM, which is a different experiment and a worse one.
    //
    // Ships for the same reason its siblings do.
    int xcast_delay_ms;
    int xcast_delay_vpid;
    // Did this daemon join a DVM that had already been running collectives?
    //
    // It decides what "I have no round number for this signature" means, and
    // the two readings are opposites. For a daemon present from the start it
    // means round 0 has not happened yet, so it stamps 0 and the counter
    // bootstraps. For one added by a grow it means the round is genuinely
    // unknown - every daemon that has been present is at some k, and a 0 from
    // this one would be read as ancient and dropped, hanging the fence.
    //
    // Told, not derived, because only the master can tell the two apart; and
    // told as a *flag* rather than as a count, because a count would be stale
    // by the time it arrived - the master goes on answering fences while the
    // grow completes. A flag cannot go stale: it stays true exactly as long
    // as it is true, and stops mattering the moment this daemon sees its
    // first release for a signature and learns that signature's real number.
    //
    // Set from the first wireup this daemon receives and never revised: a
    // later wireup describes a DVM this daemon is already part of.
    bool joined_late;
    bool joined_late_known;
    // Send a fence's release down the low-radix tree rather than the routing
    // tree. Off by default: the tree we have is the one we know works, and
    // there is no measurement yet that says which is better on hardware where
    // the cost model's constants mean what it assumes.
    bool low_radix_release;
} prte_grpcomm_globals_t;

#define PRTE_GRPCOMM_GROUP_MEMO_MAX 64
#define PRTE_GRPCOMM_FENCE_MEMO_MAX 64

/* "No round number is known here." Distinct from generation 0, which is a
 * real round: a daemon that has never taken part in a fence over a signature
 * must be able to say so, because 0 would be read as a round already long
 * released and its contribution dropped. */
#define PRTE_GRPCOMM_FENCE_GEN_UNKNOWN UINT32_MAX

/* The step a tree rollup runs at.  A rollup has exactly one - every
 * participant contributes once and one release ends it - so this is the only
 * value in use today.  A dissemination exchange numbers its steps from here
 * and stamps them on the wire; see the tracker identity commentary below. */
#define PRTE_GRPCOMM_FENCE_STEP_ROLLUP 0

typedef struct {
    pmix_list_item_t super;
    char *groupID;
    pmix_group_operation_t op;
} prte_grpcomm_group_memo_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_group_memo_t);

/* Was this process hosted by a daemon that has since failed? A wildcard rank
 * names a whole namespace rather than one process and so is never answered
 * here - use prte_grpcomm_procs_lost() for a set that may contain one.
 * Exported so the unit test can drive it against a synthetic failed set. */
PRTE_EXPORT bool prte_grpcomm_proc_departed(const pmix_proc_t *proc);

/* Did this set of participants lose anyone to a failed daemon? Unlike the
 * single-process test, a wildcard entry is expanded through the job map, so
 * a collective whose membership is written as a whole namespace is answered
 * correctly. */
PRTE_EXPORT bool prte_grpcomm_procs_lost(const pmix_proc_t *procs, size_t nprocs);

/* Advance the recovery epoch and restart every in-flight collective at it.
 * Idempotent: an epoch at or below the current one does nothing. */
PRTE_EXPORT void prte_grpcomm_advance_epoch(uint32_t to);

/* Per-collective halves of the restart, called only by the above. */
void prte_grpcomm_group_restart(void);
void prte_grpcomm_fence_restart(void);

PRTE_EXPORT extern prte_grpcomm_globals_t prte_grpcomm_globals;

/* How a completed collective is released to every daemon that took part.
 * Today that is always a broadcast of the gathered result, because fence and
 * group both roll up to the controller and only the controller has the
 * answer.  It is indirected because that is precisely the part a different
 * release changes - an allgather would leave every daemon already holding
 * the result, with nothing to release - and because it is the only way the
 * unit test can see the release a controller would emit without standing up
 * an RML.  Production code sets this once, at startup, and never again. */
/* Broadcast on a named tree. prte_grpcomm_xcast_nb is this with the routing
 * tree, which is what almost every caller wants. */
PRTE_EXPORT
int prte_grpcomm_xcast_topo(prte_rml_tag_t tag, pmix_data_buffer_t *msg,
                            prte_grpcomm_topology_t topology,
                            prte_grpcomm_xcast_complete_fn_t cbfunc,
                            void *cbdata);

/* The one seam every collective's release goes through - two sites in the
 * fence, two in the group. Putting the tree choice here rather than at each
 * of them is what stops the two collectives drifting into different methods
 * for the same job. */
typedef int (*prte_grpcomm_release_bcast_fn_t)(prte_rml_tag_t tag, pmix_data_buffer_t *msg);
PRTE_EXPORT extern prte_grpcomm_release_bcast_fn_t prte_grpcomm_release_bcast;
/* Which tree a release for this tag travels - the decision alone, so it can
 * be asserted without a DVM to send over. */
PRTE_EXPORT prte_grpcomm_topology_t prte_grpcomm_release_topology(prte_rml_tag_t tag);
PRTE_EXPORT int prte_grpcomm_release_bcast_select(prte_rml_tag_t tag,
                                                  pmix_data_buffer_t *msg);


/* Define collective signatures so we don't need to
 * track global collective id's. We provide a unique
 * signature struct for each collective type so that
 * they can be customized for that collective without
 * interfering with other collectives */
typedef struct {
    pmix_object_t super;
    pmix_proc_t *signature;
    size_t sz;
} prte_grpcomm_fence_signature_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_fence_signature_t);

/* What this daemon remembers about a signature once its fence is over: the
 * number of the NEXT fence over those participants, which is one past the
 * last generation released here. It has to outlive the tracker, because the
 * whole point is to recognize something that arrives after the tracker is
 * gone. */
typedef struct {
    pmix_list_item_t super;
    prte_grpcomm_fence_signature_t *sig;
    uint32_t next_generation;
} prte_grpcomm_fence_memo_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_fence_memo_t);

typedef struct {
    pmix_object_t super;
    pmix_group_operation_t op;
    char *groupID;
    bool assignID;
    size_t ctxid;
    bool ctxid_assigned;
    pmix_proc_t *members; // initially supplied procs
    size_t nmembers;
    size_t bootstrap;
    bool follower;
    pmix_proc_t *addmembers;  // procs supplied as add-members
    size_t naddmembers;
    pmix_proc_t *final_order;
    size_t nfinal;
    // Set when a participant asked for PMIX_GROUP_FT_COLLECTIVE: a construct
    // that loses a member should complete on the survivors rather than abort.
    // Accumulated by sticky-OR as contributions merge, so it means "some
    // surviving participant asked for it" - a participant that requested it
    // and then died before its contribution rolled up cannot be seen here.
    bool ft_collective;
} prte_grpcomm_group_signature_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_group_signature_t);

/* Which of the two operations a fence is running.
 *
 * PMIX_COLLECT_DATA names it, and nothing else may: the directive is a
 * property of the *call*, so every participant passes the same value, while
 * the payload is a property of what the local procs happened to publish and
 * differs from daemon to daemon.  Deriving the operation from the bytes would
 * therefore have daemons disagree about which collective they are in, and a
 * fence has no originator to settle it - which is the failure class that
 * withdrew the lateral movements (see docs/plans/scalable_collectives/).
 *
 * That distinction stopped being academic when PMIx learned to contribute
 * only what changed: a participant with nothing new to say contributes zero
 * bytes to an allgather it is fully a member of.
 *
 * UNKNOWN is "no contribution has said yet", not a third operation. */
typedef enum {
    PRTE_GRPCOMM_FENCE_OP_UNKNOWN = 0,
    PRTE_GRPCOMM_FENCE_OP_BARRIER,
    PRTE_GRPCOMM_FENCE_OP_ALLGATHER
} prte_grpcomm_fence_op_t;

/* Internal component object for tracking ongoing
 * allgather operations */
typedef struct {
    pmix_list_item_t super;
    /* collective's signature */
    prte_grpcomm_fence_signature_t *sig;
    pmix_status_t status;
    // Which operation this is. Carried on the wire by every contribution
    // rather than re-derived, so that a participant that disagrees can be
    // caught saying so instead of quietly running the other collective.
    prte_grpcomm_fence_op_t op;
    // ---- the tracker's identity ----
    //
    // A tracker is identified by its signature AND its generation, not by the
    // signature alone. Two rounds over the same participants can legitimately
    // be live on one daemon at the same time: xcast forwards a release to a
    // daemon's children before that daemon processes it, so a child can be
    // released, start the next round, and have its contribution arrive while
    // its parent is still in the previous one. Keyed by signature alone that
    // contribution joins the wrong collective; keyed by both it gets a
    // tracker of its own and each round accumulates separately.
    //
    // `step` is the third level, and is 0 everywhere today. A tree rollup has
    // no steps - one contribution per participant, one release - so the pair
    // is enough for it. A dissemination exchange (Bruck, recursive doubling,
    // a ring) has log2(N) or N-1 *steps within one collective*, each carrying
    // a different block, and a message from step i is not interchangeable
    // with one from step j even at the same generation. Whoever adds such a
    // movement stamps the step on the wire beside the generation and keys its
    // trackers on all three; nothing else here has to change to accommodate
    // it, which is the reason the field is here rather than added later.
    uint32_t generation;
    uint32_t step;
    /* collection bucket */
    pmix_data_buffer_t bucket;
    /* participating daemons */
    pmix_rank_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /* number of buckets expected */
    size_t nexpected;
    /* number reported in */
    size_t nreported;
    // Which child subtrees have reported, keyed the same way the group
    // tracker does it - see the note there. A fence replays its
    // contributions on a fault too, so the same duplicate-proofing applies.
    pmix_bitmap_t reported_slots;
    bool self_reported;
    bool converged;
    bool aborting;
    // this daemon's own contribution, saved so a fault can replay it
    pmix_data_buffer_t *my_contribution;
    /* controls values */
    int timeout;
    // the controller arms a timer for "timeout" seconds once a participant
    // has asked for one. Nothing else bounds a fence: the PMIx server library
    // deletes its own timeout the moment it hands the request to us, so that
    // a late answer cannot come back to a tracker it already released.
    prte_event_t tev;
    bool tev_active;
    /* callback function */
    pmix_modex_cbfunc_t cbfunc;
    /* user-provided callback data */
    void *cbdata;
} prte_grpcomm_fence_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_fence_t);

/* Internal component object for tracking ongoing
 * group operations */
typedef struct {
    pmix_list_item_t super;
    /* collective's signature */
    prte_grpcomm_group_signature_t *sig;
    pmix_status_t status;
    /* participating daemons */
    pmix_rank_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /* type of collective */
    bool bootstrap;

    /*** NON-BOOTSTRAP TRACKERS ***/
    size_t nexpected;  // number of buckets expected
    size_t nreported;  // number reported in
    // A contribution is identified by which of our routing-tree child
    // subtrees it arrived from, not merely counted: a bare counter cannot
    // tell two messages from one child apart from one message from each of
    // two, which is exactly what a replay after a fault produces. The slot
    // index is prte_rml_get_subtree_index() of the sender, the same mapping
    // prte_rml_get_num_contributors() uses to compute nexpected, so the two
    // agree by construction. Our own contribution has no subtree index and
    // is tracked separately.
    pmix_bitmap_t reported_slots;
    bool self_reported;
    // set once the rollup has been answered (released by the controller, or
    // rolled up to our parent) so a straggler cannot drive it a second time
    bool converged;
    // set when an abort has been broadcast for this op but the tracker has
    // not yet been deleted by the returning release
    bool aborting;

    /*** BOOTSTRAP TRACKERS ***/
    // "leaders" are group members reporting as
    // themselves for bootstrap - they know how
    // many leaders there are (which is in the bootstrap
    // parameter), but not who they are. Bootstrap is
    // complete when nleaders_reported == bootstrap
    // AND naddmembers_reported == naddmembers
    size_t nleaders;  // number of leaders expected
    size_t nleaders_reported;  // number reported in
    // "add-members" are procs that report with NULL
    // for the proc parameter - thereby indicating that
    // they don't know the other procs in the group
    size_t nfollowers;  // number of add-member procs expected to participate
    size_t nfollowers_reported;  // number reported in

    /* controls values */
    int timeout;
    // the controller arms a timer for "timeout" seconds once a participant
    // has asked for one, so a collective that can no longer converge fails
    // its participants instead of hanging them
    prte_event_t tev;
    bool tev_active;
    // This daemon's own contribution, kept so a fault can replay it: recovery
    // resets every tracker and each daemon re-injects what it originally
    // contributed. NULL on a daemon that is only relaying for its subtree.
    pmix_data_buffer_t *my_contribution;
    // Members lost with a failed daemon, filled in by the controller when a
    // fault-tolerant construct completes on the survivors, and carried in the
    // release so each daemon can tell its own clients who went missing.
    pmix_proc_t *departed;
    size_t ndeparted;
    void *grpinfo;  // info list of group info
    void *endpts;   // info list of endpts
    /* callback function */
    pmix_info_cbfunc_t cbfunc;
    /* user-provided callback data */
    void *cbdata;
} prte_grpcomm_group_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_group_t);

typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_info_t *info;
    size_t ninfo;
    char *data;
    size_t ndata;
    pmix_modex_cbfunc_t cbfunc;
    void *cbdata;
} prte_pmix_fence_caddy_t;
PMIX_CLASS_DECLARATION(prte_pmix_fence_caddy_t);


/* xcast functions */
PRTE_EXPORT extern
int prte_grpcomm_xcast(prte_rml_tag_t tag,
                              pmix_data_buffer_t *msg);

PRTE_EXPORT extern
int prte_grpcomm_xcast_nb(prte_rml_tag_t tag,
                                 pmix_data_buffer_t *msg,
                                 prte_grpcomm_xcast_complete_fn_t cbfunc,
                                 void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_xcast_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tg, void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_xcast_fault_handler(const prte_rml_recovery_status_t* status);

PRTE_EXPORT extern
void prte_grpcomm_xcast_ack(int status, pmix_proc_t *sender,
                                   pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tg, void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_fence_release(int status, pmix_proc_t *sender,
                                	   pmix_data_buffer_t *buffer,
                                	   prte_rml_tag_t tag, void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_fence_fault_handler(const prte_rml_recovery_status_t* status);

/* Find the tracker for this fence signature, optionally creating it. A new
 * tracker resolves the signature to its participating daemons and sizes the
 * rollup accordingly, and comes back NULL - with nothing left on the tracker
 * list - if that cannot be done. Exported so the unit test can drive it. */
PRTE_EXPORT
prte_grpcomm_fence_t *prte_grpcomm_fence_get_tracker(prte_grpcomm_fence_signature_t *sig,
                                                            uint32_t generation,
                                                            uint32_t step,
                                                            bool create);

/* Which operation this fence's directives ask for.  Only PMIX_COLLECT_DATA is
 * consulted: true is an allgather, false is a barrier, and so is its absence -
 * a caller that said nothing asked for synchronization and nothing else.
 * Never answers UNKNOWN.  Exported so the unit test can drive it. */
PRTE_EXPORT
prte_grpcomm_fence_op_t prte_grpcomm_fence_op_from_info(const pmix_info_t info[],
                                                        size_t ninfo);

/* Fold an arriving contribution's operation into the tracker's, adopting it if
 * the tracker has not heard one yet.  Returns false if the two disagree, which
 * means the participants asked for different collectives - a user error the
 * fence cannot resolve, and one that has to be caught here because it is
 * otherwise invisible: a barrier now puts nothing on the wire for PMIx's own
 * per-blob collect-flag check to compare.  Exported so the unit test can drive
 * it; the caller is what reports the disagreement. */
PRTE_EXPORT
bool prte_grpcomm_fence_op_merge(prte_grpcomm_fence_t *coll,
                                 prte_grpcomm_fence_op_t incoming);

/* The number of the next fence over this signature - one past the last
 * generation released here - or PRTE_GRPCOMM_FENCE_GEN_UNKNOWN if this daemon
 * has never released one. Exported so the unit test can drive it. */
PRTE_EXPORT
uint32_t prte_grpcomm_fence_gen_next(prte_grpcomm_fence_signature_t *sig);

/* What this daemon stamps on a contribution for a signature it has no entry
 * for: round 0 if it has been here since the start, UNKNOWN if it joined a
 * DVM that was already running collectives. Exported so the unit test can
 * drive both readings. */
PRTE_EXPORT
uint32_t prte_grpcomm_fence_gen_baseline(void);

/* Record whether this daemon joined an already-running DVM. Called once, from
 * the first wireup; later calls are ignored. Exported for the unit test. */
PRTE_EXPORT
void prte_grpcomm_fence_note_join(bool late);

/* Record that generation `gen` over this signature has been released here, so
 * the next one is gen+1. Adopts rather than increments, which is what puts a
 * daemon that joined the DVM late - and so counted none of the earlier rounds
 * - in step with everyone else after its first fence. Exported for the test. */
PRTE_EXPORT
void prte_grpcomm_fence_gen_record(prte_grpcomm_fence_signature_t *sig, uint32_t gen);

/* Is a contribution stamped `gen` one this daemon has already released?  Only
 * a stamp strictly below what we are expecting is stale; UNKNOWN never is,
 * because it carries no claim about a round at all.  Exported for the test. */
PRTE_EXPORT
bool prte_grpcomm_fence_gen_is_stale(prte_grpcomm_fence_signature_t *sig, uint32_t gen);

/* group functions */
PRTE_EXPORT extern
int prte_grpcomm_group(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT extern
void prte_grpcomm_group_fault_handler(const prte_rml_recovery_status_t* status);

/* Fill in a group signature from the directives a participant supplied,
 * along with the timeout, local status, group info and endpoint data that
 * ride alongside it. The signature takes a COPY of every array a directive
 * carries - the directives belong to the PMIx server that handed them to us.
 * Exported so the unit test can drive it against a synthetic directive set. */
PRTE_EXPORT
pmix_status_t prte_grpcomm_group_parse_directives(prte_grpcomm_group_signature_t *sig,
                                                         const pmix_info_t *directives, size_t ndirs,
                                                         int *timeout, pmix_status_t *st,
                                                         void *grpinfo, void *endpts);

PRTE_EXPORT extern
void prte_grpcomm_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);


PRTE_EXPORT extern
void prte_grpcomm_grp_release(int status, pmix_proc_t *sender,
                                	 pmix_data_buffer_t *buffer,
                                	 prte_rml_tag_t tag, void *cbdata);

static inline void print_signature(prte_grpcomm_group_signature_t *sig)
{
    char **msg = NULL;
    char *tmp;
    size_t n;

    PMIx_Argv_append_nosize(&msg, "SIGNATURE:");
    pmix_asprintf(&tmp, "\tOP: %s", PMIx_Group_operation_string(sig->op));
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tGRPID: %s", sig->groupID);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tASSIGN CTXID: %s", sig->assignID ? "T" : "F");
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    if (sig->assignID) {
        pmix_asprintf(&tmp, "\tCTXID: %lu", sig->ctxid);
        PMIx_Argv_append_nosize(&msg, tmp);
        free(tmp);
    }

    pmix_asprintf(&tmp, "\tNMEMBERS: %lu", sig->nmembers);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);
    if (0 < sig->nmembers) {
        for (n=0; n < sig->nmembers; n++) {
            pmix_asprintf(&tmp, "\t\t%s", PMIX_NAME_PRINT(&sig->members[n]));
            PMIx_Argv_append_nosize(&msg, tmp);
            free(tmp);
        }
    }

    pmix_asprintf(&tmp, "\tBOOTSTRAP: %lu", sig->bootstrap);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tFOLLOWER: %s", sig->follower ? "T" : "F");
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);

    pmix_asprintf(&tmp, "\tNADDMEMBERS: %lu", sig->naddmembers);
    PMIx_Argv_append_nosize(&msg, tmp);
    free(tmp);
    if (0 < sig->naddmembers) {
        for (n=0; n < sig->naddmembers; n++) {
            pmix_asprintf(&tmp, "\t\t%s", PMIX_NAME_PRINT(&sig->addmembers[n]));
            PMIx_Argv_append_nosize(&msg, tmp);
            free(tmp);
        }
    }

    tmp = PMIx_Argv_join(msg, '\n');
    PMIx_Argv_free(msg);
    pmix_output(0, "%s", tmp);
    free(tmp);
}

END_C_DECLS

#endif
