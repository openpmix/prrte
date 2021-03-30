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
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "rmaps_rr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

int prte_rmaps_rr_assign_root_level(prte_job_t *jdata)
{
    int i, m;
    prte_node_t *node;
    prte_proc_t *proc;
    hwloc_obj_t obj = NULL;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: assigning procs to root level for job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    for (m = 0; m < jdata->map->nodes->size; m++) {
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
            continue;
        }
        prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s", node->name);
        /* get the root object as we are not assigning
         * locale here except at the node level */
        if (NULL == node->topology || NULL == node->topology->topo) {
            /* nothing we can do */
            continue;
        }
        obj = hwloc_get_root_obj(node->topology->topo);
        for (i = 0; i < node->procs->size; i++) {
            if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr:assign skipping proc %s - from another job",
                                    PRTE_NAME_PRINT(&proc->name));
                continue;
            }
            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                               PMIX_POINTER);
        }
    }
    return PRTE_SUCCESS;
}

/* mapping by hwloc object looks a lot like mapping by node,
 * but has the added complication of possibly having different
 * numbers of objects on each node
 */
int prte_rmaps_rr_assign_byobj(prte_job_t *jdata, hwloc_obj_type_t target, unsigned cache_level)
{
    int start, j, m, n, k, npus, cpus_per_rank;
    prte_app_context_t *app;
    prte_node_t *node;
    prte_proc_t *proc;
    hwloc_obj_t obj = NULL, root;
    unsigned int nobjs;
    uint16_t u16, *u16ptr = &u16;
    char *job_cpuset;
    prte_hwloc_topo_data_t *rdata;
    hwloc_cpuset_t available, mycpus;
    bool use_hwthread_cpus;

    prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: assigning locations by %s for job %s",
                        hwloc_obj_type_string(target), PRTE_JOBID_PRINT(jdata->nspace));

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* see if they want multiple cpus/rank */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr,
                           PMIX_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* start mapping procs onto objects, filling each object as we go until
     * all procs are mapped. If one pass doesn't catch all the required procs,
     * then loop thru the list again to handle the oversubscription
     */
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        for (m = 0; m < jdata->map->nodes->size; m++) {
            if (NULL
                == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            if (NULL == node->topology || NULL == node->topology->topo) {
                prte_show_help("help-prte-rmaps-ppr.txt", "ppr-topo-missing", true, node->name);
                return PRTE_ERR_SILENT;
            }
            /* get the number of objects of this type on this node */
            nobjs = prte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target, cache_level);
            if (0 == nobjs) {
                prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr: found NO %s objects on node %s",
                                    hwloc_obj_type_string(target), node->name);
                continue;
            }

            /* get the available processors on this node */
            root = hwloc_get_root_obj(node->topology->topo);
            if (NULL == root->userdata) {
                /* incorrect */
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_BAD_PARAM;
            }
            rdata = (prte_hwloc_topo_data_t *) root->userdata;
            available = hwloc_bitmap_dup(rdata->available);
            if (NULL != job_cpuset) {
                /* deal with any "soft" cgroup specification */
                mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                         job_cpuset);
                hwloc_bitmap_and(available, mycpus, available);
                hwloc_bitmap_free(mycpus);
            }

            prte_output_verbose(2, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: found %u %s objects on node %s", nobjs,
                                hwloc_obj_type_string(target), node->name);

            /* if this is a comm_spawn situation, start with the object
             * where the parent left off and increment */
            if (!PMIX_NSPACE_INVALID(jdata->originator.nspace) && UINT_MAX != jdata->bkmark_obj) {
                start = (jdata->bkmark_obj + 1) % nobjs;
            } else {
                start = 0;
            }
            /* loop over the procs on this node */
            for (j = 0; j < node->procs->size; j++) {
                if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                /* ignore procs from other jobs */
                if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rr:assign skipping proc %s - from another job",
                                        PRTE_NAME_PRINT(&proc->name));
                    continue;
                }
                /* ignore procs from other apps */
                if (proc->app_idx != app->idx) {
                    continue;
                }

                /* Search for resource which has at least enough members for
                 * request. Seach fails if we wrap back to our starting index
                 * without finding a satisfactory resource. */
                k = start;
                do {
                    /* get the hwloc object */
                    if (NULL
                        == (obj = prte_hwloc_base_get_obj_by_type(node->topology->topo, target,
                                                                  cache_level, k))) {
                        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                        hwloc_bitmap_free(available);
                        if (NULL != job_cpuset) {
                            free(job_cpuset);
                        }
                        return PRTE_ERR_NOT_FOUND;
                    }
                    npus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                    available, obj);
                    if (npus >= cpus_per_rank) {
                        break;
                    }
                    k = (k + 1) % nobjs;
                } while (k != start);
                /* Fail if loop exits without finding an adequate resource */
                if (cpus_per_rank > npus) {
                    prte_show_help("help-prte-rmaps-base.txt", "mapping-too-low", true,
                                   cpus_per_rank, npus,
                                   prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_ERR_SILENT;
                }
                prte_output_verbose(20, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr: assigning proc to object %d", k);
                prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL, obj,
                                   PMIX_POINTER);
                /* Position at next sequential resource for next search */
                start = (k + 1) % nobjs;
                /* track the bookmark */
                jdata->bkmark_obj = start;
            }
            hwloc_bitmap_free(available);
        }
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }
    return PRTE_SUCCESS;
}
