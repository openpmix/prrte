/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Advanced Micro Devices, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 */

#ifndef PRTE_MCA_HWLOC_H
#define PRTE_MCA_HWLOC_H

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <hwloc.h>

#include "src/class/pmix_list.h"

BEGIN_C_DECLS

/**
 * Struct used to cache topology-level data used
 * for repeated lookup - the struct is attached
 * to the userdata of the root object of the
 * topology
 */
typedef struct {
    pmix_object_t super;
    bool computed;
    unsigned numa_cutoff;
} prte_hwloc_topo_data_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_hwloc_topo_data_t);

/**
 * Struct used to cache object-level data used
 * when computing process placement - the struct
 * is attached to the userdata of each object
 * in the topology upon first use of that object
 * in a placement computation
 */
typedef struct {
    pmix_object_t super;
    unsigned nprocs;
} prte_hwloc_obj_data_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_hwloc_obj_data_t);

/* define binding policies */
typedef uint16_t prte_binding_policy_t;
#define PRTE_BINDING_POLICY PRTE_UINT16

/* binding directives - these all live in the 0xff00 half of the
 * policy word, which is what lets PRTE_SET_BINDING_POLICY replace
 * the policy while preserving the qualifiers */
#define PRTE_BIND_IF_SUPPORTED   0x1000
#define PRTE_BIND_ALLOW_OVERLOAD 0x2000
#define PRTE_BIND_GIVEN          0x4000
// overload policy was given
#define PRTE_BIND_OVERLOAD_GIVEN 0x0100

/* binding policies - any changes in these
 * values must be reflected in prte/mca/rmaps/rmaps.h
 */
#define PRTE_BIND_TO_NONE            1
#define PRTE_BIND_TO_PACKAGE         2
#define PRTE_BIND_TO_NUMA            3
#define PRTE_BIND_TO_L3CACHE         4
#define PRTE_BIND_TO_L2CACHE         5
#define PRTE_BIND_TO_L1CACHE         6
#define PRTE_BIND_TO_CORE            7
#define PRTE_BIND_TO_HWTHREAD        8
#define PRTE_GET_BINDING_POLICY(pol) ((pol) &0x00ff)
#define PRTE_SET_BINDING_POLICY(target, pol) \
    (target) = (pol) | (((target) & 0xff00) | PRTE_BIND_GIVEN)
#define PRTE_SET_DEFAULT_BINDING_POLICY(target, pol)                           \
    do {                                                                       \
        if (!PRTE_BINDING_POLICY_IS_SET((target))) {                           \
            (target) = (pol) | (((target) & 0xff00) | PRTE_BIND_IF_SUPPORTED); \
        }                                                                      \
    } while (0);

/* check if policy is set */
#define PRTE_BINDING_POLICY_IS_SET(pol) ((pol) &0x4000)
/* macro to detect if binding was qualified */
#define PRTE_BINDING_REQUIRED(n) (!(PRTE_BIND_IF_SUPPORTED & (n)))
/* macro to detect if binding is forced */
#define PRTE_BIND_OVERLOAD_ALLOWED(n)  (PRTE_BIND_ALLOW_OVERLOAD & (n))
#define PRTE_BIND_OVERLOAD_SET(n) (PRTE_BIND_OVERLOAD_GIVEN & (n))

/* some global values */
PRTE_EXPORT extern hwloc_topology_t prte_hwloc_topology;
PRTE_EXPORT extern prte_binding_policy_t prte_hwloc_default_binding_policy;
PRTE_EXPORT extern char *prte_hwloc_default_cpu_list;
PRTE_EXPORT extern bool prte_hwloc_default_use_hwthread_cpus;

/**
 * Debugging output stream
 */
PRTE_EXPORT extern int prte_hwloc_base_output;
PRTE_EXPORT extern bool prte_hwloc_base_inited;

/* Ring of per-thread scratch buffers behind prte_hwloc_base_print_binding().
 * The ring is what makes two calls in one pmix_output() argument list safe -
 * every call must therefore consume a fresh slot. */
#define PRTE_HWLOC_PRINT_MAX_SIZE 50
#define PRTE_HWLOC_PRINT_NUM_BUFS 16
typedef struct {
    char *buffers[PRTE_HWLOC_PRINT_NUM_BUFS];
    int cntr;
} prte_hwloc_print_buffers_t;
prte_hwloc_print_buffers_t *prte_hwloc_get_print_buffer(void);
extern char *prte_hwloc_print_null;

PRTE_EXPORT extern char *prte_hwloc_base_topo_file;

PRTE_EXPORT int prte_hwloc_base_set_default_binding(void *jdata,
                                                    void *options);
PRTE_EXPORT int prte_hwloc_base_set_binding_policy(void *jdata, char *spec);

/**
 * Enum for what memory allocation policy we want for user allocations.
 * MAP = memory allocation policy.
 */
typedef enum { PRTE_HWLOC_BASE_MAP_NONE, PRTE_HWLOC_BASE_MAP_LOCAL_ONLY } prte_hwloc_base_map_t;

/**
 * Global reflecting the MAP (set by MCA param).
 */
PRTE_EXPORT extern prte_hwloc_base_map_t prte_hwloc_base_map;

/**
 * Enum for what to do if the hwloc framework tries to bind memory
 * and fails.  BFA = bind failure action.
 */
typedef enum {
    PRTE_HWLOC_BASE_MBFA_SILENT,
    PRTE_HWLOC_BASE_MBFA_WARN,
    PRTE_HWLOC_BASE_MBFA_ERROR
} prte_hwloc_base_mbfa_t;

/**
 * Global reflecting the BFA (set by MCA param).
 */
PRTE_EXPORT extern prte_hwloc_base_mbfa_t prte_hwloc_base_mbfa;

/**
 * Discover / load the hwloc topology (i.e., call hwloc_topology_init() and
 * hwloc_topology_load()).
 */
PRTE_EXPORT int prte_hwloc_base_get_topology(void);

/**
 * Compute and cache the per-topology summary PRRTE hangs off the root
 * object's userdata. This must be done before any NUMA-level query is
 * made against a topology - prte_hwloc_base_get_nbobjs_by_type() and
 * prte_hwloc_base_get_obj_by_type() call it for you if it has not been
 * done yet.
 */
PRTE_EXPORT void prte_hwloc_base_setup_summary(hwloc_topology_t topo);

PRTE_EXPORT hwloc_cpuset_t prte_hwloc_base_generate_cpuset(hwloc_topology_t topo,
                                                           bool use_hwthread_cpus,
                                                           char **cpulist);

PRTE_EXPORT hwloc_cpuset_t prte_hwloc_base_filter_cpus(hwloc_topology_t topo);

PRTE_EXPORT unsigned int prte_hwloc_base_get_nbobjs_by_type(hwloc_topology_t topo,
                                                            hwloc_obj_type_t target);

PRTE_EXPORT hwloc_obj_t prte_hwloc_base_get_obj_by_type(hwloc_topology_t topo,
                                                        hwloc_obj_type_t target,
                                                        unsigned int instance);

// reset all obj counters
PRTE_EXPORT void prte_hwloc_base_reset_counters(void);

/**
 * Release the cached data objects PRRTE attaches to the userdata of a
 * topology's objects, and to the topology's root. Must be called before
 * the topology itself is destroyed - hwloc does not know these pointers
 * are ours to free.
 */
PRTE_EXPORT void prte_hwloc_base_release_userdata(hwloc_topology_t topo);

/**
 * Get the number of pu's under a given hwloc object.
 */
PRTE_EXPORT unsigned int prte_hwloc_base_get_npus(hwloc_topology_t topo, bool use_hwthread_cpus,
                                                  hwloc_cpuset_t envelope, hwloc_obj_t target);
PRTE_EXPORT char *prte_hwloc_base_print_binding(prte_binding_policy_t binding);

/**
 * Provide a utility to parse a slot list against the local
 * cpus of given type, and produce a cpuset for the described binding
 */
PRTE_EXPORT int prte_hwloc_base_cpu_list_parse(const char *slot_str, hwloc_topology_t topo,
                                                bool use_hwthread_cpus, hwloc_cpuset_t cpumask);

/**
 * Make a prettyprint string for a hwloc_cpuset_t (e.g., "package
 * 2[core 3]").
 */
PRTE_EXPORT char *prte_hwloc_base_cset2str(hwloc_const_cpuset_t cpuset,
                                           bool use_hwthread_cpus,
                                           bool physical,
                                           hwloc_topology_t topo);

PRTE_EXPORT void prte_hwloc_get_binding_info(hwloc_const_cpuset_t cpuset,
                                             bool use_hwthread_cpus,
                                             hwloc_topology_t topo, int *pkgnum,
                                             char *cores, int sz);

/* get the hwloc object that corresponds to the given processor id  and type */
PRTE_EXPORT hwloc_obj_t prte_hwloc_base_get_pu(hwloc_topology_t topo, bool use_hwthread_cpus,
                                               int lid);

PRTE_EXPORT int prte_hwloc_base_open(void);
PRTE_EXPORT void prte_hwloc_base_close(void);
PRTE_EXPORT int prte_hwloc_base_register(void);
PRTE_EXPORT int prte_hwloc_print(char **output, char *prefix, hwloc_topology_t src);

PRTE_EXPORT void prte_hwloc_build_map(hwloc_topology_t topo,
                                      hwloc_cpuset_t avail,
                                      bool use_hwthread_cpus,
                                      hwloc_bitmap_t coreset);

PRTE_EXPORT bool prte_hwloc_base_core_cpus(hwloc_topology_t topo);

/* Returns true if the topology reports any HWLOC_OBJ_CORE objects. This is
 * distinct from prte_hwloc_base_core_cpus(), which also returns false when
 * cores exist but each holds a single hwthread (so cores and PUs coincide):
 * in that case cores are still usable and "core" remains a valid object to
 * map and bind to. */
PRTE_EXPORT bool prte_hwloc_base_has_cores(hwloc_topology_t topo);

END_C_DECLS

#endif /* PRTE_HWLOC_H_ */
