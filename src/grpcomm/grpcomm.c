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
static char *bcast_movement = NULL;
static int bcast_bulk_min_bytes = 0;
static int bcast_bulk_min_daemons = 0;

/* Defaults for the "auto" selection.  Both are deliberately conservative:
 * below them the tree movement is either faster or indistinguishable, and the
 * bulk movement's fixed costs (a participant list on the wire, log2(N) lateral
 * connections per daemon) are pure loss. */
#define PRTE_GRPCOMM_BULK_MIN_BYTES_DEFAULT   (256 * 1024)
#define PRTE_GRPCOMM_BULK_MIN_DAEMONS_DEFAULT 8

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

    bcast_movement = NULL;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "bcast_movement",
                               "How a broadcast's payload travels: \"tree\" "
                               "(whole payload to each routing-tree child), "
                               "\"bulk\" (scatter down the tree then allgather "
                               "across lateral links), or \"auto\" (by size). "
                               "Only the daemon that originates a broadcast "
                               "consults this; the choice it makes is carried "
                               "on the wire.",
                               PMIX_MCA_BASE_VAR_TYPE_STRING,
                               &bcast_movement);

    bcast_bulk_min_bytes = PRTE_GRPCOMM_BULK_MIN_BYTES_DEFAULT;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "bcast_bulk_min_bytes",
                               "Smallest payload \"auto\" will move with the "
                               "bulk movement",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &bcast_bulk_min_bytes);

    bcast_bulk_min_daemons = PRTE_GRPCOMM_BULK_MIN_DAEMONS_DEFAULT;
    pmix_mca_base_var_register("prte", "grpcomm", NULL, "bcast_bulk_min_daemons",
                               "Smallest DVM \"auto\" will move a payload "
                               "across with the bulk movement",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &bcast_bulk_min_daemons);

    /* Resolve the movement selection once, here, rather than re-parsing the
     * string on every broadcast.  An unrecognized spelling falls back to the
     * tree with a warning instead of being taken for "auto": the parameter is
     * a deliberate override, so silently ignoring a typo in it would hide the
     * very experiment the user is running.
     *
     * The default is "tree", not "auto", and that is a statement about
     * evidence rather than about the code. Choosing by size is only worth
     * doing once the crossover point has been measured, and measuring it needs
     * real multi-node hardware: the cost model's constants are not the
     * textbook ones here - alpha is a progress-thread hop rather than a
     * round trip, and xcast already compresses, which discounts the tree's
     * bandwidth term by an unknown factor. A container swarm on one host
     * cannot supply either. So "auto" exists, is implemented, and is opt-in
     * until somebody has that number. */
    prte_grpcomm_globals.bcast_select = PRTE_GRPCOMM_BCAST_SELECT_TREE;
    if (NULL != bcast_movement) {
        if (0 == strcasecmp(bcast_movement, "auto")) {
            prte_grpcomm_globals.bcast_select = PRTE_GRPCOMM_BCAST_SELECT_AUTO;
        } else if (0 == strcasecmp(bcast_movement, "tree")) {
            prte_grpcomm_globals.bcast_select = PRTE_GRPCOMM_BCAST_SELECT_TREE;
        } else if (0 == strcasecmp(bcast_movement, "bulk")) {
            prte_grpcomm_globals.bcast_select = PRTE_GRPCOMM_BCAST_SELECT_BULK;
        } else {
            pmix_output(0, "PRRTE: grpcomm_bcast_movement \"%s\" is not one of "
                           "auto/tree/bulk - using tree", bcast_movement);
            prte_grpcomm_globals.bcast_select = PRTE_GRPCOMM_BCAST_SELECT_TREE;
        }
    }
    prte_grpcomm_globals.bcast_bulk_min_bytes =
        (0 > bcast_bulk_min_bytes) ? 0 : (size_t) bcast_bulk_min_bytes;
    /* An exchange needs at least two participants to be an exchange at all,
     * so a configured floor below that would still be answered by the tree. */
    prte_grpcomm_globals.bcast_bulk_min_daemons =
        (2 > bcast_bulk_min_daemons) ? 2 : (size_t) bcast_bulk_min_daemons;
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
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_BULK,
                  PRTE_RML_PERSISTENT, prte_grpcomm_xcast_bulk_recv, NULL);
    /* A bulk broadcast's exchange partners are not routing-tree neighbours, so
     * the RML hands their losses here instead of repairing the tree. */
    prte_rml_lateral_set_lost_callback(prte_grpcomm_xcast_lateral_lost);

    /* fence receives */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_recv, NULL);
    /* setup recv for barrier release */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FENCE_RELEASE,
                  PRTE_RML_PERSISTENT, prte_grpcomm_fence_release, NULL);

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
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_XCAST_BULK);
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
