/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      SiPearl.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/include/constants.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/schizo/schizo.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_tsd.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "src/util/prte_cmd_line.h"

/*
 * Globals
 */
bool prte_hwloc_base_inited = false;
hwloc_topology_t prte_hwloc_topology = NULL;
prte_hwloc_base_map_t prte_hwloc_base_map = PRTE_HWLOC_BASE_MAP_NONE;
prte_hwloc_base_mbfa_t prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_WARN;
prte_hwloc_base_memattr_t prte_hwloc_base_memattr = PRTE_HWLOC_BASE_MEMATTR_DISABLED;
prte_binding_policy_t prte_hwloc_default_binding_policy = 0;
char *prte_hwloc_default_cpu_list = NULL;
char *prte_hwloc_base_topo_file = NULL;
int prte_hwloc_base_output = -1;
bool prte_hwloc_default_use_hwthread_cpus = false;

static char *prte_hwloc_base_binding_policy = NULL;
static int verbosity = 0;
static char *default_cpu_list = NULL;
static bool bind_to_core = false;
static bool bind_to_socket = false;
/* Each string MCA parameter needs its own backing store: the MCA layer
 * keeps the address we hand it, reports the current value through it,
 * and frees it at finalize. Two parameters sharing one variable means
 * each reports the other's value. */
static char *mem_alloc_policy = NULL;
static char *mem_bind_failure_action = NULL;
static char *default_membind = NULL;

int prte_hwloc_base_register(void)
{
    int ret;
    char *ptr;

    /* debug output */
    ret = pmix_mca_base_var_register("prte", "hwloc", "base", "verbose", "Debug verbosity",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &verbosity);
    pmix_mca_base_var_register_synonym(ret, "opal", "hwloc", "base", "verbose",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    if (0 < verbosity) {
        prte_hwloc_base_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_hwloc_base_output, verbosity);
    }

    /* handle some deprecated options */
    prte_hwloc_default_use_hwthread_cpus = false;
    (void) pmix_mca_base_var_register("prte", "hwloc", "base", "use_hwthreads_as_cpus",
                                      "Use hardware threads as independent cpus",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_hwloc_default_use_hwthread_cpus);

    (void) pmix_mca_base_var_register("prte", "hwloc", "base", "bind_to_core",
                                      "Bind processes to cores",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &bind_to_core);

    (void) pmix_mca_base_var_register("prte", "hwloc", "base", "bind_to_socket",
                                      "Bind processes to sockets",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &bind_to_socket);

    /* hwloc_base_mbind_policy */

    prte_hwloc_base_map = PRTE_HWLOC_BASE_MAP_NONE;
    ret = pmix_mca_base_var_register("prte", "hwloc", "default", "mem_alloc_policy",
                                     "Default general memory allocations placement policy (this is not memory binding). "
                                     "\"none\" means that no memory policy is applied. \"local_only\" means that a process' "
                                     "memory allocations will be restricted to its local NUMA domain. "
                                     "If using direct launch, this policy will not be in effect until after PMIx_Init. "
                                     "Note that operating system paging policies are unaffected by this setting. For "
                                     "example, if \"local_only\" is used and local NUMA domain memory is exhausted, a new "
                                     "memory allocation may cause paging.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &mem_alloc_policy);
    if (0 > ret) {
        return ret;
    }
    if (NULL != mem_alloc_policy) {
        if (0 == strcasecmp(mem_alloc_policy, "none")) {
            prte_hwloc_base_map = PRTE_HWLOC_BASE_MAP_NONE;
        } else if (0 == strcasecmp(mem_alloc_policy, "local_only")) {
            prte_hwloc_base_map = PRTE_HWLOC_BASE_MAP_LOCAL_ONLY;
        } else {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-hwloc-base.txt", "invalid binding_policy", true,
                           "memory allocation", mem_alloc_policy);
            return PRTE_ERR_SILENT;
        }
    }

    /* hwloc_base_bind_failure_action */
    prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_WARN;
    ret = pmix_mca_base_var_register("prte", "hwloc", "default", "mem_bind_failure_action",
                                     "What PRTE will do if it explicitly tries to bind memory to a specific NUMA "
                                     "location, and fails.  Note that this is a different case than the general "
                                     "allocation policy described by mem_alloc_policy.  A value of \"silent\" "
                                     "means that PRTE will proceed without comment. A value of \"warn\" means that "
                                     "PRTE will warn the first time this happens, but allow the job to continue "
                                     "(possibly with degraded performance).  A value of \"error\" means that PRTE "
                                     "will abort the job if this happens.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &mem_bind_failure_action);
    if (0 > ret) {
        return ret;
    }
    if (NULL != mem_bind_failure_action) {
        if (0 == strcasecmp(mem_bind_failure_action, "silent")) {
            prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_SILENT;
        } else if (0 == strcasecmp(mem_bind_failure_action, "warn")) {
            prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_WARN;
        } else if (0 == strcasecmp(mem_bind_failure_action, "error")) {
            prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_ERROR;
        } else {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-hwloc-base.txt", "invalid binding_policy", true,
                           "memory bind failure action", mem_bind_failure_action);
            return PRTE_ERR_SILENT;
        }
    }

    /* hwloc_default_membind */
    prte_hwloc_base_memattr = PRTE_HWLOC_BASE_MEMATTR_DISABLED;
    ret = pmix_mca_base_var_register("prte", "hwloc", "default", "membind",
                                     "Default memory binding for launched processes, taking the same directives "
                                     "as the --membind option: \"none\" (the default), or one of capacity, "
                                     "locality, bandwidth or latency to bind each process' memory to the NUMA "
                                     "node(s) best by that measure among the nodes local to the process' own "
                                     "cpuset. So \"bandwidth\" binds the process to the local node with the "
                                     "highest bandwidth from the cores it is bound to - which may be a node with "
                                     "no cpus of its own, such as HBM - and \"locality\" to every node local to "
                                     "its cpuset. A job's or app's --membind takes precedence. It only applies "
                                     "to processes bound to cpus - by --bind-to, by the binding --map-by "
                                     "derives, or by the default one; a "
                                     "warning is printed for those left unbound. When applied, it replaces "
                                     "mem_alloc_policy, and a failure to bind is handled according to "
                                     "mem_bind_failure_action. Requires hwloc 2.3 or later.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &default_membind);
    if (0 > ret) {
        return ret;
    }
    if (NULL != default_membind &&
        PRTE_SUCCESS != prte_hwloc_base_parse_membind(PRTE_PROC_MY_NAME->nspace,
                                                      "hwloc_default_membind",
                                                      default_membind, &prte_hwloc_base_memattr)) {
        /* the parser has already said what is wrong with the value */
        return PRTE_ERR_SILENT;
    }

    /* NOTE: for future developers and readers of this code, the binding policies are strictly
     * limited to none, hwthread, core, l1cache, l2cache, l3cache, package, and numa
     *
     * The default binding policy can be modified by any combination of the following:
     *    * overload-allowed - multiple processes can be bound to the same PU (core or HWT)
     *    * if-supported - perform the binding if it is supported by the OS, but do not
     *                     generate an error if it cannot be done
     */
    prte_hwloc_base_binding_policy = NULL;
    ret = pmix_mca_base_var_register("prte", NULL, NULL, "bindto",
                                     "Default policy for binding processes. Allowed values: none, hwthread, core, l1cache, "
                                     "l2cache, "
                                     "l3cache, numa, package, (\"none\" is the default when oversubscribed, \"core\" is "
                                     "the default otherwise). Allowed "
                                     "colon-delimited qualifiers: "
                                     "overload-allowed, if-supported, limit. For more details, see \"prterun --help bind-to\""
                                     "The full directive need not be provided — "
                                      "only enough characters are required to uniquely identify the "
                                      "directive. Directive values are case insensitive",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_hwloc_base_binding_policy);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", NULL, NULL, "bind_to",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "hwloc", "default", "binding_policy",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    /* The MCA layer owns whatever this variable points at and frees it at
     * finalize, so the deprecated shortcuts have to hand it heap memory -
     * a string literal here becomes a free() of read-only storage. */
    if (NULL == prte_hwloc_base_binding_policy) {
        if (bind_to_core) {
            prte_hwloc_base_binding_policy = strdup("core");
        } else if (bind_to_socket) {
            prte_hwloc_base_binding_policy = strdup("package");
        }
    }

    /* Allow specification of a default CPU list - a comma-delimited list of cpu ranges that
     * are the default PUs for this DVM. CPUs are to be specified as LOGICAL indices. If a
     * cpuset is provided, then all process placements and bindings will be constrained to the
     * identified CPUs. IN ESSENCE, THIS IS A USER-DEFINED "SOFT" CGROUP.
     *
     * Example: if the default binding policy is "core", then each process will be bound to the
     * first unused core underneath the topological object upon which it has been mapped. In other
     * words, if two processes are mapped to a given package, then the first process will be bound
     * to core0 of that package, and the second process will be bound to core1.
     *
     * If the cpuset specified that only cores 10, 12, and 14 were to be used, then the first
     * process would be bound to core10 and the second process would be bound to core12.
     *
     * If the default binding policy had been set to "package", and if cores 10, 12, and 14 are all
     * on the same package, then both processes would be bound to cores 10, 12, and 14. Note that
     * they would have been bound to all PUs on the package if the cpuset had not been given.
     *
     * If cores 10 and 12 are on package0, and core14 is on package1, then if the first process is
     * mapped to package0 and we are using a binding policy of "package", the first process would be
     * bound to core10 and core12. If the second process were mapped to package1, then it would be
     * bound only to core14 as that is the only PU in the cpuset that lies in package1.
     */
    default_cpu_list = NULL;
    ret = pmix_mca_base_var_register("prte", "hwloc", "default", "cpu_list",
                                     "Comma-separated list of ranges specifying logical cpus to be used by the DVM. "
                                     "Supported modifier:HWTCPUS (ranges specified in hwthreads) or CORECPUS "
                                     "(default: ranges specified in cores)",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &default_cpu_list);


    if (NULL != default_cpu_list) {
        /* The MCA layer owns this string and reports the parameter's value
         * through it, so parse a copy: splitting the modifier off in place
         * makes prte_info and every later reader see a truncated value the
         * user never set. */
        prte_hwloc_default_cpu_list = strdup(default_cpu_list);
        if (NULL == prte_hwloc_default_cpu_list) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        if (NULL != (ptr = strrchr(prte_hwloc_default_cpu_list, ':'))) {
            *ptr = '\0';
            ++ptr;
            if (0 == strcasecmp(ptr, "HWTCPUS")) {
                prte_hwloc_default_use_hwthread_cpus = true;
            } else if (0 == strcasecmp(ptr, "CORECPUS")) {
                prte_hwloc_default_use_hwthread_cpus = false;
            } else {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-hwloc-base.txt", "bad-processor-type", true,
                               default_cpu_list, ptr);
                free(prte_hwloc_default_cpu_list);
                prte_hwloc_default_cpu_list = NULL;
                return PRTE_ERR_BAD_PARAM;
            }
        }
    }

    prte_hwloc_base_topo_file = NULL;
    ret = pmix_mca_base_var_register("prte", "hwloc", "use", "topo_file",
                                     "Read local topology from file instead of directly sensing it",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_hwloc_base_topo_file);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "ras", "simulator", "topo_files",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(ret, "prte", "hwloc", "base", "use_topo_file",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* register parameters */
    return PRTE_SUCCESS;
}

int prte_hwloc_base_open(void)
{
    int rc;

    if (prte_hwloc_base_inited) {
        return PRTE_SUCCESS;
    }
    prte_hwloc_base_inited = true;

    /* Check the provided default binding policy for correctness - specifically want to ensure
     * there are no disallowed qualifiers and setup the global param.
     *
     * A bad value here is fatal, exactly as a bad "mem_alloc_policy" or
     * "mem_bind_failure_action" is in prte_hwloc_base_register() above. It
     * did not used to be: prte_init() discarded this function's return, so
     * "--prtemca bindto bogus" printed "the specified binding policy is not
     * recognized" and then launched the job anyway with the default binding
     * - having told the user their request was refused. The command-line
     * spelling of the same mistake ("--bind-to bogus") has always been
     * fatal, so the two were answering differently for one typo. */
    rc = prte_hwloc_base_set_binding_policy(NULL, prte_hwloc_base_binding_policy);
    if (PRTE_SUCCESS != rc) {
        /* the parser has already said precisely what is wrong with the
         * value, so keep prte_init() from wrapping that in a generic
         * "internal failure" report on top of it */
        return PRTE_ERR_SILENT;
    }

    return PRTE_SUCCESS;
}

void prte_hwloc_base_close(void)
{
    if (!prte_hwloc_base_inited) {
        return;
    }

    if (NULL != prte_hwloc_default_cpu_list) {
        free(prte_hwloc_default_cpu_list);
        /* several subsystems test this for NULL to decide whether the DVM
         * was given a cpu-set; leaving a dangling pointer here makes any
         * post-finalize reader read freed memory */
        prte_hwloc_default_cpu_list = NULL;
    }

    /* Drop our reference to the local topology WITHOUT destroying it.
     *
     * prte_hwloc_topology is not ours to free by the time we get here. The
     * only two callers of prte_hwloc_base_get_topology() - ess/hnp and the
     * shared daemon bring-up - each immediately wrap it in a prte_topology_t
     * and add that to prte_node_topologies, and THAT object's destructor
     * releases the userdata and destroys the topology. So the array owns
     * every topology it holds, the local one included, and this global is a
     * borrowed alias of one of its entries.
     *
     * Destroying it here as well is a double free, which is exactly why this
     * function ended up with no callers at all: it could not be called
     * either before the array release (the array would then destroy freed
     * memory) or after it (we would). Clearing the alias instead is correct
     * in both orders, so the ordering stops being a trap.
     */
    prte_hwloc_topology = NULL;

    /* All done */
    prte_hwloc_base_inited = false;
}

int prte_hwloc_base_set_default_binding(void *jd, void *opt)
{
    prte_job_t *jdata = (prte_job_t*)jd;
    prte_rmaps_options_t *options = (prte_rmaps_options_t*)opt;
    prte_mapping_policy_t mpol;

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16)) {
        /* bind to cpus */
        if (options->use_hwthreads || prte_rmaps_base.require_hwtcpus) {
            /* if we are using hwthread cpus, then bind to those */
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using byhwthread",
                                __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
        } else {
            /* bind to core */
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using bycore", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
        }
    } else if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
        /* tools are never bound */
        PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NONE);
    } else {
        /* if we are mapping by some object, then we default
         * to binding to that object */
        mpol = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
        if (PRTE_MAPPING_BYHWTHREAD == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using byhwthread", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
        } else if (PRTE_MAPPING_BYCORE == mpol) {
            /* a job that counts hwthreads as its cpus binds each proc to a
             * cpu - its hwthread - not to the whole core: the node was
             * counted as that many slots for this job, so binding to the
             * core would put two procs on the same set of cpus */
            if (options->use_hwthreads) {
                pmix_output_verbose(options->verbosity, options->stream,
                                    "setdefaultbinding[%d] binding not given - using byhwthread (hwtcpus)",
                                    __LINE__);
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
            } else {
                pmix_output_verbose(options->verbosity, options->stream,
                                    "setdefaultbinding[%d] binding not given - using bycore", __LINE__);
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
            }
        } else if (PRTE_MAPPING_BYL1CACHE == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using byL1", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L1CACHE);
        } else if (PRTE_MAPPING_BYL2CACHE == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using byL2", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L2CACHE);
        } else if (PRTE_MAPPING_BYL3CACHE == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using byL3", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L3CACHE);
        } else if (PRTE_MAPPING_BYNUMA == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using bynuma",
                                __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NUMA);
        } else if (PRTE_MAPPING_BYPACKAGE == mpol) {
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using bypackage", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_PACKAGE);
        } else if (PRTE_MAPPING_PELIST == mpol) {
            if (options->use_hwthreads) {
                /* if we are using hwthread cpus, then bind to those */
                pmix_output_verbose(options->verbosity, options->stream,
                                    "setdefaultbinding[%d] binding not given - using byhwthread for pe-list", __LINE__);
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                PRTE_BIND_TO_HWTHREAD);
            } else {
                /* otherwise bind to core */
                pmix_output_verbose(options->verbosity, options->stream,
                                    "setdefaultbinding[%d] binding not given - using bycore for pe-list", __LINE__);
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                PRTE_BIND_TO_CORE);
            }
        } else if (PRTE_MAPPING_PPR == mpol) {
            if (HWLOC_OBJ_MACHINE == options->maptype) {
                if (options->nprocs <= 2) {
                    /* we are mapping by node or some other non-object method */
                    if (options->use_hwthreads || prte_rmaps_base.require_hwtcpus) {
                        /* if we are using hwthread cpus, then bind to those */
                        pmix_output_verbose(options->verbosity, options->stream,
                                            "setdefaultbinding[%d] binding not given - using byhwthread", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                        PRTE_BIND_TO_HWTHREAD);
                    } else {
                        /* otherwise bind to core */
                        pmix_output_verbose(options->verbosity, options->stream,
                                            "setdefaultbinding[%d] binding not given - using bycore", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                        PRTE_BIND_TO_CORE);
                    }
                } else {
                    pmix_output_verbose(options->verbosity, options->stream,
                                        "setdefaultbinding[%d] binding not given - using bynuma",
                                        __LINE__);
                    PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NUMA);
                }
            } else if (HWLOC_OBJ_PACKAGE == options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_PACKAGE);
            } else if (HWLOC_OBJ_NUMANODE== options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NUMA);
            } else if (HWLOC_OBJ_L1CACHE == options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L1CACHE);
            } else if (HWLOC_OBJ_L2CACHE == options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L2CACHE);
            } else if (HWLOC_OBJ_L3CACHE == options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L3CACHE);
            } else if (HWLOC_OBJ_CORE == options->maptype) {
                /* as for mapping by core: hwthread cpus bind to a hwthread */
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                options->use_hwthreads ? PRTE_BIND_TO_HWTHREAD
                                                                       : PRTE_BIND_TO_CORE);
            } else if (HWLOC_OBJ_PU == options->maptype) {
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
            }
        } else {
            if (options->nprocs <= 2) {
                /* we are mapping by node or some other non-object method */
                if (options->use_hwthreads || prte_rmaps_base.require_hwtcpus) {
                    /* if we are using hwthread cpus, then bind to those */
                    pmix_output_verbose(options->verbosity, options->stream,
                                        "setdefaultbinding[%d] binding not given - using byhwthread", __LINE__);
                    PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                    PRTE_BIND_TO_HWTHREAD);
                } else {
                    /* otherwise bind to core */
                    pmix_output_verbose(options->verbosity, options->stream,
                                        "setdefaultbinding[%d] binding not given - using bycore", __LINE__);
                    PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding,
                                                    PRTE_BIND_TO_CORE);
                }
            } else {
                pmix_output_verbose(options->verbosity, options->stream,
                                    "setdefaultbinding[%d] binding not given - using bynuma",
                                    __LINE__);
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NUMA);
            }
        }
    }
    /* they might have set the overload-allowed flag while wanting PRRTE
     * to set the default binding - don't override it */
    if (!PRTE_BIND_OVERLOAD_SET(jdata->map->binding)) {
        if (PRTE_BIND_OVERLOAD_ALLOWED(prte_hwloc_default_binding_policy)) {
            jdata->map->binding |= PRTE_BIND_ALLOW_OVERLOAD;
        }
    }
    return PRTE_SUCCESS;
}

static bool fns_init = false;
static pmix_tsd_key_t print_tsd_key;
char *prte_hwloc_print_null = "NULL";

static void buffer_cleanup(void *value)
{
    int i;
    prte_hwloc_print_buffers_t *ptr;

    if (NULL != value) {
        ptr = (prte_hwloc_print_buffers_t *) value;
        for (i = 0; i < PRTE_HWLOC_PRINT_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
        free(ptr);
    }
}

prte_hwloc_print_buffers_t *prte_hwloc_get_print_buffer(void)
{
    prte_hwloc_print_buffers_t *ptr;
    int ret, i;

    if (!fns_init) {
        /* setup the print_args function */
        if (PRTE_SUCCESS != (ret = pmix_tsd_key_create(&print_tsd_key, buffer_cleanup))) {
            return NULL;
        }
        fns_init = true;
    }

    ret = pmix_tsd_getspecific(print_tsd_key, (void **) &ptr);
    if (PRTE_SUCCESS != ret)
        return NULL;

    if (NULL == ptr) {
        ptr = (prte_hwloc_print_buffers_t *) malloc(sizeof(prte_hwloc_print_buffers_t));
        if (NULL == ptr) {
            return NULL;
        }
        for (i = 0; i < PRTE_HWLOC_PRINT_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((PRTE_HWLOC_PRINT_MAX_SIZE + 1) * sizeof(char));
            if (NULL == ptr->buffers[i]) {
                /* hand back what we got so the cleanup below is well-defined */
                while (0 < i) {
                    free(ptr->buffers[--i]);
                }
                free(ptr);
                return NULL;
            }
        }
        ptr->cntr = 0;
        ret = pmix_tsd_setspecific(print_tsd_key, (void *) ptr);
        if (PRTE_SUCCESS != ret) {
            buffer_cleanup(ptr);
            return NULL;
        }
    }

    return (prte_hwloc_print_buffers_t *) ptr;
}


int prte_hwloc_base_set_binding_policy(void *jdat, char *spec)
{
    int i, rc, tag;
    prte_binding_policy_t tmp;
    char **quals, *myspec, *ptr, *p2, *endp;
    prte_job_t *jdata = (prte_job_t *) jdat;
    uint16_t u16;
    long lval;

    /* set default */
    tmp = 0;

    /* binding specification */
    if (NULL == spec) {
        return PRTE_SUCCESS;
    }

    /* A job we cannot record the answer on has to be refused before we parse
     * anything, not after: the "report" and "limit=N" qualifiers write to
     * jdata->attributes as they are read, so failing this check at the end
     * left them on a job whose binding was never set - the same ordering
     * mistake the policy word above used to make. */
    if (NULL != jdata && NULL == jdata->map) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    myspec = strdup(spec); // protect the input

    /* check for qualifiers */
    ptr = strchr(myspec, ':');
    if (NULL != ptr) {
        *ptr = '\0';
        ++ptr;
    }

    /* Resolve the policy word FIRST, before any qualifier can record itself
     * on the job: a spec whose policy does not parse must leave the job
     * exactly as it found it.
     *
     * An empty policy word - the ":qualifier" form, with no policy at all -
     * means "keep whatever binding policy would otherwise apply, but with
     * these qualifiers". That is what "--map-by :OVERSUBSCRIBE" has always
     * meant on the mapping side, and it has to mean the same here - so it
     * is not looked up at all. Leaving the policy bits at zero means
     * PRTE_BINDING_POLICY_IS_SET() still answers "no" and
     * PRTE_SET_DEFAULT_BINDING_POLICY() later fills the policy in while
     * preserving the qualifier half of the word. */
    if ('\0' != myspec[0]) {
        rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--bind-to", myspec,
                            prte_cli_binders, &tag);
        if (PRTE_ERR_NOT_FOUND == rc) {
            prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-hwloc-base.txt", "invalid binding_policy", true, "binding",
                           spec);
            free(myspec);
            return PRTE_ERR_BAD_PARAM;
        } else if (PRTE_SUCCESS != rc) {
            free(myspec);
            return rc;
        }
        switch (tag) {
            case PRTE_BINDER_NONE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NONE);
                break;
            case PRTE_BINDER_HWT:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);
                break;
            case PRTE_BINDER_CORE:
                /* honor the user's "core" unless the topology has no cores at all;
                 * a core that holds a single hwthread is still a core to bind to */
                if (!prte_rmaps_base.have_cores) {
                    PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);
                } else {
                    PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_CORE);
                }
                break;
            case PRTE_BINDER_L1CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L1CACHE);
                break;
            case PRTE_BINDER_L2CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L2CACHE);
                break;
            case PRTE_BINDER_L3CACHE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L3CACHE);
                break;
            case PRTE_BINDER_NUMA:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NUMA);
                break;
            case PRTE_BINDER_PACKAGE:
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_PACKAGE);
                break;
        }
    }

    if (NULL != ptr) {
        /* A trailing ':' leaves an empty qualifier list, and
         * PMIx_Argv_split() hands back NULL for that rather than an empty
         * array - so "core:" indexed a NULL and took the process down. On
         * the spawn path that process is the DVM's controller. */
        quals = PMIx_Argv_split(ptr, ':');
        for (i = 0; NULL != quals && NULL != quals[i]; i++) {
            rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--bind-to", quals[i],
                                prte_cli_bindquals, &tag);
            if (PRTE_ERR_NOT_FOUND == rc) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-hwloc-base.txt", "unrecognized-modifier", true, spec);
                PMIx_Argv_free(quals);
                free(myspec);
                return PRTE_ERR_BAD_PARAM;
            } else if (PRTE_SUCCESS != rc) {
                PMIx_Argv_free(quals);
                free(myspec);
                return rc;
            }

            if (PRTE_BINDQUAL_IF_SUPP == tag) {
                tmp |= PRTE_BIND_IF_SUPPORTED;

            } else if (PRTE_BINDQUAL_OVERLOAD == tag) {
                tmp |= (PRTE_BIND_ALLOW_OVERLOAD | PRTE_BIND_OVERLOAD_GIVEN);

            } else if (PRTE_BINDQUAL_NOOVERLOAD == tag) {
                tmp = (tmp & ~PRTE_BIND_ALLOW_OVERLOAD);
                tmp |= PRTE_BIND_OVERLOAD_GIVEN;

            } else if (PRTE_BINDQUAL_REPORT == tag) {
                if (NULL == jdata) {
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                                   "binding policy", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS, PRTE_ATTR_GLOBAL, true);

            } else if (PRTE_BINDQUAL_LIMIT == tag) {
                if (NULL == jdata) {
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                                   "binding policy", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                /* Numeric value follows the '=' (LIMIT=2). Do not index past
                 * the qualifier's full spelling - the name may be abbreviated,
                 * so "L=2" is this same option */
                p2 = pmix_cli_qualifier_value(quals[i]);
                if (NULL == p2) {
                    /* missing the value */
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-value", true,
                                   "binding limit", "LIMIT", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                /* the attribute is a uint16, so a value that does not fit has
                 * to be rejected here - truncating it silently would bind to
                 * a limit the user never asked for */
                errno = 0;
                lval = strtol(p2, &endp, 10);
                if (endp == p2 || '\0' != *endp || 0 != errno ||
                    0 >= lval || UINT16_MAX < lval) {
                    /* value is not a number, has trailing garbage, or is out
                     * of range (a limit of zero is meaningless) */
                    prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "invalid-value", true,
                                   "binding limit", "LIMIT", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                u16 = (uint16_t) lval;
                prte_set_attribute(&jdata->attributes, PRTE_JOB_BINDING_LIMIT, PRTE_ATTR_GLOBAL,
                                   &u16, PMIX_UINT16);
            }
        }
        PMIx_Argv_free(quals);
    }
    free(myspec);

    if (NULL == jdata) {
        prte_hwloc_default_binding_policy = tmp;
    } else {
        /* jdata->map was checked on the way in */
        jdata->map->binding = tmp;
    }
    return PRTE_SUCCESS;
}

int prte_hwloc_base_parse_membind(const char *nspace, const char *option,
                                  const char *spec, prte_hwloc_base_memattr_t *memattr)
{
    prte_hwloc_base_memattr_t result;
    int rc, tag;

    if (NULL == option || NULL == memattr) {
        return PRTE_ERR_BAD_PARAM;
    }
    /* a directive is one word: none of them takes a qualifier, so a ':'
     * means the value is wrong, not that the rest of it can be ignored */
    if (NULL == spec || '\0' == spec[0] || NULL != strchr(spec, ':')) {
        goto bad;
    }

    rc = prte_cli_match(nspace, option, spec, prte_cli_membinders, &tag);
    if (PRTE_ERR_NOT_FOUND == rc) {
        goto bad;
    } else if (PRTE_SUCCESS != rc) {
        /* ambiguous, or a value where none belongs - already reported */
        return rc;
    }

    /* translate the directive into what implements it */
    switch (tag) {
    case PRTE_MEMBINDER_NONE:
        result = PRTE_HWLOC_BASE_MEMATTR_DISABLED;
        break;
    case PRTE_MEMBINDER_CAPACITY:
        result = PRTE_HWLOC_BASE_MEMATTR_CAPACITY;
        break;
    case PRTE_MEMBINDER_LOCALITY:
        result = PRTE_HWLOC_BASE_MEMATTR_LOCALITY;
        break;
    case PRTE_MEMBINDER_BANDWIDTH:
        result = PRTE_HWLOC_BASE_MEMATTR_BANDWIDTH;
        break;
    case PRTE_MEMBINDER_LATENCY:
        result = PRTE_HWLOC_BASE_MEMATTR_LATENCY;
        break;
    default:
        /* a word in the table with no meaning here */
        goto bad;
    }

    *memattr = result;
    return PRTE_SUCCESS;

bad:
    prte_show_help(nspace, "help-prte-hwloc-base.txt", "invalid binding_policy", true,
                   "memory binding", (NULL == spec) ? "" : spec);
    return PRTE_ERR_SILENT;
}

const char *prte_hwloc_base_print_membind(prte_hwloc_base_memattr_t memattr)
{
    switch (memattr) {
    case PRTE_HWLOC_BASE_MEMATTR_DISABLED:
        return "NONE";
    case PRTE_HWLOC_BASE_MEMATTR_CAPACITY:
        return "CAPACITY";
    case PRTE_HWLOC_BASE_MEMATTR_LOCALITY:
        return "LOCALITY";
    case PRTE_HWLOC_BASE_MEMATTR_BANDWIDTH:
        return "BANDWIDTH";
    case PRTE_HWLOC_BASE_MEMATTR_LATENCY:
        return "LATENCY";
    default:
        return "UNKNOWN";
    }
}

prte_hwloc_base_memattr_t prte_hwloc_base_membind_resolve(void *jd, void *ap)
{
    prte_job_t *jdata = (prte_job_t *) jd;
    prte_app_context_t *app = (prte_app_context_t *) ap;
    uint16_t u16 = 0, *u16ptr = &u16;

    if (NULL != app &&
        prte_get_attribute(&app->attributes, PRTE_APP_MEMBIND, (void **) &u16ptr, PMIX_UINT16)) {
        return (prte_hwloc_base_memattr_t) u16;
    }
    if (NULL != jdata &&
        prte_get_attribute(&jdata->attributes, PRTE_JOB_MEMBIND, (void **) &u16ptr, PMIX_UINT16)) {
        return (prte_hwloc_base_memattr_t) u16;
    }
    return prte_hwloc_base_memattr;
}

int prte_hwloc_base_membind_count_unbound(void *jd, int app_idx)
{
    prte_job_t *jdata = (prte_job_t *) jd;
    prte_proc_t *proc;
    int n, count = 0;

    if (NULL == jdata || NULL == jdata->procs) {
        return 0;
    }
    for (n = 0; n < jdata->procs->size; n++) {
        proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, n);
        if (NULL == proc || app_idx != (int) proc->app_idx) {
            continue;
        }
        if (NULL == proc->cpuset || '\0' == proc->cpuset[0]) {
            ++count;
        }
    }
    return count;
}

/* hwloc_memattr_*, hwloc_get_local_numanode_objs() and struct hwloc_location
 * only exist since hwloc 2.3 - PRRTE's floor is 2.1 */
#if HWLOC_API_VERSION >= 0x00020300

#define PRTE_HWLOC_MEMATTR_ID_NONE ((hwloc_memattr_id_t)(~0U))

// translate prte_hwloc_base_memattr_t => hwloc_memattr_id_t
static const struct {
    hwloc_memattr_id_t attr;
    const char *str;
} memattr_table[PRTE_HWLOC_BASE_MEMATTR_MAX] = {
    [PRTE_HWLOC_BASE_MEMATTR_DISABLED]  = {.attr = PRTE_HWLOC_MEMATTR_ID_NONE,  .str = "disabled" },
    [PRTE_HWLOC_BASE_MEMATTR_CAPACITY]  = {.attr = HWLOC_MEMATTR_ID_CAPACITY,   .str = "capacity" },
    [PRTE_HWLOC_BASE_MEMATTR_LOCALITY]  = {.attr = HWLOC_MEMATTR_ID_LOCALITY,   .str = "locality" },
    [PRTE_HWLOC_BASE_MEMATTR_BANDWIDTH] = {.attr = HWLOC_MEMATTR_ID_BANDWIDTH,  .str = "bandwidth" },
    [PRTE_HWLOC_BASE_MEMATTR_LATENCY]   = {.attr = HWLOC_MEMATTR_ID_LATENCY,    .str = "latency" }
};

/* Log a nodeset at the given verbosity, if the hwloc output is that verbose.
 * Allocates, so daemon side only - which is the only side that calls it. */
static void log_nodeset(int level, const char *label, hwloc_const_nodeset_t set)
{
    char *list = NULL;

    if (level > pmix_output_get_verbosity(prte_hwloc_base_output)) {
        return;
    }
    if (0 > hwloc_bitmap_list_asprintf(&list, set) || NULL == list) {
        pmix_output_verbose(level, prte_hwloc_base_output, "%s: <unprintable>", label);
    } else {
        pmix_output_verbose(level, prte_hwloc_base_output, "%s: [%s]", label, list);
    }
    free(list);
}

int prte_hwloc_base_get_membind_nodeset(hwloc_topology_t topo,
                                        prte_hwloc_base_memattr_t memattr,
                                        hwloc_const_cpuset_t cpuset,
                                        hwloc_const_nodeset_t allowed,
                                        hwloc_nodeset_t nodeset)
{
    int ret = PRTE_SUCCESS;
    hwloc_memattr_id_t id;
    const char *idstr;
    unsigned long attr_flags = 0;
    struct hwloc_location loc;
    hwloc_bitmap_t my_cpuset = NULL;
    hwloc_obj_t *cands = NULL;
    unsigned ncands = 0, i;
    hwloc_uint64_t value, best_value = 0;
    bool found = false, higher_first;

    /* bozo check */
    if (NULL == topo || NULL == nodeset || 0 > (int) memattr || PRTE_HWLOC_BASE_MEMATTR_MAX <= memattr) {
        return PRTE_ERR_BAD_PARAM;
    }
    id = memattr_table[memattr].attr;
    idstr = memattr_table[memattr].str;

    /* an empty result means "nothing selected" - set it first, so that
     * every return below leaves it empty unless a node was chosen */
    hwloc_bitmap_zero(nodeset);

    if (PRTE_HWLOC_BASE_MEMATTR_DISABLED == memattr) {
        return PRTE_SUCCESS;
    }

    /* an unbound process may run anywhere it is allowed to. hwloc keeps a
     * non-const cpuset in a location, so give it a copy of ours rather
     * than casting the const away */
    my_cpuset = hwloc_bitmap_dup(NULL != cpuset ? cpuset : hwloc_topology_get_allowed_cpuset(topo));
    if (NULL == my_cpuset) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    loc.type = HWLOC_LOCATION_TYPE_CPUSET;
    loc.location.cpuset = my_cpuset;

    pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_INFO, prte_hwloc_base_output,
                        "%s: memattr=%d(%s)", __func__, (int) memattr, idstr);

    /* Step 1: the candidates are the NUMA nodes local to the cpuset. Both
     * directions are needed: LARGER keeps the node(s) whose locality holds
     * a narrow binding (a core sits inside its DDR node, and inside the
     * CPU-less HBM node that shares that locality), SMALLER keeps the nodes
     * inside a wide one (a process bound to a whole package sees each of
     * its nodes). Without this step CAPACITY and LOCALITY, which hwloc
     * defines without an initiator, would be judged across the whole
     * machine, and every process would land on the same node however far
     * from its cores. */
    if (0 != hwloc_get_local_numanode_objs(topo, &loc, &ncands, NULL,
                                           HWLOC_LOCAL_NUMANODE_FLAG_LARGER_LOCALITY
                                           | HWLOC_LOCAL_NUMANODE_FLAG_SMALLER_LOCALITY)) {
        pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_ERROR, prte_hwloc_base_output,
                            "%s: hwloc_get_local_numanode_objs() failed: %s",
                            __func__, strerror(errno));
        ret = PRTE_ERROR;
        goto quit;
    }
    if (0 == ncands) {
        ret = PRTE_ERR_NOT_FOUND;
        goto quit;
    }
    cands = (hwloc_obj_t *) malloc(ncands * sizeof(hwloc_obj_t));
    if (NULL == cands) {
        ret = PRTE_ERR_OUT_OF_RESOURCE;
        goto quit;
    }
    if (0 != hwloc_get_local_numanode_objs(topo, &loc, &ncands, cands,
                                           HWLOC_LOCAL_NUMANODE_FLAG_LARGER_LOCALITY
                                           | HWLOC_LOCAL_NUMANODE_FLAG_SMALLER_LOCALITY)) {
        pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_ERROR, prte_hwloc_base_output,
                            "%s: hwloc_get_local_numanode_objs() failed: %s",
                            __func__, strerror(errno));
        ret = PRTE_ERROR;
        goto quit;
    }

    /* Step 2: drop the candidates the process may not use. Doing this
     * before choosing means a forbidden best node yields the best
     * permitted one, not nothing at all. */
    if (NULL != allowed) {
        unsigned kept = 0;
        for (i = 0; i < ncands; i++) {
            if (hwloc_bitmap_isincluded(cands[i]->nodeset, allowed)) {
                cands[kept++] = cands[i];
            }
        }
        ncands = kept;
    }

    /* Step 3: choose among them */
    if (PRTE_HWLOC_BASE_MEMATTR_LOCALITY == memattr) {
        /* every local node - LOCALITY ranks nodes by how many PUs they
         * serve, which says nothing about this process' own */
        for (i = 0; i < ncands; i++) {
            hwloc_bitmap_or(nodeset, nodeset, cands[i]->nodeset);
        }
    } else {
        /* the best value of the attribute, in the direction hwloc says
         * is better; nodes tied for best are all kept. A node with no
         * value for this initiator (no HMAT data for it, say) cannot
         * compete and is skipped. */
        if (0 != hwloc_memattr_get_flags(topo, id, &attr_flags)) {
            ret = PRTE_ERROR;
            goto quit;
        }
        higher_first = (0 != (attr_flags & HWLOC_MEMATTR_FLAG_HIGHER_FIRST));
        for (i = 0; i < ncands; i++) {
            if (0 != hwloc_memattr_get_value(topo, id, cands[i], &loc, 0, &value)) {
                continue;
            }
            pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_DEBUG, prte_hwloc_base_output,
                                "%s: node %u %s=%llu", __func__, cands[i]->os_index, idstr,
                                (unsigned long long) value);
            if (!found || (higher_first ? value > best_value : value < best_value)) {
                hwloc_bitmap_copy(nodeset, cands[i]->nodeset);
                best_value = value;
                found = true;
            } else if (value == best_value) {
                hwloc_bitmap_or(nodeset, nodeset, cands[i]->nodeset);
            }
        }
    }

    if (hwloc_bitmap_iszero(nodeset)) {
        /* no local, permitted node - or none with a value to compare */
        ret = PRTE_ERR_NOT_FOUND;
        goto quit;
    }
    log_nodeset(PMIX_MCA_BASE_VERBOSE_INFO, "membind nodeset", nodeset);

quit:
    if (PRTE_ERR_NOT_FOUND == ret) {
        pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_WARN, prte_hwloc_base_output,
                            "%s: no usable NUMA node local to the cpuset for %s", __func__, idstr);
    }
    free(cands);
    hwloc_bitmap_free(my_cpuset);
    if (PRTE_SUCCESS != ret) {
        // never hand back a half-computed set
        hwloc_bitmap_zero(nodeset);
    }
    return ret;
}

#else /* HWLOC_API_VERSION < 0x00020300 */

int prte_hwloc_base_get_membind_nodeset(hwloc_topology_t topo,
                                        prte_hwloc_base_memattr_t memattr,
                                        hwloc_const_cpuset_t cpuset __prte_attribute_unused__,
                                        hwloc_const_nodeset_t allowed __prte_attribute_unused__,
                                        hwloc_nodeset_t nodeset)
{
    if (NULL == topo || NULL == nodeset || 0 > (int) memattr || PRTE_HWLOC_BASE_MEMATTR_MAX <= memattr) {
        return PRTE_ERR_BAD_PARAM;
    }
    hwloc_bitmap_zero(nodeset);
    if (PRTE_HWLOC_BASE_MEMATTR_DISABLED == memattr) {
        return PRTE_SUCCESS;
    }
    pmix_output_verbose(PMIX_MCA_BASE_VERBOSE_WARN, prte_hwloc_base_output,
        "%s:%d: hwloc >= 2.3 is required for memattr-based memory binding\n",
        __FUNCTION__, __LINE__);
    return PRTE_ERR_NOT_SUPPORTED;
}

#endif /* HWLOC_API_VERSION */

static void topo_data_const(prte_hwloc_topo_data_t *ptr)
{
    ptr->computed = false;
    ptr->numa_cutoff = UINT_MAX;
}
PMIX_CLASS_INSTANCE(prte_hwloc_topo_data_t,
                    pmix_object_t,
                    topo_data_const, NULL);


static void obj_data_const(prte_hwloc_obj_data_t *ptr)
{
    ptr->nprocs = 0;
}
PMIX_CLASS_INSTANCE(prte_hwloc_obj_data_t,
                    pmix_object_t,
                    obj_data_const, NULL);
