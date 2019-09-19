/*
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "src/include/constants.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/hwloc/hwloc-internal.h"


/*
 * Don't use show_help() here (or print any error message at all).
 * Let the upper layer output a relevant message, because doing so may
 * be complicated (e.g., this might be called from the PRRTE ODLS,
 * which has to do some extra steps to get error messages to be
 * displayed).
 */
int prrte_hwloc_base_set_process_membind_policy(void)
{
    int rc = 0, flags;
    hwloc_membind_policy_t policy;
    hwloc_cpuset_t cpuset;

    /* Make sure prrte_hwloc_topology has been set by the time we've
       been called */
    if (PRRTE_SUCCESS != prrte_hwloc_base_get_topology()) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Set the default memory allocation policy according to MCA
       param */
    switch (prrte_hwloc_base_map) {
    case PRRTE_HWLOC_BASE_MAP_LOCAL_ONLY:
        policy = HWLOC_MEMBIND_BIND;
        flags = HWLOC_MEMBIND_STRICT;
        break;

    case PRRTE_HWLOC_BASE_MAP_NONE:
    default:
        policy = HWLOC_MEMBIND_DEFAULT;
        flags = 0;
        break;
    }

    cpuset = hwloc_bitmap_alloc();
    if (NULL == cpuset) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
    } else {
        int e;
        hwloc_get_cpubind(prrte_hwloc_topology, cpuset, 0);
        rc = hwloc_set_membind(prrte_hwloc_topology,
                               cpuset, policy, flags);
        e = errno;
        hwloc_bitmap_free(cpuset);

        /* See if hwloc was able to do it.  If hwloc failed due to
           ENOSYS, but the base_map == NONE, then it's not really an
           error. */
        if (0 != rc && ENOSYS == e &&
            PRRTE_HWLOC_BASE_MAP_NONE == prrte_hwloc_base_map) {
            rc = 0;
        }
    }

    return (0 == rc) ? PRRTE_SUCCESS : PRRTE_ERROR;
}

int prrte_hwloc_base_memory_set(prrte_hwloc_base_memory_segment_t *segments,
                               size_t num_segments)
{
    int rc = PRRTE_SUCCESS;
    char *msg = NULL;
    size_t i;
    hwloc_cpuset_t cpuset = NULL;

    /* bozo check */
    if (PRRTE_SUCCESS != prrte_hwloc_base_get_topology()) {
        msg = "hwloc_set_area_membind() failure - topology not available";
        return prrte_hwloc_base_report_bind_failure(__FILE__, __LINE__,
                                                   msg, rc);
    }

    /* This module won't be used unless the process is already
       processor-bound.  So find out where we're processor bound, and
       bind our memory there, too. */
    cpuset = hwloc_bitmap_alloc();
    if (NULL == cpuset) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        msg = "hwloc_bitmap_alloc() failure";
        goto out;
    }
    hwloc_get_cpubind(prrte_hwloc_topology, cpuset, 0);
    for (i = 0; i < num_segments; ++i) {
        if (0 != hwloc_set_area_membind(prrte_hwloc_topology,
                                        segments[i].mbs_start_addr,
                                        segments[i].mbs_len, cpuset,
                                        HWLOC_MEMBIND_BIND,
                                        HWLOC_MEMBIND_STRICT)) {
            rc = PRRTE_ERROR;
            msg = "hwloc_set_area_membind() failure";
            goto out;
        }
    }

 out:
    if (NULL != cpuset) {
        hwloc_bitmap_free(cpuset);
    }
    if (PRRTE_SUCCESS != rc) {
        return prrte_hwloc_base_report_bind_failure(__FILE__, __LINE__, msg, rc);
    }
    return PRRTE_SUCCESS;
}

int prrte_hwloc_base_node_name_to_id(char *node_name, int *id)
{
    /* GLB: fix me */
    *id = atoi(node_name + 3);

    return PRRTE_SUCCESS;
}

int prrte_hwloc_base_membind(prrte_hwloc_base_memory_segment_t *segs,
                            size_t count, int node_id)
{
    size_t i;
    int rc = PRRTE_SUCCESS;
    char *msg = NULL;
    hwloc_cpuset_t cpuset = NULL;

    /* bozo check */
    if (PRRTE_SUCCESS != prrte_hwloc_base_get_topology()) {
        msg = "hwloc_set_area_membind() failure - topology not available";
        return prrte_hwloc_base_report_bind_failure(__FILE__, __LINE__,
                                                   msg, rc);
    }

    cpuset = hwloc_bitmap_alloc();
    if (NULL == cpuset) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        msg = "hwloc_bitmap_alloc() failure";
        goto out;
    }
    hwloc_bitmap_set(cpuset, node_id);
    for(i = 0; i < count; i++) {
        if (0 != hwloc_set_area_membind(prrte_hwloc_topology,
                                        segs[i].mbs_start_addr,
                                        segs[i].mbs_len, cpuset,
                                        HWLOC_MEMBIND_BIND,
                                        HWLOC_MEMBIND_STRICT)) {
            rc = PRRTE_ERROR;
            msg = "hwloc_set_area_membind() failure";
            goto out;
        }
    }

 out:
    if (NULL != cpuset) {
        hwloc_bitmap_free(cpuset);
    }
    if (PRRTE_SUCCESS != rc) {
        return prrte_hwloc_base_report_bind_failure(__FILE__, __LINE__, msg, rc);
    }
    return PRRTE_SUCCESS;
}
