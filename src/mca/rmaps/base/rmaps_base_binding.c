/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Inria.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/threads/tsd.h"
#include "src/util/if.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "types.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static bool membind_warned = false;

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

static void reset_usage(prte_node_t *node, pmix_nspace_t jobid)
{
    int j;
    prte_proc_t *proc;
    prte_hwloc_obj_data_t *data = NULL;
    hwloc_obj_t bound;

    prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                        "%s reset_usage: node %s has %d procs on it",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->num_procs);

    /* start by clearing any existing proc binding
     * records from the userdata in this topo */
    prte_hwloc_base_clear_usage(node->topology->topo);

    /* cycle thru the procs on the node and record
     * their usage in the topology
     */
    for (j = 0; j < node->procs->size; j++) {
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
            continue;
        }
        /* ignore procs from this job */
        if (PMIX_CHECK_NSPACE(proc->name.nspace, jobid)) {
            prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                "%s reset_usage: ignoring proc %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name));
            continue;
        }
        bound = NULL;
        /* get the object to which this proc is bound */
        if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_BOUND, (void **) &bound,
                                PMIX_POINTER)
            || NULL == bound) {
            /* this proc isn't bound - ignore it */
            prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                                "%s reset_usage: proc %s has no bind location",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name));
            continue;
        }
        /* get the userdata struct for this object - create it if necessary */
        data = (prte_hwloc_obj_data_t *) bound->userdata;
        if (NULL == data) {
            data = PRTE_NEW(prte_hwloc_obj_data_t);
            bound->userdata = data;
        }
        /* count that this proc is bound to this object */
        data->num_bound++;
        prte_output_verbose(10, prte_rmaps_base_framework.framework_output,
                            "%s reset_usage: proc %s is bound - total %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                            data->num_bound);
    }
}

static void unbind_procs(prte_job_t *jdata)
{
    int j;
    prte_proc_t *proc;

    for (j = 0; j < jdata->procs->size; j++) {
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(jdata->procs, j))) {
            continue;
        }
        prte_remove_attribute(&proc->attributes, PRTE_PROC_HWLOC_BOUND);
        prte_remove_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP);
    }
}

static int bind_generic(prte_job_t *jdata, prte_node_t *node, int target_depth)
{
    int j;
    prte_job_map_t *map;
    prte_proc_t *proc;
    hwloc_obj_t trg_obj, tmp_obj, nxt_obj;
    unsigned int ncpus;
    prte_hwloc_obj_data_t *data;
    int total_cpus, cpus_per_rank;
    hwloc_cpuset_t totalcpuset, available, mycpus;
    hwloc_obj_t locale;
    char *cpu_bitmap, *job_cpuset;
    unsigned min_bound;
    bool dobind, use_hwthread_cpus;
    struct hwloc_topology_support *support;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *rdata;
    uint16_t u16, *u16ptr = &u16;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind downward for job %s with bindings %s",
                        PRTE_JOBID_PRINT(jdata->nspace),
                        prte_hwloc_base_print_binding(jdata->map->binding));
    /* initialize */
    map = jdata->map;
    totalcpuset = hwloc_bitmap_alloc();

    dobind = false;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        dobind = true;
    }
    /* reset usage */
    reset_usage(node, jdata->nspace);

    /* get the available processors on this node */
    root = hwloc_get_root_obj(node->topology->topo);
    if (NULL == root->userdata) {
        /* incorrect */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    rdata = (prte_hwloc_topo_data_t *) root->userdata;
    available = hwloc_bitmap_dup(rdata->available);

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

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING)
        && NULL != job_cpuset) {
        mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                 job_cpuset);
        hwloc_bitmap_and(available, mycpus, available);
        hwloc_bitmap_free(mycpus);
    }

    /* cycle thru the procs */
    for (j = 0; j < node->procs->size; j++) {
        if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
            continue;
        }
        /* ignore procs from other jobs */
        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
            continue;
        }
        if ((int) PRTE_PROC_MY_NAME->rank != node->index && !dobind) {
            continue;
        }

        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(
                node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind
                && !support->cpubind->set_thisthread_cpubind) {
                if (!PRTE_BINDING_REQUIRED(map->binding)
                    || !PRTE_BINDING_POLICY_IS_SET(map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:cpubind-not-supported", true,
                               node->name);
                hwloc_bitmap_free(totalcpuset);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind && !support->membind->set_thisthread_membind
                && PRTE_BINDING_POLICY_IS_SET(map->binding)) {
                if (PRTE_HWLOC_BASE_MBFA_WARN == prte_hwloc_base_mbfa && !membind_warned) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported", true,
                                   node->name);
                    membind_warned = true;
                } else if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported-fatal",
                                   true, node->name);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_ERR_SILENT;
                }
            }
        }

        /* bozo check */
        locale = NULL;
        if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, (void **) &locale,
                                PMIX_POINTER)
            || NULL == locale) {
            prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-locale", true,
                           PRTE_NAME_PRINT(&proc->name));
            hwloc_bitmap_free(totalcpuset);
            hwloc_bitmap_free(available);
            if (NULL != job_cpuset) {
                free(job_cpuset);
            }
            return PRTE_ERR_SILENT;
        }

        /* use the min_bound object that intersects locale->cpuset at target_depth */
        tmp_obj = NULL;
        trg_obj = NULL;
        min_bound = UINT_MAX;
        while (NULL
               != (tmp_obj = hwloc_get_next_obj_by_depth(node->topology->topo, target_depth,
                                                         tmp_obj))) {
            if (!hwloc_bitmap_intersects(locale->cpuset, tmp_obj->cpuset))
                continue;

            /* if there are no available cpus under this object, then ignore it */
            if (!hwloc_bitmap_intersects(available, tmp_obj->cpuset))
                continue;

            data = (prte_hwloc_obj_data_t *) tmp_obj->userdata;
            if (NULL == data) {
                data = PRTE_NEW(prte_hwloc_obj_data_t);
                tmp_obj->userdata = data;
            }
            if (data->num_bound < min_bound) {
                min_bound = data->num_bound;
                trg_obj = tmp_obj;
            }
        }
        if (NULL == trg_obj) {
            /* there aren't any such targets under this object */
            prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-available-cpus", true, node->name);
            hwloc_bitmap_free(totalcpuset);
            hwloc_bitmap_free(available);
            if (NULL != job_cpuset) {
                free(job_cpuset);
            }
            return PRTE_ERR_SILENT;
        }
        /* record the location */
        prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_BOUND, PRTE_ATTR_LOCAL, trg_obj,
                           PMIX_POINTER);

        /* start with a clean slate */
        hwloc_bitmap_zero(totalcpuset);
        total_cpus = 0;
        nxt_obj = trg_obj;
        do {
            if (NULL == nxt_obj) {
                /* could not find enough cpus to meet request */
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-available-cpus", true,
                               node->name);
                hwloc_bitmap_free(totalcpuset);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            trg_obj = nxt_obj;
            /* get the number of available cpus under this location */
            ncpus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus, available,
                                             trg_obj);
            /* track the number bound */
            if (NULL == (data = (prte_hwloc_obj_data_t *) trg_obj->userdata)) {
                data = PRTE_NEW(prte_hwloc_obj_data_t);
                trg_obj->userdata = data;
            }
            data->num_bound++;
            /* error out if adding a proc would cause overload and that wasn't allowed,
             * and it wasn't a default binding policy (i.e., the user requested it)
             */
            if (ncpus < data->num_bound && !PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding)) {
                if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                    /* if the user specified a binding policy, then we cannot meet
                     * it since overload isn't allowed, so error out - have the
                     * message indicate that setting overload allowed will remove
                     * this restriction */
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:binding-overload", true,
                                   prte_hwloc_base_print_binding(map->binding), node->name,
                                   data->num_bound, ncpus);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_ERR_SILENT;
                } else if (1 < cpus_per_rank) {
                    /* if the user specified cpus/proc, then we weren't able
                     * to meet that request - this constitutes an error that
                     * must be reported */
                    prte_show_help("help-prte-rmaps-base.txt", "insufficient-cpus-per-proc", true,
                                   prte_hwloc_base_print_binding(map->binding), node->name,
                                   (NULL != job_cpuset) ? job_cpuset
                                   : (NULL == prte_hwloc_default_cpu_list)
                                       ? "FULL"
                                       : prte_hwloc_default_cpu_list,
                                   cpus_per_rank);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_ERR_SILENT;
                } else {
                    /* if we have the default binding policy, then just don't bind */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "%s NOT ENOUGH CPUS TO COMPLETE BINDING - BINDING NOT "
                                        "REQUIRED, REVERTING TO NOT BINDING",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    PRTE_SET_BINDING_POLICY(map->binding, PRTE_BIND_TO_NONE);
                    unbind_procs(jdata);
                    hwloc_bitmap_free(totalcpuset);
                    hwloc_bitmap_free(available);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_SUCCESS;
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
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output, "%s PROC %s BITMAP %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                            cpu_bitmap);
        prte_set_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP, PRTE_ATTR_GLOBAL, cpu_bitmap,
                           PMIX_STRING);
        if (NULL != cpu_bitmap) {
            free(cpu_bitmap);
        }
        if (4 < prte_output_get_verbosity(prte_rmaps_base_framework.framework_output)) {
            char *tmp1;
            tmp1 = prte_hwloc_base_cset2str(totalcpuset, use_hwthread_cpus, node->topology->topo);
            prte_output(prte_rmaps_base_framework.framework_output, "%s BOUND PROC %s[%s] TO %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                        node->name, tmp1);
            free(tmp1);
        }
    }
    hwloc_bitmap_free(totalcpuset);
    hwloc_bitmap_free(available);
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRTE_SUCCESS;
}

static int bind_in_place(prte_job_t *jdata, hwloc_obj_type_t target, unsigned cache_level)
{
    /* traverse the hwloc topology tree on each node downwards
     * until we find an unused object of type target - and then bind
     * the process to that target
     */
    int i, j;
    prte_job_map_t *map;
    prte_node_t *node;
    prte_proc_t *proc;
    unsigned int idx, ncpus;
    struct hwloc_topology_support *support;
    prte_hwloc_obj_data_t *data;
    hwloc_obj_t locale, sib;
    char *cpu_bitmap, *job_cpuset;
    bool found, use_hwthread_cpus;
    bool dobind;
    int cpus_per_rank;
    hwloc_cpuset_t available, mycpus;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *rdata;
    uint16_t u16, *u16ptr = &u16;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind in place for job %s with bindings %s",
                        PRTE_JOBID_PRINT(jdata->nspace),
                        prte_hwloc_base_print_binding(jdata->map->binding));
    /* initialize */
    map = jdata->map;

    dobind = false;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        dobind = true;
    }

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

    for (i = 0; i < map->nodes->size; i++) {
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }
        if ((int) PRTE_PROC_MY_NAME->rank != node->index && !dobind) {
            continue;
        }
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(
                node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind
                && !support->cpubind->set_thisthread_cpubind) {
                if (!PRTE_BINDING_REQUIRED(map->binding)
                    || !PRTE_BINDING_POLICY_IS_SET(map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:cpubind-not-supported", true,
                               node->name);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind && !support->membind->set_thisthread_membind
                && PRTE_BINDING_POLICY_IS_SET(map->binding)) {
                if (PRTE_HWLOC_BASE_MBFA_WARN == prte_hwloc_base_mbfa && !membind_warned) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported", true,
                                   node->name);
                    membind_warned = true;
                } else if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported-fatal",
                                   true, node->name);
                    if (NULL != job_cpuset) {
                        free(job_cpuset);
                    }
                    return PRTE_ERR_SILENT;
                }
            }
        }

        /* some systems do not report cores, and so we can get a situation where our
         * default binding policy will fail for no necessary reason. So if we are
         * computing a binding due to our default policy, and no cores are found
         * on this node, just silently skip it - we will not bind
         */
        if (!PRTE_BINDING_POLICY_IS_SET(map->binding)
            && HWLOC_TYPE_DEPTH_UNKNOWN
                   == hwloc_get_type_depth(node->topology->topo, HWLOC_OBJ_CORE)) {
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "Unable to bind-to core by default on node %s as no cores detected",
                                node->name);
            continue;
        }

        /* we share topologies in order
         * to save space, so we need to reset the usage info to reflect
         * our own current state
         */
        reset_usage(node, jdata->nspace);
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
            mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                     job_cpuset);
            hwloc_bitmap_and(available, mycpus, available);
            hwloc_bitmap_free(mycpus);
        }
        /* cycle thru the procs */
        for (j = 0; j < node->procs->size; j++) {
            if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                continue;
            }
            /* bozo check */
            locale = NULL;
            if (!prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, (void **) &locale,
                                    PMIX_POINTER)) {
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-locale", true,
                               PRTE_NAME_PRINT(&proc->name));
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            /* get the index of this location */
            if (UINT_MAX == (idx = prte_hwloc_base_get_obj_idx(node->topology->topo, locale))) {
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            /* get the number of cpus under this location */
            if (0
                == (ncpus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                     available, locale))) {
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-available-cpus", true,
                               node->name);
                hwloc_bitmap_free(available);
                if (NULL != job_cpuset) {
                    free(job_cpuset);
                }
                return PRTE_ERR_SILENT;
            }
            data = (prte_hwloc_obj_data_t *) locale->userdata;
            if (NULL == data) {
                data = PRTE_NEW(prte_hwloc_obj_data_t);
                locale->userdata = data;
            }
            /* if we don't have enough cpus to support this additional proc, try
             * shifting the location to a cousin that can support it - the important
             * thing is that we maintain the same level in the topology */
            if (ncpus < (data->num_bound + cpus_per_rank)) {
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "%s bind_in_place: searching right",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                sib = locale;
                found = false;
                while (NULL != (sib = sib->next_cousin)) {
                    ncpus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                     available, sib);
                    data = (prte_hwloc_obj_data_t *) sib->userdata;
                    if (NULL == data) {
                        data = PRTE_NEW(prte_hwloc_obj_data_t);
                        sib->userdata = data;
                    }
                    if ((data->num_bound + cpus_per_rank) <= ncpus) {
                        found = true;
                        locale = sib;
                        break;
                    }
                }
                if (!found) {
                    /* try the other direction */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "%s bind_in_place: searching left",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    sib = locale;
                    while (NULL != (sib = sib->prev_cousin)) {
                        ncpus = prte_hwloc_base_get_npus(node->topology->topo, use_hwthread_cpus,
                                                         available, sib);
                        data = (prte_hwloc_obj_data_t *) sib->userdata;
                        if (NULL == data) {
                            data = PRTE_NEW(prte_hwloc_obj_data_t);
                            sib->userdata = data;
                        }
                        if ((data->num_bound + cpus_per_rank) <= ncpus) {
                            found = true;
                            locale = sib;
                            break;
                        }
                    }
                }
                if (!found) {
                    /* no place to put this - see if overload is allowed */
                    if (!PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding)) {
                        if (PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                            /* if the user specified a binding policy, then we cannot meet
                             * it since overload isn't allowed, so error out - have the
                             * message indicate that setting overload allowed will remove
                             * this restriction */
                            prte_show_help("help-prte-rmaps-base.txt", "rmaps:binding-overload",
                                           true, prte_hwloc_base_print_binding(map->binding),
                                           node->name, data->num_bound, ncpus);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                            return PRTE_ERR_SILENT;
                        } else if (1 < cpus_per_rank) {
                            /* if the user specified cpus/proc, then we weren't able
                             * to meet that request - this constitutes an error that
                             * must be reported */
                            prte_show_help("help-prte-rmaps-base.txt", "insufficient-cpus-per-proc",
                                           true, prte_hwloc_base_print_binding(map->binding),
                                           node->name,
                                           (NULL != job_cpuset) ? job_cpuset
                                           : (NULL == prte_hwloc_default_cpu_list)
                                               ? "FULL"
                                               : prte_hwloc_default_cpu_list,
                                           cpus_per_rank);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                            return PRTE_ERR_SILENT;
                        } else {
                            /* if we have the default binding policy, then just don't bind */
                            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "%s NOT ENOUGH CPUS TO COMPLETE BINDING - BINDING "
                                                "NOT REQUIRED, REVERTING TO NOT BINDING",
                                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                            unbind_procs(jdata);
                            hwloc_bitmap_free(available);
                            if (NULL != job_cpuset) {
                                free(job_cpuset);
                            }
                            return PRTE_SUCCESS;
                        }
                    }
                }
            }
            /* track the number bound */
            data = (prte_hwloc_obj_data_t *) locale->userdata; // just in case it changed
            if (NULL == data) {
                data = PRTE_NEW(prte_hwloc_obj_data_t);
                locale->userdata = data;
            }
            data->num_bound++;
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "BINDING PROC %s TO %s NUMBER %u", PRTE_NAME_PRINT(&proc->name),
                                hwloc_obj_type_string(locale->type), idx);
            /* bind the proc here, masking it to any "soft" cgroup the user provided */
            mycpus = hwloc_bitmap_alloc();
            hwloc_bitmap_and(mycpus, available, locale->cpuset);
            hwloc_bitmap_list_asprintf(&cpu_bitmap, mycpus);
            hwloc_bitmap_free(mycpus);
            prte_set_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP, PRTE_ATTR_GLOBAL,
                               cpu_bitmap, PMIX_STRING);
            /* update the location, in case it changed */
            prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_BOUND, PRTE_ATTR_LOCAL, locale,
                               PMIX_POINTER);
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "%s BOUND PROC %s TO %s[%s:%u] on node %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                                cpu_bitmap, hwloc_obj_type_string(locale->type), idx, node->name);
            if (NULL != cpu_bitmap) {
                free(cpu_bitmap);
            }
        }
        hwloc_bitmap_free(available);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }

    return PRTE_SUCCESS;
}

static int bind_to_cpuset(prte_job_t *jdata)
{
    /* bind each process to prte_hwloc_base_cpu_list */
    int i, j;
    prte_job_map_t *map;
    prte_node_t *node;
    prte_proc_t *proc;
    struct hwloc_topology_support *support;
    prte_hwloc_topo_data_t *sum;
    hwloc_obj_t root;
    char *cpu_bitmap, *job_cpuset;
    unsigned id;
    prte_local_rank_t lrank;
    hwloc_bitmap_t mycpuset, tset, mycpus;
    bool dobind, use_hwthread_cpus;
    uint16_t u16, *u16ptr = &u16, ncpus, cpus_per_rank;

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING)
        || NULL == job_cpuset) {
        return PRTE_ERR_BAD_PARAM;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: bind job %s to cpus %s", PRTE_JOBID_PRINT(jdata->nspace),
                        job_cpuset);

    /* initialize */
    map = jdata->map;
    mycpuset = hwloc_bitmap_alloc();

    dobind = false;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        dobind = true;
    }

    /* see if they want multiple cpus/rank */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void **) &u16ptr,
                           PMIX_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }

    /* see if they want are using hwthreads as cpus */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    for (i = 0; i < map->nodes->size; i++) {
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, i))) {
            continue;
        }
        if ((int) PRTE_PROC_MY_NAME->rank != node->index && !dobind) {
            continue;
        }
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(
                node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind
                && !support->cpubind->set_thisthread_cpubind) {
                if (!PRTE_BINDING_REQUIRED(jdata->map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:cpubind-not-supported", true,
                               node->name);
                free(job_cpuset);
                hwloc_bitmap_free(mycpuset);
                return PRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability
             */
            if (!support->membind->set_thisproc_membind
                && !support->membind->set_thisthread_membind) {
                if (PRTE_HWLOC_BASE_MBFA_WARN == prte_hwloc_base_mbfa && !membind_warned) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported", true,
                                   node->name);
                    membind_warned = true;
                } else if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported-fatal",
                                   true, node->name);
                    free(job_cpuset);
                    hwloc_bitmap_free(mycpuset);
                    return PRTE_ERR_SILENT;
                }
            }
        }
        root = hwloc_get_root_obj(node->topology->topo);
        if (NULL == root->userdata) {
            /* something went wrong */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            free(job_cpuset);
            hwloc_bitmap_free(mycpuset);
            return PRTE_ERR_NOT_FOUND;
        }
        sum = (prte_hwloc_topo_data_t *) root->userdata;
        if (NULL == sum->available) {
            /* another error */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            free(job_cpuset);
            hwloc_bitmap_free(mycpuset);
            return PRTE_ERR_NOT_FOUND;
        }
        reset_usage(node, jdata->nspace);
        hwloc_bitmap_zero(mycpuset);

        /* filter the node-available cpus against the specified "soft" cgroup */
        mycpus = prte_hwloc_base_generate_cpuset(node->topology->topo, use_hwthread_cpus,
                                                 job_cpuset);
        hwloc_bitmap_and(mycpus, mycpus, sum->available);

        for (j = 0; j < node->procs->size; j++) {
            if (NULL == (proc = (prte_proc_t *) prte_pointer_array_get_item(node->procs, j))) {
                continue;
            }
            /* ignore procs from other jobs */
            if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                continue;
            }
            if (PRTE_BIND_ORDERED_REQUESTED(jdata->map->binding)) {
                /* assign each proc, in local rank order, to
                 * the corresponding cpu in the list */
                id = hwloc_bitmap_first(mycpus);
                lrank = 0;
                while (lrank != proc->local_rank) {
                    ncpus = 0;
                    while ((unsigned) -1 != id && ncpus < cpus_per_rank) {
                        id = hwloc_bitmap_next(mycpus, id);
                        /* set the bit of interest */
                        hwloc_bitmap_only(mycpuset, id);
                        ++ncpus;
                    }
                    if ((unsigned) -1 == id) {
                        break;
                    }
                    ++lrank;
                }
                if ((unsigned) -1 == id) {
                    /* ran out of cpus - that's an error */
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:insufficient-cpus", true,
                                   node->name, (int) proc->local_rank, job_cpuset);
                    free(job_cpuset);
                    hwloc_bitmap_free(mycpuset);
                    hwloc_bitmap_free(mycpus);
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                tset = mycpuset;
            } else {
                /* bind the proc to all assigned cpus */
                tset = mycpus;
            }
            hwloc_bitmap_list_asprintf(&cpu_bitmap, tset);
            prte_set_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP, PRTE_ATTR_GLOBAL,
                               cpu_bitmap, PMIX_STRING);
            if (NULL != cpu_bitmap) {
                free(cpu_bitmap);
            }
            hwloc_bitmap_free(mycpus);
        }
    }
    hwloc_bitmap_free(mycpuset);
    free(job_cpuset);
    return PRTE_SUCCESS;
}

int prte_rmaps_base_compute_bindings(prte_job_t *jdata)
{
    hwloc_obj_type_t hwb;
    unsigned clvl = 0;
    prte_binding_policy_t bind;
    prte_mapping_policy_t map;
    prte_node_t *node;
    int i, rc;
    struct hwloc_topology_support *support;
    int bind_depth;
    bool dobind;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: compute bindings for job %s with policy %s[%x]",
                        PRTE_JOBID_PRINT(jdata->nspace),
                        prte_hwloc_base_print_binding(jdata->map->binding), jdata->map->binding);

    map = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
    bind = PRTE_GET_BINDING_POLICY(jdata->map->binding);

    if (PRTE_MAPPING_BYUSER == map) {
        /* user specified binding by rankfile - nothing for us to do */
        return PRTE_SUCCESS;
    }

    if (PRTE_BIND_TO_NONE == bind) {
        rc = PRTE_SUCCESS;
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, NULL, PMIX_STRING)) {
            /* "soft" cgroup was given but no other
             * binding directive was provided, so bind
             * to those specific cpus */
            if (PRTE_SUCCESS != (rc = bind_to_cpuset(jdata))) {
                PRTE_ERROR_LOG(rc);
            }
        }
        return rc;
    }

    /* binding requested - convert the binding level to the hwloc obj type */
    switch (bind) {
    case PRTE_BIND_TO_PACKAGE:
        hwb = HWLOC_OBJ_PACKAGE;
        break;
    case PRTE_BIND_TO_L3CACHE:
        PRTE_HWLOC_MAKE_OBJ_CACHE(3, hwb, clvl);
        break;
    case PRTE_BIND_TO_L2CACHE:
        PRTE_HWLOC_MAKE_OBJ_CACHE(2, hwb, clvl);
        break;
    case PRTE_BIND_TO_L1CACHE:
        PRTE_HWLOC_MAKE_OBJ_CACHE(1, hwb, clvl);
        break;
    case PRTE_BIND_TO_CORE:
        hwb = HWLOC_OBJ_CORE;
        break;
    case PRTE_BIND_TO_HWTHREAD:
        hwb = HWLOC_OBJ_PU;
        break;
    default:
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
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

    if (PRTE_MAPPING_BYDIST == map) {
        /* bind every proc downwards */
        goto execute;
    }

    /* now deal with the remaining binding policies based on hardware */
    if (bind == map) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps: bindings for job %s - bind in place",
                            PRTE_JOBID_PRINT(jdata->nspace));
        if (PRTE_SUCCESS != (rc = bind_in_place(jdata, hwb, clvl))) {
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    /* we need to handle the remaining binding options on a per-node
     * basis because different nodes could potentially have different
     * topologies, with different relative depths for the two levels
     */
execute:
    /* initialize */
    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: computing bindings for job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    dobind = false;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL)
        || prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL)) {
        dobind = true;
    }

    for (i = 0; i < jdata->map->nodes->size; i++) {
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(jdata->map->nodes, i))) {
            continue;
        }
        if ((int) PRTE_PROC_MY_NAME->rank != node->index && !dobind) {
            continue;
        }
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
            /* if we don't want to launch, then we are just testing the system,
             * so ignore questions about support capabilities
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(
                node->topology->topo);
            /* check if topology supports cpubind - have to be careful here
             * as Linux doesn't currently support thread-level binding. This
             * may change in the future, though, and it isn't clear how hwloc
             * interprets the current behavior. So check both flags to be sure.
             */
            if (!support->cpubind->set_thisproc_cpubind
                && !support->cpubind->set_thisthread_cpubind) {
                if (!PRTE_BINDING_REQUIRED(jdata->map->binding)
                    || !PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                    /* we are not required to bind, so ignore this */
                    continue;
                }
                prte_show_help("help-prte-rmaps-base.txt", "rmaps:cpubind-not-supported", true,
                               node->name);
                return PRTE_ERR_SILENT;
            }
            /* check if topology supports membind - have to be careful here
             * as hwloc treats this differently than I (at least) would have
             * expected. Per hwloc, Linux memory binding is at the thread,
             * and not process, level. Thus, hwloc sets the "thisproc" flag
             * to "false" on all Linux systems, and uses the "thisthread" flag
             * to indicate binding capability - don't warn if the user didn't
             * specifically request binding
             */
            if (!support->membind->set_thisproc_membind && !support->membind->set_thisthread_membind
                && PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
                if (PRTE_HWLOC_BASE_MBFA_WARN == prte_hwloc_base_mbfa && !membind_warned) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported", true,
                                   node->name);
                    membind_warned = true;
                } else if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:membind-not-supported-fatal",
                                   true, node->name);
                    return PRTE_ERR_SILENT;
                }
            }
        }

        /* some systems do not report cores, and so we can get a situation where our
         * default binding policy will fail for no necessary reason. So if we are
         * computing a binding due to our default policy, and no cores are found
         * on this node, just silently skip it - we will not bind
         */
        if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)
            && HWLOC_TYPE_DEPTH_UNKNOWN
                   == hwloc_get_type_depth(node->topology->topo, HWLOC_OBJ_CORE)) {
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
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
            bind_depth = hwloc_get_cache_type_depth(node->topology->topo, clvl,
                                                    (hwloc_obj_cache_type_t) -1);
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
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-objects", true,
                           hwloc_obj_type_string(hwb), node->name);
            return PRTE_ERR_SILENT;
        }
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output, "%s bind_depth: %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), bind_depth);
        if (PRTE_SUCCESS != (rc = bind_generic(jdata, node, bind_depth))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}
