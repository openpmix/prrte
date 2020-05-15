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
 * Copyright (c) 2009-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/util/output.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_rr.h"

int prrte_rmaps_rr_assign_root_level(prrte_job_t *jdata)
{
    int i, m;
    prrte_node_t *node;
    prrte_proc_t *proc;
    hwloc_obj_t obj=NULL;

    prrte_output_verbose(2, prrte_rmaps_base_framework.framework_output,

                        "mca:rmaps:rr: assigning procs to root level for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    for (m=0; m < jdata->map->nodes->size; m++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
            continue;
        }
        prrte_output_verbose(2, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rr:slot working node %s",
                            node->name);
        /* get the root object as we are not assigning
         * locale here except at the node level */
        if (NULL == node->topology || NULL == node->topology->topo) {
            /* nothing we can do */
            continue;
        }
        obj = hwloc_get_root_obj(node->topology->topo);
        for (i=0; i < node->procs->size; i++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (proc->name.jobid != jdata->jobid) {
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr:assign skipping proc %s - from another job",
                                    PRRTE_NAME_PRINT(&proc->name));
                continue;
            }
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
        }
    }
    return PRRTE_SUCCESS;
}

/* mapping by hwloc object looks a lot like mapping by node,
 * but has the added complication of possibly having different
 * numbers of objects on each node
 */
int prrte_rmaps_rr_assign_byobj(prrte_job_t *jdata,
                               hwloc_obj_type_t target,
                               unsigned cache_level)
{
    int start, j, m, n, npus, cpus_per_rank;
    prrte_app_context_t *app;
    prrte_node_t *node;
    prrte_proc_t *proc;
    hwloc_obj_t obj=NULL, root;
    unsigned int nobjs;
    uint16_t u16, *u16ptr = &u16;
    char *job_cpuset;
    prrte_hwloc_topo_data_t *rdata;
    hwloc_cpuset_t available, mycpus;
    bool use_hwthread_cpus;

    prrte_output_verbose(2, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rr: assigning locations by %s for job %s",
                        hwloc_obj_type_string(target),
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CPUSET, (void**)&job_cpuset, PRRTE_STRING);

    /* see if they want multiple cpus/rank */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_PES_PER_PROC, (void**)&u16ptr, PRRTE_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* check for type of cpu being used */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_HWT_CPUS, NULL, PRRTE_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* start mapping procs onto objects, filling each object as we go until
     * all procs are mapped. If one pass doesn't catch all the required procs,
     * then loop thru the list again to handle the oversubscription
     */
    for (n=0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        for (m=0; m < jdata->map->nodes->size; m++) {
            if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, m))) {
                continue;
            }
            if (NULL == node->topology || NULL == node->topology->topo) {
                prrte_show_help("help-prrte-rmaps-ppr.txt", "ppr-topo-missing",
                               true, node->name);
                return PRRTE_ERR_SILENT;
            }
            /* get the number of objects of this type on this node */
            nobjs = prrte_hwloc_base_get_nbobjs_by_type(node->topology->topo, target, cache_level, PRRTE_HWLOC_AVAILABLE);
            if (0 == nobjs) {
                continue;
            }

            /* get the available processors on this node */
            root = hwloc_get_root_obj(node->topology->topo);
            if (NULL == root->userdata) {
                /* incorrect */
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_BAD_PARAM;
            }
            rdata = (prrte_hwloc_topo_data_t*)root->userdata;
            available = hwloc_bitmap_dup(rdata->available);
            if (NULL != job_cpuset) {
                /* deal with any "soft" cgroup specification */
                mycpus = prrte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus, job_cpuset);
                hwloc_bitmap_and(available, mycpus, available);
                hwloc_bitmap_free(mycpus);
            }

            prrte_output_verbose(2, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:rr: found %u %s objects on node %s",
                                nobjs, hwloc_obj_type_string(target), node->name);

            /* if this is a comm_spawn situation, start with the object
             * where the parent left off and increment */
            if (PRRTE_JOBID_INVALID != jdata->originator.jobid &&
                UINT_MAX != jdata->bkmark_obj) {
                start = (jdata->bkmark_obj + 1) % nobjs;
            } else {
                start = 0;
            }
            /* loop over the procs on this node */
            for (j=0; j < node->procs->size; j++) {
                if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                /* ignore procs from other jobs */
                if (proc->name.jobid != jdata->jobid) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:rr:assign skipping proc %s - from another job",
                                        PRRTE_NAME_PRINT(&proc->name));
                    continue;
                }
                /* ignore procs from other apps */
                if (proc->app_idx != app->idx) {
                    continue;
                }

                prrte_output_verbose(20, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rr: assigning proc to object %d", (j + start) % nobjs);
                /* get the hwloc object */
                if (NULL == (obj = prrte_hwloc_base_get_obj_by_type(node->topology->topo, target, cache_level, (j + start) % nobjs, PRRTE_HWLOC_AVAILABLE))) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_NOT_FOUND;
                }
                npus = prrte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                 available, obj);
                if (cpus_per_rank > npus) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "mapping-too-low", true,
                                   cpus_per_rank, npus,
                                   prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_SILENT;
                }
                prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, PRRTE_ATTR_LOCAL, obj, PRRTE_PTR);
                /* track the bookmark */
                jdata->bkmark_obj = (j + start) % nobjs;
            }
            hwloc_bitmap_free(available);
        }
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }
    return PRRTE_SUCCESS;
}
