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

#include "src/mca/base/pmix_base.h"
#include "src/util/pmix_output.h"

#include "grpcomm_internal.h"

prte_grpcomm_globals_t prte_grpcomm_globals = {
    .output = -1,
    .context_id = UINT32_MAX,
    .fence_ops = PMIX_LIST_STATIC_INIT,
    .group_ops = PMIX_LIST_STATIC_INIT
};

prte_grpcomm_release_bcast_fn_t prte_grpcomm_release_bcast = prte_grpcomm_xcast;

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
}

/**
 * Stand grpcomm up
 */
int prte_grpcomm_init(void)
{
    /* setup the trackers */
    PMIX_CONSTRUCT(&prte_grpcomm_globals.xcast_ops,
                   prte_grpcomm_xcast_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.group_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.completed_group_ops, pmix_list_t);

    /* xcast receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST,
                  PRTE_RML_PERSISTENT, prte_grpcomm_xcast_recv, NULL);
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_ACK,
                  PRTE_RML_PERSISTENT, prte_grpcomm_xcast_ack, NULL);
    /* A bulk broadcast's exchange partners are not routing-tree neighbours, so
     * the RML hands their losses here instead of repairing the tree. */

    /* fence receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_recv, NULL);
    /* setup recv for barrier release */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_release, NULL);
    /* ...and for a fence's lateral allgather, which arrives from exchange
     * partners rather than from a routing-tree child */

    /* group receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP,
                  PRTE_RML_PERSISTENT, prte_grpcomm_grp_recv, NULL);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_grp_release, NULL);
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

    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_ACK);
    prte_rml_lateral_set_lost_callback(NULL);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP);
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_GROUP_RELEASE);
    return;
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

void prte_grpcomm_advance_epoch(uint32_t to)
{
    if (to <= prte_grpcomm_globals.recovery_epoch) {
        return;
    }
    prte_grpcomm_globals.recovery_epoch = to;

    pmix_output_verbose(1, prte_grpcomm_globals.output,
                        "%s grpcomm restarting collectives at epoch %u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) to);

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
        prte_grpcomm_advance_epoch(
            prte_grpcomm_globals.recovery_epoch + 1);
    }
}
