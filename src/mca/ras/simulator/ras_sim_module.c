/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "src/class/prte_list.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"

#include "ras_sim.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, prte_list_t *nodes);
static int finalize(void);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_sim_module = {NULL, allocate, NULL, finalize};

static int allocate(prte_job_t *jdata, prte_list_t *nodes)
{
    int i, n, val, dig, num_nodes;
    prte_node_t *node;
    prte_topology_t *t;
    hwloc_topology_t topo;
    hwloc_obj_t obj;
    unsigned j, k;
    struct hwloc_topology_support *support;
    char **files = NULL;
    char **topos = NULL;
    bool use_local_topology = false;
    char **node_cnt = NULL;
    char **slot_cnt = NULL;
    char **max_slot_cnt = NULL;
    char *tmp, *job_cpuset = NULL;
    char prefix[6];
    bool use_hwthread_cpus = false;
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *rdata;
    hwloc_cpuset_t available, mycpus;

    node_cnt = prte_argv_split(prte_ras_simulator_component.num_nodes, ',');
    if (NULL != prte_ras_simulator_component.slots) {
        slot_cnt = prte_argv_split(prte_ras_simulator_component.slots, ',');
        /* backfile the slot_cnt so every topology has a cnt */
        tmp = slot_cnt[prte_argv_count(slot_cnt) - 1];
        for (n = prte_argv_count(slot_cnt); n < prte_argv_count(node_cnt); n++) {
            prte_argv_append_nosize(&slot_cnt, tmp);
        }
    }
    if (NULL != prte_ras_simulator_component.slots_max) {
        max_slot_cnt = prte_argv_split(prte_ras_simulator_component.slots_max, ',');
        /* backfill the max_slot_cnt as reqd */
        tmp = max_slot_cnt[prte_argv_count(slot_cnt) - 1];
        for (n = prte_argv_count(max_slot_cnt); n < prte_argv_count(max_slot_cnt); n++) {
            prte_argv_append_nosize(&max_slot_cnt, tmp);
        }
    }

    if (NULL != prte_ras_simulator_component.topofiles) {
        files = prte_argv_split(prte_ras_simulator_component.topofiles, ',');
        if (prte_argv_count(files) != prte_argv_count(node_cnt)) {
            prte_show_help("help-ras-base.txt", "ras-sim:mismatch", true);
            goto error_silent;
        }
    } else if (NULL != prte_ras_simulator_component.topologies) {
        topos = prte_argv_split(prte_ras_simulator_component.topologies, ',');
        if (prte_argv_count(topos) != prte_argv_count(node_cnt)) {
            prte_show_help("help-ras-base.txt", "ras-sim:mismatch", true);
            goto error_silent;
        }
    } else {
        /* use our topology */
        use_local_topology = true;
    }

    /* setup the prefix to the node names */
    snprintf(prefix, 6, "nodeA");

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING);

    /* see if they want are using hwthreads as cpus */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* process the request */
    for (n = 0; NULL != node_cnt[n]; n++) {
        num_nodes = strtol(node_cnt[n], NULL, 10);

        /* get number of digits */
        val = num_nodes;
        for (dig = 0; 0 != val; dig++) {
            val /= 10;
        }

        /* set the prefix for this group of nodes */
        prefix[4] += n;

        /* check for topology */
        if (use_local_topology) {
            /* use our topology */
            t = (prte_topology_t *) prte_pointer_array_get_item(prte_node_topologies, 0);
        } else if (NULL != files) {
            if (0 != hwloc_topology_init(&topo)) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_init");
                goto error_silent;
            }
            if (0 != hwloc_topology_set_xml(topo, files[n])) {
                prte_show_help("help-ras-simulator.txt", "hwloc failed to load xml", true,
                               files[n]);
                hwloc_topology_destroy(topo);
                goto error_silent;
            }
            /* since we are loading this from an external source, we have to
             * explicitly set a flag so hwloc sets things up correctly
             */
            if (0
                != prte_hwloc_base_topology_set_flags(topo, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM,
                                                      false)) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_set_flags");
                hwloc_topology_destroy(topo);
                goto error_silent;
            }
            if (0 != hwloc_topology_load(topo)) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_load");
                hwloc_topology_destroy(topo);
                goto error_silent;
            }
            /* remove the hostname from the topology. Unfortunately, hwloc
             * decided to add the source hostname to the "topology", thus
             * rendering it unusable as a pure topological description. So
             * we remove that information here.
             */
            obj = hwloc_get_root_obj(topo);
            for (k = 0; k < obj->infos_count; k++) {
                if (NULL == obj->infos[k].name || NULL == obj->infos[k].value) {
                    continue;
                }
                if (0 == strncmp(obj->infos[k].name, "HostName", strlen("HostName"))) {
                    free(obj->infos[k].name);
                    free(obj->infos[k].value);
                    /* left justify the array */
                    for (j = k; j < obj->infos_count - 1; j++) {
                        obj->infos[j] = obj->infos[j + 1];
                    }
                    obj->infos[obj->infos_count - 1].name = NULL;
                    obj->infos[obj->infos_count - 1].value = NULL;
                    obj->infos_count--;
                    break;
                }
            }
            /* unfortunately, hwloc does not include support info in its
             * xml output :-(( To aid in debugging, we set it here
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(topo);
            support->cpubind->set_thisproc_cpubind = prte_ras_simulator_component.have_cpubind;
            support->membind->set_thisproc_membind = prte_ras_simulator_component.have_membind;
            /* pass it thru the filter so we create the summaries required by the mappers */
            if (PRTE_SUCCESS != prte_hwloc_base_filter_cpus(topo)) {
                PRTE_ERROR_LOG(PRTE_ERROR);
            }
            /* add it to our array */
            t = PRTE_NEW(prte_topology_t);
            t->topo = topo;
            t->sig = prte_hwloc_base_get_topo_signature(topo);
            prte_pointer_array_add(prte_node_topologies, t);
        } else {
            if (0 != hwloc_topology_init(&topo)) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_init");
                goto error_silent;
            }
            if (0 != hwloc_topology_set_synthetic(topo, topos[n])) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_set_synthetic");
                hwloc_topology_destroy(topo);
                goto error_silent;
            }
            if (0 != hwloc_topology_load(topo)) {
                prte_show_help("help-ras-simulator.txt", "hwloc API fail", true, __FILE__, __LINE__,
                               "hwloc_topology_load");
                hwloc_topology_destroy(topo);
                goto error_silent;
            }
            /* remove the hostname from the topology. Unfortunately, hwloc
             * decided to add the source hostname to the "topology", thus
             * rendering it unusable as a pure topological description. So
             * we remove that information here.
             */
            obj = hwloc_get_root_obj(topo);
            for (k = 0; k < obj->infos_count; k++) {
                if (NULL == obj->infos[k].name || NULL == obj->infos[k].value) {
                    continue;
                }
                if (0 == strncmp(obj->infos[k].name, "HostName", strlen("HostName"))) {
                    free(obj->infos[k].name);
                    free(obj->infos[k].value);
                    /* left justify the array */
                    for (j = k; j < obj->infos_count - 1; j++) {
                        obj->infos[j] = obj->infos[j + 1];
                    }
                    obj->infos[obj->infos_count - 1].name = NULL;
                    obj->infos[obj->infos_count - 1].value = NULL;
                    obj->infos_count--;
                    break;
                }
            }
            /* unfortunately, hwloc does not include support info in its
             * xml output :-(( To aid in debugging, we set it here
             */
            support = (struct hwloc_topology_support *) hwloc_topology_get_support(topo);
            support->cpubind->set_thisproc_cpubind = prte_ras_simulator_component.have_cpubind;
            support->membind->set_thisproc_membind = prte_ras_simulator_component.have_membind;
            /* add it to our array */
            t = PRTE_NEW(prte_topology_t);
            t->topo = topo;
            t->sig = prte_hwloc_base_get_topo_signature(topo);
            prte_pointer_array_add(prte_node_topologies, t);
        }

        /* get the available processors on this node */
        root = hwloc_get_root_obj(topo);
        if (NULL == root->userdata) {
            /* incorrect */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return PRTE_ERR_BAD_PARAM;
        }
        rdata = (prte_hwloc_topo_data_t *) root->userdata;
        available = hwloc_bitmap_dup(rdata->available);

        if (NULL != job_cpuset) {
            mycpus = prte_hwloc_base_generate_cpuset(topo, use_hwthread_cpus, job_cpuset);
            hwloc_bitmap_and(available, mycpus, available);
            hwloc_bitmap_free(mycpus);
        }

        for (i = 0; i < num_nodes; i++) {
            node = PRTE_NEW(prte_node_t);
            prte_asprintf(&node->name, "%s%0*d", prefix, dig, i);
            node->state = PRTE_NODE_STATE_UP;
            node->slots_inuse = 0;
            if (NULL == max_slot_cnt || NULL == max_slot_cnt[n]) {
                node->slots_max = 0;
            } else {
                obj = hwloc_get_root_obj(t->topo);
                node->slots_max = prte_hwloc_base_get_npus(t->topo, use_hwthread_cpus, available,
                                                           obj);
            }
            if (NULL == slot_cnt || NULL == slot_cnt[n]) {
                node->slots = 0;
            } else {
                obj = hwloc_get_root_obj(t->topo);
                node->slots = prte_hwloc_base_get_npus(t->topo, use_hwthread_cpus, available, obj);
            }
            PRTE_RETAIN(t);
            node->topology = t;
            prte_output_verbose(1, prte_ras_base_framework.framework_output,
                                "Created Node <%10s> [%3d : %3d]", node->name, node->slots,
                                node->slots_max);
            prte_list_append(nodes, &node->super);
        }
        hwloc_bitmap_free(available);
    }

    /* record the number of allocated nodes */
    prte_num_allocated_nodes = prte_list_get_size(nodes);

    if (NULL != max_slot_cnt) {
        prte_argv_free(max_slot_cnt);
    }
    if (NULL != slot_cnt) {
        prte_argv_free(slot_cnt);
    }
    if (NULL != node_cnt) {
        prte_argv_free(node_cnt);
    }
    if (NULL != topos) {
        prte_argv_free(topos);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }
    return PRTE_SUCCESS;

error_silent:
    if (NULL != max_slot_cnt) {
        prte_argv_free(max_slot_cnt);
    }
    if (NULL != slot_cnt) {
        prte_argv_free(slot_cnt);
    }
    if (NULL != node_cnt) {
        prte_argv_free(node_cnt);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }
    return PRTE_ERR_SILENT;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}
