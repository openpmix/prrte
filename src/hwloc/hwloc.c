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

#include "src/hwloc/hwloc-internal.h"


/*
 * Globals
 */
bool prrte_hwloc_base_inited = false;
hwloc_topology_t prrte_hwloc_topology=NULL;
hwloc_cpuset_t prrte_hwloc_my_cpuset=NULL;
hwloc_cpuset_t prrte_hwloc_base_given_cpus=NULL;
prrte_hwloc_base_map_t prrte_hwloc_base_map = PRRTE_HWLOC_BASE_MAP_NONE;
prrte_hwloc_base_mbfa_t prrte_hwloc_base_mbfa = PRRTE_HWLOC_BASE_MBFA_WARN;
prrte_binding_policy_t prrte_hwloc_binding_policy=0;
char *prrte_hwloc_base_cpu_list=NULL;
bool prrte_hwloc_report_bindings=false;
hwloc_obj_type_t prrte_hwloc_levels[] = {
    HWLOC_OBJ_MACHINE,
    HWLOC_OBJ_NODE,
    HWLOC_OBJ_SOCKET,
    HWLOC_OBJ_L3CACHE,
    HWLOC_OBJ_L2CACHE,
    HWLOC_OBJ_L1CACHE,
    HWLOC_OBJ_CORE,
    HWLOC_OBJ_PU
};
bool prrte_hwloc_use_hwthreads_as_cpus = false;
char *prrte_hwloc_base_topo_file = NULL;
int prrte_hwloc_base_output = -1;

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
static bool prrte_hwloc_base_bind_to_core = false;
static bool prrte_hwloc_base_bind_to_socket = false;
static int verbosity = 0;

int prrte_hwloc_base_register(void)
{
    prrte_mca_base_var_enum_t *new_enum;
    int ret, varid;

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
    ret = prrte_mca_base_var_register("prrte", "hwloc", "base", "mem_alloc_policy",
                                "General memory allocations placement policy (this is not memory binding). "
                                "\"none\" means that no memory policy is applied. \"local_only\" means that a process' memory allocations will be restricted to its local NUMA node. "
                                "If using direct launch, this policy will not be in effect until after MPI_INIT. "
                                "Note that operating system paging policies are unaffected by this setting. For example, if \"local_only\" is used and local NUMA node memory is exhausted, a new memory allocation may cause paging.",
                                PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, 0, PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_map);
    PRRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    /* hwloc_base_bind_failure_action */
    prrte_hwloc_base_mbfa = PRRTE_HWLOC_BASE_MBFA_WARN;
    prrte_mca_base_var_enum_create("hwloc memory bind failure action", hwloc_failure_action, &new_enum);
    ret = prrte_mca_base_var_register("prrte", "hwloc", "base", "mem_bind_failure_action",
                                "What PRRTE will do if it explicitly tries to bind memory to a specific NUMA location, and fails.  Note that this is a different case than the general allocation policy described by hwloc_base_alloc_policy.  A value of \"silent\" means that PRRTE will proceed without comment. A value of \"warn\" means that PRRTE will warn the first time this happens, but allow the job to continue (possibly with degraded performance).  A value of \"error\" means that PRRTE will abort the job if this happens.",
                                PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, 0, PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_mbfa);
    PRRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    prrte_hwloc_base_binding_policy = NULL;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "binding_policy",
                                 "Policy for binding processes. Allowed values: none, hwthread, core, l1cache, l2cache, "
                                 "l3cache, socket, numa, board, cpu-list (\"none\" is the default when oversubscribed, \"core\" is "
                                 "the default when np<=2, and \"numa\" is the default when np>2). Allowed qualifiers: "
                                 "overload-allowed, if-supported, ordered",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_binding_policy);

    /* backward compatibility */
    prrte_hwloc_base_bind_to_core = false;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "bind_to_core", "Bind processes to cores",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_bind_to_core);

    prrte_hwloc_base_bind_to_socket = false;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "bind_to_socket", "Bind processes to sockets",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_bind_to_socket);

    prrte_hwloc_report_bindings = false;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "report_bindings", "Report bindings to stderr",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_report_bindings);

    prrte_hwloc_base_cpu_list = NULL;
    varid = prrte_mca_base_var_register("prrte", "hwloc", "base", "cpu_list",
                                  "Comma-separated list of ranges specifying logical cpus to be used by these processes [default: none]",
                                  PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                  PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_base_cpu_list);
    prrte_mca_base_var_register_synonym (varid, "prrte", "hwloc", "base", "slot_list", PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    prrte_mca_base_var_register_synonym (varid, "prrte", "hwloc", "base", "cpu_set", PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* declare hwthreads as independent cpus */
    prrte_hwloc_use_hwthreads_as_cpus = false;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "use_hwthreads_as_cpus",
                                 "Use hardware threads as independent cpus",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_hwloc_use_hwthreads_as_cpus);

    prrte_hwloc_base_topo_file = NULL;
    (void) prrte_mca_base_var_register("prrte", "hwloc", "base", "topo_file",
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

    if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_set_binding_policy(&prrte_hwloc_binding_policy,
                                                                 prrte_hwloc_base_binding_policy))) {
        return rc;
    }

    if (prrte_hwloc_base_bind_to_core) {
        prrte_show_help("help-prrte-hwloc-base.txt", "deprecated", true,
                       "--bind-to-core", "--bind-to core",
                       "hwloc_base_bind_to_core", "hwloc_base_binding_policy=core");
        /* set binding policy to core - error if something else already set */
        if (PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy) &&
            PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) != PRRTE_BIND_TO_CORE) {
            /* error - cannot redefine the default ranking policy */
            prrte_show_help("help-prrte-hwloc-base.txt", "redefining-policy", true,
                           "core", prrte_hwloc_base_print_binding(prrte_hwloc_binding_policy));
            return PRRTE_ERR_BAD_PARAM;
        }
        PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_CORE);
    }

    if (prrte_hwloc_base_bind_to_socket) {
        prrte_show_help("help-prrte-hwloc-base.txt", "deprecated", true,
                       "--bind-to-socket", "--bind-to socket",
                       "hwloc_base_bind_to_socket", "hwloc_base_binding_policy=socket");
        /* set binding policy to socket - error if something else already set */
        if (PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy) &&
            PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) != PRRTE_BIND_TO_SOCKET) {
            /* error - cannot redefine the default ranking policy */
            prrte_show_help("help-prrte-hwloc-base.txt", "redefining-policy", true,
                           "socket", prrte_hwloc_base_print_binding(prrte_hwloc_binding_policy));
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_SOCKET);
    }

    /* did the user provide a slot list? */
    if (NULL != prrte_hwloc_base_cpu_list) {
        /* it is okay if a binding policy was already given - just ensure that
         * we do bind to the given cpus if provided, otherwise this would be
         * ignored if someone didn't also specify a binding policy
         */
        PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_CPUSET);
    }

    /* if we are binding to hwthreads, then we must use hwthreads as cpus */
    if (PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) == PRRTE_BIND_TO_HWTHREAD) {
        prrte_hwloc_use_hwthreads_as_cpus = true;
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
    if (PRRTE_PROC_ON_LOCAL_BOARD(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'B';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_NUMA(locality)) {
        ptr->buffers[ptr->cntr][idx++] = 'N';
        ptr->buffers[ptr->cntr][idx++] = 'u';
        ptr->buffers[ptr->cntr][idx++] = ':';
    }
    if (PRRTE_PROC_ON_LOCAL_SOCKET(locality)) {
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

int prrte_hwloc_base_set_binding_policy(prrte_binding_policy_t *policy, char *spec)
{
    int i;
    prrte_binding_policy_t tmp;
    char **tmpvals, **quals;

    /* set default */
    tmp = 0;

    /* binding specification */
    if (NULL == spec) {
        if (prrte_hwloc_use_hwthreads_as_cpus) {
            /* default to bind-to hwthread */
            PRRTE_SET_DEFAULT_BINDING_POLICY(tmp, PRRTE_BIND_TO_HWTHREAD);
        } else {
            /* default to bind-to core */
            PRRTE_SET_DEFAULT_BINDING_POLICY(tmp, PRRTE_BIND_TO_CORE);
        }
    } else if (0 == strncasecmp(spec, "none", strlen("none"))) {
        PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_NONE);
    } else {
        tmpvals = prrte_argv_split(spec, ':');
        if (1 < prrte_argv_count(tmpvals) || ':' == spec[0]) {
            if (':' == spec[0]) {
                quals = prrte_argv_split(&spec[1], ',');
            } else {
                quals = prrte_argv_split(tmpvals[1], ',');
            }
            for (i=0; NULL != quals[i]; i++) {
                if (0 == strncasecmp(quals[i], "if-supported", strlen(quals[i]))) {
                    tmp |= PRRTE_BIND_IF_SUPPORTED;
                } else if (0 == strncasecmp(quals[i], "overload-allowed", strlen(quals[i])) ||
                           0 == strncasecmp(quals[i], "oversubscribe-allowed", strlen(quals[i]))) {
                    tmp |= PRRTE_BIND_ALLOW_OVERLOAD;
                } else if (0 == strncasecmp(quals[i], "ordered", strlen(quals[i]))) {
                    tmp |= PRRTE_BIND_ORDERED;
                } else {
                    /* unknown option */
                    prrte_output(0, "Unknown qualifier to binding policy: %s", spec);
                    prrte_argv_free(quals);
                    prrte_argv_free(tmpvals);
                    return PRRTE_ERR_BAD_PARAM;
                }
            }
            prrte_argv_free(quals);
        }
        if (NULL == tmpvals[0] || ':' == spec[0]) {
            PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_CORE);
            tmp &= ~PRRTE_BIND_GIVEN;
        } else {
            if (0 == strcasecmp(tmpvals[0], "hwthread")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_HWTHREAD);
            } else if (0 == strcasecmp(tmpvals[0], "core")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_CORE);
            } else if (0 == strcasecmp(tmpvals[0], "l1cache")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L1CACHE);
            } else if (0 == strcasecmp(tmpvals[0], "l2cache")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L2CACHE);
            } else if (0 == strcasecmp(tmpvals[0], "l3cache")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_L3CACHE);
            } else if (0 == strcasecmp(tmpvals[0], "socket")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_SOCKET);
            } else if (0 == strcasecmp(tmpvals[0], "numa")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_NUMA);
            } else if (0 == strcasecmp(tmpvals[0], "board")) {
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_BOARD);
            } else if (0 == strcasecmp(tmpvals[0], "cpu-list") ||
                       0 == strcasecmp(tmpvals[0], "cpulist")) {
                // Accept both "cpu-list" (which matches the
                // "--cpu-list" CLI option) and "cpulist" (because
                // people will be lazy)
                PRRTE_SET_BINDING_POLICY(tmp, PRRTE_BIND_TO_CPUSET);
            } else {
                prrte_show_help("help-prrte-hwloc-base.txt", "invalid binding_policy", true, "binding", spec);
                prrte_argv_free(tmpvals);
                return PRRTE_ERR_BAD_PARAM;
            }
        }
        prrte_argv_free(tmpvals);
    }

    *policy = tmp;
    return PRRTE_SUCCESS;
}
