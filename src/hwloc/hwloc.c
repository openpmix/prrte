/*
 * Copyright (c) 2011-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "src/include/constants.h"
#include "src/dss/dss.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/show_help.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/threads/tsd.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/hwloc/hwloc-internal.h"


/*
 * Globals
 */
bool prrte_hwloc_base_inited = false;
hwloc_topology_t prrte_hwloc_topology=NULL;
hwloc_cpuset_t prrte_hwloc_my_cpuset=NULL;
prrte_hwloc_base_map_t prrte_hwloc_base_map = PRRTE_HWLOC_BASE_MAP_NONE;
prrte_hwloc_base_mbfa_t prrte_hwloc_base_mbfa = PRRTE_HWLOC_BASE_MBFA_WARN;
prrte_binding_policy_t prrte_hwloc_default_binding_policy=0;
char *prrte_hwloc_default_cpu_list=NULL;
char *prrte_hwloc_base_topo_file = NULL;
int prrte_hwloc_base_output = -1;
bool prrte_hwloc_default_use_hwthread_cpus = false;

hwloc_obj_type_t prrte_hwloc_levels[] = {
    HWLOC_OBJ_MACHINE,
    HWLOC_OBJ_NODE,
    HWLOC_OBJ_PACKAGE,
    HWLOC_OBJ_L3CACHE,
    HWLOC_OBJ_L2CACHE,
    HWLOC_OBJ_L1CACHE,
    HWLOC_OBJ_CORE,
    HWLOC_OBJ_PU
};

static prrte_mca_base_var_enum_value_t hwloc_base_map[] = {
    {PRRTE_HWLOC_BASE_MAP_NONE, "none"},
    {PRRTE_HWLOC_BASE_MAP_LOCAL_ONLY, "local_only"},
    {0, NULL}
};

static prrte_mca_base_var_enum_value_t hwloc_failure_action[] = {
    {PRRTE_HWLOC_BASE_MBFA_SILENT, "silent"},
    {PRRTE_HWLOC_BASE_MBFA_WARN, "warn"},
    {PRRTE_HWLOC_BASE_MBFA_ERROR, "error"},
    {0, NULL}
};

static char *prrte_hwloc_base_binding_policy = NULL;
static int verbosity = 0;
static char *default_cpu_list = NULL;

int prrte_hwloc_base_register(void)
{
    prrte_mca_base_var_enum_t *new_enum;
    int ret;
    char *ptr;

    /* debug output */
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "verbose",
                                 "Debug verbosity",
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &verbosity);
    if (0 < verbosity) {
        prrte_hwloc_base_output = prrte_output_open(NULL);
        prrte_output_set_verbosity(prrte_hwloc_base_output, verbosity);
    }

    /* hwloc_base_mbind_policy */

    prrte_hwloc_base_map = PRRTE_HWLOC_BASE_MAP_NONE;
    prrte_mca_base_var_enum_create("hwloc memory allocation policy", hwloc_base_map, &new_enum);
    ret = prrte_mca_base_var_register("prrte", "hwloc", "default", "mem_alloc_policy",
                                "Default general memory allocations placement policy (this is not memory binding). "
                                "\"none\" means that no memory policy is applied. \"local_only\" means that a process' "
                                "memory allocations will be restricted to its local NUMA domain. "
                                "If using direct launch, this policy will not be in effect until after MPI_INIT. "
                                "Note that operating system paging policies are unaffected by this setting. For "
                                "example, if \"local_only\" is used and local NUMA domain memory is exhausted, a new "
                                "memory allocation may cause paging.",
                                PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, 0, PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_map);
    PRRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    /* hwloc_base_bind_failure_action */
    prrte_hwloc_base_mbfa = PRRTE_HWLOC_BASE_MBFA_WARN;
    prrte_mca_base_var_enum_create("hwloc memory bind failure action", hwloc_failure_action, &new_enum);
    ret = prrte_mca_base_var_register("prrte", "hwloc", "default", "mem_bind_failure_action",
                                "What PRRTE will do if it explicitly tries to bind memory to a specific NUMA "
                                "location, and fails.  Note that this is a different case than the general "
                                "allocation policy described by mem_alloc_policy.  A value of \"silent\" "
                                "means that PRRTE will proceed without comment. A value of \"warn\" means that "
                                "PRRTE will warn the first time this happens, but allow the job to continue "
                                "(possibly with degraded performance).  A value of \"error\" means that PRRTE "
                                "will abort the job if this happens.",
                                PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, 0, PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_mbfa);
    PRRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    /* NOTE: for future developers and readers of this code, the binding policies are strictly limited to
     *       none, hwthread, core, l1cache, l2cache, l3cache, package, and numa
     *
     * The default binding policy can be modified by any combination of the following:
     *    * overload-allowed - multiple processes can be bound to the same PU (core or HWT)
     *    * if-supported - perform the binding if it is supported by the OS, but do not
     *                     generate an error if it cannot be done
     */
    prrte_hwloc_base_binding_policy = NULL;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "default", "binding_policy",
                                 "Default policy for binding processes. Allowed values: none, hwthread, core, l1cache, l2cache, "
                                 "l3cache, package, (\"none\" is the default when oversubscribed, \"core\" is "
                                 "the default when np<=2, and \"package\" is the default when np>2). Allowed colon-delimited qualifiers: "
                                 "overload-allowed, if-supported",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_binding_policy);

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
     * If the cpuset specified that only cores 10, 12, and 14 were to be used, then the first process
     * would be bound to core10 and the second process would be bound to core12.
     *
     * If the default binding policy had been set to "package", and if cores 10, 12, and 14 are all
     * on the same package, then both processes would be bound to cores 10, 12, and 14. Note that
     * they would have been bound to all PUs on the package if the cpuset had not been given.
     *
     * If cores 10 and 12 are on package0, and core14 is on package1, then if the first process is mapped
     * to package0 and we are using a binding policy of "package", the first process would be bound to
     * core10 and core12. If the second process were mapped to package1, then it would be bound only
     * to core14 as that is the only PU in the cpuset that lies in package1.
     */
    default_cpu_list = NULL;
    prrte_mca_base_var_register("prrte", "hwloc", "default", "cpu_list",
                                "Comma-separated list of ranges specifying logical cpus to be used by the DVM. "
                                "Supported modifier:HWTCPUS (ranges specified in hwthreads) or CORECPUS "
                                "(default: ranges specified in cores)",
                                PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &default_cpu_list);

    if (NULL != default_cpu_list) {
        if (NULL != (ptr = strrchr(default_cpu_list, ':'))) {
            *ptr = '\0';
            prrte_hwloc_default_cpu_list = strdup(default_cpu_list);
            ++ptr;
            if (0 == strcasecmp(ptr, "HWTCPUS")) {
                prrte_hwloc_default_use_hwthread_cpus = true;
            } else if (0 == strcasecmp(ptr, "CORECPUS")) {
                prrte_hwloc_default_use_hwthread_cpus = false;
            } else {
                prrte_show_help("help-prrte-hwloc-base.txt", "bad-processor-type", true,
                                default_cpu_list, ptr);
                return PRRTE_ERR_BAD_PARAM;
            }
        } else {
            prrte_hwloc_default_cpu_list = strdup(default_cpu_list);
        }
    }

    prrte_hwloc_base_topo_file = NULL;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "use", "topo_file",
                                 "Read local topology from file instead of directly sensing it",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_topo_file);

    /* register parameters */
    return PRRTE_SUCCESS;
}

int prrte_hwloc_base_open(void)
{
    int rc;
    prrte_data_type_t tmp;

    if (prrte_hwloc_base_inited) {
        return PRRTE_SUCCESS;
    }
    prrte_hwloc_base_inited = true;

    /* check the provided default binding policy for correctness - specifically want to ensure
     * there are no disallowed qualifiers and setup the global param */
    if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_set_binding_policy(NULL, prrte_hwloc_base_binding_policy))) {
        return rc;
    }

    /* declare the hwloc data types */
    tmp = PRRTE_HWLOC_TOPO;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_hwloc_pack,
                                                     prrte_hwloc_unpack,
                                                     (prrte_dss_copy_fn_t)prrte_hwloc_copy,
                                                     (prrte_dss_compare_fn_t)prrte_hwloc_compare,
                                                     (prrte_dss_print_fn_t)prrte_hwloc_print,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_HWLOC_TOPO", &tmp))) {
        return rc;
    }

    return PRRTE_SUCCESS;
}

void prrte_hwloc_base_close(void)
{
    if (!prrte_hwloc_base_inited) {
        return;
    }

    /* free memory */
    if (NULL != prrte_hwloc_my_cpuset) {
        hwloc_bitmap_free(prrte_hwloc_my_cpuset);
        prrte_hwloc_my_cpuset = NULL;
    }

    if (NULL != prrte_hwloc_default_cpu_list) {
        free(prrte_hwloc_default_cpu_list);
    }

    /* destroy the topology */
    if (NULL != prrte_hwloc_topology) {
        prrte_hwloc_base_free_topology(prrte_hwloc_topology);
        prrte_hwloc_topology = NULL;
    }

    /* All done */
    prrte_hwloc_base_inited = false;
}

static bool fns_init=false;
static prrte_tsd_key_t print_tsd_key;
char* prrte_hwloc_print_null = "NULL";

static void buffer_cleanup(void *value)
{
    int i;
    prrte_hwloc_print_buffers_t *ptr;

    if (NULL != value) {
        ptr = (prrte_hwloc_print_buffers_t*)value;
        for (i=0; i < PRRTE_HWLOC_PRINT_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
        free(ptr);
    }
}

prrte_hwloc_print_buffers_t *prrte_hwloc_get_print_buffer(void)
{
    prrte_hwloc_print_buffers_t *ptr;
    int ret, i;

    if (!fns_init) {
        /* setup the print_args function */
        if (PRRTE_SUCCESS != (ret = prrte_tsd_key_create(&print_tsd_key, buffer_cleanup))) {
            return NULL;
        }
        fns_init = true;
    }

    ret = prrte_tsd_getspecific(print_tsd_key, (void**)&ptr);
    if (PRRTE_SUCCESS != ret) return NULL;

    if (NULL == ptr) {
        ptr = (prrte_hwloc_print_buffers_t*)malloc(sizeof(prrte_hwloc_print_buffers_t));
        for (i=0; i < PRRTE_HWLOC_PRINT_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((PRRTE_HWLOC_PRINT_MAX_SIZE+1) * sizeof(char));
        }
        ptr->cntr = 0;
        ret = prrte_tsd_setspecific(print_tsd_key, (void*)ptr);
    }

    return (prrte_hwloc_print_buffers_t*) ptr;
}

char* prrte_hwloc_base_print_locality(prrte_hwloc_locality_t locality)
{
    prrte_hwloc_print_buffers_t *ptr;
    int idx;

    ptr = prrte_hwloc_get_print_buffer();
    if (NULL == ptr) {
        return prrte_hwloc_print_null;
    }
    /* cycle around the ring */
    if (PRRTE_HWLOC_PRINT_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    idx = 0;

    if (PRRTE_PROC_ON_LOCAL_CLUSTER(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'C';
        ptr->buffers[ptr->cntr][idx++] = 'L';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_CU(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'C';
        ptr->buffers[ptr->cntr][idx++] = 'U';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_NODE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'N';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_PACKAGE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'S';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_L3CACHE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'L';
        ptr->buffers[ptr->cntr][idx++] = '3';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_L2CACHE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'L';
        ptr->buffers[ptr->cntr][idx++] = '2';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_L1CACHE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'L';
        ptr->buffers[ptr->cntr][idx++] = '1';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_CORE(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'C';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_HWTHREAD(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'H';
        ptr->buffers[ptr->cntr][idx++] = 'w';
        ptr->buffers[ptr->cntr][idx++] = 't';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (0 < idx) {
        ptr->buffers[ptr->cntr][idx-1] = '\0';
    } else if (PRRTE_PROC_NON_LOCAL & locality) {
        ptr->buffers[ptr->cntr][idx++] = 'N';
        ptr->buffers[ptr->cntr][idx++] = 'O';
        ptr->buffers[ptr->cntr][idx++] = 'N';
        ptr->buffers[ptr->cntr][idx++] = '\0';
    } else {
        /* must be an unknown locality */
        ptr->buffers[ptr->cntr][idx++] = 'U';
        ptr->buffers[ptr->cntr][idx++] = 'N';
        ptr->buffers[ptr->cntr][idx++] = 'K';
        ptr->buffers[ptr->cntr][idx++] = '\0';
    }

    return ptr->buffers[ptr->cntr];
}

static void obj_data_const(prrte_hwloc_obj_data_t *ptr)
{
    ptr->npus_calculated = false;
    ptr->npus = 0;
    ptr->idx = UINT_MAX;
    ptr->num_bound = 0;
}
PRRTE_CLASS_INSTANCE(prrte_hwloc_obj_data_t,
                   prrte_object_t,
                   obj_data_const, NULL);

static void sum_const(prrte_hwloc_summary_t *ptr)
{
    ptr->num_objs = 0;
    ptr->rtype = 0;
    PRRTE_CONSTRUCT(&ptr->sorted_by_dist_list, prrte_list_t);
}
static void sum_dest(prrte_hwloc_summary_t *ptr)
{
    prrte_list_item_t *item;
    while (NULL != (item = prrte_list_remove_first(&ptr->sorted_by_dist_list))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&ptr->sorted_by_dist_list);
}
PRRTE_CLASS_INSTANCE(prrte_hwloc_summary_t,
                   prrte_list_item_t,
                   sum_const, sum_dest);
static void topo_data_const(prrte_hwloc_topo_data_t *ptr)
{
    ptr->available = NULL;
    PRRTE_CONSTRUCT(&ptr->summaries, prrte_list_t);
    ptr->userdata = NULL;
}
static void topo_data_dest(prrte_hwloc_topo_data_t *ptr)
{
    prrte_list_item_t *item;

    if (NULL != ptr->available) {
        hwloc_bitmap_free(ptr->available);
    }
    while (NULL != (item = prrte_list_remove_first(&ptr->summaries))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&ptr->summaries);
    ptr->userdata = NULL;
}
PRRTE_CLASS_INSTANCE(prrte_hwloc_topo_data_t,
                   prrte_object_t,
                   topo_data_const,
                   topo_data_dest);

PRRTE_CLASS_INSTANCE(prrte_rmaps_numa_node_t,
        prrte_list_item_t,
        NULL,
        NULL);

int prrte_hwloc_base_set_binding_policy(void *jdat, char *spec)
{
    int i;
    prrte_binding_policy_t tmp;
    char **quals, *myspec, *ptr;
    prrte_job_t *jdata = (prrte_job_t*)jdat;
    size_t len;

    /* set default */
    tmp = 0;

    /* binding specification */
    if (NULL == spec) {
        return PRRTE_SUCCESS;
    }

    myspec = strdup(spec);  // protect the input

    /* check for qualifiers */
    ptr = strchr(myspec, ':');
    if (NULL != ptr) {
        *ptr = '\0';
        ++ptr;
        quals = prrte_argv_split(ptr, ':');
        for (i=0; NULL != quals[i]; i++) {
            if (0 == strcasecmp(quals[i], "if-supported")) {
                tmp |= PRRTE_BIND_IF_SUPPORTED;
            } else if (0 == strcasecmp(quals[i], "overload-allowed")) {
                tmp |= PRRTE_BIND_ALLOW_OVERLOAD;
            } else if (0 == strcasecmp(quals[i], "ordered")) {
                tmp |= PRRTE_BIND_ORDERED;
            } else if (0 == strcasecmp(quals[i], "REPORT")) {
                if (NULL == jdata) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "unsupported-default-modifier", true,
                                    "binding policy", quals[i]);
                    return PRRTE_ERR_SILENT;
                }
                prrte_set_attribute(&jdata->attributes, PRRTE_JOB_REPORT_BINDINGS,
                                    PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);
            } else {
                /* unknown option */
                prrte_show_help("help-prrte-hwloc-base.txt", "unrecognized-modifier", true, spec);
                prrte_argv_free(quals);
                free(myspec);
                return PRRTE_ERR_BAD_PARAM;
            }
        }
        prrte_argv_free(quals);
    }

    len = strlen(myspec);
    if (0 < len) {
        if (0 == strncasecmp(myspec, "none", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_NONE);
        } else if (0 == strncasecmp(myspec, "hwthread", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_HWTHREAD);
        } else if (0 == strncasecmp(myspec, "core", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_CORE);
        } else if (0 == strncasecmp(myspec, "l1cache", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L1CACHE);
        } else if (0 == strncasecmp(myspec, "l2cache", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L2CACHE);
        } else if (0 == strncasecmp(myspec, "l3cache", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L3CACHE);
        } else if (0 == strncasecmp(myspec, "package", len)) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_PACKAGE);
        } else {
            prrte_show_help("help-prrte-hwloc-base.txt", "invalid binding_policy", true, "binding", spec);
            free(myspec);
            return PRRTE_ERR_BAD_PARAM;
        }
    }
    free(myspec);

    if (NULL == jdata) {
        prrte_hwloc_default_binding_policy = tmp;
    } else {
        if (NULL == jdata->map) {
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            return PRRTE_ERR_BAD_PARAM;
        }
        jdata->map->binding = tmp;
    }
    return PRRTE_SUCCESS;
}
