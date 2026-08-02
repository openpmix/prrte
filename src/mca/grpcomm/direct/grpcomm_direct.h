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
#ifndef GRPCOMM_DIRECT_H
#define GRPCOMM_DIRECT_H

#include "prte_config.h"

#include "src/class/pmix_bitmap.h"

#include "src/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/* Tracks ongoing xcast operations to ensure all messages are delivered exactly
 * once to all daemons even in the presence of daemon failures */
typedef struct {
    pmix_object_t super;
    // list of ongoing operations, defined in grpcomm_direct_xcast.c
    pmix_list_t ops;
    // FIFO of completion callbacks for master-originated broadcasts awaiting
    // relay back to the master (see grpcomm_direct_xcast.c)
    pmix_list_t pending_completions;
    // ID of the last known completed (in our subtree) operation
    size_t op_id_completed;
    // op_id_completed when we were last promoted
    // (our subtree grew, so we can't assume completion in our new subtree)
    size_t op_id_completed_at_promotion;
    // ID of the last known initiated operation
    size_t op_id_inited;
} prte_grpcomm_xcast_t;
PRTE_MODULE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_xcast_t);

/*
 * Grpcomm interfaces
 */
typedef struct {
    prte_grpcomm_base_component_t super;
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
    // The collective recovery epoch for this daemon. A daemon failure
    // invalidates every in-flight rollup, because how many contributions each
    // daemon expects is derived from the routing tree. Recovery is a
    // simultaneous restart across the DVM, and this counter is what tells one
    // round from the next, so a contribution still in flight from before the
    // failure can be recognized as stale. Shared by fence and group: one
    // failure, one restart, one epoch.
    uint32_t recovery_epoch;
} prte_grpcomm_direct_component_t;

#define PRTE_GRPCOMM_GROUP_MEMO_MAX 64

typedef struct {
    pmix_list_item_t super;
    char *groupID;
    pmix_group_operation_t op;
} prte_grpcomm_group_memo_t;
PMIX_CLASS_DECLARATION(prte_grpcomm_group_memo_t);

/* Was this process hosted by a daemon that has since failed? A wildcard rank
 * names a whole namespace rather than one process and so is never answered
 * here - use prte_grpcomm_direct_procs_lost() for a set that may contain one.
 * Exported so the unit test can drive it against a synthetic failed set. */
PRTE_MODULE_EXPORT bool prte_grpcomm_direct_proc_departed(const pmix_proc_t *proc);

/* Did this set of participants lose anyone to a failed daemon? Unlike the
 * single-process test, a wildcard entry is expanded through the job map, so
 * a collective whose membership is written as a whole namespace is answered
 * correctly. */
PRTE_MODULE_EXPORT bool prte_grpcomm_direct_procs_lost(const pmix_proc_t *procs, size_t nprocs);

/* Advance the recovery epoch and restart every in-flight collective at it.
 * Idempotent: an epoch at or below the current one does nothing. */
PRTE_MODULE_EXPORT void prte_grpcomm_direct_advance_epoch(uint32_t to);

/* Per-collective halves of the restart, called only by the above. */
void prte_grpcomm_direct_group_restart(void);
void prte_grpcomm_direct_fence_restart(void);

PRTE_MODULE_EXPORT extern prte_grpcomm_direct_component_t prte_mca_grpcomm_direct_component;
extern prte_grpcomm_base_module_t prte_grpcomm_direct_module;


/* Define collective signatures so we don't need to
 * track global collective id's. We provide a unique
 * signature struct for each collective type so that
 * they can be customized for that collective without
 * interfering with other collectives */
typedef struct {
    pmix_object_t super;
    pmix_proc_t *signature;
    size_t sz;
} prte_grpcomm_direct_fence_signature_t;
PRTE_MODULE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_direct_fence_signature_t);

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
} prte_grpcomm_direct_group_signature_t;
PRTE_MODULE_EXPORT PMIX_CLASS_DECLARATION(prte_grpcomm_direct_group_signature_t);

/* Internal component object for tracking ongoing
 * allgather operations */
typedef struct {
    pmix_list_item_t super;
    /* collective's signature */
    prte_grpcomm_direct_fence_signature_t *sig;
    pmix_status_t status;
    /* collection bucket */
    pmix_data_buffer_t bucket;
    /* participating daemons */
    pmix_rank_t *dmns;
    /** number of participating daemons */
    size_t ndmns;
    /** my index in the dmns array */
    unsigned long my_rank;
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
    prte_grpcomm_direct_group_signature_t *sig;
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
    prte_grpcomm_direct_fence_signature_t *sig;
    pmix_data_buffer_t *buf;
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
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_xcast(prte_rml_tag_t tag,
                              pmix_data_buffer_t *msg);

PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_xcast_nb(prte_rml_tag_t tag,
                                 pmix_data_buffer_t *msg,
                                 prte_grpcomm_xcast_complete_fn_t cbfunc,
                                 void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_xcast_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tg, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_xcast_ack(int status, pmix_proc_t *sender,
                                   pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tg, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_xcast_fault_handler(const prte_rml_recovery_status_t* status);

/* fence functions */
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_fence(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo, char *data,
                              size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_fence_release(int status, pmix_proc_t *sender,
                                	   pmix_data_buffer_t *buffer,
                                	   prte_rml_tag_t tag, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_fence_fault_handler(const prte_rml_recovery_status_t* status);

/* group functions */
PRTE_MODULE_EXPORT extern
int prte_grpcomm_direct_group(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_group_fault_handler(const prte_rml_recovery_status_t* status);

PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);


PRTE_MODULE_EXPORT extern
void prte_grpcomm_direct_grp_release(int status, pmix_proc_t *sender,
                                	 pmix_data_buffer_t *buffer,
                                	 prte_rml_tag_t tag, void *cbdata);

static inline void print_signature(prte_grpcomm_direct_group_signature_t *sig)
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
