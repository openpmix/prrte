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

#include <stdlib.h>
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

#include "src/mca/base/pmix_base.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/util/prte_show_help.h"

#include "grpcomm_internal.h"

prte_grpcomm_globals_t prte_grpcomm_globals = {
    .output = -1,
    .context_id = UINT32_MAX,
    .fence_ops = PMIX_LIST_STATIC_INIT(prte_grpcomm_globals.fence_ops),
    .group_ops = PMIX_LIST_STATIC_INIT(prte_grpcomm_globals.group_ops),
    .fence_generations = PMIX_LIST_STATIC_INIT(prte_grpcomm_globals.fence_generations),
    .live_groups = PMIX_LIST_STATIC_INIT(prte_grpcomm_globals.live_groups),
    .departed = PMIX_LIST_STATIC_INIT(prte_grpcomm_globals.departed)
};

prte_grpcomm_release_bcast_fn_t prte_grpcomm_release_bcast =
    prte_grpcomm_release_bcast_select;

/* File scope, not a local: the MCA layer keeps the pointer it is handed and
 * writes through it whenever the variable is set, so a stack slot would be
 * a dangling write the moment this function returns. */
static int verbosity = 0;

void prte_grpcomm_register(void)
{
    verbosity = 0;

    /* keep the parameter spelled the way the framework spelled it -
     * "--prtemca grpcomm_base_verbose N" is in every debugging recipe
     * and in the guides, and nothing is gained by breaking it */
    pmix_mca_base_var_register("prte", "grpcomm", "base", "verbose",
                               "Debug verbosity of the grpcomm subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_grpcomm_globals.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_grpcomm_globals.output, verbosity);
    }

    /* Timing is a measurement aid rather than production instrumentation, so
     * it is off unless asked for: the clock reads it needs sit directly in the
     * broadcast path.  Note that a timing run wants a NON-debug build - see
     * src/grpcomm/AGENTS.md, "Timing runs want an optimized build". */
    prte_grpcomm_globals.enable_timing = false;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "enable_timing",
                               "Measure and report how long the collectives spend "
                               "in the operations worth timing (today, an xcast's "
                               "compression of its payload). Reported at "
                               "grpcomm_base_verbose 1.",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_grpcomm_globals.enable_timing);

    /* Fault injection for the fence's round discriminator.  Holding one
     * daemon's contribution back, while a PMIX_TIMEOUT ends the fence without
     * it, is what puts a contribution on the wire for a round that is already
     * over - the straggler the generation exists to recognize.  Nothing else
     * can arrange that window: it is otherwise a timing accident.
     *
     * Compiled in always, on purpose.  See src/grpcomm/AGENTS.md. */
    prte_grpcomm_globals.release_delay_ms = 0;
    prte_grpcomm_globals.release_delay_vpid = -1;
    prte_grpcomm_globals.group_delay_ms = 0;
    prte_grpcomm_globals.group_delay_vpid = -1;
    prte_grpcomm_globals.group_release_delay_ms = 0;
    prte_grpcomm_globals.group_release_delay_vpid = -1;
    prte_grpcomm_globals.joined_late = false;
    prte_grpcomm_globals.joined_late_known = false;

    prte_grpcomm_globals.fence_delay_ms = 0;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "fence_delay_ms",
                               "Hold this daemon's own fence contribution back "
                               "by this many milliseconds before sending it. "
                               "Fault injection for the fence's round handling; "
                               "0 (the default) sends immediately.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.fence_delay_ms);

    /* The other half of the pair: hold a daemon's own processing of a release
     * back while its children get theirs on time.  That is the only way to
     * reach the early-arrival window, where a contribution for the NEXT round
     * lands on a daemon still finishing the previous one. */
    prte_grpcomm_globals.release_delay_ms = 0;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "release_delay_ms",
                               "Hold this daemon's own processing of a fence "
                               "release back by this many milliseconds. The "
                               "forward to its children is not delayed. Fault "
                               "injection; 0 (the default) processes at once.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.release_delay_ms);

    prte_grpcomm_globals.release_delay_vpid = -1;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "release_delay_vpid",
                               "Restrict grpcomm_release_delay_ms to the daemon "
                               "with this vpid. -1 (the default) delays on "
                               "every daemon that reads the parameter.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.release_delay_vpid);

    /* -1 means "whichever daemon this is", which is what a per-node MCA file
     * wants; a vpid names one daemon in a DVM-wide setting, which is what a
     * test driving the whole DVM from one command line wants. */
    prte_grpcomm_globals.fence_delay_vpid = -1;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "fence_delay_vpid",
                               "Restrict grpcomm_fence_delay_ms to the daemon "
                               "with this vpid. -1 (the default) delays on "
                               "every daemon that reads the parameter.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.fence_delay_vpid);

    /* The same pair for the group collective.  Separate from the fence's so a
     * test can hold a group construct back without stalling every fence those
     * daemons are running at the same time. */
    prte_grpcomm_globals.group_delay_ms = 0;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "group_delay_ms",
                               "Hold this daemon's own group contribution back "
                               "by this many milliseconds before sending it. "
                               "Fault injection for the group's round handling; "
                               "0 (the default) sends immediately.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.group_delay_ms);

    prte_grpcomm_globals.group_delay_vpid = -1;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "group_delay_vpid",
                               "Restrict grpcomm_group_delay_ms to the daemon "
                               "with this vpid. -1 (the default) delays on "
                               "every daemon that reads the parameter.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.group_delay_vpid);

    /* The early-arrival half: a contribution to the next operation of a
     * reused group ID landing on a daemon that has not finished the previous
     * one.  That is the window the boolean memo got wrong. */
    prte_grpcomm_globals.group_release_delay_ms = 0;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "group_release_delay_ms",
                               "Hold this daemon's own processing of a group "
                               "release back by this many milliseconds. The "
                               "forward to its children is not delayed. Fault "
                               "injection; 0 (the default) processes at once.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.group_release_delay_ms);

    prte_grpcomm_globals.group_release_delay_vpid = -1;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "group_release_delay_vpid",
                               "Restrict grpcomm_group_release_delay_ms to the "
                               "daemon with this vpid. -1 (the default) delays "
                               "on every daemon that reads the parameter.",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.group_release_delay_vpid);

    /* And the third: hold the forward itself back, so an operation is still
     * travelling its tree when a daemon is killed underneath it.  Its two
     * siblings both act after the forward has gone, which cannot reach the
     * fault path at all - on eight daemons a release is over in microseconds.
     *
     * Non-routing trees only.  Stalling the routing tree would hold up the
     * daemon command channel rather than injecting a fault. */
    prte_grpcomm_globals.xcast_delay_ms = 0;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "xcast_delay_ms",
                               "Fault injection: hold a daemon's forward of a "
                               "broadcast on a non-routing tree back by this "
                               "many milliseconds, so the operation stays in "
                               "flight long enough for a daemon loss to be "
                               "aimed at it (0 = off)",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.xcast_delay_ms);

    prte_grpcomm_globals.xcast_delay_vpid = -1;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "xcast_delay_vpid",
                               "Restrict grpcomm_xcast_delay_ms to the daemon "
                               "with this vpid (-1 = every daemon)",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_grpcomm_globals.xcast_delay_vpid);

    /* Send a fence's release down the low-radix tree instead of the routing
     * tree.  ON by default.  The radix it uses is rml_base_radix2.
     *
     * It shipped off, on the grounds that there was no data to choose a
     * winner with.  There is now a model instead, and it is decisive in a way
     * a measurement of one swarm would not have been: a broadcasting daemon's
     * r copies cost ceil(r/t) software sends but r whole payloads on one NIC,
     * so past a payload of B*c/t bytes the fanout is pure bandwidth and the
     * optimum radix drops to 3-5 and stays there for every larger payload and
     * every DVM size.  A fence release is the modex, which is far past that
     * crossover on any machine - kilobytes at worst, and the crossover is
     * kilobytes at best.  Radix 64 costs 6.1x to 6.4x the optimum there, and
     * that figure survives sweeping the machine constants eightfold.
     *
     * What is not settled is where the crossover sits, which is a property of
     * a real network and moves with B, c and prte_num_worker_threads.  It
     * does not have to be settled to make this the default: nothing this
     * tree carries is anywhere near it.  The derivation is in
     * docs/plans/scalable_collectives/two-radix-release.rst.
     *
     * Turning it off restores the single-tree behaviour exactly - the release
     * goes down the routing tree and none of the derived-tree code runs - so
     * this remains the one switch to reach for if a release misbehaves. */
    prte_grpcomm_globals.low_radix_release = true;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "low_radix_release",
                               "Fan a fence's release out over the low-radix "
                               "release tree (rml_base_radix2) rather than the "
                               "routing tree. On by default; turn it off to "
                               "put every release back on the routing tree.",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_grpcomm_globals.low_radix_release);
}

/**
 * Stand grpcomm up
 */
int prte_grpcomm_init(void)
{
    /* A release tree at the routing tree's own radix IS the routing tree -
     * same parent, same children, for every rank and every failure pattern
     * (test_release_tree_matches_routing pins it).  rml_base_radix2 defaults
     * to rml_base_radix, so turning the release onto its own tree and setting
     * nothing else is the easy mistake, and it is a silent one: the fence
     * still works, it just does the identical fanout through the derived-tree
     * path.  Say so rather than let somebody measure it and conclude the idea
     * does not help.
     *
     * The master alone, because every daemon reads the same two parameters
     * and would otherwise say it once each. */
    if (PRTE_PROC_IS_MASTER && prte_grpcomm_globals.low_radix_release
        && prte_rml_base.radix2 == prte_rml_base.radix) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-grpcomm.txt", "release-radix-noop", true,
                       prte_rml_base.radix, prte_rml_base.radix2);
    }

    /* Losing a lateral link is not a tree fault, so the RML deliberately does
     * not repair on it - but a derived tree's broadcast may have been relying
     * on that link, and nothing else will tell it.  See lateral_link_lost(). */
    prte_rml_lateral_set_lost_callback(prte_grpcomm_xcast_lateral_lost);

    /* setup the trackers */
    PMIX_CONSTRUCT(&prte_grpcomm_globals.xcast_ops,
                   prte_grpcomm_xcast_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.group_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.completed_group_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_generations, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.live_groups, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.departed, pmix_list_t);

    /* xcast receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST,
                  PRTE_RML_PERSISTENT, prte_grpcomm_xcast_recv, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_ACK,
                  PRTE_RML_PERSISTENT, prte_grpcomm_xcast_ack, NULL);

    /* fence receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_recv, NULL);
    /* setup recv for barrier release */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_release, NULL);
    /* what has departed below us - see grpcomm_departed.c */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_DEPARTED,
                  PRTE_RML_PERSISTENT, prte_grpcomm_departed_recv, NULL);

    /* group receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP,
                  PRTE_RML_PERSISTENT, prte_grpcomm_grp_recv, NULL);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_grp_release, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_EXPIRE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_grp_expire, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_EXPIRE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_expire, NULL);
    return PRTE_SUCCESS;
}

/**
 * Tear grpcomm down
 */
void prte_grpcomm_finalize(void)
{
    PMIX_DESTRUCT(&prte_grpcomm_globals.xcast_ops);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.fence_ops);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.group_ops);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.completed_group_ops);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.fence_generations);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.live_groups);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.departed);

    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_ACK);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_DEPARTED);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_EXPIRE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_EXPIRE);
    return;
}

int prte_grpcomm_assign_context_id(size_t *ctxid)
{
    if (NULL == ctxid) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* The pool is this daemon's own counter, and nothing reconciles two of
     * them - see the header. Refusing here is what keeps the "only the master
     * assigns" rule in one place, rather than repeated at each caller. */
    if (!PRTE_PROC_IS_MASTER) {
        return PRTE_ERR_NOT_SUPPORTED;
    }
    *ctxid = (size_t) prte_grpcomm_globals.context_id;
    --prte_grpcomm_globals.context_id;
    return PRTE_SUCCESS;
}

bool prte_grpcomm_proc_departed(const pmix_proc_t *proc)
{
    prte_job_t *jdata;
    prte_proc_t *p;

    if (PMIX_RANK_WILDCARD == proc->rank || PMIX_RANK_INVALID == proc->rank) {
        return false;
    }
    if (pmix_bitmap_is_clear(&prte_rml_base.failed_dmns)) {
        /* nothing has failed, so nothing can have departed - and this keeps
         * the common, fault-free path from walking the job map at all */
        return false;
    }
    jdata = prte_get_job_data_object(proc->nspace);
    if (NULL == jdata) {
        return true;
    }
    p = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, proc->rank);
    if (NULL == p || NULL == p->node || NULL == p->node->daemon) {
        /* we cannot place it any more, which is what a node being torn down
         * looks like - treat it as gone rather than as an error */
        return true;
    }
    return pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns,
                                  p->node->daemon->name.rank);
}

/* whether any of a group's members' jobs still runs */
static bool group_lives(const prte_grpcomm_live_group_t *grp)
{
    prte_job_t *jdata;
    size_t m;

    for (m = 0; m < grp->nmembers; m++) {
        jdata = prte_get_job_data_object(grp->members[m].nspace);
        if (NULL != jdata && PRTE_JOB_STATE_UNTERMINATED > jdata->state) {
            return true;
        }
    }
    return false;
}

bool prte_grpcomm_group_id_taken(const char *grpid)
{
    prte_grpcomm_live_group_t *grp;
    pmix_nspace_t nspace;

    if (NULL == grpid) {
        return false;
    }
    /* a namespace is at most PMIX_MAX_NSLEN characters, and the lookup
     * compares only that many: a longer ID is no job's */
    if (PMIX_MAX_NSLEN >= strlen(grpid)) {
        PMIX_LOAD_NSPACE(nspace, grpid);
        if (NULL != prte_get_job_data_object(nspace)) {
            return true;
        }
    }
    PMIX_LIST_FOREACH(grp, &prte_grpcomm_globals.live_groups, prte_grpcomm_live_group_t) {
        if (0 == strcmp(grp->groupID, grpid) && group_lives(grp)) {
            return true;
        }
    }
    return false;
}

void prte_grpcomm_group_formed(const char *grpid, const pmix_proc_t *members, size_t nmembers)
{
    prte_grpcomm_live_group_t *grp, *gnext;

    if (NULL == grpid) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE(grp, gnext, &prte_grpcomm_globals.live_groups,
                           prte_grpcomm_live_group_t) {
        if (0 == strcmp(grp->groupID, grpid) || !group_lives(grp)) {
            pmix_list_remove_item(&prte_grpcomm_globals.live_groups, &grp->super);
            PMIX_RELEASE(grp);
        }
    }
    grp = PMIX_NEW(prte_grpcomm_live_group_t);
    grp->groupID = strdup(grpid);
    if (NULL == grp->groupID) {
        PMIX_RELEASE(grp);
        return;
    }
    if (0 < nmembers && NULL != members) {
        PMIX_PROC_CREATE(grp->members, nmembers);
        if (NULL == grp->members) {
            PMIX_RELEASE(grp);
            return;
        }
        memcpy(grp->members, members, nmembers * sizeof(pmix_proc_t));
        grp->nmembers = nmembers;
    }
    pmix_list_append(&prte_grpcomm_globals.live_groups, &grp->super);
}

static int proc_order(const void *a, const void *b)
{
    const pmix_proc_t *pa = (const pmix_proc_t *) a;
    const pmix_proc_t *pb = (const pmix_proc_t *) b;
    int rc;

    rc = strncmp(pa->nspace, pb->nspace, PMIX_MAX_NSLEN);
    if (0 != rc) {
        return rc;
    }
    if (pa->rank == pb->rank) {
        return 0;
    }
    return (pa->rank < pb->rank) ? -1 : 1;
}

/* a sorted copy of procs, without repeats */
static pmix_proc_t *sorted_set(const pmix_proc_t *procs, size_t n, size_t *nout)
{
    pmix_proc_t *s;
    size_t i, k;

    *nout = 0;
    PMIX_PROC_CREATE(s, n);
    if (NULL == s) {
        return NULL;
    }
    memcpy(s, procs, n * sizeof(pmix_proc_t));
    qsort(s, n, sizeof(pmix_proc_t), proc_order);
    for (i = 0, k = 0; i < n; i++) {
        if (0 == k || 0 != proc_order(&s[k - 1], &s[i])) {
            if (k != i) {
                memcpy(&s[k], &s[i], sizeof(pmix_proc_t));
            }
            ++k;
        }
    }
    *nout = k;
    return s;
}

/* Is a sorted run of one namespace's procs that whole job - its wildcard,
 * or every rank of it? */
static bool names_whole_job(const pmix_proc_t *seg, size_t n)
{
    prte_job_t *jdata;
    size_t i;

    for (i = 0; i < n; i++) {
        if (PMIX_RANK_WILDCARD == seg[i].rank) {
            return true;
        }
    }
    jdata = prte_get_job_data_object(seg[0].nspace);
    if (NULL == jdata || n != (size_t) jdata->num_procs) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (seg[i].rank != (pmix_rank_t) i) {
            return false;
        }
    }
    return true;
}

bool prte_grpcomm_same_members(const pmix_proc_t *a, size_t na,
                               const pmix_proc_t *b, size_t nb)
{
    pmix_proc_t *sa = NULL, *sb = NULL;
    size_t nsa = 0, nsb = 0, i = 0, j = 0, ie, je, k;
    bool wa, wb, same = true;

    /* the usual case: every participant passed the same array */
    if (na == nb && (0 == na || 0 == memcmp(a, b, na * sizeof(pmix_proc_t)))) {
        return true;
    }
    if (0 == na || 0 == nb || NULL == a || NULL == b) {
        return false;
    }
    sa = sorted_set(a, na, &nsa);
    sb = sorted_set(b, nb, &nsb);
    if (NULL == sa || NULL == sb) {
        /* nothing to compare with - not a match we can vouch for */
        same = false;
        goto done;
    }
    while (same && i < nsa && j < nsb) {
        if (0 != strncmp(sa[i].nspace, sb[j].nspace, PMIX_MAX_NSLEN)) {
            same = false;
            break;
        }
        for (ie = i; ie < nsa && 0 == strncmp(sa[ie].nspace, sa[i].nspace, PMIX_MAX_NSLEN); ie++) {
        }
        for (je = j; je < nsb && 0 == strncmp(sb[je].nspace, sb[j].nspace, PMIX_MAX_NSLEN); je++) {
        }
        wa = names_whole_job(&sa[i], ie - i);
        wb = names_whole_job(&sb[j], je - j);
        if (wa || wb) {
            same = (wa && wb);
        } else if ((ie - i) != (je - j)) {
            same = false;
        } else {
            for (k = 0; k < ie - i; k++) {
                if (sa[i + k].rank != sb[j + k].rank) {
                    same = false;
                    break;
                }
            }
        }
        i = ie;
        j = je;
    }
    if (same && (i != nsa || j != nsb)) {
        same = false;
    }

done:
    if (NULL != sa) {
        PMIX_PROC_FREE(sa, na);
    }
    if (NULL != sb) {
        PMIX_PROC_FREE(sb, nb);
    }
    return same;
}

static pmix_status_t make_blob(const pmix_proc_t *procs, size_t nprocs, pmix_byte_object_t *bo)
{
    pmix_data_buffer_t buf;
    pmix_rank_t me = PRTE_PROC_MY_NAME->rank;
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &me, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &buf, &nprocs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < nprocs) {
        rc = PMIx_Data_pack(NULL, &buf, (void *) procs, nprocs, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unload(&buf, bo);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    return rc;
}

pmix_status_t prte_grpcomm_participant_blob(const pmix_info_t *dirs, size_t ndirs, bool solo,
                                            pmix_byte_object_t *bo, bool *known)
{
    pmix_data_array_t *d;
    const pmix_info_t *list = NULL;
    pmix_status_t st = PMIX_SUCCESS;
    size_t n;

    PMIX_BYTE_OBJECT_CONSTRUCT(bo);
    for (n = 0; NULL != dirs && n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&dirs[n], PMIX_LOCAL_PARTICIPANTS)) {
            list = &dirs[n];
        } else if (PMIX_CHECK_KEY(&dirs[n], PMIX_LOCAL_COLLECTIVE_STATUS) &&
                   PMIX_STATUS == dirs[n].value.type) {
            st = dirs[n].value.data.status;
        }
    }
    /* A server hands an operation up with any other status only once every
     * local member has called, so there is nothing to say: all of them took
     * part. Only one that ran out of time is partial. */
    *known = true;
    if (PMIX_ERR_TIMEOUT != st && !solo) {
        return PMIX_SUCCESS;
    }
    if (NULL == list) {
        *known = false;
        return PMIX_SUCCESS;
    }
    /* checked, not assumed: the array is sized in pmix_proc_t */
    if (PMIX_DATA_ARRAY != list->value.type || NULL == (d = list->value.data.darray) ||
        PMIX_PROC != d->type || (0 < d->size && NULL == d->array)) {
        *known = false;
        return PMIX_ERR_TYPE_MISMATCH;
    }
    return make_blob((const pmix_proc_t *) d->array, d->size, bo);
}

pmix_status_t prte_grpcomm_parts_to_bo(bool known, pmix_data_buffer_t *blobs, size_t nblobs,
                                       pmix_byte_object_t *bo)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;

    PMIX_BYTE_OBJECT_CONSTRUCT(bo);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &known, 1, PMIX_BOOL);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &buf, &nblobs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < nblobs && NULL != blobs) {
        rc = PMIx_Data_copy_payload(&buf, blobs);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unload(&buf, bo);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    return rc;
}

void prte_grpcomm_parts_merge(const pmix_value_t *val, pmix_data_buffer_t *blobs, size_t *nblobs,
                              bool *unknown)
{
    pmix_data_buffer_t buf;
    pmix_byte_object_t bo, blob;
    bool known = false;
    size_t n = 0, i;
    int32_t cnt;
    pmix_status_t rc;

    if (PMIX_BYTE_OBJECT != val->type || NULL == val->data.bo.bytes) {
        *unknown = true;
        return;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    /* load copies nothing and takes the bytes - so hand it a copy */
    bo.size = val->data.bo.size;
    bo.bytes = (char *) malloc(bo.size);
    if (NULL == bo.bytes) {
        *unknown = true;
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
        return;
    }
    memcpy(bo.bytes, val->data.bo.bytes, bo.size);
    rc = PMIx_Data_load(&buf, &bo);
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &known, &cnt, PMIX_BOOL);
    }
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &n, &cnt, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && !prte_pmix_count_fits(&buf, n)) {
        rc = PMIX_ERR_BAD_PARAM;
    }
    for (i = 0; PMIX_SUCCESS == rc && i < n; i++) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &blob, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        rc = PMIx_Data_pack(NULL, blobs, &blob, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&blob);
        if (PMIX_SUCCESS == rc) {
            (*nblobs)++;
        }
    }
    if (PMIX_SUCCESS != rc || !known) {
        *unknown = true;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
}

pmix_status_t prte_grpcomm_empty_participant_blob(pmix_byte_object_t *bo)
{
    PMIX_BYTE_OBJECT_CONSTRUCT(bo);
    return make_blob(NULL, 0, bo);
}

/* What the blobs say took part, without expanding anything: the procs named
 * by rank, and - for each namespace a daemon gave as PMIX_RANK_LOCAL_PEERS -
 * the pair of that namespace and that daemon, held as a pmix_proc_t whose
 * rank is the daemon's. A member took part if it is named, or if its
 * namespace's pair with the daemon it runs under is there. Expanding the
 * pairs through the job map instead would cost every daemon a walk of the
 * whole job. */
typedef struct {
    pmix_proc_t *named;
    size_t nnamed, snamed;
    pmix_proc_t *covered;
    size_t ncovered, scovered;
    /* the daemons that sent a blob - only their members can be missing */
    pmix_rank_t *daemons;
    size_t ndaemons, sdaemons;
} prte_grpcomm_parts_t;

static int rank_order(const void *a, const void *b)
{
    pmix_rank_t ra = *(const pmix_rank_t *) a, rb = *(const pmix_rank_t *) b;

    return (ra == rb) ? 0 : ((ra < rb) ? -1 : 1);
}

static pmix_status_t parts_add(pmix_proc_t **set, size_t *n, size_t *size,
                               const char *nspace, pmix_rank_t rank)
{
    pmix_proc_t *tmp;

    if (*n == *size) {
        tmp = (pmix_proc_t *) realloc(*set, 2 * (*size + 8) * sizeof(pmix_proc_t));
        if (NULL == tmp) {
            return PMIX_ERR_NOMEM;
        }
        *set = tmp;
        *size = 2 * (*size + 8);
    }
    PMIX_LOAD_PROCID(&(*set)[*n], nspace, rank);
    (*n)++;
    return PMIX_SUCCESS;
}

static pmix_status_t add_blob(pmix_byte_object_t *bo, prte_grpcomm_parts_t *parts)
{
    pmix_data_buffer_t buf;
    pmix_rank_t vpid;
    size_t n = 0, i;
    int32_t cnt;
    pmix_proc_t *procs = NULL;
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_load(&buf, bo);
    if (PMIX_SUCCESS != rc) {
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
        return rc;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &vpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &n, &cnt, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < n) {
        if (!prte_pmix_count_fits(&buf, n)) {
            rc = PMIX_ERR_BAD_PARAM;
        } else {
            PMIX_PROC_CREATE(procs, n);
            cnt = (int32_t) n;
            rc = (NULL == procs) ? PMIX_ERR_NOMEM
                                 : PMIx_Data_unpack(NULL, &buf, procs, &cnt, PMIX_PROC);
        }
    }
    if (PMIX_SUCCESS == rc) {
        if (parts->ndaemons == parts->sdaemons) {
            pmix_rank_t *tmp = (pmix_rank_t *) realloc(parts->daemons,
                                                       2 * (parts->sdaemons + 8) *
                                                           sizeof(pmix_rank_t));
            if (NULL == tmp) {
                rc = PMIX_ERR_NOMEM;
            } else {
                parts->daemons = tmp;
                parts->sdaemons = 2 * (parts->sdaemons + 8);
            }
        }
        if (PMIX_SUCCESS == rc) {
            parts->daemons[parts->ndaemons++] = vpid;
        }
    }
    for (i = 0; PMIX_SUCCESS == rc && i < n; i++) {
        if (PMIX_RANK_LOCAL_PEERS == procs[i].rank) {
            rc = parts_add(&parts->covered, &parts->ncovered, &parts->scovered,
                           procs[i].nspace, vpid);
        } else {
            rc = parts_add(&parts->named, &parts->nnamed, &parts->snamed,
                           procs[i].nspace, procs[i].rank);
        }
    }
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, n);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    return rc;
}

pmix_status_t prte_grpcomm_blob_procs(const pmix_byte_object_t *bo, pmix_proc_t **procs,
                                      size_t *nprocs)
{
    pmix_data_buffer_t buf;
    pmix_byte_object_t copy;
    pmix_rank_t vpid;
    size_t n = 0;
    int32_t cnt;
    pmix_status_t rc;

    *procs = NULL;
    *nprocs = 0;
    if (NULL == bo->bytes || 0 == bo->size) {
        return PMIX_SUCCESS;
    }
    copy.size = bo->size;
    copy.bytes = (char *) malloc(copy.size);
    if (NULL == copy.bytes) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(copy.bytes, bo->bytes, copy.size);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_load(&buf, &copy);
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &vpid, &cnt, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &n, &cnt, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < n) {
        if (!prte_pmix_count_fits(&buf, n)) {
            rc = PMIX_ERR_BAD_PARAM;
        } else {
            PMIX_PROC_CREATE(*procs, n);
            cnt = (int32_t) n;
            rc = (NULL == *procs) ? PMIX_ERR_NOMEM
                                  : PMIx_Data_unpack(NULL, &buf, *procs, &cnt, PMIX_PROC);
            if (PMIX_SUCCESS == rc) {
                *nprocs = n;
            } else if (NULL != *procs) {
                PMIX_PROC_FREE(*procs, n);
                *procs = NULL;
            }
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    return rc;
}

/* a member, alive and taking no part, is added to out */
static pmix_status_t add_if_absent(const pmix_proc_t *m, prte_proc_t *p,
                                   const prte_grpcomm_parts_t *parts, pmix_proc_t **out,
                                   size_t *nout, size_t *size)
{
    pmix_proc_t pair;
    pmix_rank_t d;

    if (NULL == p) {
        p = prte_get_proc_object(m);
    }
    /* not taking part is something only the living can do */
    if (NULL == p || NULL == p->node || NULL == p->node->daemon ||
        PRTE_PROC_STATE_UNTERMINATED < p->state || prte_grpcomm_proc_departed(m)) {
        return PMIX_SUCCESS;
    }
    /* a daemon that sent no blob had every local member take part */
    d = p->node->daemon->name.rank;
    if (NULL == bsearch(&d, parts->daemons, parts->ndaemons, sizeof(pmix_rank_t), rank_order)) {
        return PMIX_SUCCESS;
    }
    if (NULL != bsearch(m, parts->named, parts->nnamed, sizeof(pmix_proc_t), proc_order)) {
        return PMIX_SUCCESS;
    }
    PMIX_LOAD_PROCID(&pair, m->nspace, d);
    if (NULL != bsearch(&pair, parts->covered, parts->ncovered, sizeof(pmix_proc_t),
                        proc_order)) {
        return PMIX_SUCCESS;
    }
    return parts_add(out, nout, size, m->nspace, m->rank);
}

pmix_status_t prte_grpcomm_nonparticipants(const pmix_proc_t *members, size_t nmembers,
                                           const pmix_proc_t *added, size_t nadded,
                                           pmix_data_buffer_t *blobs, size_t nblobs,
                                           pmix_proc_t **out, size_t *nout)
{
    prte_grpcomm_parts_t parts;
    pmix_proc_t *res = NULL, cand;
    size_t nres = 0, rsize = 0, i, k, nlist;
    const pmix_proc_t *list;
    pmix_data_buffer_t copy;
    pmix_byte_object_t bo;
    prte_job_t *jdata;
    prte_proc_t *p;
    int32_t cnt;
    int r;
    pmix_status_t rc = PMIX_SUCCESS;

    *out = NULL;
    *nout = 0;
    memset(&parts, 0, sizeof(parts));
    /* read the blobs from a copy, leaving the tracker's for a restart */
    PMIX_DATA_BUFFER_CONSTRUCT(&copy);
    if (NULL != blobs && 0 < nblobs) {
        rc = PMIx_Data_copy_payload(&copy, blobs);
    }
    for (i = 0; PMIX_SUCCESS == rc && i < nblobs; i++) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &copy, &bo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        rc = add_blob(&bo, &parts);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&copy);
    if (PMIX_SUCCESS == rc) {
        if (1 < parts.nnamed) {
            qsort(parts.named, parts.nnamed, sizeof(pmix_proc_t), proc_order);
        }
        if (1 < parts.ncovered) {
            qsort(parts.covered, parts.ncovered, sizeof(pmix_proc_t), proc_order);
        }
        if (1 < parts.ndaemons) {
            qsort(parts.daemons, parts.ndaemons, sizeof(pmix_rank_t), rank_order);
        }
    }

    for (k = 0; PMIX_SUCCESS == rc && k < 2; k++) {
        list = (0 == k) ? members : added;
        nlist = (0 == k) ? nmembers : nadded;
        for (i = 0; PMIX_SUCCESS == rc && NULL != list && i < nlist; i++) {
            if (PMIX_RANK_WILDCARD != list[i].rank) {
                rc = add_if_absent(&list[i], NULL, &parts, &res, &nres, &rsize);
                continue;
            }
            jdata = prte_get_job_data_object(list[i].nspace);
            if (NULL == jdata) {
                continue;
            }
            for (r = 0; PMIX_SUCCESS == rc && r < jdata->procs->size; r++) {
                p = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, r);
                if (NULL == p) {
                    continue;
                }
                PMIX_LOAD_PROCID(&cand, p->name.nspace, p->name.rank);
                rc = add_if_absent(&cand, p, &parts, &res, &nres, &rsize);
            }
        }
    }
    free(parts.named);
    free(parts.covered);
    free(parts.daemons);
    if (PMIX_SUCCESS != rc) {
        free(res);
        return rc;
    }
    /* a member named twice - as itself and by its wildcard - once */
    if (1 < nres) {
        qsort(res, nres, sizeof(pmix_proc_t), proc_order);
        for (i = 1, k = 1; i < nres; i++) {
            if (0 != proc_order(&res[k - 1], &res[i])) {
                memcpy(&res[k++], &res[i], sizeof(pmix_proc_t));
            }
        }
        nres = k;
    }
    if (0 == nres) {
        free(res);
        return PMIX_SUCCESS;
    }
    *out = res;
    *nout = nres;
    return PMIX_SUCCESS;
}

char *prte_grpcomm_procs_string(const pmix_proc_t *procs, size_t nprocs, size_t max)
{
    char *s = NULL, *t;
    size_t n;

    for (n = 0; n < nprocs && n < max; n++) {
        if (NULL == s) {
            pmix_asprintf(&t, "%s:%u", procs[n].nspace, (unsigned) procs[n].rank);
        } else {
            pmix_asprintf(&t, "%s, %s:%u", s, procs[n].nspace, (unsigned) procs[n].rank);
            free(s);
        }
        s = t;
    }
    if (nprocs > max) {
        pmix_asprintf(&t, "%s and %lu more", (NULL == s) ? "" : s,
                      (unsigned long) (nprocs - max));
        free(s);
        s = t;
    }
    return (NULL == s) ? strdup("") : s;
}

bool prte_grpcomm_group_members_differ(const char *grpid, const pmix_proc_t *members,
                                       size_t nmembers)
{
    prte_grpcomm_live_group_t *grp;

    if (NULL == grpid) {
        return false;
    }
    PMIX_LIST_FOREACH(grp, &prte_grpcomm_globals.live_groups, prte_grpcomm_live_group_t) {
        if (0 == strcmp(grp->groupID, grpid) && group_lives(grp)) {
            return !prte_grpcomm_same_members(grp->members, grp->nmembers, members, nmembers);
        }
    }
    return false;
}

void prte_grpcomm_group_ended(const char *grpid)
{
    prte_grpcomm_live_group_t *grp, *gnext;

    if (NULL == grpid) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE(grp, gnext, &prte_grpcomm_globals.live_groups,
                           prte_grpcomm_live_group_t) {
        if (0 == strcmp(grp->groupID, grpid)) {
            pmix_list_remove_item(&prte_grpcomm_globals.live_groups, &grp->super);
            PMIX_RELEASE(grp);
        }
    }
}

bool prte_grpcomm_procs_lost(const pmix_proc_t *procs, size_t nprocs)
{
    prte_job_t *jdata;
    prte_proc_t *p;
    size_t n;
    int i;

    if (pmix_bitmap_is_clear(&prte_rml_base.failed_dmns)) {
        return false;
    }
    for (n = 0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD != procs[n].rank) {
            if (prte_grpcomm_proc_departed(&procs[n])) {
                return true;
            }
            continue;
        }
        /* a wildcard names the whole namespace, so ask whether any of it was
         * on a failed daemon. Skipping these would quietly excuse a
         * collective whose membership is written as a namespace - which is
         * the common spelling - from noticing it lost anyone at all. */
        jdata = prte_get_job_data_object(procs[n].nspace);
        if (NULL == jdata) {
            return true;
        }
        for (i = 0; i < jdata->procs->size; i++) {
            p = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, i);
            if (NULL == p || NULL == p->node || NULL == p->node->daemon) {
                continue;
            }
            if (pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns,
                                       p->node->daemon->name.rank)) {
                return true;
            }
        }
    }
    return false;
}

/* The master's issued-epoch counter: the number it has handed out, which is
 * not the same as the number it has applied.  See prte_grpcomm_issue_epoch()
 * in grpcomm.h for why the two are separate. */
static uint32_t issued_epoch = 0;

uint32_t prte_grpcomm_current_epoch(void)
{
    return prte_grpcomm_globals.recovery_epoch;
}

uint32_t prte_grpcomm_issue_epoch(void)
{
    return ++issued_epoch;
}

void prte_grpcomm_advance_epoch(uint32_t to)
{
    if (to <= prte_grpcomm_globals.recovery_epoch) {
        return;
    }
    prte_grpcomm_globals.recovery_epoch = to;

    pmix_output_verbose(1, prte_grpcomm_globals.output,
                        "%s grpcomm restarting collectives at epoch %u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) to);

    /* the repaired tree may have given us a parent that holds a stale view of
     * what departed below us, or none at all - rebuild it before the
     * restarted rollups need it. Order against the restart does not matter: a
     * tracker records which subtrees reported, so nothing can be counted
     * twice, and a rollup only waits longer while the set is rebuilt. */
    prte_grpcomm_departed_resend();
    prte_grpcomm_fence_restart();
    prte_grpcomm_group_restart();
}

void prte_grpcomm_fault_handler(const prte_rml_recovery_status_t* status)
{
    prte_grpcomm_xcast_fault_handler(status);
    /* Each collective first decides what it cannot recover and fails that
     * outright, marking those trackers so the restart below leaves them
     * alone... */
    prte_grpcomm_fence_fault_handler(status);
    prte_grpcomm_group_fault_handler(status);
    /* ...then everything still in flight restarts together. The restart has
     * to be DVM-wide and simultaneous: a daemon that resets and re-sends into
     * a parent that did not would have its subtree counted twice and another
     * not at all, which is a quietly wrong result rather than a hang. The
     * global scope is what provides that - one broadcast, same order
     * everywhere, after the routing tree has been repaired. */
    if (PRTE_RML_FAULT_SCOPE_GLOBAL == status->scope) {
        /* The epoch is the master's to decide and rides on the notice as an
         * absolute value, rather than each daemon counting the notices it has
         * seen.  Counting locally makes the number a function of what a daemon
         * received: one missed broadcast leaves it a step behind for the rest
         * of the DVM's life, with every contribution it offers dropped as
         * stale, and a daemon launched into a DVM that has already recovered
         * has necessarily missed all of them.  Taking the value from the
         * notice makes the number reconstructable from any message that
         * carries one, which is what lets a joining daemon be told it. */
        prte_grpcomm_advance_epoch(status->epoch);
    }
}
