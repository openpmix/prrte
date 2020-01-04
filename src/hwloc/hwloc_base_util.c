/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (C) 2018      Mellanox Technologies, Ltd.
 *                         All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019-2020 IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#define PRRTE_HWLOC_WANT_SHMEM 1

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ENDIAN_H
#include <endian.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "src/runtime/prrte_globals.h"
#include "src/include/constants.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/os_dirpath.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/util/printf.h"
#include "src/threads/tsd.h"
#include "src/pmix/pmix-internal.h"

#include "src/hwloc/hwloc-internal.h"

static bool topo_in_shmem = false;

/*
 * Provide the hwloc object that corresponds to the given
 * processor id of the given type.  Remember: "processor" here [usually] means "core" --
 * except that on some platforms, hwloc won't find any cores; it'll
 * only find PUs (!).  On such platforms, then do the same calculation
 * but with PUs instead of COREs.
 */
hwloc_obj_t prrte_hwloc_base_get_pu(hwloc_topology_t topo,
                                   int lid,
                                   prrte_hwloc_resource_type_t rtype)
{
    hwloc_obj_type_t obj_type = HWLOC_OBJ_CORE;
    hwloc_obj_t obj;

    /* hwloc isn't able to find cores on all platforms.  Example:
       PPC64 running RHEL 5.4 (linux kernel 2.6.18) only reports NUMA
       nodes and PU's.  Fine.

       However, note that hwloc_get_obj_by_type() will return NULL in
       2 (effectively) different cases:

       - no objects of the requested type were found
       - the Nth object of the requested type was not found

       So first we have to see if we can find *any* cores by looking
       for the 0th core.  If we find it, then try to find the Nth
       core.  Otherwise, try to find the Nth PU. */
    if (prrte_hwloc_use_hwthreads_as_cpus || (NULL == hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0))) {
        obj_type = HWLOC_OBJ_PU;
    }

    if (PRRTE_HWLOC_PHYSICAL == rtype) {
        /* find the pu - note that we can only find physical PUs
         * as cores do not have unique physical numbers (they are
         * numbered within their sockets instead). So we find the
         * specified PU, and then return the core object that contains it */
        obj = hwloc_get_pu_obj_by_os_index(topo, lid);
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "physical cpu %d %s found in cpuset %s",
                             lid, (NULL == obj) ? "not" : "is",
                             (NULL == prrte_hwloc_base_cpu_list) ? "None" : prrte_hwloc_base_cpu_list));
        /* we now need to shift upward to the core including this PU */
        if (NULL != obj && HWLOC_OBJ_CORE == obj_type) {
            obj = obj->parent;
        }
        return obj;
    }

    prrte_output_verbose(5, prrte_hwloc_base_output,
                        "Searching for %d LOGICAL PU", lid);

    /* Now do the actual lookup. */
    obj = hwloc_get_obj_by_type(topo, obj_type, lid);
    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "logical cpu %d %s found in cpuset %s",
                         lid, (NULL == obj) ? "not" : "is",
                         (NULL == prrte_hwloc_base_cpu_list) ? "None" : prrte_hwloc_base_cpu_list));

    /* Found the right core (or PU). Return the object */
    return obj;
}

/* determine the node-level available cpuset based on
 * online vs allowed vs user-specified cpus
 */
int prrte_hwloc_base_filter_cpus(hwloc_topology_t topo)
{
    hwloc_obj_t root, pu;
    hwloc_cpuset_t avail = NULL, pucpus, res;
    prrte_hwloc_topo_data_t *sum;
    prrte_hwloc_obj_data_t *data;
    char **ranges=NULL, **range=NULL;
    int idx, cpu, start, end;

    root = hwloc_get_root_obj(topo);

    if (NULL == root->userdata) {
        root->userdata = (void*)PRRTE_NEW(prrte_hwloc_topo_data_t);
    }
    sum = (prrte_hwloc_topo_data_t*)root->userdata;

    /* should only ever enter here once, but check anyway */
    if (NULL != sum->available) {
        return PRRTE_SUCCESS;
    }

    /* process any specified default cpu set against this topology */
    if (NULL == prrte_hwloc_base_cpu_list) {
        /* get the root available cpuset */
        #if HWLOC_API_VERSION < 0x20000
            avail = hwloc_bitmap_alloc();
            hwloc_bitmap_and(avail, root->online_cpuset, root->allowed_cpuset);
        #else
            avail = hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));
        #endif
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base: no cpus specified - using root available cpuset"));
    } else {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base: filtering cpuset"));
        /* find the specified logical cpus */
        ranges = prrte_argv_split(prrte_hwloc_base_cpu_list, ',');
        avail = hwloc_bitmap_alloc();
        hwloc_bitmap_zero(avail);
        res = hwloc_bitmap_alloc();
        pucpus = hwloc_bitmap_alloc();
        for (idx=0; idx < prrte_argv_count(ranges); idx++) {
            range = prrte_argv_split(ranges[idx], '-');
            switch (prrte_argv_count(range)) {
            case 1:
                /* only one cpu given - get that object */
                cpu = strtoul(range[0], NULL, 10);
                if (NULL != (pu = prrte_hwloc_base_get_pu(topo, cpu, PRRTE_HWLOC_LOGICAL))) {
                    #if HWLOC_API_VERSION < 0x20000
                        hwloc_bitmap_and(pucpus, pu->online_cpuset, pu->allowed_cpuset);
                    #else
                        hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
                    #endif
                    hwloc_bitmap_or(res, avail, pucpus);
                    hwloc_bitmap_copy(avail, res);
                    data = (prrte_hwloc_obj_data_t*)pu->userdata;
                    if (NULL == data) {
                        pu->userdata = (void*)PRRTE_NEW(prrte_hwloc_obj_data_t);
                        data = (prrte_hwloc_obj_data_t*)pu->userdata;
                    }
                    data->npus++;
                }
                break;
            case 2:
                /* range given */
                start = strtoul(range[0], NULL, 10);
                end = strtoul(range[1], NULL, 10);
                for (cpu=start; cpu <= end; cpu++) {
                    if (NULL != (pu = prrte_hwloc_base_get_pu(topo, cpu, PRRTE_HWLOC_LOGICAL))) {
                        #if HWLOC_API_VERSION < 0x20000
                            hwloc_bitmap_and(pucpus, pu->online_cpuset, pu->allowed_cpuset);
                        #else
                            hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
                        #endif
                        hwloc_bitmap_or(res, avail, pucpus);
                        hwloc_bitmap_copy(avail, res);
                        data = (prrte_hwloc_obj_data_t*)pu->userdata;
                        if (NULL == data) {
                            pu->userdata = (void*)PRRTE_NEW(prrte_hwloc_obj_data_t);
                            data = (prrte_hwloc_obj_data_t*)pu->userdata;
                        }
                        data->npus++;
                    }
                }
                break;
            default:
                break;
            }
            prrte_argv_free(range);
        }
        if (NULL != ranges) {
            prrte_argv_free(ranges);
        }
        hwloc_bitmap_free(res);
        hwloc_bitmap_free(pucpus);
    }

    /* cache this info */
    sum->available = avail;

    return PRRTE_SUCCESS;
}

static void fill_cache_line_size(void)
{
    int i = 0, cache_level = 2;
    unsigned size;
    unsigned int cache_object = HWLOC_OBJ_L2CACHE;
    hwloc_obj_t obj;
    bool found = false;

    /* Look for the smallest L2 cache size */
    size = 4096;
    while (cache_level > 0 && !found) {
        i=0;
        while (1) {
            obj = prrte_hwloc_base_get_obj_by_type(prrte_hwloc_topology,
                                                  cache_object, cache_level,
                                                  i, PRRTE_HWLOC_LOGICAL);
            if (NULL == obj) {
                --cache_level;
                cache_object = HWLOC_OBJ_L1CACHE;
                break;
            } else {
                if (NULL != obj->attr &&
                    obj->attr->cache.linesize > 0 &&
                    size > obj->attr->cache.linesize) {
                    size = obj->attr->cache.linesize;
                    found = true;
                }
            }
            ++i;
        }
    }

    /* If we found an L2 cache size in the hwloc data, save it in
       prrte_cache_line_size.  Otherwise, we'll leave whatever default
       was set in prrte_init.c */
    if (found) {
        prrte_cache_line_size = (int) size;
    }
}

int prrte_hwloc_base_get_topology(void)
{
    int rc;

    prrte_output_verbose(2, prrte_hwloc_base_output,
                         "hwloc:base:get_topology");

    /* see if we already have it */
    if (NULL != prrte_hwloc_topology) {
        return PRRTE_SUCCESS;
    }

    if (NULL == prrte_hwloc_base_topo_file) {
        prrte_output_verbose(1, prrte_hwloc_base_output,
                            "hwloc:base discovering topology");
        if (0 != hwloc_topology_init(&prrte_hwloc_topology) ||
            0 != prrte_hwloc_base_topology_set_flags(prrte_hwloc_topology, 0, true) ||
            0 != hwloc_topology_load(prrte_hwloc_topology)) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_SUPPORTED);
            return PRRTE_ERR_NOT_SUPPORTED;
        }
        /* filter the cpus thru any default cpu set */
        if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_filter_cpus(prrte_hwloc_topology))) {
            hwloc_topology_destroy(prrte_hwloc_topology);
            return rc;
        }
    } else {
        prrte_output_verbose(1, prrte_hwloc_base_output,
                            "hwloc:base loading topology from file %s",
                            prrte_hwloc_base_topo_file);
        if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_set_topology(prrte_hwloc_base_topo_file))) {
            return rc;
        }
    }

    /* fill prrte_cache_line_size global with the smallest L1 cache
       line size */
    fill_cache_line_size();

    /* get or update our local cpuset - it will get used multiple
     * times, so it's more efficient to keep a global copy
     */
    prrte_hwloc_base_get_local_cpuset();

    return PRRTE_SUCCESS;
}

int prrte_hwloc_base_set_topology(char *topofile)
{
    struct hwloc_topology_support *support;

     PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                          "hwloc:base:set_topology %s", topofile));

   if (NULL != prrte_hwloc_topology) {
        hwloc_topology_destroy(prrte_hwloc_topology);
    }
    if (0 != hwloc_topology_init(&prrte_hwloc_topology)) {
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    if (0 != hwloc_topology_set_xml(prrte_hwloc_topology, topofile)) {
        hwloc_topology_destroy(prrte_hwloc_topology);
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:set_topology bad topo file"));
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    /* since we are loading this from an external source, we have to
     * explicitly set a flag so hwloc sets things up correctly
     */
    if (0 != prrte_hwloc_base_topology_set_flags(prrte_hwloc_topology,
                                                HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM,
                                                true)) {
        hwloc_topology_destroy(prrte_hwloc_topology);
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    if (0 != hwloc_topology_load(prrte_hwloc_topology)) {
        hwloc_topology_destroy(prrte_hwloc_topology);
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:set_topology failed to load"));
        return PRRTE_ERR_NOT_SUPPORTED;
    }

    /* unfortunately, hwloc does not include support info in its
     * xml output :-(( We default to assuming it is present as
     * systems that use this option are likely to provide
     * binding support
     */
    support = (struct hwloc_topology_support*)hwloc_topology_get_support(prrte_hwloc_topology);
    support->cpubind->set_thisproc_cpubind = true;
    support->membind->set_thisproc_membind = true;

    /* fill prrte_cache_line_size global with the smallest L1 cache
       line size */
    fill_cache_line_size();

    /* all done */
    return PRRTE_SUCCESS;
}

static void free_object(hwloc_obj_t obj)
{
    prrte_hwloc_obj_data_t *data;
    unsigned k;

    /* free any data hanging on this object */
    if (NULL != obj->userdata) {
        data = (prrte_hwloc_obj_data_t*)obj->userdata;
        PRRTE_RELEASE(data);
        obj->userdata = NULL;
    }

    /* loop thru our children */
    for (k=0; k < obj->arity; k++) {
        free_object(obj->children[k]);
    }
}

void prrte_hwloc_base_free_topology(hwloc_topology_t topo)
{
    hwloc_obj_t obj;
    prrte_hwloc_topo_data_t *rdata;
    unsigned k;

    if (!topo_in_shmem) {
        obj = hwloc_get_root_obj(topo);
        /* release the root-level userdata */
        if (NULL != obj->userdata) {
            rdata = (prrte_hwloc_topo_data_t*)obj->userdata;
            PRRTE_RELEASE(rdata);
            obj->userdata = NULL;
        }
        /* now recursively descend and release userdata
         * in the rest of the objects
         */
        for (k=0; k < obj->arity; k++) {
            free_object(obj->children[k]);
        }
    }
    hwloc_topology_destroy(topo);
}

void prrte_hwloc_base_get_local_cpuset(void)
{
#if HWLOC_API_VERSION < 0x20000
    hwloc_obj_t root;
#endif

    if (NULL != prrte_hwloc_topology) {
        if (NULL == prrte_hwloc_my_cpuset) {
            prrte_hwloc_my_cpuset = hwloc_bitmap_alloc();
        }

        /* get the cpus we are bound to */
        if (hwloc_get_cpubind(prrte_hwloc_topology,
                              prrte_hwloc_my_cpuset,
                              HWLOC_CPUBIND_PROCESS) < 0) {
            /* we are not bound - use the root's available cpuset */
            #if HWLOC_API_VERSION < 0x20000
                root = hwloc_get_root_obj(prrte_hwloc_topology);
                hwloc_bitmap_and(prrte_hwloc_my_cpuset, root->online_cpuset, root->allowed_cpuset);
            #else
                hwloc_bitmap_copy(prrte_hwloc_my_cpuset, hwloc_topology_get_allowed_cpuset(prrte_hwloc_topology));
            #endif
        }
    }
}

int prrte_hwloc_base_report_bind_failure(const char *file,
                                        int line,
                                        const char *msg, int rc)
{
    static int already_reported = 0;

    if (!already_reported &&
        PRRTE_HWLOC_BASE_MBFA_SILENT != prrte_hwloc_base_mbfa) {

        prrte_show_help("help-prrte-hwloc-base.txt", "mbind failure", true,
                       prrte_process_info.nodename, getpid(), file, line, msg,
                       (PRRTE_HWLOC_BASE_MBFA_WARN == prrte_hwloc_base_mbfa) ?
                       "Warning -- your job will continue, but possibly with degraded performance" :
                       "ERROR -- your job may abort or behave erraticly");
        already_reported = 1;
        return rc;
    }

    return PRRTE_SUCCESS;
}

/* determine if there is a single cpu in a bitmap */
bool prrte_hwloc_base_single_cpu(hwloc_cpuset_t cpuset)
{
    int i;
    bool one=false;

    /* count the number of bits that are set - there is
     * one bit for each available pu. We could just
     * subtract the first and last indices, but there
     * may be "holes" in the bitmap corresponding to
     * offline or unallowed cpus - so we have to
     * search for them. Return false if we anything
     * other than one
     */
    for (i=hwloc_bitmap_first(cpuset);
         i <= hwloc_bitmap_last(cpuset);
         i++) {
        if (hwloc_bitmap_isset(cpuset, i)) {
            if (one) {
                return false;
            }
            one = true;
        }
    }

    return one;
}

/* get the number of pu's under a given hwloc object */
unsigned int prrte_hwloc_base_get_npus(hwloc_topology_t topo,
                                      hwloc_obj_t obj)
{
    prrte_hwloc_obj_data_t *data;
    unsigned int cnt = 0;

    data = (prrte_hwloc_obj_data_t*)obj->userdata;
    if (NULL == data || !data->npus_calculated) {
        if (!prrte_hwloc_use_hwthreads_as_cpus) {
            /* if we are treating cores as cpus, then we really
             * want to know how many cores are in this object.
             * hwloc sets a bit for each "pu", so we can't just
             * count bits in this case as there may be more than
             * one hwthread/core. Instead, find the number of cores
             * in the system
             */
            cnt = hwloc_get_nbobjs_inside_cpuset_by_type(topo, obj->cpuset, HWLOC_OBJ_CORE);
        } else {
            hwloc_cpuset_t cpuset;

            /* if we are treating cores as cpus, or the system can't detect
             * "cores", then get the available cpuset for this object - this will
             * create and store the data
             */
            if (NULL == (cpuset = obj->cpuset)) {
                return 0;
            }
            /* count the number of bits that are set - there is
             * one bit for each available pu. We could just
             * subtract the first and last indices, but there
             * may be "holes" in the bitmap corresponding to
             * offline or unallowed cpus - so we count them with
             * the bitmap "weight" (a.k.a. population count) function
             */
            cnt = hwloc_bitmap_weight(cpuset);
        }
        /* cache the info */
        data = (prrte_hwloc_obj_data_t*)obj->userdata;  // in case it was added
        if (NULL == data) {
            data = PRRTE_NEW(prrte_hwloc_obj_data_t);
            obj->userdata = (void*)data;
        }
        data->npus = cnt;
        data->npus_calculated = true;
    }

    return data->npus;
}

unsigned int prrte_hwloc_base_get_obj_idx(hwloc_topology_t topo,
                                         hwloc_obj_t obj,
                                         prrte_hwloc_resource_type_t rtype)
{
    unsigned cache_level=0;
    prrte_hwloc_obj_data_t *data;
    hwloc_obj_t ptr;
    unsigned int nobjs, i;

    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:get_idx"));

    /* see if we already have the info */
    data = (prrte_hwloc_obj_data_t*)obj->userdata;

    if (NULL == data) {
        data = PRRTE_NEW(prrte_hwloc_obj_data_t);
        obj->userdata = (void*)data;
    }

    if (data->idx < UINT_MAX) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:get_idx already have data: %u",
                             data->idx));
        return data->idx;
    }

#if HWLOC_API_VERSION < 0x20000
    /* determine the number of objects of this type */
    if (HWLOC_OBJ_CACHE == obj->type) {
        cache_level = obj->attr->cache.depth;
    }
#endif

    nobjs = prrte_hwloc_base_get_nbobjs_by_type(topo, obj->type, cache_level, rtype);

    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:get_idx found %u objects of type %s:%u",
                         nobjs, hwloc_obj_type_string(obj->type), cache_level));

    /* find this object */
    for (i=0; i < nobjs; i++) {
        ptr = prrte_hwloc_base_get_obj_by_type(topo, obj->type, cache_level, i, rtype);
        if (ptr == obj) {
            data->idx = i;
            return i;
        }
    }
    /* if we get here, it wasn't found */
    prrte_show_help("help-prrte-hwloc-base.txt",
                   "obj-idx-failed", true,
                   hwloc_obj_type_string(obj->type), cache_level);
    return UINT_MAX;
}

/* hwloc treats cache objects as special
 * cases. Instead of having a unique type for each cache level,
 * there is a single cache object type, and the level is encoded
 * in an attribute union. So looking for cache objects involves
 * a multi-step test :-(
 */
static hwloc_obj_t df_search(hwloc_topology_t topo,
                             hwloc_obj_t start,
                             hwloc_obj_type_t target,
                             unsigned cache_level,
                             unsigned int nobj,
                             prrte_hwloc_resource_type_t rtype,
                             unsigned int *num_objs)
{
    hwloc_obj_t obj;
    int search_depth;

    search_depth = hwloc_get_type_depth(topo, target);
    if (HWLOC_TYPE_DEPTH_MULTIPLE == search_depth) {
        /* either v1.x Cache, or Groups */
#if HWLOC_API_VERSION >= 0x20000
        return NULL;
#else
        if (cache_level != HWLOC_OBJ_CACHE)
            return NULL;
        search_depth = hwloc_get_cache_type_depth(topo, cache_level, (hwloc_obj_cache_type_t) -1);
#endif
    }
    if (HWLOC_TYPE_DEPTH_UNKNOWN == search_depth)
        return NULL;

    if (PRRTE_HWLOC_LOGICAL == rtype) {
        if (num_objs)
            *num_objs = hwloc_get_nbobjs_by_depth(topo, search_depth);
        return hwloc_get_obj_by_depth(topo, search_depth, nobj);
    }
    if (PRRTE_HWLOC_PHYSICAL == rtype) {
        /* the PHYSICAL object number is stored as the os_index. When
         * counting physical objects, we can't just count the number
         * that are in the hwloc tree as the only entries in the tree
         * are LOGICAL objects - i.e., any physical gaps won't show. So
         * we instead return the MAX os_index, as this is the best we
         * can do to tell you how many PHYSICAL objects are in the system.
         *
         * NOTE: if the last PHYSICAL object is not present (e.g., the last
         * socket on the node is empty), then the count we return will
         * be wrong!
         */
        hwloc_obj_t found = NULL;
        obj = NULL;
        if (num_objs)
            *num_objs = 0;
        while ((obj = hwloc_get_next_obj_by_depth(topo, search_depth, obj)) != NULL) {
            if (num_objs && obj->os_index > *num_objs)
                *num_objs = obj->os_index;
            if (obj->os_index == nobj)
                found = obj;
        }
        return found;
    }
    if (PRRTE_HWLOC_AVAILABLE == rtype) {
        unsigned idx = 0;
        if (num_objs)
            *num_objs = hwloc_get_nbobjs_inside_cpuset_by_depth(topo, start->cpuset, search_depth);
        obj = NULL;
        while ((obj = hwloc_get_next_obj_inside_cpuset_by_depth(topo, start->cpuset, search_depth, obj)) != NULL) {
            if (idx == nobj)
                return obj;
            idx++;
        }
        return NULL;
    }
    return NULL;
}

unsigned int prrte_hwloc_base_get_nbobjs_by_type(hwloc_topology_t topo,
                                                hwloc_obj_type_t target,
                                                unsigned cache_level,
                                                prrte_hwloc_resource_type_t rtype)
{
    unsigned int num_objs;
    hwloc_obj_t obj;
    prrte_hwloc_summary_t *sum;
    prrte_hwloc_topo_data_t *data;
    int rc;

    /* bozo check */
    if (NULL == topo) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:get_nbobjs NULL topology"));
        return 0;
    }

    /* if we want the number of LOGICAL objects, we can just
     * use the hwloc accessor to get it, unless it is a CACHE
     * as these are treated as special cases
     */
    if (PRRTE_HWLOC_LOGICAL == rtype
#if HWLOC_API_VERSION < 0x20000
        && HWLOC_OBJ_CACHE != target
#endif
       ) {
        /* we should not get an error back, but just in case... */
        if (0 > (rc = hwloc_get_nbobjs_by_type(topo, target))) {
            prrte_output(0, "UNKNOWN HWLOC ERROR");
            return 0;
        }
        return rc;
    }

    /* for everything else, we have to do some work */
    num_objs = 0;
    obj = hwloc_get_root_obj(topo);

    /* first see if the topology already has this summary */
    data = (prrte_hwloc_topo_data_t*)obj->userdata;
    if (NULL == data) {
        data = PRRTE_NEW(prrte_hwloc_topo_data_t);
        obj->userdata = (void*)data;
    } else {
        PRRTE_LIST_FOREACH(sum, &data->summaries, prrte_hwloc_summary_t) {
            if (target == sum->type &&
                cache_level == sum->cache_level &&
                rtype == sum->rtype) {
                /* yep - return the value */
                PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                                     "hwloc:base:get_nbojbs pre-existing data %u of %s:%u",
                                     sum->num_objs, hwloc_obj_type_string(target), cache_level));
                return sum->num_objs;
            }
        }
    }

    /* don't already know it - go get it */
    df_search(topo, obj, target, cache_level, 0, rtype, &num_objs);

    /* cache the results for later */
    sum = PRRTE_NEW(prrte_hwloc_summary_t);
    sum->type = target;
    sum->cache_level = cache_level;
    sum->num_objs = num_objs;
    sum->rtype = rtype;
    prrte_list_append(&data->summaries, &sum->super);

    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:get_nbojbs computed data %u of %s:%u",
                         num_objs, hwloc_obj_type_string(target), cache_level));

    return num_objs;
}

/* as above, only return the Nth instance of the specified object
 * type from inside the topology
 */
hwloc_obj_t prrte_hwloc_base_get_obj_by_type(hwloc_topology_t topo,
                                            hwloc_obj_type_t target,
                                            unsigned cache_level,
                                            unsigned int instance,
                                            prrte_hwloc_resource_type_t rtype)
{
    hwloc_obj_t obj;

    /* bozo check */
    if (NULL == topo) {
        return NULL;
    }

    /* if we want the nth LOGICAL object, we can just
     * use the hwloc accessor to get it, unless it is a CACHE
     * as these are treated as special cases
     */
    if (PRRTE_HWLOC_LOGICAL == rtype
#if HWLOC_API_VERSION < 0x20000
        && HWLOC_OBJ_CACHE != target
#endif
       ) {
        return hwloc_get_obj_by_type(topo, target, instance);
    }

    /* for everything else, we have to do some work */
    obj = hwloc_get_root_obj(topo);
    return df_search(topo, obj, target, cache_level, instance, rtype, NULL);
}

static void df_clear(hwloc_topology_t topo,
                     hwloc_obj_t start)
{
    unsigned k;
    prrte_hwloc_obj_data_t *data;

    /* see how many procs are bound to us */
    data = (prrte_hwloc_obj_data_t*)start->userdata;
    if (NULL != data) {
        data->num_bound = 0;
    }

    for (k=0; k < start->arity; k++) {
        df_clear(topo, start->children[k]);
    }
}

void prrte_hwloc_base_clear_usage(hwloc_topology_t topo)
{
    hwloc_obj_t root;
    unsigned k;

    /* bozo check */
    if (NULL == topo) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:clear_usage: NULL topology"));
        return;
    }

    root = hwloc_get_root_obj(topo);
    /* must not start at root as the root object has
     * a different userdata attached to it
     */
    for (k=0; k < root->arity; k++) {
        df_clear(topo, root->children[k]);
    }
}

/* The current slot_list notation only goes to the core level - i.e., the location
 * is specified as socket:core. Thus, the code below assumes that all locations
 * are to be parsed under that notation.
 */

static int socket_to_cpu_set(char *cpus,
                             hwloc_topology_t topo,
                             prrte_hwloc_resource_type_t rtype,
                             hwloc_bitmap_t cpumask)
{
    char **range;
    int range_cnt;
    int lower_range, upper_range;
    int socket_id;
    hwloc_obj_t obj;

    if ('*' == cpus[0]) {
        /* requesting cpumask for ALL sockets */
        obj = hwloc_get_root_obj(topo);
        /* set to all available processors - essentially,
         * this specification equates to unbound
         */
        hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        return PRRTE_SUCCESS;
    }

    range = prrte_argv_split(cpus,'-');
    range_cnt = prrte_argv_count(range);
    switch (range_cnt) {
    case 1:  /* no range was present, so just one socket given */
        socket_id = atoi(range[0]);
        obj = prrte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_SOCKET, 0, socket_id, rtype);
        /* get the available cpus for this socket */
        hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        break;

    case 2:  /* range of sockets was given */
        lower_range = atoi(range[0]);
        upper_range = atoi(range[1]);
        /* cycle across the range of sockets */
        for (socket_id=lower_range; socket_id<=upper_range; socket_id++) {
            obj = prrte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_SOCKET, 0, socket_id, rtype);
            /* set the available cpus for this socket bits in the bitmask */
            hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        }
        break;
    default:
        prrte_argv_free(range);
        return PRRTE_ERROR;
    }
    prrte_argv_free(range);

    return PRRTE_SUCCESS;
}

static int socket_core_to_cpu_set(char *socket_core_list,
                                  hwloc_topology_t topo,
                                  prrte_hwloc_resource_type_t rtype,
                                  hwloc_bitmap_t cpumask)
{
    int rc=PRRTE_SUCCESS, i, j;
    char **socket_core, *corestr;
    char **range, **list;
    int range_cnt;
    int lower_range, upper_range;
    int socket_id, core_id;
    hwloc_obj_t socket, core;
    hwloc_obj_type_t obj_type = HWLOC_OBJ_CORE;

    socket_core = prrte_argv_split(socket_core_list, ':');
    socket_id = atoi(socket_core[0]);

    /* get the object for this socket id */
    if (NULL == (socket = prrte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_SOCKET, 0,
                                                          socket_id, rtype))) {
        prrte_argv_free(socket_core);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* as described in comment near top of file, hwloc isn't able
     * to find cores on all platforms. Adjust the type here if
     * required
     */
    if (NULL == hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0)) {
        obj_type = HWLOC_OBJ_PU;
    }

    for (i=1; NULL != socket_core[i]; i++) {
        if ('C' == socket_core[i][0] ||
            'c' == socket_core[i][0]) {
            corestr = &socket_core[i][1];
        } else {
            corestr = socket_core[i];
        }
        if ('*' == corestr[0]) {
            /* set to all cpus on this socket */
            hwloc_bitmap_or(cpumask, cpumask, socket->cpuset);
            /* we are done - already assigned all cores! */
            rc = PRRTE_SUCCESS;
            break;
        } else {
            range = prrte_argv_split(corestr, '-');
            range_cnt = prrte_argv_count(range);
            /* see if a range was set or not */
            switch (range_cnt) {
            case 1:  /* only one core, or a list of cores, specified */
                list = prrte_argv_split(range[0], ',');
                for (j=0; NULL != list[j]; j++) {
                    core_id = atoi(list[j]);
                    /* get that object */
                    if (NULL == (core = df_search(topo, socket, obj_type, 0,
                                                  core_id, PRRTE_HWLOC_AVAILABLE,
                                                  NULL))) {
                        prrte_argv_free(list);
                        prrte_argv_free(range);
                        prrte_argv_free(socket_core);
                        return PRRTE_ERR_NOT_FOUND;
                    }
                    /* get the cpus */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                prrte_argv_free(list);
                break;

            case 2:  /* range of core id's was given */
                prrte_output_verbose(5, prrte_hwloc_base_output,
                                    "range of cores given: start %s stop %s",
                                    range[0], range[1]);
                lower_range = atoi(range[0]);
                upper_range = atoi(range[1]);
                for (core_id=lower_range; core_id <= upper_range; core_id++) {
                    /* get that object */
                    if (NULL == (core = df_search(topo, socket, obj_type, 0,
                                                  core_id, PRRTE_HWLOC_AVAILABLE,
                                                  NULL))) {
                        prrte_argv_free(range);
                        prrte_argv_free(socket_core);
                        return PRRTE_ERR_NOT_FOUND;
                    }
                    /* get the cpus add them into the result */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                break;

            default:
                prrte_argv_free(range);
                prrte_argv_free(socket_core);
                return PRRTE_ERROR;
            }
            prrte_argv_free(range);
        }
    }
    prrte_argv_free(socket_core);

    return rc;
}

int prrte_hwloc_base_cpu_list_parse(const char *slot_str,
                                    hwloc_topology_t topo,
                                    prrte_hwloc_resource_type_t rtype,
                                    hwloc_cpuset_t cpumask)
{
    char **item, **rngs;
    int rc, i, j, k;
    hwloc_obj_t pu;
    char **range, **list;
    size_t range_cnt;
    int core_id, lower_range, upper_range;

    /* bozo checks */
    if (NULL == prrte_hwloc_topology) {
        return PRRTE_ERR_NOT_SUPPORTED;
    }
    if (NULL == slot_str || 0 == strlen(slot_str)) {
        return PRRTE_ERR_BAD_PARAM;
    }

    prrte_output_verbose(5, prrte_hwloc_base_output,
                        "slot assignment: slot_list == %s",
                        slot_str);

    /* split at ';' */
    item = prrte_argv_split(slot_str, ';');

    /* start with a clean mask */
    hwloc_bitmap_zero(cpumask);
    /* loop across the items and accumulate the mask */
    for (i=0; NULL != item[i]; i++) {
        prrte_output_verbose(5, prrte_hwloc_base_output,
                            "working assignment %s",
                            item[i]);
        /* if they specified "socket" by starting with an S/s,
         * or if they use socket:core notation, then parse the
         * socket/core info
         */
        if ('S' == item[i][0] ||
            's' == item[i][0] ||
            NULL != strchr(item[i], ':')) {
            /* specified a socket */
            if (NULL == strchr(item[i], ':')) {
                /* binding just to the socket level, though
                 * it could specify multiple sockets
                 * Skip the S and look for a ranges
                 */
                rngs = prrte_argv_split(&item[i][1], ',');
                for (j=0; NULL != rngs[j]; j++) {
                    if (PRRTE_SUCCESS != (rc = socket_to_cpu_set(rngs[j], topo, rtype, cpumask))) {
                        prrte_argv_free(rngs);
                        prrte_argv_free(item);
                        return rc;
                    }
                }
                prrte_argv_free(rngs);
            } else {
                /* binding to a socket/whatever specification */
                if ('S' == item[i][0] ||
                    's' == item[i][0]) {
                    rngs = prrte_argv_split(&item[i][1], ',');
                    for (j=0; NULL != rngs[j]; j++) {
                        if (PRRTE_SUCCESS != (rc = socket_core_to_cpu_set(rngs[j], topo, rtype, cpumask))) {
                            prrte_argv_free(rngs);
                            prrte_argv_free(item);
                            return rc;
                        }
                    }
                    prrte_argv_free(rngs);
                } else {
                    rngs = prrte_argv_split(item[i], ',');
                    for (j=0; NULL != rngs[j]; j++) {
                        if (PRRTE_SUCCESS != (rc = socket_core_to_cpu_set(rngs[j], topo, rtype, cpumask))) {
                            prrte_argv_free(rngs);
                            prrte_argv_free(item);
                            return rc;
                        }
                    }
                    prrte_argv_free(rngs);
                }
            }
        } else {
            rngs = prrte_argv_split(item[i], ',');
            for (k=0; NULL != rngs[k]; k++) {
                /* just a core specification - see if one or a range was given */
                range = prrte_argv_split(rngs[k], '-');
                range_cnt = prrte_argv_count(range);
                /* see if a range was set or not */
                switch (range_cnt) {
                case 1:  /* only one core, or a list of cores, specified */
                    list = prrte_argv_split(range[0], ',');
                    for (j=0; NULL != list[j]; j++) {
                        core_id = atoi(list[j]);
                        /* find the specified available cpu */
                        if (NULL == (pu = prrte_hwloc_base_get_pu(topo, core_id, rtype))) {
                            prrte_argv_free(range);
                            prrte_argv_free(item);
                            prrte_argv_free(rngs);
                            prrte_argv_free(list);
                            return PRRTE_ERR_SILENT;
                        }
                        /* get the cpus for that object and set them in the massk*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    prrte_argv_free(list);
                    break;

                case 2:  /* range of core id's was given */
                    lower_range = atoi(range[0]);
                    upper_range = atoi(range[1]);
                    for (core_id=lower_range; core_id <= upper_range; core_id++) {
                        /* find the specified logical available cpu */
                        if (NULL == (pu = prrte_hwloc_base_get_pu(topo, core_id, rtype))) {
                            prrte_argv_free(range);
                            prrte_argv_free(item);
                            prrte_argv_free(rngs);
                            return PRRTE_ERR_SILENT;
                        }
                        /* get the cpus for that object and set them in the mask*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    break;

                default:
                    prrte_argv_free(range);
                    prrte_argv_free(item);
                    prrte_argv_free(rngs);
                    return PRRTE_ERROR;
                }
                prrte_argv_free(range);
            }
            prrte_argv_free(rngs);
        }
    }
    prrte_argv_free(item);
    return PRRTE_SUCCESS;
}

prrte_hwloc_locality_t prrte_hwloc_base_get_relative_locality(hwloc_topology_t topo,
                                                            char *cpuset1, char *cpuset2)
{
    prrte_hwloc_locality_t locality;
    hwloc_obj_t obj;
    unsigned depth, d, width, w;
    bool shared;
    hwloc_obj_type_t type;
    int sect1, sect2;
    hwloc_cpuset_t loc1, loc2;

    /* start with what we know - they share a node on a cluster
     * NOTE: we may alter that latter part as hwloc's ability to
     * sense multi-cu, multi-cluster systems grows
     */
    locality = PRRTE_PROC_ON_NODE | PRRTE_PROC_ON_HOST | PRRTE_PROC_ON_CU | PRRTE_PROC_ON_CLUSTER;

    /* if either cpuset is NULL, then that isn't bound */
    if (NULL == cpuset1 || NULL == cpuset2) {
        return locality;
    }

    /* get the max depth of the topology */
    depth = hwloc_topology_get_depth(topo);

    /* convert the strings to cpusets */
    loc1 = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(loc1, cpuset1);
    loc2 = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(loc2, cpuset2);

    /* start at the first depth below the top machine level */
    for (d=1; d < depth; d++) {
        shared = false;
        /* get the object type at this depth */
        type = hwloc_get_depth_type(topo, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NODE != type &&
            HWLOC_OBJ_SOCKET != type &&
#if HWLOC_API_VERSION < 0x20000
            HWLOC_OBJ_CACHE != type &&
#else
            HWLOC_OBJ_L3CACHE != type &&
            HWLOC_OBJ_L2CACHE != type &&
            HWLOC_OBJ_L1CACHE != type &&
#endif
            HWLOC_OBJ_CORE != type &&
            HWLOC_OBJ_PU != type) {
            continue;
        }
        /* get the width of the topology at this depth */
        width = hwloc_get_nbobjs_by_depth(topo, d);

        /* scan all objects at this depth to see if
         * our locations overlap with them
         */
        for (w=0; w < width; w++) {
            /* get the object at this depth/index */
            obj = hwloc_get_obj_by_depth(topo, d, w);
            /* see if our locations intersect with the cpuset for this obj */
            sect1 = hwloc_bitmap_intersects(obj->cpuset, loc1);
            sect2 = hwloc_bitmap_intersects(obj->cpuset, loc2);
            /* if both intersect, then we share this level */
            if (sect1 && sect2) {
                shared = true;
                switch(obj->type) {
                case HWLOC_OBJ_NODE:
                    locality |= PRRTE_PROC_ON_NUMA;
                    break;
                case HWLOC_OBJ_SOCKET:
                    locality |= PRRTE_PROC_ON_SOCKET;
                    break;
#if HWLOC_API_VERSION < 0x20000
                case HWLOC_OBJ_CACHE:
                    if (3 == obj->attr->cache.depth) {
                        locality |= PRRTE_PROC_ON_L3CACHE;
                    } else if (2 == obj->attr->cache.depth) {
                        locality |= PRRTE_PROC_ON_L2CACHE;
                    } else {
                        locality |= PRRTE_PROC_ON_L1CACHE;
                    }
                    break;
#else
                case HWLOC_OBJ_L3CACHE:
                    locality |= PRRTE_PROC_ON_L3CACHE;
                    break;
                case HWLOC_OBJ_L2CACHE:
                    locality |= PRRTE_PROC_ON_L2CACHE;
                    break;
                case HWLOC_OBJ_L1CACHE:
                    locality |= PRRTE_PROC_ON_L1CACHE;
                    break;
#endif
                case HWLOC_OBJ_CORE:
                    locality |= PRRTE_PROC_ON_CORE;
                    break;
                case HWLOC_OBJ_PU:
                    locality |= PRRTE_PROC_ON_HWTHREAD;
                    break;
                default:
                    /* just ignore it */
                    break;
                }
                break;
            }
            /* otherwise, we don't share this
             * object - but we still might share another object
             * on this level, so we have to keep searching
             */
        }
        /* if we spanned the entire width without finding
         * a point of intersection, then no need to go
         * deeper
         */
        if (!shared) {
            break;
        }
    }

    prrte_output_verbose(5, prrte_hwloc_base_output,
                        "locality: %s",
                        prrte_hwloc_base_print_locality(locality));
    hwloc_bitmap_free(loc1);
    hwloc_bitmap_free(loc2);

    return locality;
}

/* searches the given topology for coprocessor objects and returns
 * their serial numbers as a comma-delimited string, or NULL
 * if no coprocessors are found
 */
char* prrte_hwloc_base_find_coprocessors(hwloc_topology_t topo)
{
#if HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC
    hwloc_obj_t osdev;
    unsigned i;
    char **cps = NULL;
#endif
    char *cpstring = NULL;
    int depth;

    /* coprocessors are recorded under OS_DEVICEs, so first
     * see if we have any of those
     */
    if (HWLOC_TYPE_DEPTH_UNKNOWN == (depth = hwloc_get_type_depth(topo, HWLOC_OBJ_OS_DEVICE))) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:find_coprocessors: NONE FOUND IN TOPO"));
        return NULL;
    }
#if HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC
    /* check the device objects for coprocessors */
    osdev = hwloc_get_obj_by_depth(topo, depth, 0);
    while (NULL != osdev) {
        if (HWLOC_OBJ_OSDEV_COPROC == osdev->attr->osdev.type) {
            /* got one! find and save its serial number */
            for (i=0; i < osdev->infos_count; i++) {
                if (0 == strncmp(osdev->infos[i].name, "MICSerialNumber", strlen("MICSerialNumber"))) {
                    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                                         "hwloc:base:find_coprocessors: coprocessor %s found",
                                         osdev->infos[i].value));
                    prrte_argv_append_nosize(&cps, osdev->infos[i].value);
                }
            }
        }
        osdev = osdev->next_cousin;
    }
    if (NULL != cps) {
        cpstring = prrte_argv_join(cps, ',');
        prrte_argv_free(cps);
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:find_coprocessors: hosting coprocessors %s",
                         (NULL == cpstring) ? "NONE" : cpstring));
#else
    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:find_coprocessors: the version of hwloc that PRRTE was built against (v%d.%d.%d) does not support detecting coprocessors",
                         (HWLOC_API_VERSION>>16)&&0xFF, (HWLOC_API_VERSION>>8)&0xFF, HWLOC_API_VERSION&&0xFF));
#endif
    return cpstring;
}

#define PRRTE_HWLOC_MAX_ELOG_LINE 1024

static char *hwloc_getline(FILE *fp)
{
    char *ret, *buff;
    char input[PRRTE_HWLOC_MAX_ELOG_LINE];

    ret = fgets(input, PRRTE_HWLOC_MAX_ELOG_LINE, fp);
    if (NULL != ret) {
           input[strlen(input)-1] = '\0';  /* remove newline */
           buff = strdup(input);
           return buff;
    }

    return NULL;
}

/* checks local environment to determine if this process
 * is on a coprocessor - if so, it returns the serial number
 * as a string, or NULL if it isn't on a coprocessor
 */
char* prrte_hwloc_base_check_on_coprocessor(void)
{
    /* this support currently is limited to Intel Phi processors
     * but will hopefully be extended as we get better, more
     * generalized ways of identifying coprocessors
     */
    FILE *fp;
    char *t, *cptr, *e, *cp=NULL;

    if (PRRTE_SUCCESS != prrte_os_dirpath_access("/proc/elog", S_IRUSR)) {
        /* if the file isn't there, or we don't have permission
         * to read it, then we are not on a coprocessor so far
         * as we can tell
         */
        return NULL;
    }
    if (NULL == (fp = fopen("/proc/elog", "r"))) {
        /* nothing we can do */
        return NULL;
    }
    /* look for the line containing the serial number of this
     * card - usually the first line in the file
     */
    while (NULL != (cptr = hwloc_getline(fp))) {
        if (NULL != (t = strstr(cptr, "Card"))) {
            /* we want the string right after this - delimited by
             * a colon at the end
             */
            t += 5;  // move past "Card "
            if (NULL == (e = strchr(t, ':'))) {
                /* not what we were expecting */
                free(cptr);
                continue;
            }
            *e = '\0';
            cp = strdup(t);
            free(cptr);
            break;
        }
        free(cptr);
    }
    fclose(fp);
    PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                         "hwloc:base:check_coprocessor: on coprocessor %s",
                         (NULL == cp) ? "NONE" : cp));
    return cp;
}

char* prrte_hwloc_base_print_binding(prrte_binding_policy_t binding)
{
    char *ret, *bind;
    prrte_hwloc_print_buffers_t *ptr;

    switch(PRRTE_GET_BINDING_POLICY(binding)) {
    case PRRTE_BIND_TO_NONE:
        bind = "NONE";
        break;
    case PRRTE_BIND_TO_BOARD:
        bind = "BOARD";
        break;
    case PRRTE_BIND_TO_NUMA:
        bind = "NUMA";
        break;
    case PRRTE_BIND_TO_SOCKET:
        bind = "SOCKET";
        break;
    case PRRTE_BIND_TO_L3CACHE:
        bind = "L3CACHE";
        break;
    case PRRTE_BIND_TO_L2CACHE:
        bind = "L2CACHE";
        break;
    case PRRTE_BIND_TO_L1CACHE:
        bind = "L1CACHE";
        break;
    case PRRTE_BIND_TO_CORE:
        bind = "CORE";
        break;
    case PRRTE_BIND_TO_HWTHREAD:
        bind = "HWTHREAD";
        break;
    case PRRTE_BIND_TO_CPUSET:
        bind = "CPUSET";
        break;
    default:
        bind = "UNKNOWN";
    }
    ptr = prrte_hwloc_get_print_buffer();
    if (NULL == ptr) {
        return prrte_hwloc_print_null;
    }
    /* cycle around the ring */
    if (PRRTE_HWLOC_PRINT_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }
    if (!PRRTE_BINDING_REQUIRED(binding) &&
        PRRTE_BIND_OVERLOAD_ALLOWED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRRTE_HWLOC_PRINT_MAX_SIZE,
                 "%s:IF-SUPPORTED:OVERLOAD-ALLOWED", bind);
    } else if (PRRTE_BIND_OVERLOAD_ALLOWED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRRTE_HWLOC_PRINT_MAX_SIZE,
                 "%s:OVERLOAD-ALLOWED", bind);
    } else if (!PRRTE_BINDING_REQUIRED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRRTE_HWLOC_PRINT_MAX_SIZE,
                 "%s:IF-SUPPORTED", bind);
    } else {
        snprintf(ptr->buffers[ptr->cntr], PRRTE_HWLOC_PRINT_MAX_SIZE, "%s", bind);
    }
    ret = ptr->buffers[ptr->cntr];
    ptr->cntr++;

    return ret;
}

/*
 * Turn an int bitmap to a "a-b,c" range kind of string
 */
static char *bitmap2rangestr(int bitmap)
{
    size_t i;
    int range_start, range_end;
    bool first, isset;
    char tmp[BUFSIZ];
    const int stmp = sizeof(tmp) - 1;
    static char ret[BUFSIZ];

    memset(ret, 0, sizeof(ret));

    first = true;
    range_start = -999;
    for (i = 0; i < sizeof(int) * 8; ++i) {
        isset = (bitmap & (1 << i));

        /* Do we have a running range? */
        if (range_start >= 0) {
            if (isset) {
                continue;
            } else {
                /* A range just ended; output it */
                if (!first) {
                    strncat(ret, ",", sizeof(ret) - strlen(ret) - 1);
                } else {
                    first = false;
                }

                range_end = i - 1;
                if (range_start == range_end) {
                    snprintf(tmp, stmp, "%d", range_start);
                } else {
                    snprintf(tmp, stmp, "%d-%d", range_start, range_end);
                }
                strncat(ret, tmp, sizeof(ret) - strlen(ret) - 1);

                range_start = -999;
            }
        }

        /* No running range */
        else {
            if (isset) {
                range_start = i;
            }
        }
    }

    /* If we ended the bitmap with a range open, output it */
    if (range_start >= 0) {
        if (!first) {
            strncat(ret, ",", sizeof(ret) - strlen(ret) - 1);
            first = false;
        }

        range_end = i - 1;
        if (range_start == range_end) {
            snprintf(tmp, stmp, "%d", range_start);
        } else {
            snprintf(tmp, stmp, "%d-%d", range_start, range_end);
        }
        strncat(ret, tmp, sizeof(ret) - strlen(ret) - 1);
    }

    return ret;
}

/*
 * Make a map of socket/core/hwthread tuples
 */
static int build_map(int *num_sockets_arg, int *num_cores_arg,
                     hwloc_cpuset_t cpuset, int ***map, hwloc_topology_t topo)
{
    int num_sockets, num_cores;
    int socket_index, core_index, pu_index;
    hwloc_obj_t socket, core, pu;
    int **data;

    /* Find out how many sockets we have */
    num_sockets = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_SOCKET);
    /* some systems (like the iMac) only have one
     * socket and so don't report a socket
     */
    if (0 == num_sockets) {
        num_sockets = 1;
    }
    /* Lazy: take the total number of cores that we have in the
       topology; that'll be more than the max number of cores
       under any given socket */
    num_cores = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
    *num_sockets_arg = num_sockets;
    *num_cores_arg = num_cores;

    /* Alloc a 2D array: sockets x cores. */
    data = malloc(num_sockets * sizeof(int *));
    if (NULL == data) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    data[0] = calloc(num_sockets * num_cores, sizeof(int));
    if (NULL == data[0]) {
        free(data);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    for (socket_index = 1; socket_index < num_sockets; ++socket_index) {
        data[socket_index] = data[socket_index - 1] + num_cores;
    }

    /* Iterate the PUs in this cpuset; fill in the data[][] array with
       the socket/core/pu triples */
    for (pu_index = 0,
             pu = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                      cpuset, HWLOC_OBJ_PU,
                                                      pu_index);
         NULL != pu;
         pu = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                  cpuset, HWLOC_OBJ_PU,
                                                  ++pu_index)) {
        /* Go upward and find the core this PU belongs to */
        core = pu;
        while (NULL != core && core->type != HWLOC_OBJ_CORE) {
            core = core->parent;
        }
        core_index = 0;
        if (NULL != core) {
            core_index = core->logical_index;
        }

        /* Go upward and find the socket this PU belongs to */
        socket = pu;
        while (NULL != socket && socket->type != HWLOC_OBJ_SOCKET) {
            socket = socket->parent;
        }
        socket_index = 0;
        if (NULL != socket) {
            socket_index = socket->logical_index;
        }

        /* Save this socket/core/pu combo.  LAZY: Assuming that we
           won't have more PU's per core than (sizeof(int)*8). */
        data[socket_index][core_index] |= (1 << pu->sibling_rank);
    }

    *map = data;
    return PRRTE_SUCCESS;
}

/*
 * Make a prettyprint string for a hwloc_cpuset_t
 */
int prrte_hwloc_base_cset2str(char *str, int len,
                             hwloc_topology_t topo,
                             hwloc_cpuset_t cpuset)
{
    bool first;
    int num_sockets, num_cores;
    int ret, socket_index, core_index;
    char tmp[2048];
    const int stmp = sizeof(tmp) - 1;
    int **map=NULL;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *sum;

    str[0] = tmp[stmp] = '\0';

    /* if the cpuset is all zero, then not bound */
    if (hwloc_bitmap_iszero(cpuset)) {
        return PRRTE_ERR_NOT_BOUND;
    }

    /* if the cpuset includes all available cpus, then we are unbound */
    root = hwloc_get_root_obj(topo);
    if (NULL != root->userdata) {
        sum = (prrte_hwloc_topo_data_t*)root->userdata;
        if (NULL == sum->available) {
           return PRRTE_ERROR;
        }
        if (0 != hwloc_bitmap_isincluded(sum->available, cpuset)) {
            return PRRTE_ERR_NOT_BOUND;
        }
    }

    if (PRRTE_SUCCESS != (ret = build_map(&num_sockets, &num_cores, cpuset, &map, topo))) {
        return ret;
    }
    /* Iterate over the data matrix and build up the string */
    first = true;
    for (socket_index = 0; socket_index < num_sockets; ++socket_index) {
        for (core_index = 0; core_index < num_cores; ++core_index) {
            if (map[socket_index][core_index] > 0) {
                if (!first) {
                    strncat(str, ", ", len - strlen(str) - 1);
                }
                first = false;

                snprintf(tmp, stmp, "socket %d[core %d[hwt %s]]",
                         socket_index, core_index,
                         bitmap2rangestr(map[socket_index][core_index]));
                strncat(str, tmp, len - strlen(str) - 1);
            }
        }
    }
    if (NULL != map) {
        if (NULL != map[0]) {
            free(map[0]);
        }
        free(map);
    }

    return PRRTE_SUCCESS;
}


/* given an input obj somewhere in the hwloc tree, look for a
 * numa object that contains it
 */
static hwloc_obj_t find_my_numa(hwloc_obj_t obj)
{
    hwloc_obj_t p;

#if HWLOC_API_VERSION >= 0x20000
    size_t i;
    hwloc_obj_t numa;

    p = obj;
    while (NULL != p && 0 == p->memory_arity) {
        p = p->parent;
    }
    // p should have either found a level that contains numas or reached NULL
    if (NULL == p) {
        return NULL;
    }
    for (i=0; i < p->memory_arity; ++i) {
        numa = &(p->memory_first_child[i]);

        if (hwloc_bitmap_isincluded(obj->complete_cpuset, numa->complete_cpuset)) {
            return numa;
        }
    }
#else
    p = obj;
    while (NULL != p && HWLOC_OBJ_NUMANODE != p->type) {
        p = p->parent;
    }
    // p should have either found a level that contains numas or reached NULL
    if (NULL == p) {
        return NULL;
    }
    if (hwloc_bitmap_isincluded(obj->complete_cpuset, p->complete_cpuset)) {
        return p;
    }
#endif
    return NULL;
}

/* which level from the set {socket, core, pu} has
 * the first descendent underneath the lowest numa level.
 * returns MACHINE if there is no numa level
 *
 * Eg if an hwloc tree had numas containing sockets like this
 *   <[../..][../..]><[../..][../..]>
 * the tree would be
 *   mach               +memory_children: n n
 *   s   s   s   s
 *   c c c c c c c c
 *   pppppppppppppppp
 * so this should return SOCKET
 */
static hwloc_obj_type_t first_type_under_a_numa(hwloc_topology_t topo)
{
    hwloc_obj_t p;

    p = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, 0);
#if HWLOC_API_VERSION >= 0x20000
    hwloc_obj_type_t type;

    /* climb the ladder */
    while (NULL != p && 0 == p->memory_arity) {
        if (HWLOC_OBJ_PU == p->type||
            HWLOC_OBJ_CORE == p->type ||
            HWLOC_OBJ_SOCKET == p->type) {
            type = p->type;
        }
        p = p->parent;
    }
    if (NULL != p && 0 < p->memory_arity) {
        return type;
    }
#else
    /* climb the ladder */
    while (NULL != p) {
        if (NULL == p->parent) {
            /* we are at the top and have not
             * found a NUMANODE, so the entire
             * object must be one big NUMANODE */
            return HWLOC_OBJ_MACHINE;
        }
        if (HWLOC_OBJ_NUMANODE == p->parent->type) {
            return p->type;
        }
        p = p->parent;
    }
#endif
    return HWLOC_OBJ_MACHINE;
}

/*
 * Make a prettyprint string for a cset in a map format with NUMA markers.
 * Example: [B./..]
 * Key:  [] - signifies socket
 *       <> - signifies numa
 *        / - divider between cores
 *        . - signifies PU a process not bound to
 *        B - signifies PU a process is bound to
 *        ~ - signifies PU that is disallowed, eg not in our cgroup:
 */
int prrte_hwloc_base_cset2mapstr(char *str, int len,
                                hwloc_topology_t topo,
                                hwloc_cpuset_t cpuset)
{
    char tmp[BUFSIZ];
    int core_index, pu_index;
    hwloc_obj_t socket, core, pu;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *sum;
    bool fake_on_first_socket;
    bool fake_on_first_core;
    hwloc_cpuset_t cpuset_for_socket;
    hwloc_cpuset_t cpuset_for_core;
    hwloc_obj_t prev_numa = NULL;
    hwloc_obj_t cur_numa = NULL;
    hwloc_obj_type_t type_under_numa;
    bool a_numa_marker_is_open = false;

   /* if the cpuset is all zero, then not bound */
    if (hwloc_bitmap_iszero(cpuset)) {
        return PRRTE_ERR_NOT_BOUND;
    }

    str[0] = '\0';
    memset(tmp, 0, BUFSIZ);

     /* if the cpuset includes all available cpus, then we are unbound */
    root = hwloc_get_root_obj(topo);
    if (NULL == root->userdata) {
        /* this should never happen */
        return PRRTE_ERROR;
    }
    sum = (prrte_hwloc_topo_data_t*)root->userdata;
    if (NULL == sum->available) {
        /* again, should never happen */
       return PRRTE_ERROR;
    }
    if (0 != hwloc_bitmap_isincluded(sum->available, cpuset)) {
        return PRRTE_ERR_NOT_BOUND;
    }

    /* hwloc trees aren't required to have sockets and cores,
     * just a MACHINE at the top and PU at the bottom. The 'fake_*' vars make
     * the loops always iterate at least once, even if the initial socket = ...
     * etc lookup is NULL. So we have to take a little extra care here in
     * case we are in a no-socket or no-core scenario. Thankfully, everyone
     * still has NUMA regions! */
    type_under_numa = first_type_under_a_numa(topo);

    /* Iterate over all existing sockets */
    fake_on_first_socket = true;
    socket = hwloc_get_obj_by_type(topo, HWLOC_OBJ_SOCKET, 0);
    do {
        fake_on_first_socket = false;
        strncat(str, "[", len - strlen(str) - 1);

        // if numas contain sockets, example output <[../..][../..]><[../..][../..]>
        if (HWLOC_OBJ_SOCKET == type_under_numa) {
            prev_numa = cur_numa;
            cur_numa = find_my_numa(socket);
            if (cur_numa && cur_numa != prev_numa) {
                if (a_numa_marker_is_open) {
                    strncat(str, ">", len - strlen(str) - 1);
                }
                strncat(str, "<", len - strlen(str) - 1);
                a_numa_marker_is_open = true;
            }
        }
        if (NULL != socket) {
            strncat(str, "[", len - strlen(str) - 1);
            cpuset_for_socket = socket->complete_cpuset;
        } else {
            cpuset_for_socket = root->complete_cpuset;
        }

        /* Iterate over all existing cores in this socket */
        fake_on_first_core = true;
        core_index = 0;
        core = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                   cpuset_for_socket,
                                                   HWLOC_OBJ_CORE, core_index);
        while (NULL != core || fake_on_first_core) {
            fake_on_first_core = false;

            /* if numas contain cores and are contained by sockets,
             * example output [<../..><../..>][<../../../..>]
             */
            if (HWLOC_OBJ_CORE == type_under_numa) {
                prev_numa = cur_numa;
                cur_numa = find_my_numa(core);
                if (cur_numa && cur_numa != prev_numa) {
                    if (a_numa_marker_is_open) {
                        strncat(str, ">", len - strlen(str) - 1);
                    }
                    strncat(str, "<", len - strlen(str) - 1);
                    a_numa_marker_is_open = true;
                }
            }


            if (0 < core_index) {
                strncat(str, "/", len - strlen(str) - 1);
            }

            if (NULL != core) {
                cpuset_for_core = core->complete_cpuset;
            } else {
                cpuset_for_core = cpuset_for_socket;
            }

            /* Iterate over all existing PUs in this core */
            pu_index = 0;
            pu = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                     cpuset_for_core,
                                                     HWLOC_OBJ_PU, pu_index);
            while (NULL != pu) {
                /* if numas contain PU and are contained by cores (seems unlikely)
                 * example output [<..../....>/<..../....>/<..../....>/<..../....>]
                 */
                if (HWLOC_OBJ_PU == type_under_numa) {
                    prev_numa = cur_numa;
                    cur_numa = find_my_numa(pu);
                    if (cur_numa && cur_numa != prev_numa) {
                        if (a_numa_marker_is_open) {
                            strncat(str, ">", len - strlen(str) - 1);
                        }
                        strncat(str, "<", len - strlen(str) - 1);
                        a_numa_marker_is_open = true;
                    }
                }

                /* Is this PU in the cpuset? */
                if (hwloc_bitmap_isset(cpuset, pu->os_index)) {
                    strncat(str, "B", len - strlen(str) - 1);
                } else {
                    if (hwloc_bitmap_isset(sum->available, pu->os_index)) {
                        strncat(str, ".", len - strlen(str) - 1);
                    } else {
                        strncat(str, "~", len - strlen(str) - 1);
                    }
                }
                pu = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                          cpuset_for_core,
                                                          HWLOC_OBJ_PU, ++pu_index);
            }  /* end while pu */
            if (HWLOC_OBJ_PU == type_under_numa) {
                if (a_numa_marker_is_open) {
                    strncat(str, ">", len - strlen(str) - 1);
                    a_numa_marker_is_open = false;
                }
            }
            core = hwloc_get_obj_inside_cpuset_by_type(topo,
                                                        cpuset_for_socket,
                                                        HWLOC_OBJ_CORE, ++core_index);
        } /* end while core */
        if (HWLOC_OBJ_CORE == type_under_numa) {
            if (a_numa_marker_is_open) {
                strncat(str, ">", len - strlen(str) - 1);
                a_numa_marker_is_open = false;
            }
        }
        if (NULL != socket) {
            strncat(str, "]", len - strlen(str) - 1);
            socket = socket->next_cousin;
        }
    } while (NULL != socket || fake_on_first_socket);

    if (HWLOC_OBJ_SOCKET == type_under_numa) {
        if (a_numa_marker_is_open) {
            strncat(str, ">", len - strlen(str) - 1);
            a_numa_marker_is_open = false;
        }
    }

    return PRRTE_SUCCESS;
}

static int dist_cmp_fn (prrte_list_item_t **a, prrte_list_item_t **b)
{
    prrte_rmaps_numa_node_t *aitem = *((prrte_rmaps_numa_node_t **) a);
    prrte_rmaps_numa_node_t *bitem = *((prrte_rmaps_numa_node_t **) b);

    if (aitem->dist_from_closed > bitem->dist_from_closed) {
        return 1;
    } else if( aitem->dist_from_closed == bitem->dist_from_closed ) {
        return 0;
    } else {
        return -1;
    }
}

static void sort_by_dist(hwloc_topology_t topo, char* device_name, prrte_list_t *sorted_list)
{
    hwloc_obj_t device_obj = NULL;
    hwloc_obj_t obj = NULL;
    struct hwloc_distances_s* distances;
    prrte_rmaps_numa_node_t *numa_node;
    int close_node_index;
    float latency;
    unsigned int j;
#if HWLOC_API_VERSION < 0x20000
    hwloc_obj_t root = NULL;
    int depth;
    unsigned i;
#else
    unsigned distances_nr = 0;
#endif

    for (device_obj = hwloc_get_obj_by_type(topo, HWLOC_OBJ_OS_DEVICE, 0); device_obj; device_obj = hwloc_get_next_osdev(topo, device_obj)) {
        if (device_obj->attr->osdev.type == HWLOC_OBJ_OSDEV_OPENFABRICS
                || device_obj->attr->osdev.type == HWLOC_OBJ_OSDEV_NETWORK) {
            if (!strcmp(device_obj->name, device_name)) {
                /* find numa node containing this device */
                obj = device_obj->parent;
#if HWLOC_API_VERSION < 0x20000
                while ((obj != NULL) && (obj->type != HWLOC_OBJ_NODE)) {
                    obj = obj->parent;
                }
#else
                while (obj && !obj->memory_arity) {
                    obj = obj->parent; /* no memory child, walk up */
                }
                if (obj != NULL) {
                    obj = obj->memory_first_child;
                }
#endif
                if (obj == NULL) {
                    prrte_output_verbose(5, prrte_hwloc_base_output,
                            "hwloc:base:get_sorted_numa_list: NUMA node closest to %s wasn't found.",
                            device_name);
                    return;
                } else {
                    close_node_index = obj->logical_index;
                }

                /* find distance matrix for all numa nodes */
#if HWLOC_API_VERSION < 0x20000
                distances = (struct hwloc_distances_s*)hwloc_get_whole_distance_matrix_by_type(topo, HWLOC_OBJ_NODE);
                if (NULL ==  distances) {
                    /* we can try to find distances under group object. This info can be there. */
                    depth = hwloc_get_type_depth(topo, HWLOC_OBJ_NODE);
                    if (HWLOC_TYPE_DEPTH_UNKNOWN == depth) {
                        prrte_output_verbose(5, prrte_hwloc_base_output,
                                "hwloc:base:get_sorted_numa_list: There is no information about distances on the node.");
                        return;
                    }
                    root = hwloc_get_root_obj(topo);
                    for (i = 0; i < root->arity; i++) {
                        obj = root->children[i];
                        if (obj->distances_count > 0) {
                            for(j = 0; j < obj->distances_count; j++) {
                                if (obj->distances[j]->relative_depth + 1 == (unsigned) depth) {
                                    distances = obj->distances[j];
                                    break;
                                }
                            }
                        }
                    }
                }
                /* find all distances for our close node with logical index = close_node_index as close_node_index + nbobjs*j */
                if ((NULL == distances) || (0 == distances->nbobjs)) {
                    prrte_output_verbose(5, prrte_hwloc_base_output,
                            "hwloc:base:get_sorted_numa_list: There is no information about distances on the node.");
                    return;
                }
                /* fill list of numa nodes */
                for (j = 0; j < distances->nbobjs; j++) {
                    latency = distances->latency[close_node_index + distances->nbobjs * j];
                    numa_node = PRRTE_NEW(prrte_rmaps_numa_node_t);
                    numa_node->index = j;
                    numa_node->dist_from_closed = latency;
                    prrte_list_append(sorted_list, &numa_node->super);
                }
#else
                distances_nr = 1;
                if (0 != hwloc_distances_get_by_type(topo, HWLOC_OBJ_NODE, &distances_nr, &distances,
                                                     HWLOC_DISTANCES_KIND_MEANS_LATENCY, 0) || 0 == distances_nr) {
                    prrte_output_verbose(5, prrte_hwloc_base_output,
                            "hwloc:base:get_sorted_numa_list: There is no information about distances on the node.");
                    return;
                }
                /* fill list of numa nodes */
                for (j = 0; j < distances->nbobjs; j++) {
                    latency = distances->values[close_node_index + distances->nbobjs * j];
                    numa_node = PRRTE_NEW(prrte_rmaps_numa_node_t);
                    numa_node->index = j;
                    numa_node->dist_from_closed = latency;
                    prrte_list_append(sorted_list, &numa_node->super);
                }
                hwloc_distances_release(topo, distances);
#endif
                /* sort numa nodes by distance from the closest one to PCI */
                prrte_list_sort(sorted_list, dist_cmp_fn);
                return;
            }
        }
    }
}

static int find_devices(hwloc_topology_t topo, char** device_name)
{
    hwloc_obj_t device_obj = NULL;
    int count = 0;
    for (device_obj = hwloc_get_obj_by_type(topo, HWLOC_OBJ_OS_DEVICE, 0); device_obj; device_obj = hwloc_get_next_osdev(topo, device_obj)) {
        if (device_obj->attr->osdev.type == HWLOC_OBJ_OSDEV_OPENFABRICS) {
            count++;
            free(*device_name);
            *device_name = strdup(device_obj->name);
        }
    }
    return count;
}

int prrte_hwloc_get_sorted_numa_list(hwloc_topology_t topo, char* device_name, prrte_list_t *sorted_list)
{
    hwloc_obj_t obj;
    prrte_hwloc_summary_t *sum;
    prrte_hwloc_topo_data_t *data;
    prrte_rmaps_numa_node_t *numa, *copy_numa;
    int count;

    obj = hwloc_get_root_obj(topo);

    /* first see if the topology already has this info */
    /* we call prrte_hwloc_base_get_nbobjs_by_type() before it to fill summary object so it should exist*/
    data = (prrte_hwloc_topo_data_t*)obj->userdata;
    if (NULL != data) {
        PRRTE_LIST_FOREACH(sum, &data->summaries, prrte_hwloc_summary_t) {
            if (HWLOC_OBJ_NODE == sum->type) {
                if (prrte_list_get_size(&sum->sorted_by_dist_list) > 0) {
                    PRRTE_LIST_FOREACH(numa, &(sum->sorted_by_dist_list), prrte_rmaps_numa_node_t) {
                        copy_numa = PRRTE_NEW(prrte_rmaps_numa_node_t);
                        copy_numa->index = numa->index;
                        copy_numa->dist_from_closed = numa->dist_from_closed;
                        prrte_list_append(sorted_list, &copy_numa->super);
                    }
                    return PRRTE_SUCCESS;
                }else {
                    /* don't already know it - go get it */
                    /* firstly we check if we need to autodetect OpenFabrics  devices or we have the specified one */
                    bool free_device_name = false;
                    if (!strcmp(device_name, "auto")) {
                        count = find_devices(topo, &device_name);
                        if (count > 1) {
                            free(device_name);
                            return count;
                        }
                        free_device_name = true;
                    }
                    if (!device_name) {
                        return PRRTE_ERR_NOT_FOUND;
                    } else if (free_device_name && (0 == strlen(device_name))) {
                        free(device_name);
                        return PRRTE_ERR_NOT_FOUND;
                    }
                    sort_by_dist(topo, device_name, sorted_list);
                    if (free_device_name) {
                        free(device_name);
                    }
                    /* store this info in summary object for later usage */
                    PRRTE_LIST_FOREACH(numa, sorted_list, prrte_rmaps_numa_node_t) {
                        copy_numa = PRRTE_NEW(prrte_rmaps_numa_node_t);
                        copy_numa->index = numa->index;
                        copy_numa->dist_from_closed = numa->dist_from_closed;
                        prrte_list_append(&(sum->sorted_by_dist_list), &copy_numa->super);
                    }
                    return PRRTE_SUCCESS;
                }
            }
        }
    }
    return PRRTE_ERR_NOT_FOUND;
}

char* prrte_hwloc_base_get_topo_signature(hwloc_topology_t topo)
{
    int nnuma, nsocket, nl3, nl2, nl1, ncore, nhwt;
    char *sig=NULL, *arch = NULL, *endian, *pus, *cpus;
    hwloc_obj_t obj;
    unsigned i;
    hwloc_bitmap_t complete, allowed;

    nnuma = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_NODE, 0, PRRTE_HWLOC_AVAILABLE);
    nsocket = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_SOCKET, 0, PRRTE_HWLOC_AVAILABLE);
    nl3 = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L3CACHE, 3, PRRTE_HWLOC_AVAILABLE);
    nl2 = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L2CACHE, 2, PRRTE_HWLOC_AVAILABLE);
    nl1 = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L1CACHE, 1, PRRTE_HWLOC_AVAILABLE);
    ncore = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE, 0, PRRTE_HWLOC_AVAILABLE);
    nhwt = prrte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PU, 0, PRRTE_HWLOC_AVAILABLE);

    /* get the root object so we can add the processor architecture */
    obj = hwloc_get_root_obj(topo);
    for (i=0; i < obj->infos_count; i++) {
        if (0 == strcmp(obj->infos[i].name, "Architecture")) {
            arch = obj->infos[i].value;
            break;
        }
    }
    if (NULL == arch) {
        arch = "unknown";
    }

#ifdef __BYTE_ORDER
#if __BYTE_ORDER == __LITTLE_ENDIAN
    endian = "le";
#else
    endian = "be";
#endif
#else
    endian = "unknown";
#endif

    /* print the cpu bitmap itself so we can detect mismatches in the available
     * cores across the nodes - we send the complete set along with the available
     * one in cases where the two differ */
    complete = (hwloc_bitmap_t)hwloc_topology_get_complete_cpuset(topo);
    allowed = (hwloc_bitmap_t)hwloc_topology_get_allowed_cpuset(topo);
    if (0 != hwloc_bitmap_list_asprintf(&pus, allowed)) {
        pus = strdup("unknown");
    }
    if (hwloc_bitmap_isequal(complete, allowed)) {
        cpus = strdup("");
    } else {
        if (0 != hwloc_bitmap_list_asprintf(&cpus, complete)) {
            cpus = strdup("unknown");
        }
    }
    prrte_asprintf(&sig, "%dN:%dS:%dL3:%dL2:%dL1:%dC:%dH:%s:%s:%s:%s",
             nnuma, nsocket, nl3, nl2, nl1, ncore, nhwt, pus, cpus, arch, endian);
    free(pus);
    free(cpus);
    return sig;
}

char* prrte_hwloc_base_get_locality_string(hwloc_topology_t topo,
                                          char *bitmap)
{
    hwloc_obj_t obj;
    char *locality=NULL, *tmp, *t2;
    unsigned depth, d, width, w;
    hwloc_cpuset_t cpuset, result;
    hwloc_obj_type_t type;

    /* if this proc is not bound, then there is no locality. We
     * know it isn't bound if the cpuset is NULL, or if it is
     * all 1's */
    if (NULL == bitmap) {
        return NULL;
    }
    cpuset = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(cpuset, bitmap);
    if (hwloc_bitmap_isfull(cpuset)) {
        hwloc_bitmap_free(cpuset);
        return NULL;
    }

    /* we are going to use a bitmap to save the results so
     * that we can use a hwloc utility to print them */
    result = hwloc_bitmap_alloc();

    /* get the max depth of the topology */
    depth = hwloc_topology_get_depth(topo);

    /* start at the first depth below the top machine level */
    for (d=1; d < depth; d++) {
        /* get the object type at this depth */
        type = hwloc_get_depth_type(topo, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NODE != type &&
            HWLOC_OBJ_SOCKET != type &&
#if HWLOC_API_VERSION < 0x20000
            HWLOC_OBJ_CACHE != type &&
#else
            HWLOC_OBJ_L1CACHE != type &&
            HWLOC_OBJ_L2CACHE != type &&
            HWLOC_OBJ_L3CACHE != type &&
#endif
            HWLOC_OBJ_CORE != type &&
            HWLOC_OBJ_PU != type) {
            continue;
        }

        /* get the width of the topology at this depth */
        width = hwloc_get_nbobjs_by_depth(topo, d);
        if (0 == width) {
            continue;
        }

        /* scan all objects at this depth to see if
         * the location overlaps with them
         */
        for (w=0; w < width; w++) {
            /* get the object at this depth/index */
            obj = hwloc_get_obj_by_depth(topo, d, w);
            /* see if the location intersects with it */
            if (hwloc_bitmap_intersects(obj->cpuset, cpuset)) {
                hwloc_bitmap_set(result, w);
            }
        }
        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            switch(obj->type) {
                case HWLOC_OBJ_NODE:
                    prrte_asprintf(&t2, "%sNM%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_SOCKET:
                    prrte_asprintf(&t2, "%sSK%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
#if HWLOC_API_VERSION < 0x20000
                case HWLOC_OBJ_CACHE:
                    if (3 == obj->attr->cache.depth) {
                        prrte_asprintf(&t2, "%sL3%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    } else if (2 == obj->attr->cache.depth) {
                        prrte_asprintf(&t2, "%sL2%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    } else {
                        prrte_asprintf(&t2, "%sL1%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    }
                    break;
#else
                case HWLOC_OBJ_L3CACHE:
                    prrte_asprintf(&t2, "%sL3%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_L2CACHE:
                    prrte_asprintf(&t2, "%sL2%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_L1CACHE:
                    prrte_asprintf(&t2, "%sL1%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
#endif
                case HWLOC_OBJ_CORE:
                    prrte_asprintf(&t2, "%sCR%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_PU:
                    prrte_asprintf(&t2, "%sHT%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                default:
                    /* just ignore it */
                    break;
            }
            free(tmp);
        }
        hwloc_bitmap_zero(result);
    }
    hwloc_bitmap_free(result);
    hwloc_bitmap_free(cpuset);

    /* remove the trailing colon */
    if (NULL != locality) {
        locality[strlen(locality)-1] = '\0';
    }
    return locality;
}

char* prrte_hwloc_base_get_location(char *locality,
                                   hwloc_obj_type_t type,
                                   unsigned index)
{
    char **loc;
    char *srch, *ans = NULL;
    size_t n;

    if (NULL == locality) {
        return NULL;
    }
    switch(type) {
        case HWLOC_OBJ_NODE:
            srch = "NM";
            break;
        case HWLOC_OBJ_SOCKET:
            srch = "SK";
            break;
#if HWLOC_API_VERSION < 0x20000
        case HWLOC_OBJ_CACHE:
            if (3 == index) {
                srch = "L3";
            } else if (2 == index) {
                srch = "L2";
            } else {
                srch = "L0";
            }
            break;
#else
        case HWLOC_OBJ_L3CACHE:
            srch = "L3";
            break;
        case HWLOC_OBJ_L2CACHE:
            srch = "L2";
            break;
        case HWLOC_OBJ_L1CACHE:
            srch = "L0";
            break;
#endif
        case HWLOC_OBJ_CORE:
            srch = "CR";
            break;
        case HWLOC_OBJ_PU:
            srch = "HT";
            break;
        default:
            return NULL;
    }
    loc = prrte_argv_split(locality, ':');
    for (n=0; NULL != loc[n]; n++) {
        if (0 == strncmp(loc[n], srch, 2)) {
            ans = strdup(&loc[n][2]);
            break;
        }
    }
    prrte_argv_free(loc);

    return ans;
}

prrte_hwloc_locality_t prrte_hwloc_compute_relative_locality(char *loc1, char *loc2)
{
    prrte_hwloc_locality_t locality;
    char **set1, **set2;
    hwloc_bitmap_t bit1, bit2;
    size_t n1, n2;

    /* start with what we know - they share a node on a cluster
     * NOTE: we may alter that latter part as hwloc's ability to
     * sense multi-cu, multi-cluster systems grows
     */
    locality = PRRTE_PROC_ON_NODE | PRRTE_PROC_ON_HOST | PRRTE_PROC_ON_CU | PRRTE_PROC_ON_CLUSTER;

    /* if either location is NULL, then that isn't bound */
    if (NULL == loc1 || NULL == loc2) {
        return locality;
    }

    set1 = prrte_argv_split(loc1, ':');
    set2 = prrte_argv_split(loc2, ':');
    bit1 = hwloc_bitmap_alloc();
    bit2 = hwloc_bitmap_alloc();

    /* check each matching type */
    for (n1=0; NULL != set1[n1]; n1++) {
        /* convert the location into bitmap */
        hwloc_bitmap_list_sscanf(bit1, &set1[n1][2]);
        /* find the matching type in set2 */
        for (n2=0; NULL != set2[n2]; n2++) {
            if (0 == strncmp(set1[n1], set2[n2], 2)) {
                /* convert the location into bitmap */
                hwloc_bitmap_list_sscanf(bit2, &set2[n2][2]);
                /* see if they intersect */
                if (hwloc_bitmap_intersects(bit1, bit2)) {
                    /* set the corresponding locality bit */
                    if (0 == strncmp(set1[n1], "NM", 2)) {
                        locality |= PRRTE_PROC_ON_NUMA;
                    } else if (0 == strncmp(set1[n1], "SK", 2)) {
                        locality |= PRRTE_PROC_ON_SOCKET;
                    } else if (0 == strncmp(set1[n1], "L3", 2)) {
                        locality |= PRRTE_PROC_ON_L3CACHE;
                    } else if (0 == strncmp(set1[n1], "L2", 2)) {
                        locality |= PRRTE_PROC_ON_L2CACHE;
                    } else if (0 == strncmp(set1[n1], "L1", 2)) {
                        locality |= PRRTE_PROC_ON_L1CACHE;
                    } else if (0 == strncmp(set1[n1], "CR", 2)) {
                        locality |= PRRTE_PROC_ON_CORE;
                    } else if (0 == strncmp(set1[n1], "HT", 2)) {
                        locality |= PRRTE_PROC_ON_HWTHREAD;
                    } else {
                        /* should never happen */
                        prrte_output(0, "UNRECOGNIZED LOCALITY %s", set1[n1]);
                    }
                }
                break;
            }
        }
    }
    prrte_argv_free(set1);
    prrte_argv_free(set2);
    hwloc_bitmap_free(bit1);
    hwloc_bitmap_free(bit2);
    return locality;
}

int prrte_hwloc_base_topology_export_xmlbuffer(hwloc_topology_t topology, char **xmlpath, int *buflen) {
#if HWLOC_API_VERSION < 0x20000
    return hwloc_topology_export_xmlbuffer(topology, xmlpath, buflen);
#else
    return hwloc_topology_export_xmlbuffer(topology, xmlpath, buflen, 0);
#endif
}

int prrte_hwloc_base_topology_set_flags (hwloc_topology_t topology, unsigned long flags, bool io) {
    if (io) {
#if HWLOC_API_VERSION < 0x20000
        flags |= HWLOC_TOPOLOGY_FLAG_IO_DEVICES;
#else
        int ret = hwloc_topology_set_io_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
        if (0 != ret) return ret;
#endif
    }
    return hwloc_topology_set_flags(topology, flags);
}
