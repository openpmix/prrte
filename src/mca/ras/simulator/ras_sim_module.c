/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include "src/class/pmix_list.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"

#include "ras_sim.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_sim_module = {NULL, allocate, NULL, finalize};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int i, n, val, dig, num_nodes;
    prte_node_t *node;
    prte_topology_t *t;
    hwloc_topology_t topo;
    hwloc_obj_t obj;
    char **node_cnt = NULL;
    char **slot_cnt = NULL;
    char **max_slot_cnt = NULL;
    char *tmp, *job_cpuset = NULL;
    char prefix[6];
    bool use_hwthread_cpus = false;
    hwloc_obj_t root;
    hwloc_cpuset_t available, mycpus;

    node_cnt = pmix_argv_split(prte_ras_simulator_component.num_nodes, ',');
    if (NULL != prte_ras_simulator_component.slots) {
        slot_cnt = pmix_argv_split(prte_ras_simulator_component.slots, ',');
        /* backfile the slot_cnt so every topology has a cnt */
        tmp = slot_cnt[pmix_argv_count(slot_cnt) - 1];
        for (n = pmix_argv_count(slot_cnt); n < pmix_argv_count(node_cnt); n++) {
            pmix_argv_append_nosize(&slot_cnt, tmp);
        }
    }
    if (NULL != prte_ras_simulator_component.slots_max) {
        max_slot_cnt = pmix_argv_split(prte_ras_simulator_component.slots_max, ',');
        /* backfill the max_slot_cnt as reqd */
        tmp = max_slot_cnt[pmix_argv_count(slot_cnt) - 1];
        for (n = pmix_argv_count(max_slot_cnt); n < pmix_argv_count(max_slot_cnt); n++) {
            pmix_argv_append_nosize(&max_slot_cnt, tmp);
        }
    }

    /* setup the prefix to the node names */
    snprintf(prefix, 6, "nodeA");

    /* see if this job has a "soft" cgroup assignment */
    job_cpuset = NULL;
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &job_cpuset, PMIX_STRING)) {
        job_cpuset = NULL;
    }

    /* see if they want are using hwthreads as cpus */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* use our topology */
    t = (prte_topology_t *) pmix_pointer_array_get_item(prte_node_topologies, 0);
    if (NULL == t) {
        return PRTE_ERR_NOT_FOUND;
    }
    topo = t->topo;
    if (NULL != job_cpuset) {
        available = prte_hwloc_base_generate_cpuset(topo, use_hwthread_cpus, job_cpuset);
    } else {
        available = prte_hwloc_base_filter_cpus(topo);
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

        for (i = 0; i < num_nodes; i++) {
            node = PMIX_NEW(prte_node_t);
            pmix_asprintf(&node->name, "%s%0*d", prefix, dig, i);
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
            PMIX_RETAIN(t);
            node->topology = t;
            prte_output_verbose(1, prte_ras_base_framework.framework_output,
                                "Created Node <%10s> [%3d : %3d]", node->name, node->slots,
                                node->slots_max);
            node->available = hwloc_bitmap_dup(available);
            pmix_list_append(nodes, &node->super);
        }
    }
    hwloc_bitmap_free(available);

    /* record the number of allocated nodes */
    prte_num_allocated_nodes = pmix_list_get_size(nodes);

    if (NULL != max_slot_cnt) {
        pmix_argv_free(max_slot_cnt);
    }
    if (NULL != slot_cnt) {
        pmix_argv_free(slot_cnt);
    }
    if (NULL != node_cnt) {
        pmix_argv_free(node_cnt);
    }
    if (NULL != job_cpuset) {
        free(job_cpuset);
    }
    return PRTE_SUCCESS;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}
