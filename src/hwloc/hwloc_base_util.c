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

#include "prte_config.h"

#include <errno.h>
#include <limits.h>

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
    if (NULL == pu) {
        /* a topology with cores but no PUs answers no question we can
         * usefully ask here, and dereferencing the missing PU took the
         * caller down */
        return false;
    }
    /* if the two are equal, then we really don't have
     * cores in this topology */
    if (hwloc_bitmap_isequal(obj->cpuset, pu->cpuset)) {
        return false;
    }
    /* we do have cores */
    return true;
}

bool prte_hwloc_base_has_cores(hwloc_topology_t topo)
{
    return (NULL != hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0));
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

/* Parse a logical cpu id, rejecting anything that is not a bare number.
 * strtoul() alone reports "0" for a token like "foo", which silently turns
 * a typo in a cpu-set into "bind everything to cpu 0". */
static bool parse_cpu_id(const char *str, int *id)
{
    char *endp;
    long val;

    if (NULL == str || '\0' == *str) {
        return false;
    }
    errno = 0;
    val = strtol(str, &endp, 10);
    if (endp == str || '\0' != *endp || 0 != errno || 0 > val || INT_MAX < val) {
        return false;
    }
    *id = (int) val;
    return true;
}

hwloc_cpuset_t prte_hwloc_base_generate_cpuset(hwloc_topology_t topo,
                                               bool use_hwthread_cpus,
                                               char **cpulist)
{
    hwloc_cpuset_t avail = NULL, pucpus, res;
    char **ranges = NULL, **range = NULL;
    int idx, cpu, start, end, nranges;
    hwloc_obj_t pu;
    char **cache = NULL;
    char tmp[256];
    bool bad;

    /* find the specified logical cpus */
    ranges = PMIx_Argv_split(*cpulist, ',');
    nranges = PMIx_Argv_count(ranges);
    avail = hwloc_bitmap_alloc();
    hwloc_bitmap_zero(avail);
    res = hwloc_bitmap_alloc();
    pucpus = hwloc_bitmap_alloc();
    for (idx = 0; idx < nranges; idx++) {
        range = PMIx_Argv_split(ranges[idx], '-');
        bad = false;
        switch (PMIx_Argv_count(range)) {
        case 1:
            /* only one cpu given - get that object */
            if (!parse_cpu_id(range[0], &cpu)) {
                bad = true;
                break;
            }
            pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, cpu);
            if (NULL == pu) {
                bad = true;
                break;
            }
            hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
            hwloc_bitmap_or(res, avail, pucpus);
            hwloc_bitmap_copy(avail, res);
            // cache the cpu
            PMIx_Argv_append_nosize(&cache, range[0]);
            break;
        case 2:
            /* range given */
            if (!parse_cpu_id(range[0], &start) || !parse_cpu_id(range[1], &end) ||
                end < start) {
                bad = true;
                break;
            }
            for (cpu = start; cpu <= end; cpu++) {
                pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, cpu);
                if (NULL == pu) {
                    bad = true;
                    break;
                }
                hwloc_bitmap_and(pucpus, pu->cpuset, hwloc_topology_get_allowed_cpuset(topo));
                hwloc_bitmap_or(res, avail, pucpus);
                hwloc_bitmap_copy(avail, res);
                // cache the cpu
                snprintf(tmp, sizeof(tmp), "%d", cpu);
                PMIx_Argv_append_nosize(&cache, tmp);
            }
            break;
        default:
            /* "0-1-2" and friends are not a range */
            bad = true;
            break;
        }
        if (bad) {
            pmix_show_help("help-prte-hwloc-base.txt", "unfound-cpu", true,
                           *cpulist, ranges[idx],
                           use_hwthread_cpus ? "hwthread" : "core");
            PMIx_Argv_free(ranges);
            PMIx_Argv_free(range);
            PMIx_Argv_free(cache);
            hwloc_bitmap_free(avail);
            hwloc_bitmap_free(res);
            hwloc_bitmap_free(pucpus);
            return NULL;
        }
        PMIx_Argv_free(range);
    }
    if (NULL != ranges) {
        PMIx_Argv_free(ranges);
    }
    hwloc_bitmap_free(res);
    hwloc_bitmap_free(pucpus);
    // update the cpulist in case it was expanded
    free(*cpulist);
    *cpulist = PMIx_Argv_join(cache, ',');
    PMIx_Argv_free(cache);

    return avail;
}

void prte_hwloc_base_setup_summary(hwloc_topology_t topo)
{
    hwloc_obj_t root;
    prte_hwloc_topo_data_t *sum;
    int val;
    unsigned width, w, m, N, last, maxidx;
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

    /* compute the CPU NUMA cutoff for this topology. Note that a failure
     * anywhere below has to leave the cutoff at a *finite* value: the
     * lookups that consume it scan OS indices 0..cutoff, so leaving it at
     * UINT_MAX turns every later NUMA query into a four-billion-iteration
     * walk */
    sum->numa_cutoff = 0;
    val = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);
    if (0 > val) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    width = val;
    if (0 == width) {
        return;
    }
    numas = (hwloc_bitmap_t*)malloc(width * sizeof(hwloc_bitmap_t));
    if (NULL == numas) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return;
    }
    /* Bound the OS-index scan by the largest index actually present. The
     * scan cannot rely on finding "width" nodes by OS index: a topology
     * whose NUMA nodes carry no OS index (HWLOC_UNKNOWN_INDEX, which XML
     * written by hand or by an old hwloc can produce) never satisfies
     * N < width, and the loop then runs to UINT_MAX. */
    maxidx = 0;
    for (w = 0; w < width; w++) {
        obj = hwloc_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, w);
        if (NULL != obj && HWLOC_UNKNOWN_INDEX != obj->os_index &&
            obj->os_index > maxidx) {
            maxidx = obj->os_index;
        }
    }
    sum->numa_cutoff = UINT_MAX;
    N = 0;
    last = 0;
    for (w=0; w <= maxidx && N < width; w++) {
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

/* Determine the node-level available cpuset based on online vs allowed vs
 * user-specified cpus.
 *
 * NEVER returns NULL. Every caller assigns the result straight to
 * prte_node_t.available, which the mapper copies and intersects without
 * checking - a NULL there took the HNP down with a segfault inside hwloc
 * (prte_rmaps_base_get_target_nodes -> hwloc_bitmap_copy). When the DVM's
 * cpu-set cannot be resolved against this node's topology, the honest answer
 * is that the node contributes no cpus: generate_cpuset() has already told
 * the user which entry did not resolve, and an empty set makes the mapper
 * report the node as unusable through its normal path instead of quietly
 * ignoring the constraint the user asked for.
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
        if (NULL == avail) {
            avail = hwloc_bitmap_alloc();
            hwloc_bitmap_zero(avail);
        }
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

    /* If we found a cache line size in the hwloc data, save it in
       prte_cache_line_size.  Otherwise, we'll leave whatever default
       was set in prte_init.c */
    if (found) {
        prte_cache_line_size = (int) size;
    }
}

/* both of these are only ever reached from prte_hwloc_base_get_topology()
 * below, so they are private to this file */
static int set_topology(char *topofile);
static int topology_set_flags(hwloc_topology_t topology, unsigned long flags, bool io);

int prte_hwloc_base_get_topology(void)
{
    int rc;
    unsigned i, j;
    hwloc_obj_t obj;

    pmix_output_verbose(2, prte_hwloc_base_output,
                        "hwloc:base:get_topology");

    /* see if we already have it */
    if (NULL != prte_hwloc_topology) {
        return PRTE_SUCCESS;
    }

    if (NULL == prte_hwloc_base_topo_file) {
        pmix_output_verbose(1, prte_hwloc_base_output,
                            "hwloc:base discovering topology");
        if (0 != hwloc_topology_init(&prte_hwloc_topology)) {
            /* hwloc leaves the handle untouched on failure - do not hand a
             * garbage pointer to prte_hwloc_base_close() */
            prte_hwloc_topology = NULL;
            PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
            return PRTE_ERR_NOT_SUPPORTED;
        }
        if (0 != topology_set_flags(prte_hwloc_topology, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM, true) ||
            0 != hwloc_topology_load(prte_hwloc_topology)) {
            hwloc_topology_destroy(prte_hwloc_topology);
            prte_hwloc_topology = NULL;
            PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
            return PRTE_ERR_NOT_SUPPORTED;
        }
    } else {
        pmix_output_verbose(1, prte_hwloc_base_output,
                            "hwloc:base loading topology from file %s",
                            prte_hwloc_base_topo_file);
        if (PRTE_SUCCESS != (rc = set_topology(prte_hwloc_base_topo_file))) {
            return rc;
        }
    }

   /* remove a few things that we know cause topology differences
    * that aren't really of interest
    */
    obj = hwloc_get_root_obj(prte_hwloc_topology);
    i = 0;
    while (i < obj->infos_count) {
        if (NULL != obj->infos[i].name && NULL != obj->infos[i].value &&
            (0 == strcmp(obj->infos[i].name, "HostName") ||
             0 == strcmp(obj->infos[i].name, "ProcessName"))) {
            free(obj->infos[i].name);
            free(obj->infos[i].value);
            /* left justify the array */
            for (j = i; j < obj->infos_count - 1; j++) {
                obj->infos[j] = obj->infos[j + 1];
            }
            obj->infos[obj->infos_count - 1].name = NULL;
            obj->infos[obj->infos_count - 1].value = NULL;
            obj->infos_count--;
            /* the entry that just shifted into slot i has not been examined
             * yet, so do not advance - two removable entries side by side
             * used to leave the second one behind */
            continue;
        }
        i++;
    }

    /* fill prte_cache_line_size global with the smallest L2 cache
       line size, falling back to L1 */
    fill_cache_line_size();

    // create the summary
    prte_hwloc_base_setup_summary(prte_hwloc_topology);
    return PRTE_SUCCESS;
}

/* Replace the process-wide topology with the one described by an XML file.
 *
 * Every failure path here has to NULL the global. This used to destroy the
 * topology and leave the pointer set, so prte_hwloc_base_close() saw a
 * non-NULL handle and released its userdata and destroyed it a second time -
 * a use-after-free on the way out of a run that merely named an unreadable
 * hwloc_use_topo_file.
 */
static int set_topology(char *topofile)
{
    int rc;
    struct hwloc_topology_support *support;

    PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output,
                        "hwloc:base:set_topology %s", topofile));

    if (NULL != prte_hwloc_topology) {
        /* the cached summary and placement counters are ours, not hwloc's */
        prte_hwloc_base_release_userdata(prte_hwloc_topology);
        hwloc_topology_destroy(prte_hwloc_topology);
        prte_hwloc_topology = NULL;
    }
    if (0 != hwloc_topology_init(&prte_hwloc_topology)) {
        prte_hwloc_topology = NULL;
        return PRTE_ERR_NOT_SUPPORTED;
    }
    if (0 != hwloc_topology_set_xml(prte_hwloc_topology, topofile)) {
        hwloc_topology_destroy(prte_hwloc_topology);
        prte_hwloc_topology = NULL;
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base:set_topology bad topo file"));
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* no extra topology flags for an imported description - the support
     * bits it needs are asserted by hand below, after the load */
    rc = topology_set_flags(prte_hwloc_topology, 0, true);
    if (0 != rc) {
        hwloc_topology_destroy(prte_hwloc_topology);
        prte_hwloc_topology = NULL;
        return PRTE_ERR_NOT_SUPPORTED;
    }

    if (0 != hwloc_topology_load(prte_hwloc_topology)) {
        hwloc_topology_destroy(prte_hwloc_topology);
        prte_hwloc_topology = NULL;
        PMIX_OUTPUT_VERBOSE((5, prte_hwloc_base_output, "hwloc:base:set_topology failed to load"));
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* hwloc zeroes every support bit of an imported topology - it has no way
     * to know what the machine the XML describes can do, and says so:
     * "binding is disabled by default ... also marked by putting zeroes in
     * the corresponding supported feature bits". But this option means "this
     * file describes the machine I am running on", and
     * prte_rmaps_base_check_support() refuses any *explicitly requested*
     * binding when those bits are clear - so "--prtemca hwloc_use_topo_file
     * X --bind-to core" was answered with "at least one node does NOT support
     * binding processes to cpus" on a machine that supports it perfectly,
     * which leaves the option usable only with --bind-to none.
     *
     * So assert them, as this code has always meant to. Note the "always
     * meant to": the assertion used to sit *before* hwloc_topology_load(),
     * and hwloc's own header says the support structure "only contains valid
     * information after" the load - the load filled the struct in and wiped
     * it. It has to be here, after.
     *
     * HWLOC_TOPOLOGY_FLAG_IMPORT_SUPPORT is not the answer: it reports what
     * the *exporting* machine could do, which is a different question and,
     * per the commit that took it back out, not a reliable one. */
    support = (struct hwloc_topology_support *)
              hwloc_topology_get_support(prte_hwloc_topology);
    if (NULL != support) {
        support->cpubind->set_thisproc_cpubind = true;
        support->cpubind->set_thisthread_cpubind = true;
        support->membind->set_thisproc_membind = true;
        support->membind->set_thisthread_membind = true;
    }

    /* all done */
    return PRTE_SUCCESS;
}

/* get the number of pu's under a given hwloc object */
unsigned int prte_hwloc_base_get_npus(hwloc_topology_t topo, bool use_hwthread_cpus,
                                      hwloc_cpuset_t envelope, hwloc_obj_t obj)
{
    unsigned int cnt = 0;
    hwloc_cpuset_t avail;

    if (NULL == obj || NULL == obj->cpuset) {
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
            /* The cutoff has to exist before we can answer. Returning 0 here
             * is not a safe default - it reports "this node has no NUMA
             * domains", which is how a map-by numa job silently maps nothing
             * on any topology whose summary had not been built yet. */
            prte_hwloc_base_setup_summary(topo);
            sum = (prte_hwloc_topo_data_t *) root->userdata;
            if (NULL == sum) {
                return 0;
            }
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
            /* see the note in prte_hwloc_base_get_nbobjs_by_type() */
            prte_hwloc_base_setup_summary(topo);
            sum = (prte_hwloc_topo_data_t *) root->userdata;
            if (NULL == sum) {
                return NULL;
            }
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

    /* Every package id below comes from a user-supplied string, so the
     * lookup can legitimately fail. It used to be dereferenced unchecked,
     * which turned "--cpu-set P99" into a segfault. */
    range = PMIx_Argv_split(cpus, '-');
    range_cnt = PMIx_Argv_count(range);
    switch (range_cnt) {
    case 1: /* no range was present, so just one package given */
        if (!parse_cpu_id(range[0], &package_id)) {
            PMIx_Argv_free(range);
            return PRTE_ERR_BAD_PARAM;
        }
        obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
        if (NULL == obj) {
            PMIx_Argv_free(range);
            return PRTE_ERR_NOT_FOUND;
        }
        /* get the available cpus for this package */
        hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        break;

    case 2: /* range of packages was given */
        if (!parse_cpu_id(range[0], &lower_range) ||
            !parse_cpu_id(range[1], &upper_range) ||
            upper_range < lower_range) {
            PMIx_Argv_free(range);
            return PRTE_ERR_BAD_PARAM;
        }
        /* cycle across the range of packages */
        for (package_id = lower_range; package_id <= upper_range; package_id++) {
            obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
            if (NULL == obj) {
                PMIx_Argv_free(range);
                return PRTE_ERR_NOT_FOUND;
            }
            /* set the available cpus for this package bits in the bitmask */
            hwloc_bitmap_or(cpumask, cpumask, obj->cpuset);
        }
        break;
    default:
        PMIx_Argv_free(range);
        return PRTE_ERROR;
    }
    PMIx_Argv_free(range);

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

    package_core = PMIx_Argv_split(package_core_list, ':');
    if (NULL == package_core || !parse_cpu_id(package_core[0], &package_id)) {
        PMIx_Argv_free(package_core);
        return PRTE_ERR_BAD_PARAM;
    }

    /* get the object for this package id */
    package = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, package_id);
    if (NULL == package) {
        PMIx_Argv_free(package_core);
        return PRTE_ERR_NOT_FOUND;
    }

    /* as described in comment near top of file, hwloc isn't able
     * to find cores on all platforms. Adjust the type here if
     * required
     */
    if (NULL == prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0)) {
        obj_type = HWLOC_OBJ_PU;
    }

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
            range = PMIx_Argv_split(corestr, '-');
            range_cnt = PMIx_Argv_count(range);
            /* see if a range was set or not */
            switch (range_cnt) {
            case 1: /* only one core, or a list of cores, specified */
                list = PMIx_Argv_split(range[0], ',');
                for (j = 0; NULL != list[j]; j++) {
                    /* get the indexed core from this package */
                    if (!parse_cpu_id(list[j], &core_id)) {
                        rc = PRTE_ERR_BAD_PARAM;
                        break;
                    }
                    /* The id is relative to this package. Ask hwloc for the
                     * nth object *inside* the package's cpuset rather than
                     * offsetting into the global index by "cores per package
                     * times package id" - that arithmetic silently names an
                     * object in the wrong package the moment two packages
                     * do not hold the same number of them. */
                    core = hwloc_get_obj_inside_cpuset_by_type(topo, package->cpuset,
                                                               obj_type, core_id);
                    if (NULL == core) {
                        rc = PRTE_ERR_NOT_FOUND;
                        break;
                    }
                    /* get the cpus */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                PMIx_Argv_free(list);
                break;

            case 2: /* range of core id's was given */
                pmix_output_verbose(5, prte_hwloc_base_output,
                                    "range of cores given: start %s stop %s", range[0], range[1]);
                if (!parse_cpu_id(range[0], &lower_range) ||
                    !parse_cpu_id(range[1], &upper_range)) {
                    PMIx_Argv_free(range);
                    PMIx_Argv_free(package_core);
                    return PRTE_ERR_BAD_PARAM;
                }
                for (j = lower_range; j <= upper_range; j++) {
                    /* the indexed object within this package - see the note
                     * on the single-id case above */
                    core = hwloc_get_obj_inside_cpuset_by_type(topo, package->cpuset,
                                                               obj_type, j);
                    if (NULL == core) {
                        rc = PRTE_ERR_NOT_FOUND;
                        break;
                    }
                    /* get the cpus add them into the result */
                    hwloc_bitmap_or(cpumask, cpumask, core->cpuset);
                }
                break;

            default:
                PMIx_Argv_free(range);
                PMIx_Argv_free(package_core);
                return PRTE_ERROR;
            }
            PMIx_Argv_free(range);
        }
    }
    PMIx_Argv_free(package_core);

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

    /* bozo checks - test the topology we were actually handed. Checking the
     * process-wide one instead let a NULL "topo" through on any daemon that
     * had sensed its own topology, which is all of them. */
    if (NULL == topo) {
        return PRTE_ERR_NOT_SUPPORTED;
    }
    if (NULL == slot_str || 0 == strlen(slot_str)) {
        return PRTE_ERR_BAD_PARAM;
    }

    pmix_output_verbose(5, prte_hwloc_base_output, "slot assignment: slot_list == %s", slot_str);

    /* split at ';' */
    item = PMIx_Argv_split(slot_str, ';');

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
                rngs = PMIx_Argv_split(&item[i][1], ',');
                for (j = 0; NULL != rngs[j]; j++) {
                    if (PRTE_SUCCESS != (rc = package_to_cpu_set(rngs[j], topo, cpumask))) {
                        PMIx_Argv_free(rngs);
                        PMIx_Argv_free(item);
                        return rc;
                    }
                }
                PMIx_Argv_free(rngs);
            } else {
                if ('P' == item[i][0] || 'p' == item[i][0] || 'S' == item[i][0]
                    || 's' == item[i][0]) {
                    lst = &item[i][1];
                } else {
                    lst = item[i];
                }
                if (PRTE_SUCCESS != (rc = package_core_to_cpu_set(lst, topo, cpumask))) {
                    PMIx_Argv_free(item);
                    return rc;
                }
            }
        } else {
            rngs = PMIx_Argv_split(item[i], ',');
            for (k = 0; NULL != rngs[k]; k++) {
                /* just a core specification - see if one or a range was given */
                range = PMIx_Argv_split(rngs[k], '-');
                range_cnt = PMIx_Argv_count(range);
                /* see if a range was set or not */
                switch (range_cnt) {
                case 1: /* only one core, or a list of cores, specified */
                    list = PMIx_Argv_split(range[0], ',');
                    for (j = 0; NULL != list[j]; j++) {
                        if (!parse_cpu_id(list[j], &core_id)) {
                            PMIx_Argv_free(range);
                            PMIx_Argv_free(item);
                            PMIx_Argv_free(rngs);
                            PMIx_Argv_free(list);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        /* find the specified available cpu */
                        if (NULL == (pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, core_id))) {
                            PMIx_Argv_free(range);
                            PMIx_Argv_free(item);
                            PMIx_Argv_free(rngs);
                            PMIx_Argv_free(list);
                            return PRTE_ERR_NOT_FOUND;
                        }
                        /* get the cpus for that object and set them in the massk*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    PMIx_Argv_free(list);
                    break;

                case 2: /* range of core id's was given */
                    if (!parse_cpu_id(range[0], &lower_range) ||
                        !parse_cpu_id(range[1], &upper_range)) {
                        PMIx_Argv_free(range);
                        PMIx_Argv_free(item);
                        PMIx_Argv_free(rngs);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    for (core_id = lower_range; core_id <= upper_range; core_id++) {
                        /* find the specified logical available cpu */
                        if (NULL == (pu = prte_hwloc_base_get_pu(topo, use_hwthread_cpus, core_id))) {
                            PMIx_Argv_free(range);
                            PMIx_Argv_free(item);
                            PMIx_Argv_free(rngs);
                            return PRTE_ERR_NOT_FOUND;
                        }
                        /* get the cpus for that object and set them in the mask*/
                        hwloc_bitmap_or(cpumask, cpumask, pu->cpuset);
                    }
                    break;

                default:
                    PMIx_Argv_free(range);
                    PMIx_Argv_free(item);
                    PMIx_Argv_free(rngs);
                    return PRTE_ERROR;
                }
                PMIx_Argv_free(range);
            }
            PMIx_Argv_free(rngs);
        }
    }
    PMIx_Argv_free(item);
    return PRTE_SUCCESS;
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

static int compare_unsigned(const void *a, const void *b)
{
    unsigned x = *(const unsigned *) a;
    unsigned y = *(const unsigned *) b;

    /* subtracting unsigneds and truncating to int gets the order wrong for
     * values more than INT_MAX apart */
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

/* Resolve the set bits of a cpuset to the objects that own them.
 *
 * The bits of an hwloc cpuset are always PU *OS* indices. What PRRTE reports
 * to a user is a list of cores - or of hwthreads, when the job is treating
 * hwthreads as cpus - and every PRRTE grammar that accepts such a list
 * (--cpu-set, the rankfile slot lists) is expressed in hwloc *logical*
 * indices. So a rendering has to walk each bit up to the object it is being
 * reported in terms of and take that object's index, logical unless the
 * caller asked for physical.
 *
 * Printing the raw bit numbers instead is right only by coincidence - when
 * a PU's logical and OS indices happen to agree - and it is wrong on
 * precisely the machines where the answer matters: any SMT node rendered in
 * hwthreads (PU 1 of core 0 is OS index 24 on a 24-core part), and any node
 * whose firmware interleaves cpu numbers across packages. Reporting a core
 * that way is worse than cosmetic: the user reads the number back out and
 * hands it to --cpu-set, which resolves it logically.
 *
 * On success writes a malloc'd, sorted, duplicate-free array of indices to
 * *sites and returns how many there are. Returns -1 if a bit could not be
 * resolved (already reported through show_help) or on allocation failure.
 */
static int collect_sites(hwloc_topology_t topo, hwloc_const_cpuset_t bitmap,
                         bool as_hwthreads, bool physical,
                         unsigned **sites)
{
    hwloc_obj_type_t leaf = as_hwthreads ? HWLOC_OBJ_PU : HWLOC_OBJ_CORE;
    unsigned *indices, val;
    int nsites = 0, n, weight, id;
    hwloc_obj_t obj;
    bool unique;

    *sites = NULL;
    weight = hwloc_bitmap_weight(bitmap);
    if (0 >= weight) {
        /* weight() is -1 for an infinitely-set bitmap, which no topology
         * produces, and 0 for an empty one - neither has any sites */
        return 0;
    }
    /* each set bit contributes at most one site, so this is an exact bound */
    indices = (unsigned *) malloc(weight * sizeof(unsigned));
    if (NULL == indices) {
        return -1;
    }

    for (id = hwloc_bitmap_first(bitmap); -1 != id; id = hwloc_bitmap_next(bitmap, id)) {
        obj = hwloc_get_pu_obj_by_os_index(topo, (unsigned) id);
        /* a PU is its own leaf; a core is the first CORE ancestor */
        while (NULL != obj && leaf != obj->type) {
            obj = obj->parent;
        }
        if (NULL == obj) {
            pmix_show_help("help-prte-hwloc-base.txt", "pu-not-found", true, (unsigned) id);
            free(indices);
            return -1;
        }
        val = physical ? obj->os_index : obj->logical_index;
        if (HWLOC_UNKNOWN_INDEX == val) {
            /* XML written by hand, or by an hwloc old enough not to record
             * OS indices, carries no physical index. The logical one always
             * exists, so report that rather than (unsigned)-1. */
            val = obj->logical_index;
        }
        /* several PUs can sit under one core, so the same site comes up
         * once per hwthread */
        unique = true;
        for (n = 0; n < nsites; n++) {
            if (indices[n] == val) {
                unique = false;
                break;
            }
        }
        if (unique) {
            indices[nsites] = val;
            ++nsites;
        }
    }

    qsort(indices, nsites, sizeof(unsigned), compare_unsigned);
    *sites = indices;
    return nsites;
}

/* Are we reporting this topology in hwthreads? Either because the job asked
 * for it, or because the topology has no cores to report - hwloc does not
 * find cores on every platform (PPC64 on old Linux kernels reports only NUMA
 * nodes and PUs), and on such a machine a core-based rendering resolves
 * nothing and every binding comes out "UNBOUND". */
static bool report_as_hwthreads(hwloc_topology_t topo, bool use_hwthread_cpus)
{
    return (use_hwthread_cpus || !prte_hwloc_base_has_cores(topo));
}

char *prte_hwloc_base_cpuset2ranges(hwloc_topology_t topo,
                                    hwloc_const_cpuset_t bitmap,
                                    bool use_hwthread_cpus,
                                    bool physical)
{
    unsigned *sites = NULL, start, end;
    int nsites, n;
    char **output = NULL, *result;
    char tmp[64];

    nsites = collect_sites(topo, bitmap, report_as_hwthreads(topo, use_hwthread_cpus),
                           physical, &sites);
    if (0 >= nsites) {
        free(sites);
        return NULL;
    }

    /* Walk the sorted indices, collapsing consecutive runs into "lo-hi".
     * [start,end] is the run currently being accumulated; it is emitted as
     * soon as the next index does not extend it, and once more at the end
     * for the run left dangling. */
    start = sites[0];
    end = sites[0];
    for (n = 1; n <= nsites; n++) {
        if (n < nsites && sites[n] == end + 1) {
            end = sites[n];
            continue;
        }
        if (start == end) {
            snprintf(tmp, sizeof(tmp), "%u", start);
        } else {
            snprintf(tmp, sizeof(tmp), "%u-%u", start, end);
        }
        PMIx_Argv_append_nosize(&output, tmp);
        if (n < nsites) {
            start = sites[n];
            end = sites[n];
        }
    }
    free(sites);

    result = PMIx_Argv_join(output, ',');
    PMIx_Argv_free(output);
    return result;
}

/* Append one formatted item to a bounded buffer, snprintf-style.
 *
 * "total" is how many bytes the output would need so far, which can exceed
 * "buflen" once we have truncated - so the room handed to vsnprintf is
 * clamped rather than subtracted. Returns what this item would have needed;
 * the caller adds it to its running total. An earlier version advanced the
 * write pointer through a run of set bits without also charging the bytes
 * against the remaining budget, so a process bound to more than a few cores
 * walked straight off the end of the caller's buffer - in the HNP, which
 * holds every node's topology, so the corruption surfaced somewhere else
 * entirely.
 */
static int bounded_append(char *buf, size_t buflen, size_t total,
                          const char *fmt, ...)
    __prte_attribute_format__(__printf__, 4, 5);

static int bounded_append(char *buf, size_t buflen, size_t total,
                          const char *fmt, ...)
{
    char *at = (total < buflen) ? buf + total : NULL;
    size_t room = (total < buflen) ? buflen - total : 0;
    va_list ap;
    int res;

    va_start(ap, fmt);
    res = vsnprintf(at, room, fmt, ap);
    va_end(ap);
    return res;
}

/*
 * Render a process' binding as XML: one <package> element per package the
 * process has cpus in, each holding one <core>/<hwt> element per site.
 *
 * "cores" always comes back NUL-terminated, and the caller emits it verbatim
 * inside its <binding> element.
 *
 * This used to render only the *sites*, into the caller's single <package>
 * element, and report the package number through an out parameter - which
 * made the output of a process bound across more than one package not merely
 * incomplete but wrong: each package overwrote the previous one from the top
 * of the buffer, so the rendering named the last package and dropped every
 * earlier one's cores. The short form of the same binding
 * (prte_hwloc_base_cset2str, "--display map") has always reported every
 * package, so the two renderings of one binding disagreed - and the one that
 * was wrong is the machine-readable one. It is reachable: PRRTE's own
 * cross-package diagnostic points users at the rankfile mapper, and a
 * rankfile slot list of "P0:0;P1:0" produces exactly this.
 */
void prte_hwloc_get_binding_info(hwloc_const_cpuset_t cpuset,
                                 bool use_hwthread_cpus, bool physical,
                                 hwloc_topology_t topo,
                                 char *cores, int sz)
{
    int n, npkgs, nsites, i, res;
    size_t total = 0, buflen;
    hwloc_cpuset_t avail;
    hwloc_obj_t pkg;
    unsigned *sites = NULL;
    bool as_hwt;
    const char *type;

    if (NULL == cores || 0 >= sz) {
        return;
    }
    buflen = (size_t) sz;
    cores[0] = '\0';

    /* If the cpuset is all zero, then something is wrong. Say so and stop -
     * the scan below would otherwise find nothing and leave the message in
     * place only by accident. The element name carries no space: this goes
     * into the document "--display map:parseable" produces, and an element
     * whose name contains a space does not parse. */
    if (hwloc_bitmap_iszero(cpuset)) {
        snprintf(cores, buflen, "%*c<empty_cpuset/>\n", 16, ' ');
        return;
    }

    /* get the number of packages in the topology */
    npkgs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE);
    avail = hwloc_bitmap_alloc();
    as_hwt = report_as_hwthreads(topo, use_hwthread_cpus);
    type = as_hwt ? "hwt" : "core";

    for (n = 0; n < npkgs; n++) {
        pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, n);
        if (NULL == pkg || NULL == pkg->cpuset) {
            continue;
        }
        /* see if we have any here */
        hwloc_bitmap_and(avail, cpuset, pkg->cpuset);
        if (hwloc_bitmap_iszero(avail)) {
            continue;
        }
        nsites = collect_sites(topo, avail, as_hwt, physical, &sites);
        if (0 >= nsites) {
            free(sites);
            sites = NULL;
            continue;
        }
        res = bounded_append(cores, buflen, total, "%*c<package id=\"%d\">\n",
                             16, ' ', n);
        if (0 > res) {
            free(sites);
            break;
        }
        total += (size_t) res;
        for (i = 0; i < nsites; i++) {
            res = bounded_append(cores, buflen, total, "%*c<%s>%u</%s>\n",
                                 20, ' ', type, sites[i], type);
            if (0 > res) {
                break;
            }
            total += (size_t) res;
        }
        free(sites);
        sites = NULL;
        res = bounded_append(cores, buflen, total, "%*c</package>\n", 16, ' ');
        if (0 > res) {
            break;
        }
        total += (size_t) res;
    }
    hwloc_bitmap_free(avail);
}

/*
 * Make a prettyprint string for a hwloc_cpuset_t
 */
char *prte_hwloc_base_cset2str(hwloc_const_cpuset_t cpuset,
                               bool use_hwthread_cpus,
                               bool physical,
                               hwloc_topology_t topo)
{
    int n, npkgs;
    hwloc_cpuset_t avail;
    char **output = NULL, *result, *ranges, *ans;
    hwloc_obj_t pkg;
    const char *prefix;

    /* if the cpuset is all zero, then something is wrong */
    if (hwloc_bitmap_iszero(cpuset)) {
        return strdup("EMPTY CPUSET");
    }

    /* NOTE: there used to be an "unbound" shortcut here that tested the
     * cpuset against the node's available cpus and hwloc_bitmap_isfull().
     * A topology's cpuset is a finite set, so isfull() is never true for
     * it and the branch could not fire; it only ever leaked the bitmap it
     * had just allocated. Deciding that a process is unbound is the
     * caller's job anyway - each one already prints "UNBOUND" when the
     * proc carries no cpuset at all - so the test is gone rather than
     * repaired, which would have changed --display bind output. */

    if (report_as_hwthreads(topo, use_hwthread_cpus)) {
        prefix = physical ? "hwt:P" : "hwt:L";
    } else {
        prefix = physical ? "core:P" : "core:L";
    }

    /* get the number of packages in the topology */
    npkgs = prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE);
    avail = hwloc_bitmap_alloc();

    for (n = 0; n < npkgs; n++) {
        pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, n);
        if (NULL == pkg || NULL == pkg->cpuset) {
            continue;
        }
        /* see if we have any here */
        hwloc_bitmap_and(avail, cpuset, pkg->cpuset);
        if (hwloc_bitmap_iszero(avail)) {
            continue;
        }
        ranges = prte_hwloc_base_cpuset2ranges(topo, avail, use_hwthread_cpus, physical);
        if (NULL == ranges) {
            PMIx_Argv_free(output);
            hwloc_bitmap_free(avail);
            return NULL;
        }
        pmix_asprintf(&ans, "package[%d][%s%s]", n, prefix, ranges);
        free(ranges);
        PMIx_Argv_append_nosize(&output, ans);
        free(ans);
    }

    if (NULL != output) {
        result = PMIx_Argv_join(output, ' ');
        PMIx_Argv_free(output);
    } else {
        result = NULL;
    }
    hwloc_bitmap_free(avail);
    return result;
}

static int topology_set_flags(hwloc_topology_t topology, unsigned long flags, bool io)
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

/* Wide enough for the hex cpuset of a very large machine:
 * hwloc_bitmap_snprintf() spends ~11 characters per 32 bits of cpuset, so
 * this covers a few thousand PUs. The buffer used to be declared 1024 bytes
 * while the cpuset call was told it had PRTE_HWLOC_MAX_STRING - a stack
 * overflow waiting for a machine wide enough to reach it. */
#define PRTE_HWLOC_MAX_STRING 2048

static void print_hwloc_obj(char **output, char *prefix, hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t obj2;
    char string[PRTE_HWLOC_MAX_STRING], *tmp, *tmp2, *pfx;
    unsigned nchildren;
    struct hwloc_topology_support *support;

    /* Count every child, not just the normal ones. In hwloc 2.x an object's
     * children are split across four lists, and "arity" covers only the
     * first of them - so reporting it alone understates the object for
     * exactly the levels a user is most likely to be looking for. */
    nchildren = obj->arity + obj->memory_arity + obj->io_arity + obj->misc_arity;

    /* print the object type */
    hwloc_obj_type_snprintf(string, sizeof(string), obj, 1);
    pmix_asprintf(&pfx, "\n%s\t", (NULL == prefix) ? "" : prefix);
    pmix_asprintf(&tmp, "%sType: %s Number of child objects: %u%sName=%s",
                  (NULL == prefix) ? "" : prefix, string, nchildren, pfx,
                  (NULL == obj->name) ? "NULL" : obj->name);
    if (0 < hwloc_obj_attr_snprintf(string, sizeof(string), obj, pfx, 1)) {
        /* print the attributes */
        pmix_asprintf(&tmp2, "%s%s%s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    /* print the cpusets - apparently, some new HWLOC types don't
     * have cpusets, so protect ourselves here
     */
    if (NULL != obj->cpuset) {
        hwloc_bitmap_snprintf(string, sizeof(string), obj->cpuset);
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
    /* Walk *all* the children. "obj->children" is the normal-children array
     * only: in hwloc 2.x NUMA nodes hang off memory_first_child, and the
     * I/O objects this directory deliberately asks hwloc to keep
     * (HWLOC_TYPE_FILTER_KEEP_IMPORTANT, see topology_set_flags) hang off
     * io_first_child. Recursing through the array therefore printed a
     * topology with no NUMA domains in it at all - on a runtime whose
     * "--map-by numa" is the thing a user is most often trying to
     * understand when they ask for the topology. hwloc_get_next_child()
     * walks normal, then memory, then I/O, then Misc. */
    obj2 = hwloc_get_next_child(topo, obj, NULL);
    while (NULL != obj2) {
        /* print the object */
        print_hwloc_obj(&tmp2, pfx, topo, obj2);
        obj2 = hwloc_get_next_child(topo, obj, obj2);
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

    /* every caller hands us a topology out of prte_node_topologies and
     * prints the result unconditionally, so answer rather than crash if the
     * node's topology has not arrived */
    *output = NULL;
    if (NULL == src) {
        return PRTE_ERR_NOT_FOUND;
    }
    /* get root object */
    obj = hwloc_get_root_obj(src);
    if (NULL == obj) {
        return PRTE_ERR_NOT_FOUND;
    }
    /* print it */
    print_hwloc_obj(&tmp, prefix, src, obj);
    *output = tmp;
    return PRTE_SUCCESS;
}

/* zero the placement counters hanging off every object at one level of a
 * topology. Only ever called for a level below the root: depth 0 carries a
 * prte_hwloc_topo_data_t, and reinterpreting that as a counter would
 * scribble on numa_cutoff. */
static void reset_level_counters(hwloc_topology_t topo, int depth)
{
    hwloc_obj_t obj;
    prte_hwloc_obj_data_t *objcnt;
    unsigned width, w;

    width = hwloc_get_nbobjs_by_depth(topo, depth);
    for (w = 0; w < width; w++) {
        obj = hwloc_get_obj_by_depth(topo, depth, w);
        if (NULL != obj && NULL != obj->userdata) {
            objcnt = (prte_hwloc_obj_data_t *) obj->userdata;
            objcnt->nprocs = 0;
        }
    }
}

void prte_hwloc_base_reset_counters(void)
{
    prte_topology_t *ptopo;
    hwloc_topology_t topo;
    hwloc_obj_type_t type;
    unsigned depth, d;
    int n;

    /* this can be a fairly expensive operation as we must traverse
     * all objects of interest in all topologies since we cannot
     * know which ones might have been used. Fortunately, we almost
     * always have only one topology, and there aren't that many
     * objects in it - so this normally goes fairly quickly
     */

    /* the registry does not exist until prte_init(), and this is reachable
     * from anything that resolves a mapping */
    if (NULL == prte_node_topologies) {
        return;
    }

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
            /* if it isn't one of interest, then ignore it. NUMA nodes are
             * deliberately absent from this list - they cannot appear at a
             * normal depth in hwloc 2.x, and are swept separately below. */
            if (HWLOC_OBJ_PACKAGE != type &&
                HWLOC_OBJ_L1CACHE != type && HWLOC_OBJ_L2CACHE != type && HWLOC_OBJ_L3CACHE != type &&
                HWLOC_OBJ_CORE != type && HWLOC_OBJ_PU != type) {
                continue;
            }
            reset_level_counters(topo, d);
        }
        /* NUMA nodes live at a special depth of their own, so a walk over
         * 0..get_depth() never visits one. Binding attaches a counter to a
         * NUMA node exactly as it does to a package (--bind-to numa), and
         * missing them here left those counters accumulating across every
         * job in the life of a DVM: the second "--bind-to numa:limit=N" job
         * found every domain already at its limit and could not be bound. */
        reset_level_counters(topo, HWLOC_TYPE_DEPTH_NUMANODE);
    }
}

/* release the userdata hanging off every object at one level of a topology */
static void release_level_userdata(hwloc_topology_t topo, int depth)
{
    hwloc_obj_t obj;
    unsigned width, w;

    width = hwloc_get_nbobjs_by_depth(topo, depth);
    for (w = 0; w < width; w++) {
        obj = hwloc_get_obj_by_depth(topo, depth, w);
        if (NULL != obj && NULL != obj->userdata) {
            /* both of the things we attach - the topology summary on the
             * root and the per-object placement counters - are PMIx
             * objects, so a release is correct either way */
            PMIX_RELEASE(obj->userdata);
            obj->userdata = NULL;
        }
    }
}

void prte_hwloc_base_release_userdata(hwloc_topology_t topo)
{
    int depth, d;

    if (NULL == topo) {
        return;
    }

    /* the normal levels, root (depth 0) included */
    depth = hwloc_topology_get_depth(topo);
    for (d = 0; d < depth; d++) {
        release_level_userdata(topo, d);
    }
    /* NUMA nodes are not part of the normal depth hierarchy in hwloc 2.x,
     * so they have to be swept separately - binding can attach counters to
     * them just as it does to packages and cores */
    release_level_userdata(topo, HWLOC_TYPE_DEPTH_NUMANODE);
}
