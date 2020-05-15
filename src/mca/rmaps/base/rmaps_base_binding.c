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
 * Copyright (c) 2011-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Inria.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/if.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/threads/tsd.h"

#include "types.h"
#include "src/util/show_help.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"

static bool membind_warned=false;

/* CRITICAL NOTE: the hwloc topology tree is in a shared memory
 * region that is passed to the applications for their use. HWLOC
 * does NOT provide any locking support in this shmem region. Thus,
 * it is critical that the topology tree information itself remain
 * unmodified.
 *
 * We can, however, fiddle with the userdata attached to an object
 * in the topology tree because the applications that might also
 * be attached to the shared memory region don't have visibility
 * into the userdata. They also cannot conflict with us as they
 * cannot write into the shared memory region. So we leave the
 * topology itself untouched (critical!) and confine ourselves
 * to recording usage etc in the userdata object */


static void reset_usage(prrte_node_t *node, prrte_jobid_t jobid)
{
    int j;
    prrte_proc_t *proc;
    prrte_hwloc_obj_data_t *data=NULL;
    hwloc_obj_t bound;

    prrte_output_verbose(10, prrte_rmaps_base_framework.framework_output,
                        "%s reset_usage: node %s has %d procs on it",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        node->name, node->num_procs);

    /* start by clearing any existing proc binding
     * records from the userdata in this topo */
    prrte_hwloc_base_clear_usage(node->topology->topo);

    /* cycle thru the procs on the node and record
     * their usage in the topology
     */
    for (j=0; j < node->procs->size; j++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
            continue;
        }
        /* ignore procs from this job */
        if (proc->name.jobid == jobid) {
            prrte_output_verbose(10, prrte_rmaps_base_framework.framework_output,
                                "%s reset_usage: ignoring proc %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&proc->name));
            continue;
        }
        bound = NULL;
        /* get the object to which this proc is bound */
        if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_BOUND, (void**)&bound, PRRTE_PTR) ||
            NULL == bound) {
            /* this proc isn't bound - ignore it */
            prrte_output_verbose(10, prrte_rmaps_base_framework.framework_output,
                                "%s reset_usage: proc %s has no bind location",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&proc->name));
            continue;
        }
        /* get the userdata struct for this object - create it if necessary */
        data = (prrte_hwloc_obj_data_t*)bound->userdata;
        if (NULL == data) {
            data = PRRTE_NEW(prrte_hwloc_obj_data_t);
            bound->userdata = data;
        }
        /* count that this proc is bound to this object */
        data->num_bound++;
        prrte_output_verbose(10, prrte_rmaps_base_framework.framework_output,
                            "%s reset_usage: proc %s is bound - total %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&proc->name), data->num_bound);
    }
}

static void unbind_procs(prrte_job_t *jdata)
{
    int j;
    prrte_proc_t *proc;

    for (j=0; j < jdata->procs->size; j++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, j))) {
            continue;
        }
        prrte_remove_attribute(&proc->attributes, PRRTE_PROC_HWLOC_BOUND);
        prrte_remove_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP);
    }
}

static int bind_generic(prrte_job_t *jdata,
                        prrte_node_t *node,
                        int target_depth)
{
    int j;
    prrte_job_map_t *map;
    prrte_proc_t *proc;
    hwloc_obj_t trg_obj, tmp_obj, nxt_obj;
    unsigned int ncpus;
    prrte_hwloc_obj_data_t *data;
    int total_cpus, cpus_per_rank;
    hwloc_cpuset_t totalcpuset, available, mycpus;
    hwloc_obj_t locale;
    char *cpu_bitmap, *job_cpuset;
    unsigned min_bound;
    bool dobind, use_hwthread_cpus;
    struct hwloc_topology_support *support;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *rdata;
    uint16_t u16, *u16ptr = &u16;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind downward for job %s with bindings %s",
                        PRRTE_JOBID_PRINT(jdata->jobid),
                        prrte_hwloc_base_print_binding(jdata->map->binding));
    /* initialize */
    map = jdata->map;
    totalcpuset = hwloc_bitmap_alloc();

    dobind = false;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        dobind = true;
    }
    /* reset usage */
    reset_usage(node, jdata->jobid);

    /* get the available processors on this node */
    root = hwloc_get_root_obj(node->topology->topo);
    if (NULL == root->userdata) {
        /* incorrect */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    rdata = (prrte_hwloc_topo_data_t*)root->userdata;
    available = hwloc_bitmap_dup(rdata->available);

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

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CPUSET, (void**)&job_cpuset, PRRTE_STRING) &&
        NULL != job_cpuset) {
        mycpus = prrte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus, job_cpuset);
        hwloc_bitmap_and(available, mycpus, available);
        hwloc_bitmap_free(mycpus);
    }

    /* cycle thru the procs */
    for (j=0; j < node->procs->size; j++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
            continue;
        }
        /* ignore procs from other jobs */
        if (proc->name.jobid != jdata->jobid) {
            continue;
        }
        if ((int)PRRTE_PROC_MY_NAME->vpid != node->index && !dobind) {
            continue;
        }

        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support*)hwloc_topology_get_support(node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind &&
                !support->cpubind->set_thisthread_cpubind) {
                if (!PRRTE_BINDING_REQUIRED(map->binding) ||
                    !PRRTE_BINDING_POLICY_IS_SET(map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:cpubind-not-supported", true, node->name);
                hwloc_bitmap_free(totalcpuset);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind &&
                !support->membind->set_thisthread_membind &&
                PRRTE_BINDING_POLICY_IS_SET(map->binding)) {
                if (PRRTE_HWLOC_BASE_MBFA_WARN == prrte_hwloc_base_mbfa && !membind_warned) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported", true, node->name);
                    membind_warned = true;
                } else if (PRRTE_HWLOC_BASE_MBFA_ERROR == prrte_hwloc_base_mbfa) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported-fatal", true, node->name);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_SILENT;
                }
            }
        }

        /* bozo check */
        locale = NULL;
        if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR) ||
            NULL == locale) {
            prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-locale", true, PRRTE_NAME_PRINT(&proc->name));
            hwloc_bitmap_free(totalcpuset);
            hwloc_bitmap_free(available);
            if (NULL != job_cpuset) {
                free(job_cpuset);
            }
            return PRRTE_ERR_SILENT;
        }

        /* use the min_bound object that intersects locale->cpuset at target_depth */
        tmp_obj = NULL;
        trg_obj = NULL;
        min_bound = UINT_MAX;
        while (NULL != (tmp_obj = hwloc_get_next_obj_by_depth(node->topology->topo, target_depth, tmp_obj))) {
            if (!hwloc_bitmap_intersects(locale->cpuset, tmp_obj->cpuset))
                continue;

            /* if there are no available cpus under this object, then ignore it */
            if (!hwloc_bitmap_intersects(available, tmp_obj->cpuset))
                continue;

            data = (prrte_hwloc_obj_data_t*)tmp_obj->userdata;
            if (NULL == data) {
                data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                tmp_obj->userdata = data;
            }
            if (data->num_bound < min_bound) {
                min_bound = data->num_bound;
                trg_obj = tmp_obj;
            }
        }
        if (NULL == trg_obj) {
            /* there aren't any such targets under this object */
            prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-available-cpus", true, node->name);
            hwloc_bitmap_free(totalcpuset);
            hwloc_bitmap_free(available);
            if (NULL != job_cpuset) {
                free(job_cpuset);
            }
            return PRRTE_ERR_SILENT;
        }
        /* record the location */
        prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_BOUND, PRRTE_ATTR_LOCAL, trg_obj, PRRTE_PTR);

        /* start with a clean slate */
        hwloc_bitmap_zero(totalcpuset);
        total_cpus = 0;
        nxt_obj = trg_obj;
        do {
            if (NULL == nxt_obj) {
                /* could not find enough cpus to meet request */
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-available-cpus", true, node->name);
                hwloc_bitmap_free(totalcpuset);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            trg_obj = nxt_obj;
            /* get the number of available cpus under this location */
            ncpus = prrte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                              available, trg_obj);
            /* track the number bound */
            if (NULL == (data = (prrte_hwloc_obj_data_t*)trg_obj->userdata)) {
                data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                trg_obj->userdata = data;
            }
            data->num_bound++;
            /* error out if adding a proc would cause overload and that wasn't allowed,
             * and it wasn't a default binding policy (i.e., the user requested it)
             */
            if (ncpus < data->num_bound &&
                !PRRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding)) {
                if (PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                    /* if the user specified a binding policy, then we cannot meet
                     * it since overload isn't allowed, so error out - have the
                     * message indicate that setting overload allowed will remove
                     * this restriction */
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:binding-overload", true,
                                   prrte_hwloc_base_print_binding(map->binding), node->name,
                                   data->num_bound, ncpus);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_SILENT;
                } else if (1 < cpus_per_rank) {
                    /* if the user specified cpus/proc, then we weren't able
                     * to meet that request - this constitutes an error that
                     * must be reported */
                    prrte_show_help("help-prrte-rmaps-base.txt", "insufficient-cpus-per-proc", true,
                                   prrte_hwloc_base_print_binding(map->binding), node->name,
                                   (NULL != job_cpuset) ? job_cpuset : (NULL == prrte_hwloc_default_cpu_list) ? "FULL" : prrte_hwloc_default_cpu_list,
                                   cpus_per_rank);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_SILENT;
                } else {
                    /* if we have the default binding policy, then just don't bind */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "%s NOT ENOUGH CPUS TO COMPLETE BINDING - BINDING NOT REQUIRED, REVERTING TO NOT BINDING",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                    PRRTE_SET_BINDING_POLICY(map->binding, PRRTE_BIND_TO_NONE);
                    unbind_procs(jdata);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_SUCCESS;
                }
            }
            /* bind the proc here */
            hwloc_bitmap_or(totalcpuset, totalcpuset, trg_obj->cpuset);
            /* track total #cpus */
            total_cpus += ncpus;
            /* move to the next location, in case we need it */
            nxt_obj = trg_obj->next_cousin;
        } while (total_cpus < cpus_per_rank);
        hwloc_bitmap_list_asprintf(&cpu_bitmap, totalcpuset);
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "%s PROC %s BITMAP %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&proc->name), cpu_bitmap);
        prrte_set_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, PRRTE_ATTR_GLOBAL, cpu_bitmap, PRRTE_STRING);
        if (NULL != cpu_bitmap) {
            free(cpu_bitmap);
        }
        if (4 < prrte_output_get_verbosity(prrte_rmaps_base_framework.framework_output)) {
            char *tmp1;
            tmp1 = prrte_hwloc_base_cset2str(totalcpuset, use_hwthread_cpus, node->topology->topo);
            prrte_output(prrte_rmaps_base_framework.framework_output,
                        "%s BOUND PROC %s[%s] TO %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&proc->name), node->name, tmp1);
            free(tmp1);
        }
    }
    hwloc_bitmap_free(totalcpuset);
    hwloc_bitmap_free(available);
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRRTE_SUCCESS;
}

static int bind_in_place(prrte_job_t *jdata,
                         hwloc_obj_type_t target,
                         unsigned cache_level)
{
    /* traverse the hwloc topology tree on each node downwards
     * until we find an unused object of type target - and then bind
     * the process to that target
     */
    int i, j;
    prrte_job_map_t *map;
    prrte_node_t *node;
    prrte_proc_t *proc;
    unsigned int idx, ncpus;
    struct hwloc_topology_support *support;
    prrte_hwloc_obj_data_t *data;
    hwloc_obj_t locale, sib;
    char *cpu_bitmap, *job_cpuset;
    bool found, use_hwthread_cpus;
    bool dobind;
    int cpus_per_rank;
    hwloc_cpuset_t available, mycpus;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *rdata;
    uint16_t u16, *u16ptr = &u16;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind in place for job %s with bindings %s",
                        PRRTE_JOBID_PRINT(jdata->jobid),
                        prrte_hwloc_base_print_binding(jdata->map->binding));
    /* initialize */
    map = jdata->map;

    dobind = false;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        dobind = true;
    }

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

    for (i=0; i < map->nodes->size; i++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }
        if ((int)PRRTE_PROC_MY_NAME->vpid != node->index && !dobind) {
            continue;
        }
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support*)hwloc_topology_get_support(node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind &&
                !support->cpubind->set_thisthread_cpubind) {
                if (!PRRTE_BINDING_REQUIRED(map->binding) ||
                    !PRRTE_BINDING_POLICY_IS_SET(map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:cpubind-not-supported", true, node->name);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind &&
                !support->membind->set_thisthread_membind &&
                PRRTE_BINDING_POLICY_IS_SET(map->binding)) {
                if (PRRTE_HWLOC_BASE_MBFA_WARN == prrte_hwloc_base_mbfa && !membind_warned) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported", true, node->name);
                    membind_warned = true;
                } else if (PRRTE_HWLOC_BASE_MBFA_ERROR == prrte_hwloc_base_mbfa) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported-fatal", true, node->name);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRRTE_ERR_SILENT;
                }
            }
        }

        /* some systems do not report cores, and so we can get a situation where our
         * default binding policy will fail for no necessary reason. So if we are
         * computing a binding due to our default policy, and no cores are found
         * on this node, just silently skip it - we will not bind
         */
        if (!PRRTE_BINDING_POLICY_IS_SET(map->binding) &&
            HWLOC_TYPE_DEPTH_UNKNOWN == hwloc_get_type_depth(node->topology->topo, HWLOC_OBJ_CORE)) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "Unable to bind-to core by default on node %s as no cores detected",
                                node->name);
            continue;
        }

        /* we share topologies in order
         * to save space, so we need to reset the usage info to reflect
         * our own current state
         */
        reset_usage(node, jdata->jobid);
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
            mycpus = prrte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus, job_cpuset);
            hwloc_bitmap_and(available, mycpus, available);
            hwloc_bitmap_free(mycpus);
        }

        /* cycle thru the procs */
        for (j=0; j < node->procs->size; j++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (proc->name.jobid != jdata->jobid) {
                continue;
            }
            /* bozo check */
            locale = NULL;
            if (!prrte_get_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE, (void**)&locale, PRRTE_PTR)) {
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-locale", true, PRRTE_NAME_PRINT(&proc->name));
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            /* get the index of this location */
            if (UINT_MAX == (idx = prrte_hwloc_base_get_obj_idx(node->topology->topo, locale, PRRTE_HWLOC_AVAILABLE))) {
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            /* get the number of cpus under this location */
            if (0 == (ncpus = prrte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                        available, locale))) {
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-available-cpus", true, node->name);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRRTE_ERR_SILENT;
            }
            data = (prrte_hwloc_obj_data_t*)locale->userdata;
            if (NULL == data) {
                data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                locale->userdata = data;
            }
            /* if we don't have enough cpus to support this additional proc, try
             * shifting the location to a cousin that can support it - the important
             * thing is that we maintain the same level in the topology */
            if (ncpus < (data->num_bound+cpus_per_rank)) {
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "%s bind_in_place: searching right",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                sib = locale;
                found = false;
                while (NULL != (sib = sib->next_cousin)) {
                    ncpus = prrte_hwloc_base_get_npus(node->topology->topo,
                                                      use_hwthread_cpus,
                                                      available, sib);
                    data = (prrte_hwloc_obj_data_t*)sib->userdata;
                    if (NULL == data) {
                        data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                        sib->userdata = data;
                    }
                    if ((data->num_bound+cpus_per_rank) <= ncpus) {
                        found = true;
                        locale = sib;
                        break;
                    }
                }
                if (!found) {
                    /* try the other direction */
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "%s bind_in_place: searching left",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                    sib = locale;
                    while (NULL != (sib = sib->prev_cousin)) {
                        ncpus = prrte_hwloc_base_get_npus(node->topology->topo,
                                                          use_hwthread_cpus,
                                                          available, sib);
                        data = (prrte_hwloc_obj_data_t*)sib->userdata;
                        if (NULL == data) {
                            data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                            sib->userdata = data;
                        }
                        if ((data->num_bound+cpus_per_rank) <= ncpus) {
                            found = true;
                            locale = sib;
                            break;
                        }
                    }
                }
                if (!found) {
                    /* no place to put this - see if overload is allowed */
                    if (!PRRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding)) {
                        if (PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                            /* if the user specified a binding policy, then we cannot meet
                             * it since overload isn't allowed, so error out - have the
                             * message indicate that setting overload allowed will remove
                             * this restriction */
                            prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:binding-overload", true,
                                           prrte_hwloc_base_print_binding(map->binding), node->name,
                                           data->num_bound, ncpus);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                            return PRRTE_ERR_SILENT;
                        } else if (1 < cpus_per_rank) {
                            /* if the user specified cpus/proc, then we weren't able
                             * to meet that request - this constitutes an error that
                             * must be reported */
                            prrte_show_help("help-prrte-rmaps-base.txt", "insufficient-cpus-per-proc", true,
                                           prrte_hwloc_base_print_binding(map->binding), node->name,
                                           (NULL != job_cpuset) ? job_cpuset : (NULL == prrte_hwloc_default_cpu_list) ? "FULL" : prrte_hwloc_default_cpu_list,
                                           cpus_per_rank);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                            return PRRTE_ERR_SILENT;
                        } else {
                            /* if we have the default binding policy, then just don't bind */
                            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                                "%s NOT ENOUGH CPUS TO COMPLETE BINDING - BINDING NOT REQUIRED, REVERTING TO NOT BINDING",
                                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                            unbind_procs(jdata);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                           return PRRTE_SUCCESS;
                        }
                    }
                }
            }
            /* track the number bound */
            data = (prrte_hwloc_obj_data_t*)locale->userdata;  // just in case it changed
            if (NULL == data) {
                data = PRRTE_NEW(prrte_hwloc_obj_data_t);
                locale->userdata = data;
            }
            data->num_bound++;
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "BINDING PROC %s TO %s NUMBER %u",
                                PRRTE_NAME_PRINT(&proc->name),
                                hwloc_obj_type_string(locale->type), idx);
            /* bind the proc here, masking it to any "soft" cgroup the user provided */
            mycpus = hwloc_bitmap_alloc();
            hwloc_bitmap_and(mycpus, available, locale->cpuset);
            hwloc_bitmap_list_asprintf(&cpu_bitmap, mycpus);
            hwloc_bitmap_free(mycpus);
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, PRRTE_ATTR_GLOBAL, cpu_bitmap, PRRTE_STRING);
            /* update the location, in case it changed */
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_BOUND, PRRTE_ATTR_LOCAL, locale, PRRTE_PTR);
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "%s BOUND PROC %s TO %s[%s:%u] on node %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&proc->name),
                                cpu_bitmap, hwloc_obj_type_string(locale->type),
                                idx, node->name);
            if (NULL != cpu_bitmap) {
                free(cpu_bitmap);
            }
        }
        hwloc_bitmap_free(available);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRRTE_SUCCESS;
}

static int bind_to_cpuset(prrte_job_t *jdata)
{
    /* bind each process to prrte_hwloc_base_cpu_list */
    int i, j;
    prrte_job_map_t *map;
    prrte_node_t *node;
    prrte_proc_t *proc;
    struct hwloc_topology_support *support;
    prrte_hwloc_topo_data_t *sum;
    hwloc_obj_t root;
    char *cpu_bitmap, *job_cpuset;
    unsigned id;
    prrte_local_rank_t lrank;
    hwloc_bitmap_t mycpuset, tset, mycpus;
    bool dobind, use_hwthread_cpus;
    uint16_t u16, *u16ptr = &u16, ncpus, cpus_per_rank;

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CPUSET, (void**)&job_cpuset, PRRTE_STRING) ||
        NULL == job_cpuset) {
        return PRRTE_ERR_BAD_PARAM;
    }

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind job %s to cpus %s",
                        PRRTE_JOBID_PRINT(jdata->jobid), job_cpuset);

    /* initialize */
    map = jdata->map;
    mycpuset = hwloc_bitmap_alloc();

    dobind = false;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        dobind = true;
    }


    /* see if they want multiple cpus/rank */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_PES_PER_PROC, (void**)&u16ptr, PRRTE_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* see if they want are using hwthreads as cpus */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_HWT_CPUS, NULL, PRRTE_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    for (i=0; i < map->nodes->size; i++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }
        if ((int)PRRTE_PROC_MY_NAME->vpid != node->index && !dobind) {
            continue;
        }
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support*)hwloc_topology_get_support(node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind &&
                !support->cpubind->set_thisthread_cpubind) {
                if (!PRRTE_BINDING_REQUIRED(jdata->map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:cpubind-not-supported", true, node->name);
                free(job_cpuset);
                hwloc_bitmap_free(mycpuset);
                return PRRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability
             */
            if (!support->membind->set_thisproc_membind &&
                !support->membind->set_thisthread_membind) {
                if (PRRTE_HWLOC_BASE_MBFA_WARN == prrte_hwloc_base_mbfa && !membind_warned) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported", true, node->name);
                    membind_warned = true;
                } else if (PRRTE_HWLOC_BASE_MBFA_ERROR == prrte_hwloc_base_mbfa) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported-fatal", true, node->name);
                    free(job_cpuset);
                    hwloc_bitmap_free(mycpuset);
                    return PRRTE_ERR_SILENT;
                }
            }
        }
        root = hwloc_get_root_obj(node->topology->topo);
        if (NULL == root->userdata) {
            /* something went wrong */
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            free(job_cpuset);
            hwloc_bitmap_free(mycpuset);
            return PRRTE_ERR_NOT_FOUND;
        }
        sum = (prrte_hwloc_topo_data_t*)root->userdata;
        if (NULL == sum->available) {
            /* another error */
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            free(job_cpuset);
            hwloc_bitmap_free(mycpuset);
            return PRRTE_ERR_NOT_FOUND;
        }
        reset_usage(node, jdata->jobid);
        hwloc_bitmap_zero(mycpuset);

        /* filter the node-available cpus against the specified "soft" cgroup */
        mycpus = prrte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus, job_cpuset);
        hwloc_bitmap_and(mycpus, mycpus, sum->available);

        for (j=0; j < node->procs->size; j++) {
            if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (proc->name.jobid != jdata->jobid) {
                continue;
            }
            if (PRRTE_BIND_ORDERED_REQUESTED(jdata->map->binding)) {
                /* assign each proc, in local rank order, to
                 * the corresponding cpu in the list */
                id = hwloc_bitmap_first(mycpus);
                lrank = 0;
                while (lrank != proc->local_rank) {
                    ncpus = 0;
                    while ((unsigned)-1 != id && ncpus < cpus_per_rank) {
                        id = hwloc_bitmap_next(mycpus, id);
                        /* set the bit of interest */
                        hwloc_bitmap_only(mycpuset, id);
                        ++ncpus;
                    }
                    if ((unsigned)-1 == id) {
                        break;
                    }
                    ++lrank;
                }
                if ((unsigned)-1 ==id) {
                    /* ran out of cpus - that's an error */
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:insufficient-cpus", true,
                                   node->name, (int)proc->local_rank, job_cpuset);
                    free(job_cpuset);
                    hwloc_bitmap_free(mycpuset);
                    hwloc_bitmap_free(mycpus);
                    return PRRTE_ERR_OUT_OF_RESOURCE;
                }
                tset = mycpuset;
            } else {
                /* bind the proc to all assigned cpus */
                tset = mycpus;
            }
            hwloc_bitmap_list_asprintf(&cpu_bitmap, tset);
            prrte_set_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, PRRTE_ATTR_GLOBAL, cpu_bitmap, PRRTE_STRING);
            if (NULL != cpu_bitmap) {
                free(cpu_bitmap);
            }
            hwloc_bitmap_free(mycpus);
        }
    }
    hwloc_bitmap_free(mycpuset);
    free(job_cpuset);
    return PRRTE_SUCCESS;
}

int prrte_rmaps_base_compute_bindings(prrte_job_t *jdata)
{
    hwloc_obj_type_t hwb;
    unsigned clvl=0;
    prrte_binding_policy_t bind;
    prrte_mapping_policy_t map;
    prrte_node_t *node;
    int i, rc;
    struct hwloc_topology_support *support;
    int bind_depth;
    bool dobind;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: compute bindings for job %s with policy %s[%x]",
                        PRRTE_JOBID_PRINT(jdata->jobid),
                        prrte_hwloc_base_print_binding(jdata->map->binding), jdata->map->binding);

    map = PRRTE_GET_MAPPING_POLICY(jdata->map->mapping);
    bind = PRRTE_GET_BINDING_POLICY(jdata->map->binding);

    if (PRRTE_MAPPING_BYUSER == map) {
        /* user specified binding by rankfile - nothing for us to do */
        return PRRTE_SUCCESS;
    }

    if (PRRTE_BIND_TO_NONE == bind) {
        rc = PRRTE_SUCCESS;
        if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_CPUSET, NULL, PRRTE_STRING)) {
            /* "soft" cgroup was given but no other
             * binding directive was provided, so bind
             * to those specific cpus */
            if (PRRTE_SUCCESS != (rc = bind_to_cpuset(jdata))) {
                PRRTE_ERROR_LOG(rc);
            }
        }
        return rc;
    }

    /* binding requested - convert the binding level to the hwloc obj type */
    switch (bind) {
    case PRRTE_BIND_TO_PACKAGE:
        hwb = HWLOC_OBJ_PACKAGE;
        break;
    case PRRTE_BIND_TO_L3CACHE:
        PRRTE_HWLOC_MAKE_OBJ_CACHE(3, hwb, clvl);
        break;
    case PRRTE_BIND_TO_L2CACHE:
        PRRTE_HWLOC_MAKE_OBJ_CACHE(2, hwb, clvl);
        break;
    case PRRTE_BIND_TO_L1CACHE:
        PRRTE_HWLOC_MAKE_OBJ_CACHE(1, hwb, clvl);
        break;
    case PRRTE_BIND_TO_CORE:
        hwb = HWLOC_OBJ_CORE;
        break;
    case PRRTE_BIND_TO_HWTHREAD:
        hwb = HWLOC_OBJ_PU;
        break;
    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if the job was mapped by the corresponding target, then
     * we bind in place
     *
     * otherwise, we have to bind either up or down the hwloc
     * tree. If we are binding upwards (e.g., mapped to hwthread
     * but binding to core), then we just climb the tree to find
     * the first matching object.
     *
     * if we are binding downwards (e.g., mapped to node and bind
     * to core), then we have to do a round-robin assigment of
     * procs to the resources below.
     */

    if (PRRTE_MAPPING_BYDIST == map) {
        /* bind every proc downwards */
        goto execute;
    }

    /* now deal with the remaining binding policies based on hardware */
    if (bind == map) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps: bindings for job %s - bind in place",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        if (PRRTE_SUCCESS != (rc = bind_in_place(jdata, hwb, clvl))) {
            PRRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    /* we need to handle the remaining binding options on a per-node
     * basis because different nodes could potentially have different
     * topologies, with different relative depths for the two levels
     */
  execute:
    /* initialize */
    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: computing bindings for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    dobind = false;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PRRTE_BOOL) ||
        prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DISPLAY_DIFF, NULL, PRRTE_BOOL)) {
        dobind = true;
    }

    for (i=0; i < jdata->map->nodes->size; i++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, i))) {
            continue;
        }
        if ((int)PRRTE_PROC_MY_NAME->vpid != node->index && !dobind) {
            continue;
        }
        if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_DO_NOT_LAUNCH, NULL, PRRTE_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support*)hwloc_topology_get_support(node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind &&
                !support->cpubind->set_thisthread_cpubind) {
                if (!PRRTE_BINDING_REQUIRED(jdata->map->binding) ||
                    !PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:cpubind-not-supported", true, node->name);
                return PRRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind &&
                !support->membind->set_thisthread_membind &&
                PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                if (PRRTE_HWLOC_BASE_MBFA_WARN == prrte_hwloc_base_mbfa && !membind_warned) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported", true, node->name);
                    membind_warned = true;
                } else if (PRRTE_HWLOC_BASE_MBFA_ERROR == prrte_hwloc_base_mbfa) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:membind-not-supported-fatal", true, node->name);
                    return PRRTE_ERR_SILENT;
                }
            }
        }

        /* some systems do not report cores, and so we can get a situation where our
         * default binding policy will fail for no necessary reason. So if we are
         * computing a binding due to our default policy, and no cores are found
         * on this node, just silently skip it - we will not bind
         */
        if (!PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding) &&
            HWLOC_TYPE_DEPTH_UNKNOWN == hwloc_get_type_depth(node->topology->topo, HWLOC_OBJ_CORE)) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "Unable to bind-to core by default on node %s as no cores detected",
                                node->name);
            continue;
        }

        /* determine the relative depth on this node */
#if HWLOC_API_VERSION < 0x20000
        if (HWLOC_OBJ_CACHE == hwb) {
            /* must use a unique function because blasted hwloc
             * just doesn't deal with caches very well...sigh
             */
            bind_depth = hwloc_get_cache_type_depth(node->topology->topo, clvl, (hwloc_obj_cache_type_t)-1);
        } else
#endif
            bind_depth = hwloc_get_type_depth(node->topology->topo, hwb);
#if HWLOC_API_VERSION < 0x20000
        if (0 > bind_depth)
#else
        if (0 > bind_depth && HWLOC_TYPE_DEPTH_NUMANODE != bind_depth)
#endif
        {
            /* didn't find such an object */
            prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:no-objects",
                           true, hwloc_obj_type_string(hwb), node->name);
            return PRRTE_ERR_SILENT;
        }
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "%s bind_depth: %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            bind_depth);
        if (PRRTE_SUCCESS != (rc = bind_generic(jdata, node, bind_depth))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRRTE_SUCCESS;
}
