/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 */

#ifndef PRRTE_MCA_HWLOC_H
#define PRRTE_MCA_HWLOC_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <stdint.h>
#include <stdarg.h>
#include PRRTE_HWLOC_HEADER
#if ! PRRTE_HWLOC_HEADER_GIVEN
  #if HWLOC_API_VERSION >= 0x20000
  #include <hwloc/shmem.h>
  #endif
#endif

#include "src/class/prrte_list.h"
#include "src/class/prrte_value_array.h"
#include "src/dss/dss_types.h"

BEGIN_C_DECLS

/* ******************************************************************** */
/* Although we cannot bind if --without-hwloc is set,
 * we do still need to know some basic locality data
 * like on_node and not_on_node. So ensure that we
 * always have access to that much info by including
 * the definitions here, outside the if-have-hwloc test
 */
typedef uint16_t prrte_hwloc_locality_t;
#define PRRTE_HWLOC_LOCALITY_T PRRTE_UINT16

/** Process locality definitions */
enum {
    PRRTE_PROC_LOCALITY_UNKNOWN  = 0x0000,
    PRRTE_PROC_NON_LOCAL         = 0x8000,
    PRRTE_PROC_ON_CLUSTER        = 0x0001,
    PRRTE_PROC_ON_CU             = 0x0002,
    PRRTE_PROC_ON_HOST           = 0x0004,
    PRRTE_PROC_ON_BOARD          = 0x0008,
    PRRTE_PROC_ON_NODE           = 0x000c,   // same host and board
    PRRTE_PROC_ON_NUMA           = 0x0010,
    PRRTE_PROC_ON_SOCKET         = 0x0020,
    PRRTE_PROC_ON_L3CACHE        = 0x0040,
    PRRTE_PROC_ON_L2CACHE        = 0x0080,
    PRRTE_PROC_ON_L1CACHE        = 0x0100,
    PRRTE_PROC_ON_CORE           = 0x0200,
    PRRTE_PROC_ON_HWTHREAD       = 0x0400,
    PRRTE_PROC_ALL_LOCAL         = 0x0fff,
};

/** Process locality macros */
#define PRRTE_PROC_ON_LOCAL_CLUSTER(n)   (!!((n) & PRRTE_PROC_ON_CLUSTER))
#define PRRTE_PROC_ON_LOCAL_CU(n)        (!!((n) & PRRTE_PROC_ON_CU))
#define PRRTE_PROC_ON_LOCAL_HOST(n)      (!!((n) & PRRTE_PROC_ON_HOST))
#define PRRTE_PROC_ON_LOCAL_BOARD(n)     (!!((n) & PRRTE_PROC_ON_BOARD))
#define PRRTE_PROC_ON_LOCAL_NODE(n)      (PRRTE_PROC_ON_LOCAL_HOST(n) && PRRTE_PROC_ON_LOCAL_BOARD(n))
#define PRRTE_PROC_ON_LOCAL_NUMA(n)      (!!((n) & PRRTE_PROC_ON_NUMA))
#define PRRTE_PROC_ON_LOCAL_SOCKET(n)    (!!((n) & PRRTE_PROC_ON_SOCKET))
#define PRRTE_PROC_ON_LOCAL_L3CACHE(n)   (!!((n) & PRRTE_PROC_ON_L3CACHE))
#define PRRTE_PROC_ON_LOCAL_L2CACHE(n)   (!!((n) & PRRTE_PROC_ON_L2CACHE))
#define PRRTE_PROC_ON_LOCAL_L1CACHE(n)   (!!((n) & PRRTE_PROC_ON_L1CACHE))
#define PRRTE_PROC_ON_LOCAL_CORE(n)      (!!((n) & PRRTE_PROC_ON_CORE))
#define PRRTE_PROC_ON_LOCAL_HWTHREAD(n)  (!!((n) & PRRTE_PROC_ON_HWTHREAD))

/* ******************************************************************** */

/**
 * Struct used to describe a section of memory (starting address
 * and length). This is really the same thing as an iovec, but
 * we include a separate type for it for at least 2 reasons:
 *
 * 1. Some OS's iovec definitions are exceedingly lame (e.g.,
 * Solaris 9 has the length argument as an int, instead of a
 * size_t).
 *
 * 2. We reserve the right to expand/change this struct in the
 * future.
 */
typedef struct {
    /** Starting address of segment */
    void *mbs_start_addr;
    /** Length of segment */
    size_t mbs_len;
} prrte_hwloc_base_memory_segment_t;

/* define type of processor info requested */
typedef uint8_t prrte_hwloc_resource_type_t;
#define PRRTE_HWLOC_PHYSICAL   1
#define PRRTE_HWLOC_LOGICAL    2
#define PRRTE_HWLOC_AVAILABLE  3

/* structs for storing info on objects */
typedef struct {
    prrte_object_t super;
    hwloc_cpuset_t available;
    bool npus_calculated;
    unsigned int npus;
    unsigned int idx;
    unsigned int num_bound;
} prrte_hwloc_obj_data_t;
PRRTE_CLASS_DECLARATION(prrte_hwloc_obj_data_t);

typedef struct {
    prrte_list_item_t super;
    hwloc_obj_type_t type;
    unsigned cache_level;
    unsigned int num_objs;
    prrte_hwloc_resource_type_t rtype;
    prrte_list_t sorted_by_dist_list;
} prrte_hwloc_summary_t;
PRRTE_CLASS_DECLARATION(prrte_hwloc_summary_t);

typedef struct {
    prrte_object_t super;
    hwloc_cpuset_t available;
    prrte_list_t summaries;

    /** \brief Additional space for custom data */
    void *userdata;
} prrte_hwloc_topo_data_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_hwloc_topo_data_t);

/* define binding policies */
typedef uint16_t prrte_binding_policy_t;
#define PRRTE_BINDING_POLICY PRRTE_UINT16

/* binding directives */
#define PRRTE_BIND_IF_SUPPORTED      0x1000
#define PRRTE_BIND_ALLOW_OVERLOAD    0x2000
#define PRRTE_BIND_GIVEN             0x4000
/* bind each rank to the cpu in the given
 * cpu list based on its node-local-rank */
#define PRRTE_BIND_ORDERED           0x8000

/* binding policies - any changes in these
 * values must be reflected in prrte/mca/rmaps/rmaps.h
 */
#define PRRTE_BIND_TO_NONE           1
#define PRRTE_BIND_TO_BOARD          2
#define PRRTE_BIND_TO_NUMA           3
#define PRRTE_BIND_TO_SOCKET         4
#define PRRTE_BIND_TO_L3CACHE        5
#define PRRTE_BIND_TO_L2CACHE        6
#define PRRTE_BIND_TO_L1CACHE        7
#define PRRTE_BIND_TO_CORE           8
#define PRRTE_BIND_TO_HWTHREAD       9
#define PRRTE_BIND_TO_CPUSET         10
#define PRRTE_GET_BINDING_POLICY(pol) \
    ((pol) & 0x0fff)
#define PRRTE_SET_BINDING_POLICY(target, pol) \
    (target) = (pol) | (((target) & 0x2000) | PRRTE_BIND_GIVEN)
#define PRRTE_SET_DEFAULT_BINDING_POLICY(target, pol)            \
    do {                                                        \
        if (!PRRTE_BINDING_POLICY_IS_SET((target))) {            \
            (target) = (pol) | (((target) & 0xf000) |           \
                                PRRTE_BIND_IF_SUPPORTED);        \
        }                                                       \
    } while(0);

/* check if policy is set */
#define PRRTE_BINDING_POLICY_IS_SET(pol) \
    ((pol) & 0x4000)
/* macro to detect if binding was qualified */
#define PRRTE_BINDING_REQUIRED(n) \
    (!(PRRTE_BIND_IF_SUPPORTED & (n)))
/* macro to detect if binding is forced */
#define PRRTE_BIND_OVERLOAD_ALLOWED(n) \
    (PRRTE_BIND_ALLOW_OVERLOAD & (n))
#define PRRTE_BIND_ORDERED_REQUESTED(n) \
    (PRRTE_BIND_ORDERED & (n))


/* some global values */
PRRTE_EXPORT extern hwloc_topology_t prrte_hwloc_topology;
PRRTE_EXPORT extern prrte_binding_policy_t prrte_hwloc_binding_policy;
PRRTE_EXPORT extern hwloc_cpuset_t prrte_hwloc_my_cpuset;
PRRTE_EXPORT extern bool prrte_hwloc_report_bindings;
PRRTE_EXPORT extern hwloc_obj_type_t prrte_hwloc_levels[];
PRRTE_EXPORT extern bool prrte_hwloc_use_hwthreads_as_cpus;

#if HWLOC_API_VERSION < 0x20000
#define HWLOC_OBJ_L3CACHE HWLOC_OBJ_CACHE
#define HWLOC_OBJ_L2CACHE HWLOC_OBJ_CACHE
#define HWLOC_OBJ_L1CACHE HWLOC_OBJ_CACHE
#if HWLOC_API_VERSION < 0x10a00
#define HWLOC_OBJ_NUMANODE HWLOC_OBJ_NODE
#define HWLOC_OBJ_PACKAGE HWLOC_OBJ_SOCKET
#endif
#define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 0
#define HAVE_HWLOC_TOPOLOGY_DUP 0
#else
#define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 1
#define HAVE_HWLOC_TOPOLOGY_DUP 1
#endif

/**
 * Debugging output stream
 */
PRRTE_EXPORT extern int prrte_hwloc_base_output;
PRRTE_EXPORT extern bool prrte_hwloc_base_inited;
PRRTE_EXPORT extern bool prrte_hwloc_topology_inited;

/* we always must have some minimal locality support */
#define PRRTE_HWLOC_PRINT_MAX_SIZE   50
#define PRRTE_HWLOC_PRINT_NUM_BUFS   16
typedef struct {
    char *buffers[PRRTE_HWLOC_PRINT_NUM_BUFS];
    int cntr;
} prrte_hwloc_print_buffers_t;
prrte_hwloc_print_buffers_t *prrte_hwloc_get_print_buffer(void);
extern char* prrte_hwloc_print_null;
PRRTE_EXPORT char* prrte_hwloc_base_print_locality(prrte_hwloc_locality_t locality);

PRRTE_EXPORT extern char *prrte_hwloc_base_cpu_list;
PRRTE_EXPORT extern hwloc_cpuset_t prrte_hwloc_base_given_cpus;
PRRTE_EXPORT extern char *prrte_hwloc_base_topo_file;

/* convenience macro for debugging */
#define PRRTE_HWLOC_SHOW_BINDING(n, v, t)                                \
    do {                                                                \
        char tmp1[1024];                                                \
        hwloc_cpuset_t bind;                                            \
        bind = prrte_hwloc_alloc();                                      \
        if (hwloc_get_cpubind(t, bind,                                  \
                              HWLOC_CPUBIND_PROCESS) < 0) {             \
            prrte_output_verbose(n, v,                                   \
                                "CANNOT DETERMINE BINDING AT %s:%d",    \
                                __FILE__, __LINE__);                    \
        } else {                                                        \
            prrte_hwloc_base_cset2mapstr(tmp1, sizeof(tmp1), t, bind);   \
            prrte_output_verbose(n, v,                                   \
                                "BINDINGS AT %s:%d: %s",                \
                                __FILE__, __LINE__, tmp1);              \
        }                                                               \
        hwloc_bitmap_free(bind);                                        \
    } while(0);

#if HWLOC_API_VERSION < 0x20000
#define PRRTE_HWLOC_MAKE_OBJ_CACHE(level, obj, cache_level)              \
    do {                                                                \
        obj = HWLOC_OBJ_CACHE;                                          \
        cache_level = level;                                            \
    } while(0)
#else
#define PRRTE_HWLOC_MAKE_OBJ_CACHE(level, obj, cache_level)              \
    do {                                                                \
        obj = HWLOC_OBJ_L##level##CACHE;                                \
        cache_level = 0;                                                \
    } while(0)
#endif

PRRTE_EXPORT prrte_hwloc_locality_t prrte_hwloc_base_get_relative_locality(hwloc_topology_t topo,
                                                                          char *cpuset1, char *cpuset2);

PRRTE_EXPORT int prrte_hwloc_base_set_binding_policy(prrte_binding_policy_t *policy, char *spec);

/**
 * Loads prrte_hwloc_my_cpuset (global variable in
 * src/hwloc/hwloc-internal.h) for this process.  prrte_hwloc_my_cpuset
 * will be loaded with this process' binding, or, if the process is
 * not bound, use the hwloc root object's (available and online)
 * cpuset.
 */
PRRTE_EXPORT void prrte_hwloc_base_get_local_cpuset(void);

struct prrte_rmaps_numa_node_t {
    prrte_list_item_t super;
    int index;
    float dist_from_closed;
};
typedef struct prrte_rmaps_numa_node_t prrte_rmaps_numa_node_t;
PRRTE_CLASS_DECLARATION(prrte_rmaps_numa_node_t);

/**
 * Enum for what memory allocation policy we want for user allocations.
 * MAP = memory allocation policy.
 */
typedef enum {
    PRRTE_HWLOC_BASE_MAP_NONE,
    PRRTE_HWLOC_BASE_MAP_LOCAL_ONLY
} prrte_hwloc_base_map_t;

/**
 * Global reflecting the MAP (set by MCA param).
 */
PRRTE_EXPORT extern prrte_hwloc_base_map_t prrte_hwloc_base_map;

/**
 * Enum for what to do if the hwloc framework tries to bind memory
 * and fails.  BFA = bind failure action.
 */
typedef enum {
    PRRTE_HWLOC_BASE_MBFA_SILENT,
    PRRTE_HWLOC_BASE_MBFA_WARN,
    PRRTE_HWLOC_BASE_MBFA_ERROR
} prrte_hwloc_base_mbfa_t;

/**
 * Global reflecting the BFA (set by MCA param).
 */
PRRTE_EXPORT extern prrte_hwloc_base_mbfa_t prrte_hwloc_base_mbfa;

/**
 * Discover / load the hwloc topology (i.e., call hwloc_topology_init() and
 * hwloc_topology_load()).
 */
PRRTE_EXPORT int prrte_hwloc_base_get_topology(void);

/**
 * Set the hwloc topology to that from the given topo file
 */
PRRTE_EXPORT int prrte_hwloc_base_set_topology(char *topofile);

PRRTE_EXPORT int prrte_hwloc_base_filter_cpus(hwloc_topology_t topo);

/**
 * Free the hwloc topology.
 */
PRRTE_EXPORT void prrte_hwloc_base_free_topology(hwloc_topology_t topo);
PRRTE_EXPORT unsigned int prrte_hwloc_base_get_nbobjs_by_type(hwloc_topology_t topo,
                                                              hwloc_obj_type_t target,
                                                              unsigned cache_level,
                                                              prrte_hwloc_resource_type_t rtype);
PRRTE_EXPORT void prrte_hwloc_base_clear_usage(hwloc_topology_t topo);

PRRTE_EXPORT hwloc_obj_t prrte_hwloc_base_get_obj_by_type(hwloc_topology_t topo,
                                                          hwloc_obj_type_t target,
                                                          unsigned cache_level,
                                                          unsigned int instance,
                                                          prrte_hwloc_resource_type_t rtype);
PRRTE_EXPORT unsigned int prrte_hwloc_base_get_obj_idx(hwloc_topology_t topo,
                                                       hwloc_obj_t obj,
                                                       prrte_hwloc_resource_type_t rtype);

PRRTE_EXPORT int prrte_hwloc_get_sorted_numa_list(hwloc_topology_t topo,
                                    char* device_name,
                                    prrte_list_t *sorted_list);

/**
 * Get the number of pu's under a given hwloc object.
 */
PRRTE_EXPORT unsigned int prrte_hwloc_base_get_npus(hwloc_topology_t topo,
                                                    hwloc_obj_t target);
PRRTE_EXPORT char* prrte_hwloc_base_print_binding(prrte_binding_policy_t binding);

/**
 * Determine if there is a single cpu in a bitmap.
 */
PRRTE_EXPORT bool prrte_hwloc_base_single_cpu(hwloc_cpuset_t cpuset);

/**
 * Provide a utility to parse a slot list against the local
 * cpus of given type, and produce a cpuset for the described binding
 */
PRRTE_EXPORT int prrte_hwloc_base_cpu_list_parse(const char *slot_str,
                                                  hwloc_topology_t topo,
                                                  prrte_hwloc_resource_type_t rtype,
                                                  hwloc_cpuset_t cpumask);

PRRTE_EXPORT char* prrte_hwloc_base_find_coprocessors(hwloc_topology_t topo);
PRRTE_EXPORT char* prrte_hwloc_base_check_on_coprocessor(void);


/**
 * Report a bind failure using the normal mechanisms if a component
 * fails to bind memory -- according to the value of the
 * hwloc_base_bind_failure_action MCA parameter.
 */
PRRTE_EXPORT int prrte_hwloc_base_report_bind_failure(const char *file,
                                                      int line,
                                                      const char *msg,
                                                      int rc);

/**
 * This function sets the process-wide memory affinity policy
 * according to prrte_hwloc_base_map and prrte_hwloc_base_mbfa.  It needs
 * to be a separate, standalone function (as opposed to being done
 * during prrte_hwloc_base_open()) because prrte_hwloc_topology is not
 * loaded by prrte_hwloc_base_open().  Hence, an upper layer needs to
 * invoke this function after prrte_hwloc_topology has been loaded.
 */
PRRTE_EXPORT int prrte_hwloc_base_set_process_membind_policy(void);

PRRTE_EXPORT int prrte_hwloc_base_membind(prrte_hwloc_base_memory_segment_t *segs,
                                          size_t count, int node_id);

PRRTE_EXPORT int prrte_hwloc_base_node_name_to_id(char *node_name, int *id);

PRRTE_EXPORT int prrte_hwloc_base_memory_set(prrte_hwloc_base_memory_segment_t *segments,
                                             size_t num_segments);

/* datatype support */
PRRTE_EXPORT int prrte_hwloc_pack(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals,
                                  prrte_data_type_t type);
PRRTE_EXPORT int prrte_hwloc_unpack(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals,
                                    prrte_data_type_t type);
PRRTE_EXPORT int prrte_hwloc_copy(hwloc_topology_t *dest,
                                  hwloc_topology_t src,
                                  prrte_data_type_t type);
PRRTE_EXPORT int prrte_hwloc_compare(const hwloc_topology_t topo1,
                                     const hwloc_topology_t topo2,
                                     prrte_data_type_t type);
PRRTE_EXPORT int prrte_hwloc_print(char **output, char *prefix,
                                   hwloc_topology_t src,
                                   prrte_data_type_t type);

/**
 * Make a prettyprint string for a hwloc_cpuset_t (e.g., "socket
 * 2[core 3]").
 */
PRRTE_EXPORT int prrte_hwloc_base_cset2str(char *str, int len,
                                           hwloc_topology_t topo,
                                           hwloc_cpuset_t cpuset);

/**
 * Make a prettyprint string for a cset in a map format.
 * Example: [B./..]
 * Key:  [] - signifies socket
 *        / - divider between cores
 *        . - signifies PU a process not bound to
 *        B - signifies PU a process is bound to
 */
PRRTE_EXPORT int prrte_hwloc_base_cset2mapstr(char *str, int len,
                                              hwloc_topology_t topo,
                                              hwloc_cpuset_t cpuset);

/* get the hwloc object that corresponds to the given processor id  and type */
PRRTE_EXPORT hwloc_obj_t prrte_hwloc_base_get_pu(hwloc_topology_t topo,
                                                 int lid,
                                                 prrte_hwloc_resource_type_t rtype);

/* get the topology "signature" so we can check for differences - caller
 * if responsible for freeing the returned string */
PRRTE_EXPORT char* prrte_hwloc_base_get_topo_signature(hwloc_topology_t topo);


/* get a string describing the locality of a given process */
PRRTE_EXPORT char* prrte_hwloc_base_get_locality_string(hwloc_topology_t topo, char *bitmap);

/* extract a location from the locality string */
PRRTE_EXPORT char* prrte_hwloc_base_get_location(char *locality,
                                                 hwloc_obj_type_t type,
                                                 unsigned index);

PRRTE_EXPORT prrte_hwloc_locality_t prrte_hwloc_compute_relative_locality(char *loc1, char *loc2);

PRRTE_EXPORT int prrte_hwloc_base_topology_export_xmlbuffer(hwloc_topology_t topology, char **xmlpath, int *buflen);

PRRTE_EXPORT int prrte_hwloc_base_topology_set_flags (hwloc_topology_t topology, unsigned long flags, bool io);

PRRTE_EXPORT int prrte_hwloc_base_open(void);
PRRTE_EXPORT void prrte_hwloc_base_close(void);
PRRTE_EXPORT int prrte_hwloc_base_register(void);

END_C_DECLS

#endif /* PRRTE_HWLOC_H_ */
