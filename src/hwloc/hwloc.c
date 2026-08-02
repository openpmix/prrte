/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <limits.h>

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
#include "src/util/prte_cmd_line.h"

/*
 * Globals
 */
bool prte_hwloc_base_inited = false;
hwloc_topology_t prte_hwloc_topology = NULL;
prte_hwloc_base_map_t prte_hwloc_base_map = PRTE_HWLOC_BASE_MAP_NONE;
prte_hwloc_base_mbfa_t prte_hwloc_base_mbfa = PRTE_HWLOC_BASE_MBFA_WARN;
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
            pmix_show_help("help-prte-hwloc-base.txt", "invalid binding_policy", true,
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
            pmix_show_help("help-prte-hwloc-base.txt", "invalid binding_policy", true,
                           "memory bind failure action", mem_bind_failure_action);
            return PRTE_ERR_SILENT;
        }
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
                pmix_show_help("help-prte-hwloc-base.txt", "bad-processor-type", true,
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

    /* destroy the topology */
    if (NULL != prte_hwloc_topology) {
        prte_hwloc_base_release_userdata(prte_hwloc_topology);
        hwloc_topology_destroy(prte_hwloc_topology);
        prte_hwloc_topology = NULL;
    }

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
            pmix_output_verbose(options->verbosity, options->stream,
                                "setdefaultbinding[%d] binding not given - using bycore", __LINE__);
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
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
                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
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
    int i;
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
     * meant on the mapping side, and it has to mean the same here. It did
     * not: pmix_check_cli_option() compares only min(strlen(a), strlen(b))
     * characters, so an empty string matches the first option it is tested
     * against - which is "none". "--bind-to :overload-allowed" therefore
     * disabled binding outright, silently, and took the default mapping
     * policy down with it. Leaving the policy bits at zero here means
     * PRTE_BINDING_POLICY_IS_SET() still answers "no" and
     * PRTE_SET_DEFAULT_BINDING_POLICY() later fills the policy in while
     * preserving the qualifier half of the word. */
    if ('\0' != myspec[0]) {
        if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_NONE)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NONE);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_HWT)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_CORE)) {
            /* honor the user's "core" unless the topology has no cores at all;
             * a core that holds a single hwthread is still a core to bind to */
            if (!prte_rmaps_base.have_cores) {
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_HWTHREAD);
            } else {
                PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_CORE);
            }

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_L1CACHE)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L1CACHE);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_L2CACHE)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L2CACHE);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_L3CACHE)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_L3CACHE);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_NUMA)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_NUMA);

        } else if (PMIX_CHECK_CLI_OPTION(myspec, PRTE_CLI_PACKAGE)) {
            PRTE_SET_BINDING_POLICY(tmp, PRTE_BIND_TO_PACKAGE);

        } else {
            pmix_show_help("help-prte-hwloc-base.txt", "invalid binding_policy", true, "binding",
                           spec);
            free(myspec);
            return PRTE_ERR_BAD_PARAM;
        }
    }

    if (NULL != ptr) {
        quals = PMIx_Argv_split(ptr, ':');
        for (i = 0; NULL != quals[i]; i++) {
            if (PMIX_CHECK_CLI_OPTION(quals[i], PRTE_CLI_IF_SUPP)) {
                tmp |= PRTE_BIND_IF_SUPPORTED;

            } else if (PMIX_CHECK_CLI_OPTION(quals[i], PRTE_CLI_OVERLOAD)) {
                tmp |= (PRTE_BIND_ALLOW_OVERLOAD | PRTE_BIND_OVERLOAD_GIVEN);

            } else if (PMIX_CHECK_CLI_OPTION(quals[i], PRTE_CLI_NOOVERLOAD)) {
                tmp = (tmp & ~PRTE_BIND_ALLOW_OVERLOAD);
                tmp |= PRTE_BIND_OVERLOAD_GIVEN;

            } else if (PMIX_CHECK_CLI_OPTION(quals[i], PRTE_CLI_REPORT)) {
                if (NULL == jdata) {
                    pmix_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                                   "binding policy", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                prte_set_attribute(&jdata->attributes, PRTE_JOB_REPORT_BINDINGS, PRTE_ATTR_GLOBAL,
                                   NULL, PMIX_BOOL);

            } else if (PMIX_CHECK_CLI_OPTION(quals[i], PRTE_CLI_LIMIT)) {
                if (NULL == jdata) {
                    pmix_show_help("help-prte-rmaps-base.txt", "unsupported-default-modifier", true,
                                   "binding policy", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                /* Numeric value follows the '=' (LIMIT=2). Do not index past
                 * the qualifier's full spelling - the name may be abbreviated
                 * to any unambiguous prefix, so "L=2" is this same option */
                p2 = pmix_cli_qualifier_value(quals[i]);
                if (NULL == p2) {
                    /* missing the value */
                    pmix_show_help("help-prte-rmaps-base.txt", "invalid-value", true,
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
                    pmix_show_help("help-prte-rmaps-base.txt", "invalid-value", true,
                                   "binding limit", "LIMIT", quals[i]);
                    PMIx_Argv_free(quals);
                    free(myspec);
                    return PRTE_ERR_SILENT;
                }
                u16 = (uint16_t) lval;
                prte_set_attribute(&jdata->attributes, PRTE_JOB_BINDING_LIMIT, PRTE_ATTR_GLOBAL,
                                   &u16, PMIX_UINT16);

            } else {
                /* unknown option */
                pmix_show_help("help-prte-hwloc-base.txt", "unrecognized-modifier", true, spec);
                PMIx_Argv_free(quals);
                free(myspec);
                return PRTE_ERR_BAD_PARAM;
            }
        }
        PMIx_Argv_free(quals);
    }
    free(myspec);

    if (NULL == jdata) {
        prte_hwloc_default_binding_policy = tmp;
    } else {
        if (NULL == jdata->map) {
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return PRTE_ERR_BAD_PARAM;
        }
        jdata->map->binding = tmp;
    }
    return PRTE_SUCCESS;
}

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
