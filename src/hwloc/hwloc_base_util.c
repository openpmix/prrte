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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (C) 2018      Mellanox Technologies, Ltd.
 *                         All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Advanced Micro Devices, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#define PRTE_HWLOC_WANT_SHMEM 1

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_ENDIAN_H
#    include <endian.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#if HAVE_FCNTL_H
#    include <fcntl.h>
#endif

#include "src/include/constants.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_tsd.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "src/hwloc/hwloc-internal.h"

bool prte_hwloc_base_core_cpus(hwloc_topology_t topo)
{
    hwloc_obj_t obj;
    hwloc_obj_t pu;

    obj = hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0);
    if (NULL == obj) {
        return false;
    }
    /* see if the cpuset of a core match that of a PU */
    pu = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, 0);
    /* if the two are equal, then we really don't have
     * cores in this topology */
    if (hwloc_bitmap_isequal(obj->cpuset, pu->cpuset)) {
        return false;
    }
    /* we do have cores */
    return true;
}

/*
 * Provide the hwloc object that corresponds to the given
 * processor id of the given type.  Remember: "processor" here [usually] means "core" --
 * except that on some platforms, hwloc won't find any cores; it'll
 * only find PUs (!).  On such platforms, then do the same calculation
 * but with PUs instead of COREs.
 */
hwloc_obj_t prte_hwloc_base_get_pu(hwloc_topology_t topo, bool use_hwthread_cpus, int lid)
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
    if (use_hwthread_cpus || !prte_hwloc_base_core_cpus(topo)) {
        obj_type = HWLOC_OBJ_PU;
    }

    pmix_output_verbose(5, prte_hwloc_base_output,
                        "Searching for %d LOGICAL PU", lid);

    /* Now do the actual lookup. */
    obj = hwloc_get_obj_by_type(topo, obj_type, lid);
    pmix_output_verbose(5, prte_hwloc_base_output,
                        "logical cpu %d %s found", lid, (NULL == obj) ? "not" : "is");

    /* Found the right core (or PU). Return the object */
    return obj;
}

hwloc_cpuset_t prte_hwloc_base_generate_cpuset(hwloc_topology_t topo,
                                               bool use_hwthread_cpus,
                                               char **cpulist)
{
    hwloc_cpuset_t avail = NULL, pucpus, res;
    char **ranges = NULL, **range = NULL;
    int idx, cpu, start, end;
    hwloc_obj_t pu;
    char **cache = NULL;
    char tmp[256];

    /* find the specified logical cpus */
    ranges = PMIX_ARGV_SPLIT_COMPAT(*cpulist, ',');
    avail = hwloc_bitmap_alloc();
    hwloc_bitmap_zero(avail);
    res = hwloc_bitmap_alloc();
    pucpus = hwloc_bitmap_alloc();
    for (idx = 0; idx < PMIX_ARGV_COUNT_COMPAT(ranges); idx++) {
        range = PMIX_ARGV_SPLIT_COMPAT(ranges[idx], '-');
        switch (PMIX_ARGV_COUNT_COMPAT(range)) {
        case 1:
            /* only one cpu given - get that object */
            cpu = strtoul(range[0], NULL, 10);
            pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, cpu);
            if (NULL == pu) {
                pmix_show_help("help-prte-hwloc-base.txt", "unfound-cpu", true,
                               *cpulist, cpu, "logical",
                               use_hwthread_cpus ? "hwthread" : "core");
                PMIX_ARGV_FREE_COMPAT(ranges);
                PMIX_ARGV_FREE_COMPAT(range);
                PMIX_ARGV_FREE_COMPAT(cache);
                hwloc_bitmap_free(avail);
                hwloc_bitmap_free(res);
                hwloc_bitmap_free(pucpus);
                return NULL;
            }
            hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
            hwloc_bitmap_or(res, avail, pucpus);
            hwloc_bitmap_copy(avail, res);
            // cache the cpu
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cache, range[0]);
            break;
        case 2:
            /* range given */
            start = strtoul(range[0], NULL, 10);
            end = strtoul(range[1], NULL, 10);
            for (cpu = start; cpu <= end; cpu++) {
                pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, cpu);
                if (NULL == pu) {
                    pmix_show_help("help-prte-hwloc-base.txt", "unfound-cpu", true,
                                   cpulist, cpu, "logical",
                                   use_hwthread_cpus ? "hwthread" : "core");
                    PMIX_ARGV_FREE_COMPAT(ranges);
                    PMIX_ARGV_FREE_COMPAT(range);
                    PMIX_ARGV_FREE_COMPAT(cache);
                    hwloc_bitmap_free(avail);
                    hwloc_bitmap_free(res);
                    hwloc_bitmap_free(pucpus);
                    return NULL;
                }
                hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
                hwloc_bitmap_or(res, avail, pucpus);
                hwloc_bitmap_copy(avail, res);
                // cache the cpu
                snprintf(tmp, 256, "%d", cpu);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cache, tmp);
            }
            break;
        default:
            break;
        }
        PMIX_ARGV_FREE_COMPAT(range);
    }
    if (NULL != ranges) {
        PMIX_ARGV_FREE_COMPAT(ranges);
    }
    hwloc_bitmap_free(res);
    hwloc_bitmap_free(pucpus);
    // update the cpulist in case it was expanded
    free(*cpulist);
    *cpulist = PMIX_ARGV_JOIN_COMPAT(cache, ',');
    PMIX_ARGV_FREE_COMPAT(cache);

    return avail;
}

void prte_hwloc_base_setup_summary(hwloc_topology_t topo)
{
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *sum;
    int val;
    unsigned width, w, m, N, last;
    hwloc_bitmap_t *numas;
    hwloc_obj_t obj;

    /* Historically, CPU packages contained a single cpu die
     * and nothing else. NUMA was therefore determined by simply
     * looking at the memory bus attached to the socket where
     * the package resided - all cpus in the package were
     * exclusively "under" that NUMA. Since each socket had a
     * unique NUMA, you could easily map by them.

     * More recently, packages have started to contain multiple
     * cpu dies as well as memory and sometimes even fabric die.
     * In these cases, the memory bus of the cpu dies in the
     * package generally share any included memory die. This
     * complicates the memory situation, leaving NUMA domains
     * no longer cleanly delineated by processor (i.e.., the
     * NUMA domains overlap each other).
     *
     * Fortunately, the OS index of non-CPU NUMA domains starts
     * at 255 and counts downward (at least for GPUs) - while
     * the index of CPU NUMA domains starts at 0 and counts
     * upward. We can therefore separate the two by excluding
     * NUMA domains with an OS index above the level where
     * they first begin to intersect
     */

    root = hwloc_get_root_obj(topo);
    if (NULL == root->userdata) {
        root->userdata = (void *) PMIX_NEW(prte_hwloc_topo_data_t);
    }
    sum = (prte_hwloc_topo_data_t *) root->userdata;

    /* only need to do this once */
    if (sum->computed) {
        return;
    }
    sum->computed = true;

    /* compute the CPU NUMA cutoff for this topology */
    val = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);
    if (0 > val) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    width = val;
    if (0 == width) {
        sum->numa_cutoff = 0;
        return;
    }
    numas = (hwloc_bitmap_t*)malloc(width * sizeof(hwloc_bitmap_t));
    N = 0;
    last = 0;
    for (w=0; w < UINT_MAX && N < width; w++) {
        /* get the object at this index */
        obj = hwloc_get_numanode_obj_by_os_index(topo, w);
        if (NULL == obj) {
            continue;
        }
        /* check for overlap with all preceding numas */
        for (m=0; m < N; m++) {
            if (hwloc_bitmap_intersects(obj->cpuset, numas[m])) {
                // if it intersects anyone, then we are done
                sum->numa_cutoff = last+1;
                break;
            }
        }
        if (UINT_MAX != sum->numa_cutoff) {
            break;
        } else {
            last = w;
            /* cache this bitmap */
            numas[N] = hwloc_bitmap_alloc();
            hwloc_bitmap_copy(numas[N], obj->cpuset);
            ++N;
        }
    }
    if (UINT_MAX == sum->numa_cutoff) {
        sum->numa_cutoff = last + 1;
    }
    for (m=0; m < N; m++) {
        hwloc_bitmap_free(numas[m]);
    }
    free(numas);
}

/* determine the node-level available cpuset based on
 * online vs allowed vs user-specified cpus
 */
hwloc_cpuset_t prte_hwloc_base_filter_cpus(hwloc_topology_t topo)
{
    hwloc_cpuset_t avail = NULL;

    /* process any specified default cpu set against this topology */
    if (NULL == prte_hwloc_default_cpu_list) {
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                             "hwloc:base: no cpus specified - using root available cpuset"));
        avail = hwloc_bitmap_alloc();
        hwloc_bitmap_copy(avail, hwloc_topology_get_allowed_cpuset(topo));

    } else {
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base: filtering cpuset"));
        avail = prte_hwloc_base_generate_cpuset(topo, prte_hwloc_default_use_hwthread_cpus,
                                                &prte_hwloc_default_cpu_list);
    }

    return avail;
}

static void fill_cache_line_size(void)
{
    bool found = false;
    unsigned size = 0, cache_level = 2, i = 0;
    hwloc_obj_type_t cache_object = HWLOC_OBJ_L2CACHE;
    hwloc_obj_t obj;

    /* Look for the smallest L2 cache size */
    size = 4096;
    while (cache_level > 0 && !found) {
        i = 0;
        while (1) {
            obj = hwloc_get_obj_by_type(prte_hwloc_topology, cache_object, i);
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
       prte_cache_line_size.  Otherwise, we'll leave whatever default
       was set in prte_init.c */
    if (found) {
        prte_cache_line_size = (int) size;
    }
}

int prte_hwloc_base_get_topology(void)
{
    int rc;

    pmix_output_verbose(2, prte_hwloc_base_output,
                        "hwloc:base:get_topology");

    /* see if we already have it */
    if (NULL != prte_hwloc_topology) {
        return PRTE_SUCCESS;
    }

    if (NULL == prte_hwloc_base_topo_file) {
        pmix_output_verbose(1, prte_hwloc_base_output,
                            "hwloc:base discovering topology");
        if (0 != hwloc_topology_init(&prte_hwloc_topology) ||
            0 != prte_hwloc_base_topology_set_flags(prte_hwloc_topology, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM, true) ||
            0 != hwloc_topology_load(prte_hwloc_topology)) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
            return PRTE_ERR_NOT_SUPPORTED;
        }
    } else {
        pmix_output_verbose(1, prte_hwloc_base_output,
                            "hwloc:base loading topology from file %s",
                            prte_hwloc_base_topo_file);
        if (PRTE_SUCCESS != (rc = prte_hwloc_base_set_topology(prte_hwloc_base_topo_file))) {
            return rc;
        }
        prte_hwloc_synthetic_topo = true;
    }

    /* fill prte_cache_line_size global with the smallest L1 cache
       line size */
    fill_cache_line_size();

    // create the summary
    prte_hwloc_base_setup_summary(prte_hwloc_topology);
    return PRTE_SUCCESS;
}

int prte_hwloc_base_set_topology(char *topofile)
{
    hwloc_obj_t obj;
    unsigned j, k;
    int rc;
    struct hwloc_topology_support *support;

    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                        "hwloc:base:set_topology %s", topofile));

    if (NULL != prte_hwloc_topology) {
        hwloc_topology_destroy(prte_hwloc_topology);
    }
    if (0 != hwloc_topology_init(&prte_hwloc_topology)) {
        return PRTE_ERR_NOT_SUPPORTED;
    }
    if (0 != hwloc_topology_set_xml(prte_hwloc_topology, topofile)) {
        hwloc_topology_destroy(prte_hwloc_topology);
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base:set_topology bad topo file"));
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* since we are loading this from an external source, we have to
     * explicitly set a flag so hwloc sets things up correctly
     */
    rc = prte_hwloc_base_topology_set_flags(prte_hwloc_topology, 0, true);
    if (0 != rc) {
        hwloc_topology_destroy(prte_hwloc_topology);
        return PRTE_ERR_NOT_SUPPORTED;
    }
    /* unfortunately, early hwloc does not include support info in its
     * xml output :-(( We default to assuming it is present as
     * systems that use this option are likely to provide
     * binding support
     */
    support = (struct hwloc_topology_support *) hwloc_topology_get_support(prte_hwloc_topology);
    support->cpubind->set_thisproc_cpubind = true;
    support->membind->set_thisproc_membind = true;

    if (0 != hwloc_topology_load(prte_hwloc_topology)) {
        hwloc_topology_destroy(prte_hwloc_topology);
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base:set_topology failed to load"));
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* remove the hostname from the topology. Unfortunately, hwloc
     * decided to add the source hostname to the "topology", thus
     * rendering it unusable as a pure topological description. So
     * we remove that information here.
     */
    obj = hwloc_get_root_obj(prte_hwloc_topology);
    for (k = 0; k < obj->infos_count; k++) {
        if (NULL == obj->infos ||
            NULL == obj->infos[k].name ||
            NULL == obj->infos[k].value) {
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

    /* fill prte_cache_line_size global with the smallest L1 cache
       line size */
    fill_cache_line_size();

    /* all done */
    return PRTE_SUCCESS;
}

int prte_hwloc_base_report_bind_failure(const char *file, int line, const char *msg, int rc)
{
    static int already_reported = 0;

    if (!already_reported && PRTE_HWLOC_BASE_MBFA_SILENT != prte_hwloc_base_mbfa) {

        pmix_show_help(
            "help-prte-hwloc-base.txt", "mbind failure", true, prte_process_info.nodename, getpid(),
            file, line, msg,
            (PRTE_HWLOC_BASE_MBFA_WARN == prte_hwloc_base_mbfa)
                ? "Warning -- your job will continue, but possibly with degraded performance"
                : "ERROR -- your job may abort or behave erraticly");
        already_reported = 1;
        return rc;
    }

    return PRTE_SUCCESS;
}

/* determine if there is a single cpu in a bitmap */
bool prte_hwloc_base_single_cpu(hwloc_cpuset_t cpuset)
{
    int i;
    bool one = false;

    /* count the number of bits that are set - there is
     * one bit for each available pu. We could just
     * subtract the first and last indices, but there
     * may be "holes" in the bitmap corresponding to
     * offline or unallowed cpus - so we have to
     * search for them. Return false if we anything
     * other than one
     */
    for (i = hwloc_bitmap_first(cpuset); i <= hwloc_bitmap_last(cpuset); i++) {
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
unsigned int prte_hwloc_base_get_npus(hwloc_topology_t topo, bool use_hwthread_cpus,
                                      hwloc_cpuset_t envelope, hwloc_obj_t obj)
{
    unsigned int cnt = 0;
    hwloc_cpuset_t avail;

    if (NULL == obj->cpuset) {
        return 0;
    }

    if (NULL == envelope) {
        avail = hwloc_bitmap_dup(obj->cpuset);
    } else {
        avail = hwloc_bitmap_alloc();
        hwloc_bitmap_and(avail, obj->cpuset, envelope);
    }

    if (!use_hwthread_cpus) {
        /* if we are treating cores as cpus, then we really
         * want to know how many cores are in this object.
         * hwloc sets a bit for each "pu", so we can't just
         * count bits in this case as there may be more than
         * one hwthread/core. Instead, find the number of cores
         * in the system
         */
        cnt = hwloc_get_nbobjs_inside_cpuset_by_type(topo, avail, HWLOC_OBJ_CORE);
    } else {
        /* count the number of bits that are set - there is
         * one bit for each available pu. We could just
         * subtract the first and last indices, but there
         * may be "holes" in the bitmap corresponding to
         * offline or unallowed cpus - so we count them with
         * the bitmap "weight" (a.k.a. population count) function
         */
        cnt = hwloc_bitmap_weight(avail);
    }
    hwloc_bitmap_free(avail);

    return cnt;
}

unsigned int prte_hwloc_base_get_obj_idx(hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t ptr;
    unsigned int nobjs, i;

    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base:get_idx"));

    nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, obj->type);

    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                         "hwloc:base:get_idx found %u objects of type %s", nobjs,
                         hwloc_obj_type_string(obj->type)));

    /* find this object */
    for (i = 0; i < nobjs; i++) {
        ptr = prte_hwloc_base_get_obj_by_type(topo, obj->type, i);
        if (ptr == obj) {
            return i;
        }
    }
    /* if we get here, it wasn't found */
    pmix_show_help("help-prte-hwloc-base.txt", "obj-idx-failed", true,
                   hwloc_obj_type_string(obj->type));
    return UINT_MAX;
}

unsigned int prte_hwloc_base_get_nbobjs_by_type(hwloc_topology_t topo,
                                                hwloc_obj_type_t target)
{
    unsigned w, rc;
    hwloc_obj_t obj, root;
    prte_hwloc_topo_data_t *sum;

    /* if the type is NUMA, then we need to only count the
     * CPU NUMAs and ignore the GPU NUMAs as we only deal
     * with CPUs at this time */
    if (HWLOC_OBJ_NUMANODE == target) {

        root = hwloc_get_root_obj(topo);
        sum = (prte_hwloc_topo_data_t *) root->userdata;
        if (NULL == sum) {
            return 0;
        }

        rc = 0;
        for (w=0; w < sum->numa_cutoff; w++) {
            obj = hwloc_get_numanode_obj_by_os_index(topo, w);
            if (NULL != obj) {
                ++rc;
            }
        }
        return rc;
    }
    rc = hwloc_get_nbobjs_by_type(topo, target);
    if (UINT_MAX == rc) {
        pmix_output(0, "UNKNOWN HWLOC ERROR");
        return 0;
    }
    return rc;
}

hwloc_obj_t prte_hwloc_base_get_obj_by_type(hwloc_topology_t topo,
                                            hwloc_obj_type_t target,
                                            unsigned int instance)
{
    unsigned w, cnt;
    hwloc_obj_t obj, root;
    prte_hwloc_topo_data_t *sum;

    /* if we are looking for NUMA, then ignore all the
     * GPU NUMAs */
    if (HWLOC_OBJ_NUMANODE == target) {
        root = hwloc_get_root_obj(topo);
        sum = (prte_hwloc_topo_data_t *) root->userdata;
        if (NULL == sum) {
            return NULL;
        }

        cnt = 0;
        for (w=0; w < sum->numa_cutoff; w++) {
            obj = hwloc_get_numanode_obj_by_os_index(topo, w);
            if (NULL != obj) {
                if (cnt == instance) {
                    return obj;
                }
                ++cnt;
            }
        }
        return NULL;
    }
    return hwloc_get_obj_by_type(topo, target, instance);
}

/* The current slot_list notation only goes to the core level - i.e., the location
 * is specified as package:core. Thus, the code below assumes that all locations
 * are to be parsed under that notation.
 */

static int package_to_cpu_set(char *cpus, hwloc_topology_t topo, hwloc_bitmap_t cpumask)
{
    char **range;
    int range_cnt;
    int lower_range, upper_range;
    int package_id;
    hwloc_obj_t obj;

    if ('*' == cpus[0]) {
        /* requesting cpumask for ALL packages */
        obj = hwloc_get_root_obj(topo);
        /* set to all available processors - essentially,
         * this specification equates to unbound
         */
        hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        return PRTE_SUCCESS;
    }

    range = PMIX_ARGV_SPLIT_COMPAT(cpus, '-');
    range_cnt = PMIX_ARGV_COUNT_COMPAT(range);
    switch (range_cnt) {
    case 1: /* no range was present, so just one package given */
        package_id = atoi(range[0]);
        obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
        /* get the available cpus for this package */
        hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        break;

    case 2: /* range of packages was given */
        lower_range = atoi(range[0]);
        upper_range = atoi(range[1]);
        /* cycle across the range of packages */
        for (package_id = lower_range; package_id <= upper_range; package_id++) {
            obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
            /* set the available cpus for this package bits in the bitmask */
            hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        }
        break;
    default:
        PMIX_ARGV_FREE_COMPAT(range);
        return PRTE_ERROR;
    }
    PMIX_ARGV_FREE_COMPAT(range);

    return PRTE_SUCCESS;
}

static int package_core_to_cpu_set(char *package_core_list, hwloc_topology_t topo,
                                   hwloc_bitmap_t cpumask)
{
    int rc = PRTE_SUCCESS, i, j;
    char **package_core, *corestr;
    char **range, **list;
    int range_cnt;
    int lower_range, upper_range;
    int package_id, core_id;
    hwloc_obj_t package, core;
    hwloc_obj_type_t obj_type = HWLOC_OBJ_CORE;
    unsigned int npus;
    bool hwthreadcpus = false;

    package_core = PMIX_ARGV_SPLIT_COMPAT(package_core_list, ':');
    package_id = atoi(package_core[0]);

    /* get the object for this package id */
    package = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
    if (NULL == package) {
        PMIX_ARGV_FREE_COMPAT(package_core);
        return PRTE_ERR_NOT_FOUND;
    }

    /* as described in comment near top of file, hwloc isn't able
     * to find cores on all platforms. Adjust the type here if
     * required
     */
    if (NULL == prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0)) {
        obj_type = HWLOC_OBJ_PU;
        hwthreadcpus = true;
    }
    npus = prte_hwloc_base_get_npus(topo, hwthreadcpus, NULL, package);
    npus = npus * package_id;

    for (i = 1; NULL != package_core[i]; i++) {
        if ('C' == package_core[i][0] || 'c' == package_core[i][0]) {
            corestr = &package_core[i][1];
        } else {
            corestr = package_core[i];
        }
        if ('*' == corestr[0]) {
            /* set to all cpus on this package */
            hwloc_bitmap_or(cpumask, cpumask, package->cpuset);
            /* we are done - already assigned all cores! */
            rc = PRTE_SUCCESS;
            break;
        } else {
            range = PMIX_ARGV_SPLIT_COMPAT(corestr, '-');
            range_cnt = PMIX_ARGV_COUNT_COMPAT(range);
            /* see if a range was set or not */
            switch (range_cnt) {
            case 1: /* only one core, or a list of cores, specified */
                list = PMIX_ARGV_SPLIT_COMPAT(range[0], ',');
                for (j = 0; NULL != list[j]; j++) {
                    /* get the indexed core from this package */
                    core_id = atoi(list[j]) + npus;
                    /* get that object */
                    core = prte_hwloc_base_get_obj_by_type(topo, obj_type, core_id);
                    if (NULL == core) {
                        rc = PRTE_ERR_NOT_FOUND;
                        break;
                    }
                    /* get the cpus */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                PMIX_ARGV_FREE_COMPAT(list);
                break;

            case 2: /* range of core id's was given */
                pmix_output_verbose(5, prte_hwloc_base_output,
                                    "range of cores given: start %s stop %s", range[0], range[1]);
                lower_range = atoi(range[0]);
                upper_range = atoi(range[1]);
                for (j = lower_range; j <= upper_range; j++) {
                    /* get the indexed core from this package */
                    core_id = j + npus;
                    /* get that object */
                    core = prte_hwloc_base_get_obj_by_type(topo, obj_type, core_id);
                    if (NULL == core) {
                        rc = PRTE_ERR_NOT_FOUND;
                        break;
                    }
                    /* get the cpus add them into the result */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                break;

            default:
                PMIX_ARGV_FREE_COMPAT(range);
                PMIX_ARGV_FREE_COMPAT(package_core);
                return PRTE_ERROR;
            }
            PMIX_ARGV_FREE_COMPAT(range);
        }
    }
    PMIX_ARGV_FREE_COMPAT(package_core);

    return rc;
}

int prte_hwloc_base_cpu_list_parse(const char *slot_str, hwloc_topology_t topo,
                                   bool use_hwthread_cpus, hwloc_cpuset_t cpumask)
{
    char **item, **rngs, *lst;
    int rc, i, j, k;
    hwloc_obj_t pu;
    char **range, **list;
    size_t range_cnt;
    int core_id, lower_range, upper_range;

    /* bozo checks */
    if (NULL == prte_hwloc_topology) {
        return PRTE_ERR_NOT_SUPPORTED;
    }
    if (NULL == slot_str || 0 == strlen(slot_str)) {
        return PRTE_ERR_BAD_PARAM;
    }

    pmix_output_verbose(5, prte_hwloc_base_output, "slot assignment: slot_list == %s", slot_str);

    /* split at ';' */
    item = PMIX_ARGV_SPLIT_COMPAT(slot_str, ';');

    /* start with a clean mask */
    hwloc_bitmap_zero(cpumask);
    /* loop across the items and accumulate the mask */
    for (i = 0; NULL != item[i]; i++) {
        pmix_output_verbose(5, prte_hwloc_base_output, "working assignment %s", item[i]);
        /* if they specified "package" by starting with an S/s,
         * or if they use package:core notation, then parse the
         * package/core info
         */
        if ('P' == item[i][0] || 'p' == item[i][0] || 'S' == item[i][0] || 's' == item[i][0]
            || // backward compatibility
            NULL != strchr(item[i], ':')) {
            /* specified a package */
            if (NULL == strchr(item[i], ':')) {
                /* binding just to the package level, though
                 * it could specify multiple packages
                 * Skip the P and look for ranges
                 */
                rngs = PMIX_ARGV_SPLIT_COMPAT(&item[i][1], ',');
                for (j = 0; NULL != rngs[j]; j++) {
                    if (PRTE_SUCCESS != (rc = package_to_cpu_set(rngs[j], topo, cpumask))) {
                        PMIX_ARGV_FREE_COMPAT(rngs);
                        PMIX_ARGV_FREE_COMPAT(item);
                        return rc;
                    }
                }
                PMIX_ARGV_FREE_COMPAT(rngs);
            } else {
                if ('P' == item[i][0] || 'p' == item[i][0] || 'S' == item[i][0]
                    || 's' == item[i][0]) {
                    lst = &item[i][1];
                } else {
                    lst = item[i];
                }
                if (PRTE_SUCCESS != (rc = package_core_to_cpu_set(lst, topo, cpumask))) {
                    PMIX_ARGV_FREE_COMPAT(item);
                    return rc;
                }
            }
        } else {
            rngs = PMIX_ARGV_SPLIT_COMPAT(item[i], ',');
            for (k = 0; NULL != rngs[k]; k++) {
                /* just a core specification - see if one or a range was given */
                range = PMIX_ARGV_SPLIT_COMPAT(rngs[k], '-');
                range_cnt = PMIX_ARGV_COUNT_COMPAT(range);
                /* see if a range was set or not */
                switch (range_cnt) {
                case 1: /* only one core, or a list of cores, specified */
                    list = PMIX_ARGV_SPLIT_COMPAT(range[0], ',');
                    for (j = 0; NULL != list[j]; j++) {
                        core_id = atoi(list[j]);
                        /* find the specified available cpu */
                        if (NULL == (pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, core_id))) {
                            PMIX_ARGV_FREE_COMPAT(range);
                            PMIX_ARGV_FREE_COMPAT(item);
                            PMIX_ARGV_FREE_COMPAT(rngs);
                            PMIX_ARGV_FREE_COMPAT(list);
                            return PRTE_ERR_NOT_FOUND;
                        }
                        /* get the cpus for that object and set them in the massk*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    PMIX_ARGV_FREE_COMPAT(list);
                    break;

                case 2: /* range of core id's was given */
                    lower_range = atoi(range[0]);
                    upper_range = atoi(range[1]);
                    for (core_id = lower_range; core_id <= upper_range; core_id++) {
                        /* find the specified logical available cpu */
                        if (NULL == (pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, core_id))) {
                            PMIX_ARGV_FREE_COMPAT(range);
                            PMIX_ARGV_FREE_COMPAT(item);
                            PMIX_ARGV_FREE_COMPAT(rngs);
                            return PRTE_ERR_NOT_FOUND;
                        }
                        /* get the cpus for that object and set them in the mask*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    break;

                default:
                    PMIX_ARGV_FREE_COMPAT(range);
                    PMIX_ARGV_FREE_COMPAT(item);
                    PMIX_ARGV_FREE_COMPAT(rngs);
                    return PRTE_ERROR;
                }
                PMIX_ARGV_FREE_COMPAT(range);
            }
            PMIX_ARGV_FREE_COMPAT(rngs);
        }
    }
    PMIX_ARGV_FREE_COMPAT(item);
    return PRTE_SUCCESS;
}

static void prte_hwloc_base_get_relative_locality_by_depth(hwloc_topology_t topo, unsigned d,
                                                           hwloc_cpuset_t loc1, hwloc_cpuset_t loc2,
                                                           prte_hwloc_locality_t *locality,
                                                           bool *shared)
{
    unsigned width, w;
    hwloc_obj_t obj;
    int sect1, sect2;

    /* get the width of the topology at this depth */
    width = hwloc_get_nbobjs_by_depth(topo, d);

    /* scan all objects at this depth to see if
     * our locations overlap with them
     */
    for (w = 0; w < width; w++) {
        /* get the object at this depth/index */
        obj = hwloc_get_obj_by_depth(topo, d, w);
        /* see if our locations intersect with the cpuset for this obj */
        sect1 = hwloc_bitmap_intersects(obj->cpuset, loc1);
        sect2 = hwloc_bitmap_intersects(obj->cpuset, loc2);
        /* if both intersect, then we share this level */
        if (sect1 && sect2) {
            *shared = true;
            switch (obj->type) {
            case HWLOC_OBJ_PACKAGE:
                *locality |= PRTE_PROC_ON_PACKAGE;
                break;
            case HWLOC_OBJ_NUMANODE:
                *locality |= PRTE_PROC_ON_NUMA;
                break;
            case HWLOC_OBJ_L3CACHE:
                *locality |= PRTE_PROC_ON_L3CACHE;
                break;
            case HWLOC_OBJ_L2CACHE:
                *locality |= PRTE_PROC_ON_L2CACHE;
                break;
            case HWLOC_OBJ_L1CACHE:
                *locality |= PRTE_PROC_ON_L1CACHE;
                break;
            case HWLOC_OBJ_CORE:
                *locality |= PRTE_PROC_ON_CORE;
                break;
            case HWLOC_OBJ_PU:
                *locality |= PRTE_PROC_ON_HWTHREAD;
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
}

prte_hwloc_locality_t prte_hwloc_base_get_relative_locality(hwloc_topology_t topo, char *cpuset1,
                                                            char *cpuset2)
{
    prte_hwloc_locality_t locality;
    hwloc_cpuset_t loc1, loc2;
    unsigned depth, d;
    bool shared;
    hwloc_obj_type_t type;

    /* start with what we know - they share a node on a cluster
     * NOTE: we may alter that latter part as hwloc's ability to
     * sense multi-cu, multi-cluster systems grows
     */
    locality = PRTE_PROC_ON_NODE | PRTE_PROC_ON_HOST | PRTE_PROC_ON_CU | PRTE_PROC_ON_CLUSTER;

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
    for (d = 1; d < depth; d++) {
        shared = false;
        /* get the object type at this depth */
        type = hwloc_get_depth_type(topo, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NUMANODE != type && HWLOC_OBJ_PACKAGE != type &&
            HWLOC_OBJ_L3CACHE != type && HWLOC_OBJ_L2CACHE != type && HWLOC_OBJ_L1CACHE != type &&
            HWLOC_OBJ_CORE != type && HWLOC_OBJ_PU != type) {
            continue;
        }
        prte_hwloc_base_get_relative_locality_by_depth(topo, d, loc1, loc2, &locality, &shared);

        /* if we spanned the entire width without finding
         * a point of intersection, then no need to go
         * deeper
         */
        if (!shared) {
            break;
        }
    }

    prte_hwloc_base_get_relative_locality_by_depth(topo, (unsigned) HWLOC_TYPE_DEPTH_NUMANODE, loc1,
                                                   loc2, &locality, &shared);

    pmix_output_verbose(5, prte_hwloc_base_output, "locality: %s",
                        prte_hwloc_base_print_locality(locality));
    hwloc_bitmap_free(loc1);
    hwloc_bitmap_free(loc2);

    return locality;
}

/* searches the given topology for coprocessor objects and returns
 * their serial numbers as a comma-delimited string, or NULL
 * if no coprocessors are found
 */
char *prte_hwloc_base_find_coprocessors(hwloc_topology_t topo)
{
    hwloc_obj_t osdev;
    unsigned i;
    char **cps = NULL;
    char *cpstring = NULL;
    int depth;

    /* coprocessors are recorded under OS_DEVICEs, so first
     * see if we have any of those
     */
    if (HWLOC_TYPE_DEPTH_UNKNOWN == (depth = hwloc_get_type_depth(topo, HWLOC_OBJ_OS_DEVICE))) {
        PMIX_OUTPUT_VERBOSE(
            (5, prte_hwloc_base_output, "hwloc:base:find_coprocessors: NONE FOUND IN TOPO"));
        return NULL;
    }

    /* check the device objects for coprocessors */
    osdev = hwloc_get_obj_by_depth(topo, depth, 0);
    while (NULL != osdev) {
        if (HWLOC_OBJ_OSDEV_COPROC == osdev->attr->osdev.type) {
            /* got one! find and save its serial number */
            for (i = 0; i < osdev->infos_count; i++) {
                if (0
                    == strncmp(osdev->infos[i].name, "MICSerialNumber",
                               strlen("MICSerialNumber"))) {
                    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                                         "hwloc:base:find_coprocessors: coprocessor %s found",
                                         osdev->infos[i].value));
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cps, osdev->infos[i].value);
                }
            }
        }
        osdev = osdev->next_cousin;
    }
    if (NULL != cps) {
        cpstring = PMIX_ARGV_JOIN_COMPAT(cps, ',');
        PMIX_ARGV_FREE_COMPAT(cps);
    }
    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                         "hwloc:base:find_coprocessors: hosting coprocessors %s",
                         (NULL == cpstring) ? "NONE" : cpstring));
    return cpstring;
}

#define PRTE_HWLOC_MAX_ELOG_LINE 1024

static char *hwloc_getline(FILE *fp)
{
    char *ret, *buff;
    char input[PRTE_HWLOC_MAX_ELOG_LINE];

    ret = fgets(input, PRTE_HWLOC_MAX_ELOG_LINE, fp);
    if (NULL != ret) {
        input[strlen(input) - 1] = '\0'; /* remove newline */
        buff = strdup(input);
        return buff;
    }

    return NULL;
}

/* checks local environment to determine if this process
 * is on a coprocessor - if so, it returns the serial number
 * as a string, or NULL if it isn't on a coprocessor
 */
char *prte_hwloc_base_check_on_coprocessor(void)
{
    /* this support currently is limited to Intel Phi processors
     * but will hopefully be extended as we get better, more
     * generalized ways of identifying coprocessors
     */
    FILE *fp;
    char *t, *cptr, *e, *cp = NULL;

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
            t += 5; // move past "Card "
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
    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                         "hwloc:base:check_coprocessor: on coprocessor %s",
                         (NULL == cp) ? "NONE" : cp));
    return cp;
}

char *prte_hwloc_base_print_binding(prte_binding_policy_t binding)
{
    char *ret, *bind;
    prte_hwloc_print_buffers_t *ptr;

    switch (PRTE_GET_BINDING_POLICY(binding)) {
    case PRTE_BIND_TO_NONE:
        bind = "NONE";
        break;
    case PRTE_BIND_TO_PACKAGE:
        bind = "PACKAGE";
        break;
    case PRTE_BIND_TO_NUMA:
        bind = "NUMA";
        break;
    case PRTE_BIND_TO_L3CACHE:
        bind = "L3CACHE";
        break;
    case PRTE_BIND_TO_L2CACHE:
        bind = "L2CACHE";
        break;
    case PRTE_BIND_TO_L1CACHE:
        bind = "L1CACHE";
        break;
    case PRTE_BIND_TO_CORE:
        bind = "CORE";
        break;
    case PRTE_BIND_TO_HWTHREAD:
        bind = "HWTHREAD";
        break;
    default:
        bind = "UNKNOWN";
    }
    ptr = prte_hwloc_get_print_buffer();
    if (NULL == ptr) {
        return prte_hwloc_print_null;
    }
    /* cycle around the ring */
    if (PRTE_HWLOC_PRINT_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }
    if (!PRTE_BINDING_REQUIRED(binding) && PRTE_BIND_OVERLOAD_ALLOWED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRTE_HWLOC_PRINT_MAX_SIZE,
                 "%s:IF-SUPPORTED:OVERLOAD-ALLOWED", bind);
    } else if (PRTE_BIND_OVERLOAD_ALLOWED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRTE_HWLOC_PRINT_MAX_SIZE, "%s:OVERLOAD-ALLOWED", bind);
    } else if (!PRTE_BINDING_REQUIRED(binding)) {
        snprintf(ptr->buffers[ptr->cntr], PRTE_HWLOC_PRINT_MAX_SIZE, "%s:IF-SUPPORTED", bind);
    } else {
        snprintf(ptr->buffers[ptr->cntr], PRTE_HWLOC_PRINT_MAX_SIZE, "%s", bind);
    }
    ret = ptr->buffers[ptr->cntr];
    ptr->cntr++;

    return ret;
}

void prte_hwloc_build_map(hwloc_topology_t topo,
                          hwloc_cpuset_t avail,
                          bool use_hwthread_cpus,
                          hwloc_bitmap_t coreset)
{
    unsigned k, obj_index, core_index;
    hwloc_obj_t pu, core;

    /* the bits in the cpuset _always_ represent hwthreads, so
     * we have to manually determine which core each bit is under
     * so we can report the cpus in terms of "cores" */
    /* start with the first set bit */
    hwloc_bitmap_zero(coreset);
    k = hwloc_bitmap_first(avail);
    obj_index = 0;
    while (k != (unsigned) -1) {
        if (use_hwthread_cpus) {
            /* mark this thread as occupied */
            hwloc_bitmap_set(coreset, k);
        } else {
            /* Go upward and find the core this PU belongs to */
            pu = hwloc_get_obj_inside_cpuset_by_type(topo, avail, HWLOC_OBJ_PU, obj_index);
            core = pu;
            while (NULL != core && core->type != HWLOC_OBJ_CORE) {
                core = core->parent;
            }
            core_index = 0;
            if (NULL != core) {
                core_index = core->logical_index;
            }
            /* mark everything since the last place as
             * being empty */
            hwloc_bitmap_set(coreset, core_index);
        }
        /* move to the next set bit */
        k = hwloc_bitmap_next(avail, k);
        ++obj_index;
    }
}

/* formatting core/hwt binding information as xml elements */
static int bitmap_list_snprintf_exp(char *__hwloc_restrict buf, size_t buflen,
                                    const struct hwloc_bitmap_s *__hwloc_restrict set,
                                    char *type)
{
    int ret = 0;
    char *tmp = buf;
    int prev = -1;
    ssize_t size = buflen;
    int res = -1;

    /* mark the end in case we do nothing later */
    if (buflen > 0) {
        tmp[0] = '\0';
    }

    while (1) {
        int begin, end;

        begin = hwloc_bitmap_next(set, prev);
        if (begin == -1) {
            break;
        }
        end = hwloc_bitmap_next_unset(set, begin);

        if (end == begin + 1) {
            res = snprintf(tmp, size, "%*c<%s>%d</%s>\n", 20, ' ', type, begin, type);
        } else if (end == -1) {
            res = snprintf(tmp, size, "%*c<%s>%d</%s>\n", 20, ' ', type, begin, type);
        } else {
            for (int i = begin; i <= end - 1; i++) {
                res = snprintf(tmp, size, "%*c<%s>%d</%s>\n", 20, ' ', type, i, type);
                if (i != (end - 1)) {
                    tmp += res;
                }
            }
        }
        if (res < 0) {
            return -1;
        }
        ret += res;

        if (res >= size) {
            res = size > 0 ? (int)size - 1 : 0;
        }

        tmp += res;
        size -= res;

        if (end == -1) {
            break;
        } else {
            prev = end - 1;
        }
    }
    return ret;
}

/*
 * Output is undefined if a rank is bound to more than 1 package
 */
void prte_hwloc_get_binding_info(hwloc_const_cpuset_t cpuset,
                               bool use_hwthread_cpus, hwloc_topology_t topo,
                               int *pkgnum, char *cores, int sz)
{
    int n, npkgs, npus, ncores;
    hwloc_cpuset_t avail, coreset = NULL;
    hwloc_obj_t pkg;
    bool bits_as_cores = false;

    /* if the cpuset is all zero, then something is wrong */
    if (hwloc_bitmap_iszero(cpuset)) {
        snprintf(cores, sz, "\n%*c<EMPTY CPUSET/>\n", 20, ' ');
    }

    /* if the cpuset includes all available cpus, and
     * the available cpus were not externally constrained,
     * then we are unbound */
    avail = prte_hwloc_base_filter_cpus(topo);
    if (hwloc_bitmap_isequal(cpuset, avail) &&
        hwloc_bitmap_isfull(avail)) {
        snprintf(cores, sz, "\n%*c<UNBOUND/>\n", 20, ' ');
    }
    hwloc_bitmap_free(avail);

    /* get the number of packages in the topology */
    npkgs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE);
    avail = hwloc_bitmap_alloc();
    npus = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
    ncores = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);

    if (npus == ncores && !use_hwthread_cpus) {
        /* the bits in this bitmap represent cores */
        bits_as_cores = true;
    }
    if (!use_hwthread_cpus && !bits_as_cores) {
        coreset = hwloc_bitmap_alloc();
    }

    /* binding happens within a package and not across packages */
    for (n = 0; n < npkgs; n++) {
        pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, n);
        /* see if we have any here */
        hwloc_bitmap_and(avail, cpuset, pkg->cpuset);

        if (hwloc_bitmap_iszero(avail)) {
            continue;
        }

        if (bits_as_cores) {
            /* can just use the hwloc fn directly */
            bitmap_list_snprintf_exp(cores, sz, avail, "core");
        } else if (use_hwthread_cpus) {
            /* can just use the hwloc fn directly */
            bitmap_list_snprintf_exp(cores, sz, avail, "hwt");
        } else {
            prte_hwloc_build_map(topo, avail, use_hwthread_cpus | bits_as_cores, coreset);
            /* now print out the string */
            bitmap_list_snprintf_exp(cores, sz, coreset, "core");
        }
        *pkgnum = n;
    }
    hwloc_bitmap_free(avail);
    if (NULL != coreset) {
        hwloc_bitmap_free(coreset);
    }
}

static int compare_unsigned(const void *a, const void *b)
{
    return (*(unsigned *)a - *(unsigned *)b);
}

/* generate a logical string output of a hwloc_cpuset_t */
static int build_map(char *answer, size_t size,
                     hwloc_const_cpuset_t bitmap,
                     bool physical, char *prefix,
                     hwloc_topology_t topo)
{
    unsigned indices[2048], id;
    int nsites = 0, n, start, end, idx;
    hwloc_obj_t pu;
    char tmp[128];
    bool inrange, first, unique;
    unsigned val;

    for (id = hwloc_bitmap_first(bitmap);
         id != (unsigned)-1;
         id = hwloc_bitmap_next(bitmap, id)) {
        // the id's are for threads, but we want cores
        pu = hwloc_get_pu_obj_by_os_index(topo, id);

        // go upward to find the core that contains this pu
        while (NULL != pu && pu->type != HWLOC_OBJ_CORE) {
            pu = pu->parent;
        }
        if (NULL == pu) {
            pmix_show_help("help-prte-hwloc-base.txt", "pu-not-found", true, id);
            return PRTE_ERR_SILENT;
        }
        if (physical) {
            // record the physical site
            val = pu->os_index;
        } else {
            // record the logical site
            val = pu->logical_index;
        }
        // add it uniquely to the array of indices - it could be a duplicate
        unique = true;
        for (n=0; n < nsites; n++) {
            if (indices[n] == val) {
                unique = false;
                break;
            }
        }
        if (unique) {
            indices[nsites] = val;
            ++nsites;
            if (2048 == nsites) {
                pmix_show_help("help-prte-hwloc-base.txt", "too-many-sites", true);
                return PRTE_ERR_SILENT;
            }
        }
    }

    /* this should never happen as it would mean that the bitmap was
     * empty, which is something we checked before calling this function */
    if (0 == nsites) {
        return PRTE_ERR_NOT_FOUND;
    }

    if (1 == nsites) {
        // only bound to one location - most common case
        snprintf(answer, size, "%s%u", prefix, indices[0]);
        return PRTE_SUCCESS;
    }

    // sort them
    qsort(indices, nsites, sizeof(unsigned), compare_unsigned);

    // parse through and look for ranges
    start = indices[0];
    end = indices[0];
    inrange = false;
    first = true;
    // prep the answer
    snprintf(answer, size, "%s", prefix);
    idx = strlen(prefix);

    for (n=1; n < nsites; n++) {
       // see if we are in a range
        if (1 == (indices[n]-end)) {
            inrange = true;
            end = indices[n];
            continue;
        }
        // we are not in a range, or we are
        // at the end of a range
        if (inrange) {
            // we are at the end of the range
            if (start == end) {
                if (first) {
                    snprintf(tmp, 128, "%u", start);
                    first = false;
                } else {
                    snprintf(tmp, 128, ",%u", start);
                }
                memcpy(&answer[idx], tmp, strlen(tmp));
                idx += strlen(tmp);
            } else {
                if (first) {
                    snprintf(tmp, 128, "%u-%u", start, end);
                    first = false;
                } else {
                    snprintf(tmp, 128, ",%u-%u", start, end);
                }
                memcpy(&answer[idx], tmp, strlen(tmp));
                idx += strlen(tmp);
            }
            // mark the end of the range
            inrange = false;
            start = indices[n];
            end = indices[n];
        } else {
            if (first) {
                snprintf(tmp, 128, "%u", start);
                first = false;
            } else {
                snprintf(tmp, 128, ",%u", start);
            }
            memcpy(&answer[idx], tmp, strlen(tmp));
            idx += strlen(tmp);
            inrange = false;
            start = indices[n];
            end = indices[n];
        }
    }
    // see if we have a dangling entry
    if (start == end) {
        if (first) {
            snprintf(tmp, 128, "%u", start);
        } else {
            snprintf(tmp, 128, ",%u", start);
        }
        memcpy(&answer[idx], tmp, strlen(tmp));
        snprintf(tmp, 128, "%u", start);
    } else {
        if (first) {
            snprintf(tmp, 128, "%u-%u", start, end);
            first = false;
        } else {
            snprintf(tmp, 128, ",%u-%u", start, end);
        }
        memcpy(&answer[idx], tmp, strlen(tmp));
        idx += strlen(tmp);
    }
    return PRTE_SUCCESS;
}

/*
 * Make a prettyprint string for a hwloc_cpuset_t
 */
char *prte_hwloc_base_cset2str(hwloc_const_cpuset_t cpuset,
                               bool use_hwthread_cpus,
                               bool physical,
                               hwloc_topology_t topo)
{
    int n, npkgs, npus, ncores;
    char tmp[2048], ans[4096];
    hwloc_cpuset_t avail, coreset = NULL;
    char **output = NULL, *result;
    hwloc_obj_t pkg;
    bool bits_as_cores = false;
    int complete;
    char *prefix;

    /* if the cpuset is all zero, then something is wrong */
    if (hwloc_bitmap_iszero(cpuset)) {
        return strdup("EMPTY CPUSET");
    }

    /* if the cpuset includes all available cpus, and
     * the available cpus were not externally constrained,
     * then we are unbound */
    avail = prte_hwloc_base_filter_cpus(topo);
    if (hwloc_bitmap_isequal(cpuset, avail) &&
        hwloc_bitmap_isfull(avail)) {
        return strdup("UNBOUND");
    }
    hwloc_bitmap_free(avail);

    /* get the number of packages in the topology */
    npkgs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE);
    avail = hwloc_bitmap_alloc();

    npus = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
    ncores = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
    if (npus == ncores && !use_hwthread_cpus) {
        /* the bits in this bitmap represent cores */
        bits_as_cores = true;
    }
    if (!use_hwthread_cpus && !bits_as_cores) {
        coreset = hwloc_bitmap_alloc();
    }
    if (bits_as_cores || !use_hwthread_cpus) {
        if (physical) {
            prefix = "core:P";
        } else {
            prefix = "core:L";
        }
    } else {
        if (physical) {
            prefix = "hwt:P";
        } else {
            prefix = "hwt:L";
        }
    }

    for (n = 0; n < npkgs; n++) {
        memset(tmp, 0, sizeof(tmp));
        memset(ans, 0, sizeof(ans));
        pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, n);
        /* see if we have any here */
        hwloc_bitmap_and(avail, cpuset, pkg->cpuset);
        if (hwloc_bitmap_iszero(avail)) {
            continue;
        }
        if (bits_as_cores) {
            /* can just use the hwloc fn directly */
            hwloc_bitmap_list_snprintf(tmp, 2048, avail);
            snprintf(ans, 4096, "package[%d][%s%s]", n, prefix, tmp);
        } else if (use_hwthread_cpus) {
            /* can just use the hwloc fn directly */
            hwloc_bitmap_list_snprintf(tmp, 2048, avail);
            snprintf(ans, 4096, "package[%d][%s%s]", n, prefix, tmp);
        } else {
            // build the map for this cpuset
            complete = build_map(tmp, 2048, avail,
                                 physical, prefix, topo);
            if (PRTE_SUCCESS == complete) {
                snprintf(ans, 4096, "package[%d][%s]", n, tmp);
            } else {
                PMIX_ARGV_FREE_COMPAT(output);
                return NULL;
            }
        }
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&output, ans);
    }

    if (NULL != output) {
        result = PMIX_ARGV_JOIN_COMPAT(output, ' ');
        PMIX_ARGV_FREE_COMPAT(output);
    } else {
        result = NULL;
    }
    hwloc_bitmap_free(avail);
    if (NULL != coreset) {
        hwloc_bitmap_free(coreset);
    }
    return result;
}

static char* construct_range(char **vals)
{
    int n, cnt;
    char buf[4096], **ans = NULL, *str;

    cnt = 1;
    for (n=0; NULL != vals[n]; n++) {
        if (NULL == vals[n+1]) {
            if (1 == cnt) {
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, vals[n]);
            } else {
                snprintf(buf, 4096, "%d:%s", cnt, vals[n]);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, buf);
            }
            break;
        }
        if (0 == strcmp(vals[n], vals[n+1])) {
            cnt++;
        } else {
            if (1 == cnt) {
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, vals[n]);
            } else {
                snprintf(buf, 4096, "%d:%s", cnt, vals[n]);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&ans, buf);
            }
            cnt = 1;
        }
    }

    str = PMIX_ARGV_JOIN_COMPAT(ans, ',');
    return str;
}

char *prte_hwloc_base_get_topo_signature(hwloc_topology_t topo)
{
    char *sig = NULL, *arch = NULL, *endian;
    hwloc_obj_t obj;
    unsigned i, nobjs, n, ncpus;
    char buffer[4096], **scratch = NULL, **answer = NULL;
    int rc;
    hwloc_cpuset_t avail, available;

    rc = hwloc_topology_export_synthetic(topo, buffer, 4096,
                                         HWLOC_TOPOLOGY_EXPORT_SYNTHETIC_FLAG_NO_ATTRS);
    if (0 > rc) {
        // create out own signature - start with packages
        scratch = NULL;
        available = hwloc_bitmap_alloc();
        avail = prte_hwloc_base_filter_cpus(prte_hwloc_topology);
        nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE);
        for (n=0; n < nobjs; n++) {
            obj = prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_PACKAGE, n);
            hwloc_bitmap_and(available, avail, obj->cpuset);
            ncpus = hwloc_bitmap_weight(available);
            snprintf(buffer, 4096, "%u", ncpus);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&scratch, buffer);
        }
        sig = construct_range(scratch);
        snprintf(buffer, 4096, "PKG[%s]", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(scratch);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&answer, buffer);
        // now account for NUMA
        scratch = NULL;
        nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);
        for (n=0; n < nobjs; n++) {
            obj = prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_NUMANODE, n);
            hwloc_bitmap_and(available, avail, obj->cpuset);
            ncpus = hwloc_bitmap_weight(available);
            snprintf(buffer, 4096, "%u", ncpus);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&scratch, buffer);
        }
        sig = construct_range(scratch);
        snprintf(buffer, 4096, "NUMA[%s]", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(scratch);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&answer, buffer);
        // L3caches
        scratch = NULL;
        nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L3CACHE);
        for (n=0; n < nobjs; n++) {
            obj = prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_L3CACHE, n);
            hwloc_bitmap_and(available, avail, obj->cpuset);
            ncpus = hwloc_bitmap_weight(available);
            snprintf(buffer, 4096, "%u", ncpus);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&scratch, buffer);
        }
        sig = construct_range(scratch);
        snprintf(buffer, 4096, "L3[%s]", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(scratch);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&answer, buffer);
        // L2caches
        scratch = NULL;
        nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L2CACHE);
        for (n=0; n < nobjs; n++) {
            obj = prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_L2CACHE, n);
            hwloc_bitmap_and(available, avail, obj->cpuset);
            ncpus = hwloc_bitmap_weight(available);
            snprintf(buffer, 4096, "%u", ncpus);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&scratch, buffer);
        }
        sig = construct_range(scratch);
        snprintf(buffer, 4096, "L2[%s]", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(scratch);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&answer, buffer);
        // L1caches
        scratch = NULL;
        nobjs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_L1CACHE);
        for (n=0; n < nobjs; n++) {
            obj = prte_hwloc_base_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_L1CACHE, n);
            hwloc_bitmap_and(available, avail, obj->cpuset);
            ncpus = hwloc_bitmap_weight(available);
            snprintf(buffer, 4096, "%u", ncpus);
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&scratch, buffer);
        }
        sig = construct_range(scratch);
        snprintf(buffer, 4096, "L1[%s]", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(scratch);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&answer, buffer);
        // setup the signature
        sig = PMIX_ARGV_JOIN_COMPAT(answer, ';');
        snprintf(buffer, 4096, "%s", sig);
        free(sig);
        PMIX_ARGV_FREE_COMPAT(answer);
        hwloc_bitmap_free(avail);
    }

    /* get the root object so we can add the processor architecture */
    obj = hwloc_get_root_obj(topo);
    for (i = 0; i < obj->infos_count; i++) {
        if (0 == strcmp(obj->infos[i].name, "Architecture")) {
            arch = obj->infos[i].value;
            break;
        }
    }
    if (NULL == arch) {
        arch = "unknown";
    }

#ifdef __BYTE_ORDER
#    if __BYTE_ORDER == __LITTLE_ENDIAN
    endian = "le";
#    else
    endian = "be";
#    endif
#else
    endian = "unknown";
#endif

    // form the final signature
    pmix_asprintf(&sig, "%s:%s:%s", buffer, arch, endian);
    return sig;
}

static int prte_hwloc_base_get_locality_string_by_depth(hwloc_topology_t topo, int d,
                                                        hwloc_cpuset_t cpuset,
                                                        hwloc_cpuset_t result)
{
    hwloc_obj_t obj;
    unsigned width, w;

    /* get the width of the topology at this depth */
    width = hwloc_get_nbobjs_by_depth(topo, d);
    if (0 == width) {
        return -1;
    }

    /* scan all objects at this depth to see if
     * the location overlaps with them
     */
    for (w = 0; w < width; w++) {
        /* get the object at this depth/index */
        obj = hwloc_get_obj_by_depth(topo, d, w);
        /* see if the location intersects with it */
        if (hwloc_bitmap_intersects(obj->cpuset, cpuset)) {
            hwloc_bitmap_set(result, w);
        }
    }

    return 0;
}

char *prte_hwloc_base_get_locality_string(hwloc_topology_t topo, char *bitmap)
{
    char *locality = NULL, *tmp, *t2;
    unsigned depth, d;
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
    for (d = 1; d < depth; d++) {
        /* get the object type at this depth */
        type = hwloc_get_depth_type(topo, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NUMANODE != type && HWLOC_OBJ_PACKAGE != type &&
            HWLOC_OBJ_L1CACHE != type && HWLOC_OBJ_L2CACHE != type && HWLOC_OBJ_L3CACHE != type &&
            HWLOC_OBJ_CORE != type && HWLOC_OBJ_PU != type) {
            continue;
        }

        if (prte_hwloc_base_get_locality_string_by_depth(topo, d, cpuset, result) < 0) {
            continue;
        }

        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            switch (type) {
            case HWLOC_OBJ_NUMANODE:
                pmix_asprintf(&t2, "%sNM%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_PACKAGE:
                pmix_asprintf(&t2, "%sSK%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_L3CACHE:
                pmix_asprintf(&t2, "%sL3%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_L2CACHE:
                pmix_asprintf(&t2, "%sL2%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_L1CACHE:
                pmix_asprintf(&t2, "%sL1%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_CORE:
                pmix_asprintf(&t2, "%sCR%s:", (NULL == locality) ? "" : locality, tmp);
                if (NULL != locality) {
                    free(locality);
                }
                locality = t2;
                break;
            case HWLOC_OBJ_PU:
                pmix_asprintf(&t2, "%sHT%s:", (NULL == locality) ? "" : locality, tmp);
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

    if (prte_hwloc_base_get_locality_string_by_depth(topo, HWLOC_TYPE_DEPTH_NUMANODE, cpuset,
                                                     result)
        == 0) {
        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            pmix_asprintf(&t2, "%sNM%s:", (NULL == locality) ? "" : locality, tmp);
            if (NULL != locality) {
                free(locality);
            }
            locality = t2;
            free(tmp);
        }
        hwloc_bitmap_zero(result);
    }

    hwloc_bitmap_free(result);
    hwloc_bitmap_free(cpuset);

    /* remove the trailing colon */
    if (NULL != locality) {
        locality[strlen(locality) - 1] = '\0';
    }
    return locality;
}

char *prte_hwloc_base_get_location(char *locality, hwloc_obj_type_t type, unsigned index)
{
    char **loc;
    char *srch, *ans = NULL;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(index);

    if (NULL == locality) {
        return NULL;
    }
    switch (type) {
    case HWLOC_OBJ_NUMANODE:
        srch = "NM";
        break;
    case HWLOC_OBJ_PACKAGE:
        srch = "SK";
        break;
    case HWLOC_OBJ_L3CACHE:
        srch = "L3";
        break;
    case HWLOC_OBJ_L2CACHE:
        srch = "L2";
        break;
    case HWLOC_OBJ_L1CACHE:
        srch = "L1";
        break;
    case HWLOC_OBJ_CORE:
        srch = "CR";
        break;
    case HWLOC_OBJ_PU:
        srch = "HT";
        break;
    default:
        return NULL;
    }
    loc = PMIX_ARGV_SPLIT_COMPAT(locality, ':');
    for (n = 0; NULL != loc[n]; n++) {
        if (0 == strncmp(loc[n], srch, 2)) {
            ans = strdup(&loc[n][2]);
            break;
        }
    }
    PMIX_ARGV_FREE_COMPAT(loc);

    return ans;
}

prte_hwloc_locality_t prte_hwloc_compute_relative_locality(char *loc1, char *loc2)
{
    prte_hwloc_locality_t locality;
    char **set1, **set2;
    hwloc_bitmap_t bit1, bit2;
    size_t n1, n2;

    /* start with what we know - they share a node on a cluster
     * NOTE: we may alter that latter part as hwloc's ability to
     * sense multi-cu, multi-cluster systems grows
     */
    locality = PRTE_PROC_ON_NODE | PRTE_PROC_ON_HOST | PRTE_PROC_ON_CU | PRTE_PROC_ON_CLUSTER;

    /* if either location is NULL, then that isn't bound */
    if (NULL == loc1 || NULL == loc2) {
        return locality;
    }

    set1 = PMIX_ARGV_SPLIT_COMPAT(loc1, ':');
    set2 = PMIX_ARGV_SPLIT_COMPAT(loc2, ':');
    bit1 = hwloc_bitmap_alloc();
    bit2 = hwloc_bitmap_alloc();

    /* check each matching type */
    for (n1 = 0; NULL != set1[n1]; n1++) {
        /* convert the location into bitmap */
        hwloc_bitmap_list_sscanf(bit1, &set1[n1][2]);
        /* find the matching type in set2 */
        for (n2 = 0; NULL != set2[n2]; n2++) {
            if (0 == strncmp(set1[n1], set2[n2], 2)) {
                /* convert the location into bitmap */
                hwloc_bitmap_list_sscanf(bit2, &set2[n2][2]);
                /* see if they intersect */
                if (hwloc_bitmap_intersects(bit1, bit2)) {
                    /* set the corresponding locality bit */
                    if (0 == strncmp(set1[n1], "SK", 2)) {
                        locality |= PRTE_PROC_ON_PACKAGE;
                    } else if (0 == strncmp(set1[n1], "NM", 2)) {
                        locality |= PRTE_PROC_ON_NUMA;
                    } else if (0 == strncmp(set1[n1], "L3", 2)) {
                        locality |= PRTE_PROC_ON_L3CACHE;
                    } else if (0 == strncmp(set1[n1], "L2", 2)) {
                        locality |= PRTE_PROC_ON_L2CACHE;
                    } else if (0 == strncmp(set1[n1], "L1", 2)) {
                        locality |= PRTE_PROC_ON_L1CACHE;
                    } else if (0 == strncmp(set1[n1], "CR", 2)) {
                        locality |= PRTE_PROC_ON_CORE;
                    } else if (0 == strncmp(set1[n1], "HT", 2)) {
                        locality |= PRTE_PROC_ON_HWTHREAD;
                    } else {
                        /* should never happen */
                        pmix_output(0, "UNRECOGNIZED LOCALITY %s", set1[n1]);
                    }
                }
                break;
            }
        }
    }
    PMIX_ARGV_FREE_COMPAT(set1);
    PMIX_ARGV_FREE_COMPAT(set2);
    hwloc_bitmap_free(bit1);
    hwloc_bitmap_free(bit2);
    return locality;
}

int prte_hwloc_base_topology_export_xmlbuffer(hwloc_topology_t topology, char **xmlpath,
                                              int *buflen)
{
    return hwloc_topology_export_xmlbuffer(topology, xmlpath, buflen, 0);
}

int prte_hwloc_base_topology_set_flags(hwloc_topology_t topology, unsigned long flags, bool io)
{
    if (io) {
        int ret = hwloc_topology_set_io_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
        if (0 != ret) {
            return ret;
        }
    }
    // Blacklist the "gl" component due to potential conflicts.
    // See "https://github.com/open-mpi/ompi/issues/10025" for
    // an explanation
    hwloc_topology_set_components(topology, HWLOC_TOPOLOGY_COMPONENTS_FLAG_BLACKLIST, "gl");
    return hwloc_topology_set_flags(topology, flags);
}

#define PRTE_HWLOC_MAX_STRING 2048

static void print_hwloc_obj(char **output, char *prefix, hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t obj2;
    char string[1024], *tmp, *tmp2, *pfx;
    unsigned i;
    struct hwloc_topology_support *support;

    /* print the object type */
    hwloc_obj_type_snprintf(string, 1024, obj, 1);
    pmix_asprintf(&pfx, "\n%s\t", (NULL == prefix) ? "" : prefix);
    pmix_asprintf(&tmp, "%sType: %s Number of child objects: %u%sName=%s",
                  (NULL == prefix) ? "" : prefix, string, obj->arity, pfx,
                  (NULL == obj->name) ? "NULL" : obj->name);
    if (0 < hwloc_obj_attr_snprintf(string, 1024, obj, pfx, 1)) {
        /* print the attributes */
        pmix_asprintf(&tmp2, "%s%s%s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    /* print the cpusets - apparently, some new HWLOC types don't
     * have cpusets, so protect ourselves here
     */
    if (NULL != obj->cpuset) {
        hwloc_bitmap_snprintf(string, PRTE_HWLOC_MAX_STRING, obj->cpuset);
        pmix_asprintf(&tmp2, "%s%sCpuset:  %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (HWLOC_OBJ_MACHINE == obj->type) {
        /* root level object - add support values */
        support = (struct hwloc_topology_support *) hwloc_topology_get_support(topo);
        pmix_asprintf(&tmp2, "%s%sBind CPU proc:   %s%sBind CPU thread: %s", tmp, pfx,
                      (support->cpubind->set_thisproc_cpubind) ? "TRUE" : "FALSE", pfx,
                      (support->cpubind->set_thisthread_cpubind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
        pmix_asprintf(&tmp2, "%s%sBind MEM proc:   %s%sBind MEM thread: %s", tmp, pfx,
                      (support->membind->set_thisproc_membind) ? "TRUE" : "FALSE", pfx,
                      (support->membind->set_thisthread_membind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
    }
    pmix_asprintf(&tmp2, "%s%s\n", (NULL == *output) ? "" : *output, tmp);
    free(tmp);
    free(pfx);
    pmix_asprintf(&pfx, "%s\t", (NULL == prefix) ? "" : prefix);
    for (i = 0; i < obj->arity; i++) {
        obj2 = obj->children[i];
        /* print the object */
        print_hwloc_obj(&tmp2, pfx, topo, obj2);
    }
    free(pfx);
    if (NULL != *output) {
        free(*output);
    }
    *output = tmp2;
}

int prte_hwloc_print(char **output, char *prefix, hwloc_topology_t src)
{
    hwloc_obj_t obj;
    char *tmp = NULL;

    /* get root object */
    obj = hwloc_get_root_obj(src);
    /* print it */
    print_hwloc_obj(&tmp, prefix, src, obj);
    *output = tmp;
    return PRTE_SUCCESS;
}

void prte_hwloc_base_reset_counters(void)
{
    prte_topology_t *ptopo;
    hwloc_topology_t topo;
    hwloc_obj_type_t type;
    hwloc_obj_t obj;
    prte_hwloc_obj_data_t *objcnt;
    unsigned width, w;
    unsigned depth, d;
    int n;

    /* this can be a fairly expensive operation as we must traverse
     * all objects of interest in all topologies since we cannot
     * know which ones might have been used. Fortunately, we almost
     * always have only one topology, and there aren't that many
     * objects in it - so this normally goes fairly quickly
     */

    for (n = 0; n < prte_node_topologies->size; n++) {
        ptopo = (prte_topology_t *) pmix_pointer_array_get_item(prte_node_topologies, n);
        if (NULL == ptopo) {
            continue;
        }
        topo = ptopo->topo;

        /* get the max depth of the topology */
        depth = hwloc_topology_get_depth(topo);

        /* start at the first depth below the top machine level */
        for (d = 1; d < depth; d++) {
            /* get the object type at this depth */
            type = hwloc_get_depth_type(topo, d);
            /* if it isn't one of interest, then ignore it */
            if (HWLOC_OBJ_NUMANODE != type && HWLOC_OBJ_PACKAGE != type &&
                HWLOC_OBJ_L1CACHE != type && HWLOC_OBJ_L2CACHE != type && HWLOC_OBJ_L3CACHE != type &&
                HWLOC_OBJ_CORE != type && HWLOC_OBJ_PU != type) {
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
            for (w = 0; w < width; w++) {
                /* get the object at this depth/index */
                obj = hwloc_get_obj_by_depth(topo, d, w);
                if (NULL != obj->userdata) {
                    objcnt = (prte_hwloc_obj_data_t*)obj->userdata;
                    objcnt->nprocs = 0;
                }
            }
        }
    }
}
