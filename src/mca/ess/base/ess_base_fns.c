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
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <errno.h>

#include "src/util/output.h"
#include "src/pmix/pmix-internal.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ess/base/base.h"

int prrte_ess_base_proc_binding(void)
{
    hwloc_obj_t node, obj;
    hwloc_cpuset_t cpus, nodeset;
    hwloc_obj_type_t target;
    unsigned int cache_level = 0;
    struct hwloc_topology_support *support;
    char *map;
    int ret;
    char *error=NULL;
    hwloc_cpuset_t mycpus;

    /* Determine if we were pre-bound or not - this also indicates
     * that we were launched via mpirun, bound or not */
    if (NULL != getenv(PRRTE_MCA_PREFIX"prrte_bound_at_launch")) {
        prrte_proc_is_bound = true;
        if (NULL != (map = getenv(PRRTE_MCA_PREFIX"prrte_base_applied_binding"))) {
            prrte_proc_applied_binding = hwloc_bitmap_alloc();
            if (0 != (ret = hwloc_bitmap_list_sscanf(prrte_proc_applied_binding, map))) {
                error = "applied_binding parse";
                goto error;
            }
        }
        if (prrte_hwloc_report_bindings || 4 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
            /* print out a shorthand notation to avoid pulling in the entire topology tree */
            map = NULL;
            PRRTE_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_LOCALITY_STRING,
                                           PRRTE_PROC_MY_NAME, &map, PMIX_STRING);
            if (PRRTE_SUCCESS == ret && NULL != map) {
                prrte_output(0, "MCW rank %s bound to %s",
                            PRRTE_VPID_PRINT(PRRTE_PROC_MY_NAME->vpid), map);
                free(map);
            } else {
                prrte_output(0, "MCW rank %s not bound", PRRTE_VPID_PRINT(PRRTE_PROC_MY_NAME->vpid));
            }
        }
        return PRRTE_SUCCESS;
    } else if (NULL != getenv(PRRTE_MCA_PREFIX"prrte_externally_bound")) {
        prrte_proc_is_bound = true;
        /* see if we were launched by a PMIx-enabled system */
        map = NULL;
        PRRTE_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_LOCALITY_STRING,
                                       PRRTE_PROC_MY_NAME, &map, PMIX_STRING);
        if (PRRTE_SUCCESS == ret && NULL != map) {
            /* we were - no need to pull in the topology */
            if (prrte_hwloc_report_bindings || 4 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
                prrte_output(0, "MCW rank %s bound to %s",
                            PRRTE_VPID_PRINT(PRRTE_PROC_MY_NAME->vpid), map);
            }
            free(map);
            return PRRTE_SUCCESS;
        }
        /* the topology system will pickup the binding pattern */
    }

    /* load the topology as we will likely need it */
    if (PRRTE_SUCCESS != prrte_hwloc_base_get_topology()) {
        /* there is nothing we can do, so just return */
        return PRRTE_SUCCESS;
    }

    /* see if we were bound when launched */
    if (!prrte_proc_is_bound) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                             "%s Not bound at launch",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(prrte_hwloc_topology);
        /* get our node object */
        node = hwloc_get_root_obj(prrte_hwloc_topology);
        nodeset = node->cpuset;
        /* get our bindings */
        cpus = hwloc_bitmap_alloc();
        if (hwloc_get_cpubind(prrte_hwloc_topology, cpus, HWLOC_CPUBIND_PROCESS) < 0) {
            /* we are NOT bound if get_cpubind fails, nor can we be bound - the
             * environment does not support it
             */
            hwloc_bitmap_free(cpus);
            PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                 "%s Binding not supported",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto MOVEON;
        }
        /* we are bound if the two cpusets are not equal,
         * or if there is only ONE cpu available to us
         */
        if (0 != hwloc_bitmap_compare(cpus, nodeset) ||
            prrte_hwloc_base_single_cpu(nodeset) ||
            prrte_hwloc_base_single_cpu(cpus)) {
            /* someone external set it - indicate it is set
             * so that we know
             */
            prrte_proc_is_bound = true;
            hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, cpus);
            hwloc_bitmap_free(cpus);
            PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                 "%s Process was externally bound",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        } else if (support->cpubind->set_thisproc_cpubind &&
                   PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy) &&
                   PRRTE_BIND_TO_NONE != PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
            /* the system is capable of doing processor affinity, but it
             * has not yet been set - see if a slot_list was given
             */
            hwloc_bitmap_zero(cpus);
            if (PRRTE_BIND_TO_CPUSET == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                if (PRRTE_SUCCESS != (ret = prrte_hwloc_base_cpu_list_parse(prrte_hwloc_base_cpu_list,
                                                                           prrte_hwloc_topology,
                                                                           PRRTE_HWLOC_LOGICAL, cpus))) {
                    error = "Setting processor affinity failed";
                    hwloc_bitmap_free(cpus);
                    goto error;
                }
                if (0 > hwloc_set_cpubind(prrte_hwloc_topology, cpus, 0)) {
                    error = "Setting processor affinity failed";
                    hwloc_bitmap_free(cpus);
                    goto error;
                }
                hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, cpus);
                hwloc_bitmap_free(cpus);
                prrte_proc_is_bound = true;
                PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                     "%s Process bound according to slot_list",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            } else {
                /* cleanup */
                hwloc_bitmap_free(cpus);
                /* get the node rank */
                if (PRRTE_NODE_RANK_INVALID == prrte_process_info.my_node_rank) {
                    /* this is not an error - could be due to being
                     * direct launched - so just ignore and leave
                     * us unbound
                     */
                    PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                         "%s Process not bound - no node rank available",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                    goto MOVEON;
                }
                /* if the binding policy is hwthread, then we bind to the nrank-th
                 * hwthread on this node
                 */
                if (PRRTE_BIND_TO_HWTHREAD == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                    if (NULL == (obj = prrte_hwloc_base_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_PU,
                                                                       0, prrte_process_info.my_node_rank, PRRTE_HWLOC_LOGICAL))) {
                        ret = PRRTE_ERR_NOT_FOUND;
                        error = "Getting hwthread object";
                        goto error;
                    }
                    cpus = obj->cpuset;
                    if (0 > hwloc_set_cpubind(prrte_hwloc_topology, cpus, 0)) {
                        ret = PRRTE_ERROR;
                        error = "Setting processor affinity failed";
                        goto error;
                    }
                    hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, cpus);
                    PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                         "%s Process bound to hwthread",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                } else if (PRRTE_BIND_TO_CORE == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                    /* if the binding policy is core, then we bind to the nrank-th
                     * core on this node
                     */
                    if (NULL == (obj = prrte_hwloc_base_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_CORE,
                                                                       0, prrte_process_info.my_node_rank, PRRTE_HWLOC_LOGICAL))) {
                        ret = PRRTE_ERR_NOT_FOUND;
                        error = "Getting core object";
                        goto error;
                    }
                    cpus = obj->cpuset;
                    if (0 > hwloc_set_cpubind(prrte_hwloc_topology, cpus, 0)) {
                        error = "Setting processor affinity failed";
                        ret = PRRTE_ERROR;
                        goto error;
                    }
                    hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, cpus);
                    PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                         "%s Process bound to core",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
                } else {
                    /* for all higher binding policies, we bind to the specified
                     * object that the nrank-th core belongs to
                     */
                    if (NULL == (obj = prrte_hwloc_base_get_obj_by_type(prrte_hwloc_topology, HWLOC_OBJ_CORE,
                                                                       0, prrte_process_info.my_node_rank, PRRTE_HWLOC_LOGICAL))) {
                        ret = PRRTE_ERR_NOT_FOUND;
                        error = "Getting core object";
                        goto error;
                    }
                    if (PRRTE_BIND_TO_L1CACHE == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                        PRRTE_HWLOC_MAKE_OBJ_CACHE(1, target, cache_level);
                    } else if (PRRTE_BIND_TO_L2CACHE == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                        PRRTE_HWLOC_MAKE_OBJ_CACHE(2, target, cache_level);
                    } else if (PRRTE_BIND_TO_L3CACHE == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                        PRRTE_HWLOC_MAKE_OBJ_CACHE(3, target, cache_level);
                    } else if (PRRTE_BIND_TO_SOCKET == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                        target = HWLOC_OBJ_SOCKET;
                    } else if (PRRTE_BIND_TO_NUMA == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                        target = HWLOC_OBJ_NODE;
                    } else {
                        ret = PRRTE_ERR_NOT_FOUND;
                        error = "Binding policy not known";
                        goto error;
                    }
                    for (obj = obj->parent; NULL != obj; obj = obj->parent) {
                        if (target == obj->type) {
#if HWLOC_API_VERSION < 0x20000
                            if (HWLOC_OBJ_CACHE == target && cache_level != obj->attr->cache.depth) {
                                continue;
                            }
#else
                            /* do something with cache_level to stop the compiler complaints */
                            ++cache_level;
#endif
                            /* this is the place! */
                            cpus = obj->cpuset;
                            if (0 > hwloc_set_cpubind(prrte_hwloc_topology, cpus, 0)) {
                                ret = PRRTE_ERROR;
                                error = "Setting processor affinity failed";
                                goto error;
                            }
                            hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, cpus);
                            prrte_proc_is_bound = true;
                            PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                                                 "%s Process bound to %s",
                                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                                 hwloc_obj_type_string(target)));
                            break;
                        }
                    }
                    if (!prrte_proc_is_bound) {
                        ret = PRRTE_ERROR;
                        error = "Setting processor affinity failed";
                        goto error;
                    }
                }
            }
        }
    } else {
        PRRTE_OUTPUT_VERBOSE((5, prrte_ess_base_framework.framework_output,
                             "%s Process bound at launch",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    }

 MOVEON:
    /* get the cpus we are bound to */
    mycpus = hwloc_bitmap_alloc();
    if (hwloc_get_cpubind(prrte_hwloc_topology,
                          mycpus,
                          HWLOC_CPUBIND_PROCESS) < 0) {
        if (NULL != prrte_process_info.cpuset) {
            free(prrte_process_info.cpuset);
            prrte_process_info.cpuset = NULL;
        }
        if (prrte_hwloc_report_bindings || 4 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
            prrte_output(0, "MCW rank %d is not bound",
                        PRRTE_PROC_MY_NAME->vpid);
        }
    } else {
        /* store/update the string representation of our local binding */
        if (NULL != prrte_process_info.cpuset) {
            free(prrte_process_info.cpuset);
            prrte_process_info.cpuset = NULL;
        }
        hwloc_bitmap_list_asprintf(&prrte_process_info.cpuset, mycpus);
        /* report the binding, if requested */
        if (prrte_hwloc_report_bindings || 4 < prrte_output_get_verbosity(prrte_ess_base_framework.framework_output)) {
            char tmp1[1024], tmp2[1024];
            if (PRRTE_ERR_NOT_BOUND == prrte_hwloc_base_cset2str(tmp1, sizeof(tmp1), prrte_hwloc_topology, mycpus)) {
                prrte_output(0, "MCW rank %d is not bound (or bound to all available processors)", PRRTE_PROC_MY_NAME->vpid);
            } else {
                prrte_hwloc_base_cset2mapstr(tmp2, sizeof(tmp2), prrte_hwloc_topology, mycpus);
                prrte_output(0, "MCW rank %d bound to %s: %s",
                            PRRTE_PROC_MY_NAME->vpid, tmp1, tmp2);
            }
        }
    }
    hwloc_bitmap_free(mycpus);
    /* push our cpuset so others can calculate our locality */
    if (NULL != prrte_process_info.cpuset) {
        PRRTE_MODEX_SEND_VALUE(ret, PMIX_GLOBAL, PMIX_CPUSET,
                              prrte_process_info.cpuset, PMIX_STRING);
    }
    return PRRTE_SUCCESS;

 error:
    if (PRRTE_ERR_SILENT != ret) {
        prrte_show_help("help-prrte-runtime",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }

    return PRRTE_ERR_SILENT;
}
