/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "src/class/pmix_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

static int prte_rmaps_rf_map(prte_job_t *jdata,
                             prte_rmaps_options_t *options);

prte_rmaps_base_module_t prte_rmaps_rank_file_module = {
    .map_job = prte_rmaps_rf_map
};

static int prte_rmaps_rank_file_parse(const char *);
static char *prte_rmaps_rank_file_parse_string_or_int(void);

/*
 * Local variable
 */
static pmix_pointer_array_t rankmap;
static int num_ranks = 0;

/*
 * Create a rank_file  mapping for the job.
 */
static int prte_rmaps_rf_map(prte_job_t *jdata,
                             prte_rmaps_options_t *options)
{
    prte_app_context_t *app = NULL;
    int32_t i, k;
    pmix_list_t node_list;
    prte_node_t *node, *nd, *root_node;
    pmix_rank_t rank, entry, vpid_start, rank_base;
    int32_t num_slots;
    prte_rmaps_rank_file_map_t *rfmap;
    int32_t relative_index, tmp_cnt;
    int rc;
    prte_proc_t *proc;
    char *slots = NULL;
    /* see rmaps_rr.c: reset the per-node "mapped" flags only on the genuine
     * first mapping pass so per-app dispatch (one entry per app) does not
     * re-add nodes a previous app already placed in the job map */
    bool initial_map = (0 == jdata->map->num_nodes);
    char *rankfile = NULL;
    char *affinity_file = NULL;
    hwloc_cpuset_t proc_bitmap, bitmap;
    char *cpu_bitmap;
    char *avail_bitmap = NULL;
    char *overlap_bitmap = NULL;
    char *req_bitmap = NULL;
    bool physical;

    /* only handle initial launch of rf job */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s being restarted - rank_file cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    /* The policy is what selects the mapper. This reads the resolved options
     * rather than the job map: in per-app dispatch each app answers for
     * itself, and asking the job meant a per-app "--map-by rankfile" never
     * reached this mapper at all - the job's policy is whatever default was
     * resolved for the apps that gave no directive */
    if (PRTE_MAPPING_BYUSER != PRTE_GET_MAPPING_POLICY(options->map)) {
        /* NOT FOR US */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s not using rankfile policy",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (options->ordered) {
        /* NOT FOR US */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s binding order requested - rank_file cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    /* prefer per-app MAP_FILE if doing per-app dispatch for a single app */
    rankfile = NULL;
    if (options->app_idx >= 0) {
        prte_app_context_t *aptr = (prte_app_context_t *)
            pmix_pointer_array_get_item(jdata->apps, options->app_idx);
        if (NULL != aptr) {
            prte_get_attribute(&aptr->attributes, PRTE_APP_MAP_FILE,
                               (void **) &rankfile, PMIX_STRING);
        }
    }
    if (NULL == rankfile) {
        prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &rankfile, PMIX_STRING);
    }
    if (NULL == rankfile) {
        /* we cannot do it */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s no rankfile specified",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_BAD_PARAM;
    }

    physical = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_file: mapping job %s", PRTE_JOBID_PRINT(jdata->nspace));

    options->map = PRTE_MAPPING_BYUSER;

    /* setup the node list */
    PMIX_CONSTRUCT(&node_list, pmix_list_t);

    /* pickup the first app - there must be at least one. This returns
     * directly rather than through "error", which reclaims a rankmap that
     * has not been constructed yet */
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    if (NULL == app) {
        PMIX_LIST_DESTRUCT(&node_list);
        free(rankfile);
        return PRTE_ERR_SILENT;
    }

    /* start at the beginning... */
    vpid_start = 0;
    /* ...of the rankfile, which is not the same place as the beginning of
     * the job. In per-app dispatch the file this app named numbers that
     * app's ranks, and the global rank each one lands on depends on how
     * many procs the apps before it took - the base hands us that cursor.
     * In whole-job dispatch it is zero and the two coincide. */
    rank_base = (pmix_rank_t) options->start_vpid;
    /* Zero the job-wide count only when we were handed the whole job. The
     * per-app (MPMD) dispatch calls us once per app, and resetting here
     * would leave jdata->num_procs holding just the last app's count while
     * jdata->procs holds every app's procs - a mismatch the job packer and
     * unpacker disagree about, corrupting the launch message. */
    if (options->app_idx < 0) {
        jdata->num_procs = 0;
    }
    /* rankmap and num_ranks are module-static and are rebuilt for every map;
     * a stale count from a previous job in this DVM would be handed to an app
     * that gave no -n as its process count */
    num_ranks = 0;
    PMIX_CONSTRUCT(&rankmap, pmix_pointer_array_t);
    rc = pmix_pointer_array_init(&rankmap,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                 PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&rankmap);
        PMIX_LIST_DESTRUCT(&node_list);
        free(rankfile);
        return PRTE_ERROR;
    }

    /* parse the rankfile, storing its results in the rankmap */
    if (PRTE_SUCCESS != (rc = prte_rmaps_rank_file_parse(rankfile))) {
        rc = PRTE_ERR_SILENT;
        goto error;
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }
        if (options->app_idx >= 0 && (int)i != options->app_idx) {
            continue;
        }

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, jdata, app,
                                              options->map, initial_map, false, true);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* set the number of procs to the number of entries in that rankfile */
        if (PRTE_FLAG_TEST(app, PRTE_APP_FLAG_COMPUTED)) {
            app->num_procs = num_ranks;
            if (0 == app->num_procs) {
                prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
        }
        for (k = 0; k < app->num_procs; k++) {
            /* "entry" numbers the rankfile, "rank" numbers the job */
            entry = vpid_start + k;
            rank = rank_base + k;
            /* get the rankfile entry for this rank */
            rfmap = (prte_rmaps_rank_file_map_t *) pmix_pointer_array_get_item(&rankmap, entry);
            if (NULL == rfmap) {
                /* if this job was given a slot-list, then use it */
                if (NULL != options->cpuset) {
                    slots = options->cpuset;
                } else if (NULL != prte_hwloc_default_cpu_list) {
                    /* if we were give a default slot-list, then use it */
                    slots = prte_hwloc_default_cpu_list;
                } else {
                    /* all ranks must be specified */
                    prte_show_help("help-rmaps_rank_file.txt", "missing-rank", true, entry,
                                   rankfile);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                /* take the next node off of the available list */
                node = NULL;
                PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                {
                    /* if adding one to this node would oversubscribe it, then try
                     * the next one */
                    if (nd->slots <= (int) nd->num_procs) {
                        continue;
                    }
                    /* take this one */
                    node = nd;
                    break;
                }
                if (NULL == node) {
                    /* all would be oversubscribed, so take the least loaded
                     * one. Track the running minimum in its own variable: this
                     * used to borrow k, the loop counter carrying the rank we
                     * are placing, so a single unlisted rank on a full
                     * allocation rewrote the loop's position and the ranks it
                     * went on to assign */
                    pmix_rank_t least = UINT32_MAX;
                    PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                    {
                        if (nd->num_procs < least) {
                            least = nd->num_procs;
                            node = nd;
                        }
                    }
                }
                /* if we still have nothing, then something is very wrong */
                if (NULL == node) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto error;
                }
            } else {
                if (0 < strlen(rfmap->slot_list)) {
                    slots = rfmap->slot_list;
                } else {
                    slots = NULL;
                }
                /* find the node where this proc was assigned */
                node = NULL;
                PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                {
                    if (NULL != rfmap->node_name && prte_quickmatch(nd, rfmap->node_name) ){
                        node = nd;
                        break;
                    } else if (NULL != rfmap->node_name
                               && (('+' == rfmap->node_name[0])
                                   && (('n' == rfmap->node_name[1])
                                       || ('N' == rfmap->node_name[1])))) {

                        relative_index = atoi(strtok(rfmap->node_name, "+n"));
                        if (relative_index >= (int) pmix_list_get_size(&node_list)
                            || (0 > relative_index)) {
                            prte_show_help("help-rmaps_rank_file.txt", "bad-index", true,
                                           rfmap->node_name);
                            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                            rc = PRTE_ERR_BAD_PARAM;
                            goto error;
                        }
                        root_node = (prte_node_t *) pmix_list_get_first(&node_list);
                        for (tmp_cnt = 0; tmp_cnt < relative_index; tmp_cnt++) {
                            root_node = (prte_node_t *) pmix_list_get_next(root_node);
                        }
                        node = root_node;
                        break;
                    }
                }
            }
            if (NULL == node) {
                /* rfmap is NULL for a rank the file did not list, which the
                 * fallback above placed on a node of its own choosing */
                prte_show_help("help-rmaps_rank_file.txt", "bad-host", true,
                               (NULL == rfmap) ? "N/A" : rfmap->node_name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            if (!options->donotlaunch) {
                rc = prte_rmaps_base_check_support(jdata, node, options);
                if (PRTE_SUCCESS != rc) {
                    goto error;
                }
            }
            prte_rmaps_base_get_cpuset(jdata, node, options);
            if (NULL == options->job_cpuset) {
                // the prior function will have printed out the error
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            if (!prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, options)) {
                /* rfmap is NULL for a rank the file did not list, which the
                 * fallback above placed on a node of its own choosing */
                prte_show_help("help-rmaps_rank_file.txt", "bad-host", true,
                               (NULL == rfmap) ? "N/A" : rfmap->node_name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            /* check if we are oversubscribed. This runs after the proc has
             * been placed, as it does in every other mapper: it reads the
             * node's proc count, so asking before placement judged the node
             * one proc behind - and released a "proc" that had not been
             * created yet on the way out */
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_SUCCESS != rc &&
                PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                PMIX_RELEASE(proc);
                goto error;
            }
            /* set the vpid */
            proc->name.rank = rank;
            /* insert the proc into the proper place */
            PMIX_RETAIN(proc);
            rc = pmix_pointer_array_set_item(jdata->procs, proc->name.rank, proc);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_RELEASE(proc);
                goto error;
            }
            jdata->num_procs++;
            PMIX_RELEASE(proc);
            /* Assign this process to these slots
             * Allow for overload in cases where different ranks are assigned to
             * the same PU, but it must be requested by the user.
             */
            if (NULL != slots &&
                (PRTE_BIND_TO_NONE != PRTE_GET_BINDING_POLICY(jdata->map->binding) || options->overload) ) {
                if (NULL == node->topology || NULL == node->topology->topo) {
                    // Not allowed - for rank-file, we must have the topology
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true,
                                   node->name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                proc_bitmap = hwloc_bitmap_alloc();

                /* parse the slot_list to find the package and core */
                rc = prte_hwloc_base_cpu_list_parse(slots, node->topology->topo, options->use_hwthreads, proc_bitmap);
                if (PRTE_ERR_NOT_FOUND == rc) {
                    char *tmp = prte_hwloc_base_cset2str(hwloc_topology_get_allowed_cpuset(node->topology->topo),
                                                         false, physical, node->topology->topo);
                    prte_show_help("help-rmaps_rank_file.txt", "missing-cpu", true,
                                   prte_tool_basename, slots, tmp);
                    free(tmp);
                    rc = PRTE_ERR_SILENT;
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                } else if (PRTE_ERROR == rc) {
                    prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_SILENT;
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                } else if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                }
                /* note that we cannot set the proc locale to any specific object
                 * as the slot list may have assigned it to more than one - so
                 * leave that field NULL
                 */

                /* set the proc to the specified map */
                hwloc_bitmap_list_asprintf(&cpu_bitmap, proc_bitmap);
                proc->cpuset = strdup(cpu_bitmap);

                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rank_file: convert slots from <%s> to <%s>",
                                    slots, cpu_bitmap);

                /* Check to see if these slots are available on this node */
                if (!hwloc_bitmap_isincluded(proc_bitmap, node->available) && !options->overload) {
                    bitmap = hwloc_bitmap_alloc();
                    hwloc_bitmap_andnot(bitmap, proc_bitmap, node->available);

                    /* The user wrote the slot list in logical cpu ids, so
                     * every set we show back has to be in the same terms.
                     * proc->cpuset and a raw bitmap render are PU *OS*
                     * indices - the wire format - which is a different
                     * numbering on any node whose firmware does not number
                     * its cpus in hwloc's order. */
                    req_bitmap = prte_hwloc_base_cpuset2ranges(node->topology->topo, proc_bitmap,
                                                               options->use_hwthreads, false);
                    avail_bitmap = prte_hwloc_base_cpuset2ranges(node->topology->topo,
                                                                 node->available,
                                                                 options->use_hwthreads, false);
                    overlap_bitmap = prte_hwloc_base_cpuset2ranges(node->topology->topo, bitmap,
                                                                   options->use_hwthreads, false);

                    prte_show_help("help-rmaps_rank_file.txt", "rmaps:proc-slots-overloaded", true,
                                   PRTE_NAME_PRINT(&proc->name),
                                   node->name,
                                   (NULL == req_bitmap) ? "NONE" : req_bitmap,
                                   (NULL == avail_bitmap) ? "NONE" : avail_bitmap,
                                   (NULL == overlap_bitmap) ? "NONE" : overlap_bitmap);
                    /* these three were never released - the error label
                     * below does not know about them */
                    free(req_bitmap);
                    req_bitmap = NULL;
                    free(avail_bitmap);
                    avail_bitmap = NULL;
                    free(overlap_bitmap);
                    overlap_bitmap = NULL;

                    hwloc_bitmap_free(bitmap);
                    hwloc_bitmap_free(proc_bitmap);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto error;
                }

                /* Mark these slots as taken on this node */
                hwloc_bitmap_andnot(node->available, node->available, proc_bitmap);

                /* cleanup */
                free(cpu_bitmap);
                hwloc_bitmap_free(proc_bitmap);
            }
        }
        /* update the starting point */
        vpid_start += app->num_procs;
        rank_base += app->num_procs;
        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        PMIX_LIST_DESTRUCT(&node_list);
        PMIX_CONSTRUCT(&node_list, pmix_list_t);
    }
    PMIX_LIST_DESTRUCT(&node_list);

    /* cleanup the rankmap */
    for (i = 0; i < rankmap.size; i++) {
        if (NULL != (rfmap = pmix_pointer_array_get_item(&rankmap, i))) {
            PMIX_RELEASE(rfmap);
        }
    }
    PMIX_DESTRUCT(&rankmap);
    if (NULL != rankfile) {
        free(rankfile);
    }
    /* compute local/app ranks - in per-app dispatch mode (app_idx >= 0)
     * the base computes the ranks with the correct cross-app numbering,
     * so skip it here */
    if (options->app_idx < 0) {
        rc = prte_rmaps_base_compute_vpids(jdata, options, -1, NULL);
    }
    return rc;

error:
    PMIX_LIST_DESTRUCT(&node_list);
    /* the rankmap is module-static, so a map that failed part way through
     * has to hand back its entries here or they survive into the next job */
    for (i = 0; i < rankmap.size; i++) {
        if (NULL != (rfmap = pmix_pointer_array_get_item(&rankmap, i))) {
            PMIX_RELEASE(rfmap);
        }
    }
    PMIX_DESTRUCT(&rankmap);
    num_ranks = 0;
    if (NULL != rankfile) {
        free(rankfile);
    }

    return rc;
}

static int prte_rmaps_rank_file_parse(const char *rankfile)
{
    int token;
    int rc = PRTE_SUCCESS;
    int cnt;
    char *node_name = NULL;
    char **argv;
    char buff[RMAPS_RANK_FILE_MAX_SLOTS];
    char *value;
    int rank = -1;
    int i;
    prte_node_t *hnp_node;
    prte_rmaps_rank_file_map_t *rfmap = NULL;
    pmix_pointer_array_t *assigned_ranks_array;
    char tmp_rank_assignment[RMAPS_RANK_FILE_MAX_SLOTS];

    /* keep track of rank assignments */
    assigned_ranks_array = PMIX_NEW(pmix_pointer_array_t);
    rc = pmix_pointer_array_init(assigned_ranks_array,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                 PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(assigned_ranks_array);
        return PRTE_ERROR;
    }

    /* get the hnp node's info */
    hnp_node = (prte_node_t *) (prte_node_pool->addr[0]);

    prte_rmaps_rank_file_done = false;
    prte_rmaps_rank_file_in = fopen(rankfile, "r");

    if (NULL == prte_rmaps_rank_file_in) {
        prte_show_help("help-rmaps_rank_file.txt", "no-rankfile", true,
                       prte_tool_basename, rankfile, prte_tool_basename);
        rc = PRTE_ERR_NOT_FOUND;
        goto unlock;
    }

    while (!prte_rmaps_rank_file_done) {
        token = prte_rmaps_rank_file_lex();

        switch (token) {
        case PRTE_RANKFILE_ERROR:
            prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_QUOTED_STRING:
            prte_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true,
                           "QUOTED_STRING", rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_NEWLINE:
            rank = -1;
            if (NULL != node_name) {
                free(node_name);
            }
            node_name = NULL;
            rfmap = NULL;
            break;
        case PRTE_RANKFILE_RANK:
            token = prte_rmaps_rank_file_lex();
            if (PRTE_RANKFILE_INT == token) {
                rank = prte_rmaps_rank_file_value.ival;
                rfmap = PMIX_NEW(prte_rmaps_rank_file_map_t);
                pmix_pointer_array_set_item(&rankmap, rank, rfmap);
                num_ranks++; // keep track of number of provided ranks
            } else {
                prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            break;
        case PRTE_RANKFILE_USERNAME:
            prte_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true, "USERNAME",
                           rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_EQUAL:
            if (rank < 0) {
                prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            token = prte_rmaps_rank_file_lex();
            switch (token) {
            case PRTE_RANKFILE_HOSTNAME:
            case PRTE_RANKFILE_IPV4:
            case PRTE_RANKFILE_IPV6:
            case PRTE_RANKFILE_STRING:
            case PRTE_RANKFILE_INT:
            case PRTE_RANKFILE_RELATIVE:
                if (PRTE_RANKFILE_INT == token) {
                    snprintf(buff,RMAPS_RANK_FILE_MAX_SLOTS,  "%d", prte_rmaps_rank_file_value.ival);
                    value = buff;
                } else {
                    value = prte_rmaps_rank_file_value.sval;
                }
                argv = PMIx_Argv_split(value, '@');
                cnt = PMIx_Argv_count(argv);
                if (NULL != node_name) {
                    free(node_name);
                }
                if (1 == cnt) {
                    node_name = strdup(argv[0]);
                } else if (2 == cnt) {
                    node_name = strdup(argv[1]);
                } else {
                    prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    PMIx_Argv_free(argv);
                    node_name = NULL;
                    goto unlock;
                }
                PMIx_Argv_free(argv);

                // Strip off the FQDN if present, ignore IP addresses
                if (!prte_keep_fqdn_hostnames && !pmix_net_isaddr(node_name)) {
                    char *ptr;
                    if (NULL != (ptr = strchr(node_name, '.'))) {
                        *ptr = '\0';
                    }
                }

                /* check the rank item */
                if (NULL == rfmap) {
                    prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    goto unlock;
                }
                /* check if this is the local node */
                if (prte_check_host_is_local(node_name)) {
                    rfmap->node_name = strdup(hnp_node->name);
                } else {
                    rfmap->node_name = strdup(node_name);
                }
            }
            break;
        case PRTE_RANKFILE_SLOT:
            if (NULL == node_name || rank < 0
                || NULL == (value = prte_rmaps_rank_file_parse_string_or_int())) {
                prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }

            /* check for a duplicate rank assignment. The record has to be
             * filed under the rank it describes, and it has to be a copy:
             * indexing every record at 0 (and pointing them all at one reused
             * stack buffer) meant a rankfile whose first "slot=" line was for
             * any rank but 0 rejected its own rank 0 line as a duplicate, and
             * a genuine duplicate of any other rank went unnoticed */
            if (NULL != pmix_pointer_array_get_item(assigned_ranks_array, rank)) {
                prte_show_help("help-rmaps_rank_file.txt", "bad-assign", true, rank,
                               pmix_pointer_array_get_item(assigned_ranks_array, rank), rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                free(value);
                goto unlock;
            } else {
                /* prepare rank assignment string for the help message in case of a bad-assign */
                snprintf(tmp_rank_assignment, RMAPS_RANK_FILE_MAX_SLOTS, "%s slot=%s", node_name, value);
                pmix_pointer_array_set_item(assigned_ranks_array, rank,
                                            strdup(tmp_rank_assignment));
            }

            /* check the rank item */
            if (NULL == rfmap) {
                prte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                free(value);
                goto unlock;
            }
            for (i = 0; i < RMAPS_RANK_FILE_MAX_SLOTS && '\0' != value[i]; i++) {
                rfmap->slot_list[i] = value[i];
            }
            free(value);
            break;
        }
    }
unlock:
    /* every exit has to give the file and the lexer back - the error paths
     * used to jump straight past the close, leaking a descriptor and the
     * lexer's buffers for every malformed rankfile */
    if (NULL != prte_rmaps_rank_file_in) {
        fclose(prte_rmaps_rank_file_in);
        prte_rmaps_rank_file_in = NULL;
        prte_rmaps_rank_file_lex_destroy();
    }
    if (NULL != node_name) {
        free(node_name);
    }
    for (i = 0; i < assigned_ranks_array->size; i++) {
        value = (char *) pmix_pointer_array_get_item(assigned_ranks_array, i);
        if (NULL != value) {
            free(value);
        }
    }
    PMIX_RELEASE(assigned_ranks_array);
    return rc;
}

static char *prte_rmaps_rank_file_parse_string_or_int(void)
{
    int rc;
    char tmp_str[RMAPS_RANK_FILE_MAX_SLOTS];

    if (PRTE_RANKFILE_EQUAL != prte_rmaps_rank_file_lex()) {
        return NULL;
    }

    rc = prte_rmaps_rank_file_lex();
    switch (rc) {
    case PRTE_RANKFILE_STRING:
        return strdup(prte_rmaps_rank_file_value.sval);
    case PRTE_RANKFILE_INT:
        snprintf(tmp_str, RMAPS_RANK_FILE_MAX_SLOTS, "%d", prte_rmaps_rank_file_value.ival);
        return strdup(tmp_str);
    default:
        return NULL;
    }
}
