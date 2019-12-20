/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/mca/base/base.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/rmaps/base/static-components.h"

/*
 * Global variables
 */
prrte_rmaps_base_t prrte_rmaps_base = {{{0}}};
bool prrte_rmaps_base_pernode = false;
int prrte_rmaps_base_n_pernode = 0;
int prrte_rmaps_base_n_persocket = 0;

/*
 * Local variables
 */
static char *rmaps_base_mapping_policy = NULL;
static char *rmaps_base_ranking_policy = NULL;
static bool rmaps_base_bycore = false;
static bool rmaps_base_byslot = false;
static bool rmaps_base_bynode = false;
static bool rmaps_base_no_schedule_local = false;
static bool rmaps_base_no_oversubscribe = false;
static bool rmaps_base_oversubscribe = false;
static bool rmaps_base_display_devel_map = false;
static bool rmaps_base_display_diffable_map = false;
static char *rmaps_base_topo_file = NULL;
static char *rmaps_dist_device = NULL;
static bool rmaps_base_inherit = false;

static int prrte_rmaps_base_register(prrte_mca_base_register_flag_t flags)
{
    int var_id;

    prrte_rmaps_base_pernode = false;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "pernode",
                                         "Launch one ppn as directed",
                                         PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                         PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                         &prrte_rmaps_base_pernode);
    (void) prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps", "ppr", "pernode", 0);

    prrte_rmaps_base_n_pernode = 0;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "n_pernode",
                                         "Launch n procs/node", PRRTE_MCA_BASE_VAR_TYPE_INT,
                                         NULL, 0, 0,
                                         PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rmaps_base_n_pernode);
    (void) prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps","ppr", "n_pernode", 0);

    prrte_rmaps_base_n_persocket = 0;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "n_persocket",
                                         "Launch n procs/socket", PRRTE_MCA_BASE_VAR_TYPE_INT,
                                         NULL, 0, 0,
                                         PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rmaps_base_n_persocket);
    (void) prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps","ppr", "n_persocket", 0);

    prrte_rmaps_base.ppr = NULL;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "pattern",
                                         "Comma-separated list of number of processes on a given resource type [default: none]",
                                         PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rmaps_base.ppr);
    (void) prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps","ppr", "pattern", 0);

    /* define default mapping policy */
    rmaps_base_mapping_policy = NULL;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "mapping_policy",
                                         "Mapping Policy [slot | hwthread | core (default:np<=2) | l1cache | l2cache | l3cache | socket (default:np>2) | numa | board | node | seq | dist | ppr], with allowed modifiers :PE=y,SPAN,OVERSUBSCRIBE,NOOVERSUBSCRIBE",
                                          PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                          PRRTE_INFO_LVL_9,
                                          PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                          &rmaps_base_mapping_policy);
    (void) prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps", "base", "schedule_policy",
                                               PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* define default ranking policy */
    rmaps_base_ranking_policy = NULL;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "ranking_policy",
                                       "Ranking Policy [slot (default:np<=2) | hwthread | core | l1cache | l2cache | l3cache | socket (default:np>2) | numa | board | node], with modifier :SPAN or :FILL",
                                       PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &rmaps_base_ranking_policy);

    /* backward compatibility */
    rmaps_base_bycore = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "bycore",
                                       "Whether to map and rank processes round-robin by core",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_bycore);

    rmaps_base_byslot = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "byslot",
                                       "Whether to map and rank processes round-robin by slot",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_byslot);

    rmaps_base_bynode = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "bynode",
                                       "Whether to map and rank processes round-robin by node",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_bynode);

    /* #cpus/rank to use */
    prrte_rmaps_base.cpus_per_rank = 0;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", "base", "cpus_per_proc",
                                         "Number of cpus to use for each rank [1-2**15 (default=1)]",
                                         PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                         PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rmaps_base.cpus_per_rank);
    prrte_mca_base_var_register_synonym(var_id, "prrte", "rmaps", "base", "cpus_per_rank", 0);

    rmaps_dist_device = NULL;
    var_id = prrte_mca_base_var_register("prrte", "rmaps", NULL, "dist_device",
                                         "If specified, map processes near to this device. Any device name that is identified by the lstopo hwloc utility as Net or OpenFabrics (for example eth0, mlx4_0, etc) or special name as auto ",
                                         PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                         PRRTE_INFO_LVL_9,
                                         PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                         &rmaps_dist_device);

    rmaps_base_no_schedule_local = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "no_schedule_local",
                                       "If false, allow scheduling MPI applications on the same node as mpirun (default).  If true, do not schedule any MPI applications on the same node as mpirun",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_no_schedule_local);

    /** default condition that allows oversubscription */
    rmaps_base_no_oversubscribe = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "no_oversubscribe",
                                       "If true, then do not allow oversubscription of nodes - mpirun will return an error if there aren't enough nodes to launch all processes without oversubscribing",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_no_oversubscribe);

    rmaps_base_oversubscribe = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "oversubscribe",
                                       "If true, then allow oversubscription of nodes and overloading of processing elements",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_oversubscribe);

    /* should we display the map after determining it? */
    prrte_rmaps_base.display_map = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "display_map",
                                       "Whether to display the process map after it is computed",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rmaps_base.display_map);

    rmaps_base_display_devel_map = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "display_devel_map",
                                       "Whether to display a developer-detail process map after it is computed",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_display_devel_map);

    /* should we display the topology along with the map? */
    prrte_display_topo_with_map = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "display_topo_with_map",
                                       "Whether to display the topology with the map",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_display_topo_with_map);

    rmaps_base_display_diffable_map = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "display_diffable_map",
                                       "Whether to display a diffable process map after it is computed",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_display_diffable_map);

    rmaps_base_topo_file = NULL;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "topology",
                                       "hwloc topology file (xml format) describing the topology of the compute nodes [default: none]",
                                       PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_topo_file);

    rmaps_base_inherit = false;
    (void) prrte_mca_base_var_register("prrte", "rmaps", "base", "inherit",
                                       "Whether child jobs shall inherit launch directives",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &rmaps_base_inherit);

    return PRRTE_SUCCESS;
}

static int prrte_rmaps_base_close(void)
{
    prrte_list_item_t *item;

    /* cleanup globals */
    while (NULL != (item = prrte_list_remove_first(&prrte_rmaps_base.selected_modules))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_rmaps_base.selected_modules);

    return prrte_mca_base_framework_components_close(&prrte_rmaps_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_rmaps_base_open(prrte_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PRRTE_CONSTRUCT(&prrte_rmaps_base.selected_modules, prrte_list_t);
    prrte_rmaps_base.slot_list = NULL;
    prrte_rmaps_base.mapping = 0;
    prrte_rmaps_base.ranking = 0;
    prrte_rmaps_base.device = NULL;
    prrte_rmaps_base.inherit = rmaps_base_inherit;

    /* if a topology file was given, then set our topology
     * from it. Even though our actual topology may differ,
     * mpirun only needs to see the compute node topology
     * for mapping purposes
     */
    if (NULL != rmaps_base_topo_file) {
        if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_set_topology(rmaps_base_topo_file))) {
            prrte_show_help("help-prrte-rmaps-base.txt", "topo-file", true, rmaps_base_topo_file);
            return PRRTE_ERR_SILENT;
        }
    }

    /* check for violations that has to be detected before we parse the mapping option */
    if (NULL != prrte_rmaps_base.ppr) {
        prrte_show_help("help-prrte-rmaps-base.txt", "deprecated", true,
                       "--ppr, -ppr", "--map-by ppr:<pattern>",
                       "rmaps_base_pattern, rmaps_ppr_pattern",
                       "rmaps_base_mapping_policy=ppr:<pattern>");
        /* if the mapping policy is NULL, then we can proceed */
        if (NULL == rmaps_base_mapping_policy) {
            prrte_asprintf(&rmaps_base_mapping_policy, "ppr:%s", prrte_rmaps_base.ppr);
        } else {
            return PRRTE_ERR_SILENT;
        }
    }

    if (0 < prrte_rmaps_base.cpus_per_rank) {
        prrte_show_help("help-prrte-rmaps-base.txt", "deprecated", true,
                       "--cpus-per-proc, -cpus-per-proc, --cpus-per-rank, -cpus-per-rank",
                       "--map-by <obj>:PE=N, default <obj>=NUMA",
                       "rmaps_base_cpus_per_proc", "rmaps_base_mapping_policy=<obj>:PE=N, default <obj>=NUMA");
    }

    if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_set_mapping_policy(NULL, &prrte_rmaps_base.mapping,
                                                                 &prrte_rmaps_base.device,
                                                                 rmaps_base_mapping_policy))) {
        return rc;
    }

    if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_set_ranking_policy(&prrte_rmaps_base.ranking,
                                                                 prrte_rmaps_base.mapping,
                                                                 rmaps_base_ranking_policy))) {
        return rc;
    }

    if (rmaps_base_bycore) {
        prrte_show_help("help-prrte-rmaps-base.txt", "deprecated", true,
                       "--bycore, -bycore", "--map-by core",
                       "rmaps_base_bycore", "rmaps_base_mapping_policy=core");
        /* set mapping policy to bycore - error if something else already set */
        if ((PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) &&
            PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping) != PRRTE_MAPPING_BYCORE) {
            /* error - cannot redefine the default mapping policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "bycore", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_BYCORE);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
        /* set ranking policy to bycore - error if something else already set */
        if ((PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking)) &&
            PRRTE_GET_RANKING_POLICY(prrte_rmaps_base.ranking) != PRRTE_RANK_BY_CORE) {
            /* error - cannot redefine the default ranking policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "ranking",
                           "bycore", prrte_rmaps_base_print_ranking(prrte_rmaps_base.ranking));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_RANKING_POLICY(prrte_rmaps_base.ranking, PRRTE_RANK_BY_CORE);
        PRRTE_SET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking, PRRTE_RANKING_GIVEN);
    }

    if (rmaps_base_byslot) {
        prrte_show_help("help-prrte-rmaps-base.txt", "deprecated", true,
                       "--byslot, -byslot", "--map-by slot",
                       "rmaps_base_byslot", "rmaps_base_mapping_policy=slot");
        /* set mapping policy to byslot - error if something else already set */
        if ((PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) &&
            PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping) != PRRTE_MAPPING_BYSLOT) {
            /* error - cannot redefine the default mapping policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "byslot", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_BYSLOT);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
        /* set ranking policy to byslot - error if something else already set */
        if ((PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking)) &&
            PRRTE_GET_RANKING_POLICY(prrte_rmaps_base.ranking) != PRRTE_RANK_BY_SLOT) {
            /* error - cannot redefine the default ranking policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "ranking",
                           "byslot", prrte_rmaps_base_print_ranking(prrte_rmaps_base.ranking));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_RANKING_POLICY(prrte_rmaps_base.ranking, PRRTE_RANK_BY_SLOT);
        PRRTE_SET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking, PRRTE_RANKING_GIVEN);
    }

    if (rmaps_base_bynode) {
        prrte_show_help("help-prrte-rmaps-base.txt", "deprecated", true,
                       "--bynode, -bynode", "--map-by node",
                       "rmaps_base_bynode", "rmaps_base_mapping_policy=node");
        /* set mapping policy to bynode - error if something else already set */
        if ((PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) &&
            PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping) != PRRTE_MAPPING_BYNODE) {
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "bynode", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_BYNODE);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
        /* set ranking policy to bynode - error if something else already set */
        if ((PRRTE_RANKING_GIVEN & PRRTE_GET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking)) &&
            PRRTE_GET_RANKING_POLICY(prrte_rmaps_base.ranking) != PRRTE_RANK_BY_NODE) {
            /* error - cannot redefine the default ranking policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "ranking",
                           "bynode", prrte_rmaps_base_print_ranking(prrte_rmaps_base.ranking));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_RANKING_POLICY(prrte_rmaps_base.ranking, PRRTE_RANK_BY_NODE);
        PRRTE_SET_RANKING_DIRECTIVE(prrte_rmaps_base.ranking, PRRTE_RANKING_GIVEN);
    }

    if (0 < prrte_rmaps_base.cpus_per_rank) {
        /* if we were asked for cpus/proc, then we have to
         * bind to those cpus - any other binding policy is an
         * error
         */
        if (PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy)) {
            if (prrte_hwloc_use_hwthreads_as_cpus) {
                if (PRRTE_BIND_TO_HWTHREAD != PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) &&
                    PRRTE_BIND_TO_NONE != PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "mismatch-binding", true,
                                   prrte_rmaps_base.cpus_per_rank, "use-hwthreads-as-cpus",
                                   prrte_hwloc_base_print_binding(prrte_hwloc_binding_policy),
                                   "bind-to hwthread");
                    return PRRTE_ERR_SILENT;
                }
            } else if (PRRTE_BIND_TO_CORE != PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) &&
                       PRRTE_BIND_TO_NONE != PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                prrte_show_help("help-prrte-rmaps-base.txt", "mismatch-binding", true,
                               prrte_rmaps_base.cpus_per_rank, "cores as cpus",
                               prrte_hwloc_base_print_binding(prrte_hwloc_binding_policy),
                               "bind-to core");
                return PRRTE_ERR_SILENT;
            }
        } else {
            if (prrte_hwloc_use_hwthreads_as_cpus) {
                PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_HWTHREAD);
            } else {
                PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_CORE);
            }
        }
        if (1 < prrte_rmaps_base.cpus_per_rank) {
            /* we need to ensure we are mapping to a high-enough level to have
             * multiple cpus beneath it - by default, we'll go to the NUMA level */
            if (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) {
                if (PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping) == PRRTE_MAPPING_BYHWTHREAD ||
                  (PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping) == PRRTE_MAPPING_BYCORE &&
                  !prrte_hwloc_use_hwthreads_as_cpus)) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "mapping-too-low-init", true);
                    return PRRTE_ERR_SILENT;
                }
            } else {
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "%s rmaps:base pe/rank set - setting mapping to BYNUMA",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_BYNUMA);
                PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
            }
        }
    }

    if (prrte_rmaps_base_pernode) {
        /* if the user didn't specify a mapping directive, then match it */
        if (!(PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            /* ensure we set the mapping policy to ppr */
            PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_PPR);
            PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
            /* define the ppr */
            prrte_rmaps_base.ppr = strdup("1:node");
        }
    }

    if (0 < prrte_rmaps_base_n_pernode) {
         /* if the user didn't specify a mapping directive, then match it */
         if (!(PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
             /* ensure we set the mapping policy to ppr */
             PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_PPR);
             PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
             /* define the ppr */
             prrte_asprintf(&prrte_rmaps_base.ppr, "%d:node", prrte_rmaps_base_n_pernode);
         }
    }

    if (0 < prrte_rmaps_base_n_persocket) {
        /* if the user didn't specify a mapping directive, then match it */
        if (!(PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            /* ensure we set the mapping policy to ppr */
            PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_PPR);
            PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
            /* define the ppr */
            prrte_asprintf(&prrte_rmaps_base.ppr, "%d:socket", prrte_rmaps_base_n_persocket);
        }
    }

    /* Should we schedule on the local node or not? */
    if (rmaps_base_no_schedule_local) {
        prrte_rmaps_base.mapping |= PRRTE_MAPPING_NO_USE_LOCAL;
    }

    /* Should we oversubscribe or not? */
    if (rmaps_base_no_oversubscribe) {
        if ((PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) &&
            !(PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            /* error - cannot redefine the default mapping policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "no-oversubscribe", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
    }

    /** force oversubscription permission */
    if (rmaps_base_oversubscribe) {
        if ((PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) &&
            (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            /* error - cannot redefine the default mapping policy */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "oversubscribe", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_UNSET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
        /* also set the overload allowed flag */
        prrte_hwloc_binding_policy |= PRRTE_BIND_ALLOW_OVERLOAD;
    }

    /* should we display a detailed (developer-quality) version of the map after determining it? */
    if (rmaps_base_display_devel_map) {
        prrte_rmaps_base.display_map = true;
        prrte_devel_level_output = true;
    }

    /* should we display a diffable report of proc locations after determining it? */
    if (rmaps_base_display_diffable_map) {
        prrte_rmaps_base.display_map = true;
        prrte_display_diffable_output = true;
    }

    /* Open up all available components */
    rc = prrte_mca_base_framework_components_open(&prrte_rmaps_base_framework, flags);

    /* check to see if any component indicated a problem */
    if (PRRTE_MAPPING_CONFLICTED & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) {
        /* the component would have already reported the error, so
         * tell the rest of the chain to shut up
         */
        return PRRTE_ERR_SILENT;
    }

    /* All done */
    return rc;
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, rmaps, "PRRTE Mapping Subsystem",
                                 prrte_rmaps_base_register, prrte_rmaps_base_open, prrte_rmaps_base_close,
                                 prrte_rmaps_base_static_components, 0);

PRRTE_CLASS_INSTANCE(prrte_rmaps_base_selected_module_t,
                   prrte_list_item_t,
                   NULL, NULL);


static int check_modifiers(char *ck, prrte_mapping_policy_t *tmp)
{
    char **ck2, *ptr;
    int i;
    bool found = false;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "%s rmaps:base check modifiers with %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == ck) ? "NULL" : ck);

    if (NULL == ck) {
        return PRRTE_SUCCESS;
    }

    ck2 = prrte_argv_split(ck, ',');
    for (i=0; NULL != ck2[i]; i++) {
        if (0 == strncasecmp(ck2[i], "span", strlen(ck2[i]))) {
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SPAN);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_GIVEN);
            found = true;
        } else if (0 == strncasecmp(ck2[i], "pe", strlen("pe"))) {
            /* break this at the = sign to get the number */
            if (NULL == (ptr = strchr(ck2[i], '='))) {
                /* missing the value */
                prrte_show_help("help-prrte-rmaps-base.txt", "missing-value", true, "pe", ck2[i]);
                prrte_argv_free(ck2);
                return PRRTE_ERR_SILENT;
            }
            ptr++;
            prrte_rmaps_base.cpus_per_rank = strtol(ptr, NULL, 10);
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "%s rmaps:base setting pe/rank to %d",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                prrte_rmaps_base.cpus_per_rank);
            found = true;
        } else if (0 == strncasecmp(ck2[i], "oversubscribe", strlen(ck2[i]))) {
            PRRTE_UNSET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
            found = true;
        } else if (0 == strncasecmp(ck2[i], "nooversubscribe", strlen(ck2[i]))) {
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRRTE_SET_MAPPING_DIRECTIVE(*tmp, PRRTE_MAPPING_SUBSCRIBE_GIVEN);
            found = true;
        } else {
            /* unrecognized modifier */
            prrte_argv_free(ck2);
            return PRRTE_ERR_BAD_PARAM;
        }
    }
    prrte_argv_free(ck2);
    if (found) {
        return PRRTE_SUCCESS;
    }
    return PRRTE_ERR_TAKE_NEXT_OPTION;
}

int prrte_rmaps_base_set_mapping_policy(prrte_job_t *jdata,
                                       prrte_mapping_policy_t *policy,
                                       char **device, char *inspec)
{
    char *ck;
    char *ptr, *cptr;
    prrte_mapping_policy_t tmp;
    int rc;
    size_t len;
    char *spec;
    char *pch;

    /* set defaults */
    tmp = 0;
    if (NULL != device) {
        *device = NULL;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "%s rmaps:base set policy with %s device %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == inspec) ? "NULL" : inspec,
                        (NULL == device) ? "NULL" : "NONNULL");

    if (NULL == inspec) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYSOCKET);
        goto setpolicy;
    }

    spec = strdup(inspec);  // protect the input string
    /* see if a colon was included - if so, then we have a policy + modifier */
    ck = strchr(spec, ':');
    if (NULL != ck) {
        /* if the colon is the first character of the string, then we
         * just have modifiers on the default mapping policy */
        if (ck == spec) {
            ck++;  // step over the colon
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "%s rmaps:base only modifiers %s provided - assuming bysocket mapping",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), ck);
            PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYSOCKET);
            if (PRRTE_ERR_SILENT == (rc = check_modifiers(ck, &tmp)) &&
                PRRTE_ERR_BAD_PARAM != rc) {
                free(spec);
                return PRRTE_ERR_SILENT;
            }
            free(spec);
            goto setpolicy;
        }
        *ck = '\0';  // terminate spec where the colon was
        ck++;    // step past the colon
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "%s rmaps:base policy %s modifiers %s provided",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), spec, ck);

        if (0 == strncasecmp(spec, "ppr", strlen(spec))) {
            /* at this point, ck points to a string that contains at least
             * two fields (specifying the #procs/obj and the object we are
             * to map by). we have to allow additional modifiers here - e.g.,
             * specifying #pe's/proc or oversubscribe - so check for modifiers. if
             * they are present, ck will look like "N:obj:mod1,mod2,mod3"
             */
            if (NULL == (ptr = strchr(ck, ':'))) {
                /* this is an error - there had to be at least one
                 * colon to delimit the number from the object type
                 */
                prrte_show_help("help-prrte-rmaps-base.txt", "invalid-pattern", true, inspec);
                free(spec);
                return PRRTE_ERR_SILENT;
            }
            ptr++; // move past the colon
            /* at this point, ptr is pointing to the beginning of the string that describes
             * the object plus any modifiers (i.e., "obj:mod1,mod2". We first check to see if there
             * is another colon indicating that there are modifiers to the request */
            if (NULL != (cptr = strchr(ptr, ':'))) {
                /* there are modifiers, so we terminate the object string
                 * at the location of the colon */
                *cptr = '\0';
                /* step over that colon */
                cptr++;
                /* now check for modifiers  - may be none, so
                 * don't emit an error message if the modifier
                 * isn't recognized */
                if (PRRTE_ERR_SILENT == (rc = check_modifiers(cptr, &tmp)) &&
                    PRRTE_ERR_BAD_PARAM != rc) {
                    free(spec);
                    return PRRTE_ERR_SILENT;
                }
            }
            /* now save the pattern */
            if (NULL == jdata || NULL == jdata->map) {
                prrte_rmaps_base.ppr = strdup(ck);
            } else {
                jdata->map->ppr = strdup(ck);
            }
            PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_PPR);
            PRRTE_SET_MAPPING_DIRECTIVE(tmp, PRRTE_MAPPING_GIVEN);
            free(spec);
            goto setpolicy;
        }
        if (PRRTE_SUCCESS != (rc = check_modifiers(ck, &tmp)) &&
            PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            if (PRRTE_ERR_BAD_PARAM == rc) {
                prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-modifier", true, inspec);
            }
            free(spec);
            return rc;
        }
    }
    len = strlen(spec);
    if (0 == strncasecmp(spec, "slot", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYSLOT);
    } else if (0 == strncasecmp(spec, "node", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYNODE);
    } else if (0 == strncasecmp(spec, "seq", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_SEQ);
    } else if (0 == strncasecmp(spec, "core", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYCORE);
    } else if (0 == strncasecmp(spec, "l1cache", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL1CACHE);
    } else if (0 == strncasecmp(spec, "l2cache", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL2CACHE);
    } else if (0 == strncasecmp(spec, "l3cache", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYL3CACHE);
    } else if (0 == strncasecmp(spec, "socket", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYSOCKET);
    } else if (0 == strncasecmp(spec, "numa", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYNUMA);
    } else if (0 == strncasecmp(spec, "board", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYBOARD);
    } else if (0 == strncasecmp(spec, "hwthread", len)) {
        PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYHWTHREAD);
        /* if we are mapping processes to individual hwthreads, then
         * we need to treat those hwthreads as separate cpus
         */
        prrte_hwloc_use_hwthreads_as_cpus = true;
    } else if (0 == strncasecmp(spec, "dist", len)) {
        if (NULL != rmaps_dist_device) {
            if (NULL != (pch = strchr(rmaps_dist_device, ':'))) {
                *pch = '\0';
            }
            if (NULL != device) {
                *device = strdup(rmaps_dist_device);
            }
            PRRTE_SET_MAPPING_POLICY(tmp, PRRTE_MAPPING_BYDIST);
        } else {
            prrte_show_help("help-prrte-rmaps-base.txt", "device-not-specified", true);
            free(spec);
            return PRRTE_ERR_SILENT;
        }
    } else {
        prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true, "mapping", spec);
        free(spec);
        return PRRTE_ERR_SILENT;
    }
    free(spec);
    PRRTE_SET_MAPPING_DIRECTIVE(tmp, PRRTE_MAPPING_GIVEN);

 setpolicy:
    if (NULL == jdata || NULL == jdata->map) {
        *policy = tmp;
    } else {
        jdata->map->mapping = tmp;
    }

    return PRRTE_SUCCESS;
}

int prrte_rmaps_base_set_ranking_policy(prrte_ranking_policy_t *policy,
                                       prrte_mapping_policy_t mapping,
                                       char *spec)
{
    prrte_mapping_policy_t map;
    prrte_ranking_policy_t tmp;
    char **ck;
    size_t len;

    /* set default */
    tmp = 0;

    if (NULL == spec) {
        /* check for map-by object directives - we set the
         * ranking to match if one was given
         */
        if (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(mapping)) {
            map = PRRTE_GET_MAPPING_POLICY(mapping);
            switch (map) {
            case PRRTE_MAPPING_BYSLOT:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
                break;
            case PRRTE_MAPPING_BYNODE:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NODE);
                break;
            case PRRTE_MAPPING_BYCORE:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_CORE);
                break;
            case PRRTE_MAPPING_BYL1CACHE:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L1CACHE);
                break;
            case PRRTE_MAPPING_BYL2CACHE:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L2CACHE);
                break;
            case PRRTE_MAPPING_BYL3CACHE:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L3CACHE);
                break;
            case PRRTE_MAPPING_BYSOCKET:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SOCKET);
                break;
            case PRRTE_MAPPING_BYNUMA:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NUMA);
                break;
            case PRRTE_MAPPING_BYBOARD:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_BOARD);
                break;
            case PRRTE_MAPPING_BYHWTHREAD:
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_HWTHREAD);
                break;
            default:
                /* anything not tied to a specific hw obj can rank by slot */
                PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
                break;
            }
        } else {
            /* if no map-by was given, default to by-slot */
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
        }
    } else {
        ck = prrte_argv_split(spec, ':');
        if (2 < prrte_argv_count(ck)) {
            /* incorrect format */
            prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true, "ranking", policy);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        if (2 == prrte_argv_count(ck)) {
            if (0 == strncasecmp(ck[1], "span", strlen(ck[1]))) {
                PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_SPAN);
            } else if (0 == strncasecmp(ck[1], "fill", strlen(ck[1]))) {
                PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_FILL);
            } else {
                /* unrecognized modifier */
                prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-modifier", true, ck[1]);
                prrte_argv_free(ck);
                return PRRTE_ERR_SILENT;
            }
        }
        len = strlen(ck[0]);
        if (0 == strncasecmp(ck[0], "slot", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SLOT);
        } else if (0 == strncasecmp(ck[0], "node", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NODE);
        } else if (0 == strncasecmp(ck[0], "hwthread", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_HWTHREAD);
        } else if (0 == strncasecmp(ck[0], "core", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_CORE);
        } else if (0 == strncasecmp(ck[0], "l1cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L1CACHE);
        } else if (0 == strncasecmp(ck[0], "l2cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L2CACHE);
        } else if (0 == strncasecmp(ck[0], "l3cache", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_L3CACHE);
        } else if (0 == strncasecmp(ck[0], "socket", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_SOCKET);
        } else if (0 == strncasecmp(ck[0], "numa", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_NUMA);
        } else if (0 == strncasecmp(ck[0], "board", len)) {
            PRRTE_SET_RANKING_POLICY(tmp, PRRTE_RANK_BY_BOARD);
        } else {
            prrte_show_help("help-prrte-rmaps-base.txt", "unrecognized-policy", true, "ranking", rmaps_base_ranking_policy);
            prrte_argv_free(ck);
            return PRRTE_ERR_SILENT;
        }
        prrte_argv_free(ck);
        PRRTE_SET_RANKING_DIRECTIVE(tmp, PRRTE_RANKING_GIVEN);
    }

    *policy = tmp;
    return PRRTE_SUCCESS;
}
