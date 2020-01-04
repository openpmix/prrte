/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */
#include "prrte_config.h"

#include "src/mca/base/prrte_mca_base_var.h"

#include "src/dss/dss_internal.h"

/**
 * globals
 */
bool prrte_dss_initialized = false;
int prrte_dss_verbose = -1;  /* by default disabled */
int prrte_dss_initial_size = -1;
int prrte_dss_threshold_size = -1;
prrte_pointer_array_t prrte_dss_types = {{0}};
prrte_data_type_t prrte_dss_num_reg_types = {0};
static prrte_dss_buffer_type_t default_buf_type = PRRTE_DSS_BUFFER_NON_DESC;

/* variable group id */
static int prrte_dss_group_id = -1;

static prrte_mca_base_var_enum_value_t buffer_type_values[] = {
    {PRRTE_DSS_BUFFER_NON_DESC, "non-described"},
    {PRRTE_DSS_BUFFER_FULLY_DESC, "described"},
    {0, NULL}
};

prrte_dss_t prrte_dss = {
    prrte_dss_pack,
    prrte_dss_unpack,
    prrte_dss_copy,
    prrte_dss_compare,
    prrte_dss_print,
    prrte_dss_structured,
    prrte_dss_peek,
    prrte_dss_unload,
    prrte_dss_load,
    prrte_dss_copy_payload,
    prrte_dss_register,
    prrte_dss_lookup_data_type,
    prrte_dss_dump_data_types,
    prrte_dss_dump
};

/**
 * Object constructors, destructors, and instantiations
 */
/** Value **/
static void prrte_value_construct(prrte_value_t* ptr)
{
    ptr->key = NULL;
    ptr->type = PRRTE_UNDEF;
    memset(&ptr->data, 0, sizeof(ptr->data));
}
static void prrte_value_destruct(prrte_value_t* ptr)
{
    if (NULL != ptr->key) {
        free(ptr->key);
    }
    if (PRRTE_STRING == ptr->type &&
        NULL != ptr->data.string) {
        free(ptr->data.string);
    } else if (PRRTE_BYTE_OBJECT == ptr->type &&
        NULL != ptr->data.bo.bytes) {
        free(ptr->data.bo.bytes);
    } else if (PRRTE_LIST == ptr->type &&
        NULL != ptr->data.ptr) {
        PRRTE_LIST_RELEASE(ptr->data.ptr);
    }
}
PRRTE_CLASS_INSTANCE(prrte_value_t,
                   prrte_list_item_t,
                   prrte_value_construct,
                   prrte_value_destruct);


static void prrte_buffer_construct (prrte_buffer_t* buffer)
{
    /** set the default buffer type */
    buffer->type = default_buf_type;

    /* Make everything NULL to begin with */

    buffer->base_ptr = buffer->pack_ptr = buffer->unpack_ptr = NULL;
    buffer->bytes_allocated = buffer->bytes_used = 0;
}

static void prrte_buffer_destruct (prrte_buffer_t* buffer)
{
    if (NULL != buffer->base_ptr) {
        free (buffer->base_ptr);
    }
}

PRRTE_CLASS_INSTANCE(prrte_buffer_t,
                   prrte_object_t,
                   prrte_buffer_construct,
                   prrte_buffer_destruct);


static void prrte_dss_type_info_construct(prrte_dss_type_info_t *obj)
{
    obj->odti_name = NULL;
    obj->odti_pack_fn = NULL;
    obj->odti_unpack_fn = NULL;
    obj->odti_copy_fn = NULL;
    obj->odti_compare_fn = NULL;
    obj->odti_print_fn = NULL;
    obj->odti_structured = false;
}

static void prrte_dss_type_info_destruct(prrte_dss_type_info_t *obj)
{
    if (NULL != obj->odti_name) {
        free(obj->odti_name);
    }
}

PRRTE_CLASS_INSTANCE(prrte_dss_type_info_t, prrte_object_t,
                   prrte_dss_type_info_construct,
                   prrte_dss_type_info_destruct);


static void prrte_pstat_construct(prrte_pstats_t *obj)
{
    memset(obj->node, 0, sizeof(obj->node));
    memset(obj->cmd, 0, sizeof(obj->cmd));
    obj->rank = 0;
    obj->pid = 0;
    obj->state[0] = 'U';
    obj->state[1] = '\0';
    obj->percent_cpu = 0.0;
    obj->time.tv_sec = 0;
    obj->time.tv_usec = 0;
    obj->priority = -1;
    obj->num_threads = -1;
    obj->pss = 0.0;
    obj->vsize = 0.0;
    obj->rss = 0.0;
    obj->peak_vsize = 0.0;
    obj->processor = -1;
    obj->sample_time.tv_sec = 0;
    obj->sample_time.tv_usec = 0;
}
PRRTE_CLASS_INSTANCE(prrte_pstats_t, prrte_list_item_t,
                   prrte_pstat_construct,
                   NULL);

static void diskstat_cons(prrte_diskstats_t *ptr)
{
    ptr->disk = NULL;
}
static void diskstat_dest(prrte_diskstats_t *ptr)
{
    if (NULL != ptr->disk) {
        free(ptr->disk);
    }
}
PRRTE_CLASS_INSTANCE(prrte_diskstats_t,
                   prrte_list_item_t,
                   diskstat_cons, diskstat_dest);

static void netstat_cons(prrte_netstats_t *ptr)
{
    ptr->net_interface = NULL;
}
static void netstat_dest(prrte_netstats_t *ptr)
{
    if (NULL != ptr->net_interface) {
        free(ptr->net_interface);
    }
}
PRRTE_CLASS_INSTANCE(prrte_netstats_t,
                   prrte_list_item_t,
                   netstat_cons, netstat_dest);

static void prrte_node_stats_construct(prrte_node_stats_t *obj)
{
    obj->la = 0.0;
    obj->la5 = 0.0;
    obj->la15 = 0.0;
    obj->total_mem = 0;
    obj->free_mem = 0.0;
    obj->buffers = 0.0;
    obj->cached = 0.0;
    obj->swap_cached = 0.0;
    obj->swap_total = 0.0;
    obj->swap_free = 0.0;
    obj->mapped = 0.0;
    obj->sample_time.tv_sec = 0;
    obj->sample_time.tv_usec = 0;
    PRRTE_CONSTRUCT(&obj->diskstats, prrte_list_t);
    PRRTE_CONSTRUCT(&obj->netstats, prrte_list_t);
}
static void prrte_node_stats_destruct(prrte_node_stats_t *obj)
{
    prrte_list_item_t *item;
    while (NULL != (item = prrte_list_remove_first(&obj->diskstats))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&obj->diskstats);
    while (NULL != (item = prrte_list_remove_first(&obj->netstats))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&obj->netstats);
}
PRRTE_CLASS_INSTANCE(prrte_node_stats_t, prrte_object_t,
                   prrte_node_stats_construct,
                   prrte_node_stats_destruct);


static void prrte_envar_construct(prrte_envar_t *obj)
{
    obj->envar = NULL;
    obj->value = NULL;
    obj->separator = '\0';
}
static void prrte_envar_destruct(prrte_envar_t *obj)
{
    if (NULL != obj->envar) {
        free(obj->envar);
    }
    if (NULL != obj->value) {
        free(obj->value);
    }
}
PRRTE_CLASS_INSTANCE(prrte_envar_t,
                   prrte_list_item_t,
                   prrte_envar_construct,
                   prrte_envar_destruct);

int prrte_dss_register_vars (void)
{
    prrte_mca_base_var_enum_t *new_enum;
    char *enviro_val;
    int ret;

    enviro_val = getenv("PRRTE_dss_debug");
    if (NULL != enviro_val) {  /* debug requested */
        prrte_dss_verbose = 0;
    }

    prrte_dss_group_id = prrte_mca_base_var_group_register ("prrte", "dss", NULL, NULL);

    /** set the default buffer type. If we are in debug mode, then we default
     * to fully described buffers. Otherwise, we default to non-described for brevity
     * and performance
     */
#if PRRTE_ENABLE_DEBUG
    default_buf_type = PRRTE_DSS_BUFFER_FULLY_DESC;
#else
    default_buf_type = PRRTE_DSS_BUFFER_NON_DESC;
#endif

    ret = prrte_mca_base_var_enum_create ("buffer types", buffer_type_values, &new_enum);
    if (PRRTE_SUCCESS != ret) {
        fprintf (stderr, "Fail A\n");
        return ret;
    }

    ret = prrte_mca_base_var_register ("prrte", "dss", NULL, "buffer_type",
                                 "Set the default mode for PRRTE buffers (0=non-described, 1=described)",
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_8, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &default_buf_type);
    PRRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    /* setup the initial size of the buffer. */
    prrte_dss_initial_size = PRRTE_DSS_DEFAULT_INITIAL_SIZE;
    ret = prrte_mca_base_var_register ("prrte", "dss", NULL, "buffer_initial_size", NULL,
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_8, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prrte_dss_initial_size);
    if (0 > ret) {
        return ret;
    }

    /* the threshold as to where to stop doubling the size of the buffer
     * allocated memory and start doing additive increases */
    prrte_dss_threshold_size = PRRTE_DSS_DEFAULT_THRESHOLD_SIZE;
    ret = prrte_mca_base_var_register ("prrte", "dss", NULL, "buffer_threshold_size", NULL,
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRRTE_INFO_LVL_8, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prrte_dss_threshold_size);

    return (0 > ret) ? ret : PRRTE_SUCCESS;
}

int prrte_dss_open(void)
{
    int rc;
    prrte_data_type_t tmp;

    if (prrte_dss_initialized) {
        return PRRTE_SUCCESS;
    }

    /* Lock DSS MCA variables */
    prrte_mca_base_var_group_set_var_flag (prrte_dss_group_id, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE, false);

    /* Setup the types array */
    PRRTE_CONSTRUCT(&prrte_dss_types, prrte_pointer_array_t);
    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_init(&prrte_dss_types,
                                                      PRRTE_DSS_ID_DYNAMIC,
                                                      PRRTE_DSS_ID_MAX,
                                                      PRRTE_DSS_ID_MAX))) {
        return rc;
    }
    prrte_dss_num_reg_types = 0;

    /* Register all the intrinsic types */

    tmp = PRRTE_NULL;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_null,
                                          prrte_dss_unpack_null,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_null,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_null,
                                          (prrte_dss_print_fn_t)prrte_dss_print_null,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_NULL", &tmp))) {
        return rc;
    }
    tmp = PRRTE_BYTE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_byte,
                                          prrte_dss_unpack_byte,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_byte,
                                          (prrte_dss_print_fn_t)prrte_dss_print_byte,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_BYTE", &tmp))) {
        return rc;
    }
    tmp = PRRTE_BOOL;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_bool,
                                          prrte_dss_unpack_bool,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_bool,
                                          (prrte_dss_print_fn_t)prrte_dss_print_bool,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_BOOL", &tmp))) {
        return rc;
    }
    tmp = PRRTE_INT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int,
                                          prrte_dss_unpack_int,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_int,
                                          (prrte_dss_print_fn_t)prrte_dss_print_int,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_INT", &tmp))) {
        return rc;
    }
    tmp = PRRTE_UINT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int,
                                          prrte_dss_unpack_int,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_uint,
                                          (prrte_dss_print_fn_t)prrte_dss_print_uint,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_UINT", &tmp))) {
        return rc;
    }
    tmp = PRRTE_INT8;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_byte,
                                          prrte_dss_unpack_byte,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_int8,
                                          (prrte_dss_print_fn_t)prrte_dss_print_int8,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_INT8", &tmp))) {
        return rc;
    }
    tmp = PRRTE_UINT8;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_byte,
                                          prrte_dss_unpack_byte,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_uint8,
                                          (prrte_dss_print_fn_t)prrte_dss_print_uint8,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_UINT8", &tmp))) {
        return rc;
    }
    tmp = PRRTE_INT16;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int16,
                                          prrte_dss_unpack_int16,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_int16,
                                          (prrte_dss_print_fn_t)prrte_dss_print_int16,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_INT16", &tmp))) {
        return rc;
    }
    tmp = PRRTE_UINT16;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int16,
                                          prrte_dss_unpack_int16,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_uint16,
                                          (prrte_dss_print_fn_t)prrte_dss_print_uint16,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_UINT16", &tmp))) {
        return rc;
    }
    tmp = PRRTE_INT32;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int32,
                                          prrte_dss_unpack_int32,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_int32,
                                          (prrte_dss_print_fn_t)prrte_dss_print_int32,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_INT32", &tmp))) {
        return rc;
    }
    tmp = PRRTE_UINT32;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int32,
                                          prrte_dss_unpack_int32,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_uint32,
                                          (prrte_dss_print_fn_t)prrte_dss_print_uint32,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_UINT32", &tmp))) {
        return rc;
    }
    tmp = PRRTE_INT64;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int64,
                                          prrte_dss_unpack_int64,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_int64,
                                          (prrte_dss_print_fn_t)prrte_dss_print_int64,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_INT64", &tmp))) {
        return rc;
    }
    tmp = PRRTE_UINT64;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_int64,
                                          prrte_dss_unpack_int64,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_uint64,
                                          (prrte_dss_print_fn_t)prrte_dss_print_uint64,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_UINT64", &tmp))) {
        return rc;
    }
    tmp = PRRTE_SIZE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_sizet,
                                          prrte_dss_unpack_sizet,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_size,
                                          (prrte_dss_print_fn_t)prrte_dss_print_size,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_SIZE", &tmp))) {
        return rc;
    }
    tmp = PRRTE_PID;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_pid,
                                          prrte_dss_unpack_pid,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_pid,
                                          (prrte_dss_print_fn_t)prrte_dss_print_pid,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_PID", &tmp))) {
        return rc;
    }
    tmp = PRRTE_STRING;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_string,
                                          prrte_dss_unpack_string,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_string,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_string,
                                          (prrte_dss_print_fn_t)prrte_dss_print_string,
                                          PRRTE_DSS_STRUCTURED,
                                          "PRRTE_STRING", &tmp))) {
        return rc;
    }
    tmp = PRRTE_DATA_TYPE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_data_type,
                                          prrte_dss_unpack_data_type,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_dt,
                                          (prrte_dss_print_fn_t)prrte_dss_print_data_type,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_DATA_TYPE", &tmp))) {
        return rc;
    }

    tmp = PRRTE_BYTE_OBJECT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_byte_object,
                                          prrte_dss_unpack_byte_object,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_byte_object,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_byte_object,
                                          (prrte_dss_print_fn_t)prrte_dss_print_byte_object,
                                          PRRTE_DSS_STRUCTURED,
                                          "PRRTE_BYTE_OBJECT", &tmp))) {
        return rc;
    }

    tmp = PRRTE_PSTAT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_pstat,
                                                     prrte_dss_unpack_pstat,
                                                     (prrte_dss_copy_fn_t)prrte_dss_copy_pstat,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_pstat,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_pstat,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_PSTAT", &tmp))) {
        return rc;
    }

    tmp = PRRTE_NODE_STAT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_node_stat,
                                                     prrte_dss_unpack_node_stat,
                                                     (prrte_dss_copy_fn_t)prrte_dss_copy_node_stat,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_node_stat,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_node_stat,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_NODE_STAT", &tmp))) {
        return rc;
    }
    tmp = PRRTE_VALUE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_value,
                                                     prrte_dss_unpack_value,
                                                     (prrte_dss_copy_fn_t)prrte_dss_copy_value,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_value,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_value,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_VALUE", &tmp))) {
        return rc;
    }
    tmp = PRRTE_BUFFER;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_buffer_contents,
                                                     prrte_dss_unpack_buffer_contents,
                                                     (prrte_dss_copy_fn_t)prrte_dss_copy_buffer_contents,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_buffer_contents,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_buffer_contents,
                                                     PRRTE_DSS_STRUCTURED,
                                                     "PRRTE_BUFFER", &tmp))) {
        return rc;
    }
    tmp = PRRTE_FLOAT;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_float,
                                                     prrte_dss_unpack_float,
                                                     (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_float,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_float,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_FLOAT", &tmp))) {
        return rc;
    }
    tmp = PRRTE_DOUBLE;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_double,
                                                     prrte_dss_unpack_double,
                                                     (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_double,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_double,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_DOUBLE", &tmp))) {
        return rc;
    }
    tmp = PRRTE_TIMEVAL;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_timeval,
                                                     prrte_dss_unpack_timeval,
                                                     (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_timeval,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_timeval,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_TIMEVAL", &tmp))) {
        return rc;
    }
     tmp = PRRTE_TIME;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_time,
                                                     prrte_dss_unpack_time,
                                                     (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                                     (prrte_dss_compare_fn_t)prrte_dss_compare_time,
                                                     (prrte_dss_print_fn_t)prrte_dss_print_time,
                                                     PRRTE_DSS_UNSTRUCTURED,
                                                     "PRRTE_TIME", &tmp))) {
        return rc;
    }

    tmp = PRRTE_NAME;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_name,
                                          prrte_dss_unpack_name,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_name,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_name,
                                          (prrte_dss_print_fn_t)prrte_dss_print_name,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_NAME", &tmp))) {
        return rc;
    }

    tmp = PRRTE_JOBID;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_jobid,
                                          prrte_dss_unpack_jobid,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_jobid,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_jobid,
                                          (prrte_dss_print_fn_t)prrte_dss_print_jobid,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_JOBID", &tmp))) {
        return rc;
    }

    tmp = PRRTE_VPID;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_vpid,
                                          prrte_dss_unpack_vpid,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_vpid,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_vpid,
                                          (prrte_dss_print_fn_t)prrte_dss_print_vpid,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_VPID", &tmp))) {
        return rc;
    }


    tmp = PRRTE_STATUS;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_status,
                                          prrte_dss_unpack_status,
                                          (prrte_dss_copy_fn_t)prrte_dss_std_copy,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_status,
                                          (prrte_dss_print_fn_t)prrte_dss_print_status,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_STATUS", &tmp))) {
        return rc;
    }

    tmp = PRRTE_ENVAR;
    if (PRRTE_SUCCESS != (rc = prrte_dss.register_type(prrte_dss_pack_envar,
                                          prrte_dss_unpack_envar,
                                          (prrte_dss_copy_fn_t)prrte_dss_copy_envar,
                                          (prrte_dss_compare_fn_t)prrte_dss_compare_envar,
                                          (prrte_dss_print_fn_t)prrte_dss_print_envar,
                                          PRRTE_DSS_UNSTRUCTURED,
                                          "PRRTE_ENVAR", &tmp))) {
        return rc;
    }
    /* All done */

    prrte_dss_initialized = true;
    return PRRTE_SUCCESS;
}


int prrte_dss_close(void)
{
    int32_t i;

    if (!prrte_dss_initialized) {
        return PRRTE_SUCCESS;
    }
    prrte_dss_initialized = false;

    for (i = 0 ; i < prrte_pointer_array_get_size(&prrte_dss_types) ; ++i) {
        prrte_dss_type_info_t *info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, i);
        if (NULL != info) {
            prrte_pointer_array_set_item(&prrte_dss_types, i, NULL);
            PRRTE_RELEASE(info);
        }
    }

    PRRTE_DESTRUCT(&prrte_dss_types);

    return PRRTE_SUCCESS;
}

bool prrte_dss_structured(prrte_data_type_t type)
{
    int i;

    /* find the type */
    for (i = 0 ; i < prrte_dss_types.size ; ++i) {
        prrte_dss_type_info_t *info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, i);
        if (NULL != info && info->odti_type == type) {
            return info->odti_structured;
        }
    }

    /* default to false */
    return false;
}
