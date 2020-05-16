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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */
#include "prte_config.h"

#include "src/mca/base/prte_mca_base_var.h"

#include "src/dss/dss_internal.h"

/**
 * globals
 */
bool prte_dss_initialized = false;
int prte_dss_verbose = -1;  /* by default disabled */
int prte_dss_initial_size = -1;
int prte_dss_threshold_size = -1;
prte_pointer_array_t prte_dss_types = {{0}};
prte_data_type_t prte_dss_num_reg_types = {0};
static prte_dss_buffer_type_t default_buf_type = PRTE_DSS_BUFFER_NON_DESC;

/* variable group id */
static int prte_dss_group_id = -1;

static prte_mca_base_var_enum_value_t buffer_type_values[] = {
    {PRTE_DSS_BUFFER_NON_DESC, "non-described"},
    {PRTE_DSS_BUFFER_FULLY_DESC, "described"},
    {0, NULL}
};

prte_dss_t prte_dss = {
    prte_dss_pack,
    prte_dss_unpack,
    prte_dss_copy,
    prte_dss_compare,
    prte_dss_print,
    prte_dss_structured,
    prte_dss_peek,
    prte_dss_unload,
    prte_dss_load,
    prte_dss_copy_payload,
    prte_dss_register,
    prte_dss_lookup_data_type,
    prte_dss_dump_data_types,
    prte_dss_dump
};

/**
 * Object constructors, destructors, and instantiations
 */
/** Value **/
static void prte_value_construct(prte_value_t* ptr)
{
    ptr->key = NULL;
    ptr->type = PRTE_UNDEF;
    memset(&ptr->data, 0, sizeof(ptr->data));
}
static void prte_value_destruct(prte_value_t* ptr)
{
    if (NULL != ptr->key) {
        free(ptr->key);
    }
    if (PRTE_STRING == ptr->type &&
        NULL != ptr->data.string) {
        free(ptr->data.string);
    } else if (PRTE_BYTE_OBJECT == ptr->type &&
        NULL != ptr->data.bo.bytes) {
        free(ptr->data.bo.bytes);
    } else if (PRTE_LIST == ptr->type &&
        NULL != ptr->data.ptr) {
        PRTE_LIST_RELEASE(ptr->data.ptr);
    }
}
PRTE_CLASS_INSTANCE(prte_value_t,
                   prte_list_item_t,
                   prte_value_construct,
                   prte_value_destruct);


static void prte_buffer_construct (prte_buffer_t* buffer)
{
    /** set the default buffer type */
    buffer->type = default_buf_type;

    /* Make everything NULL to begin with */

    buffer->base_ptr = buffer->pack_ptr = buffer->unpack_ptr = NULL;
    buffer->bytes_allocated = buffer->bytes_used = 0;
}

static void prte_buffer_destruct (prte_buffer_t* buffer)
{
    if (NULL != buffer->base_ptr) {
        free (buffer->base_ptr);
    }
}

PRTE_CLASS_INSTANCE(prte_buffer_t,
                   prte_object_t,
                   prte_buffer_construct,
                   prte_buffer_destruct);


static void prte_dss_type_info_construct(prte_dss_type_info_t *obj)
{
    obj->odti_name = NULL;
    obj->odti_pack_fn = NULL;
    obj->odti_unpack_fn = NULL;
    obj->odti_copy_fn = NULL;
    obj->odti_compare_fn = NULL;
    obj->odti_print_fn = NULL;
    obj->odti_structured = false;
}

static void prte_dss_type_info_destruct(prte_dss_type_info_t *obj)
{
    if (NULL != obj->odti_name) {
        free(obj->odti_name);
    }
}

PRTE_CLASS_INSTANCE(prte_dss_type_info_t, prte_object_t,
                   prte_dss_type_info_construct,
                   prte_dss_type_info_destruct);


static void prte_pstat_construct(prte_pstats_t *obj)
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
PRTE_CLASS_INSTANCE(prte_pstats_t, prte_list_item_t,
                   prte_pstat_construct,
                   NULL);

static void diskstat_cons(prte_diskstats_t *ptr)
{
    ptr->disk = NULL;
}
static void diskstat_dest(prte_diskstats_t *ptr)
{
    if (NULL != ptr->disk) {
        free(ptr->disk);
    }
}
PRTE_CLASS_INSTANCE(prte_diskstats_t,
                   prte_list_item_t,
                   diskstat_cons, diskstat_dest);

static void netstat_cons(prte_netstats_t *ptr)
{
    ptr->net_interface = NULL;
}
static void netstat_dest(prte_netstats_t *ptr)
{
    if (NULL != ptr->net_interface) {
        free(ptr->net_interface);
    }
}
PRTE_CLASS_INSTANCE(prte_netstats_t,
                   prte_list_item_t,
                   netstat_cons, netstat_dest);

static void prte_node_stats_construct(prte_node_stats_t *obj)
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
    PRTE_CONSTRUCT(&obj->diskstats, prte_list_t);
    PRTE_CONSTRUCT(&obj->netstats, prte_list_t);
}
static void prte_node_stats_destruct(prte_node_stats_t *obj)
{
    prte_list_item_t *item;
    while (NULL != (item = prte_list_remove_first(&obj->diskstats))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&obj->diskstats);
    while (NULL != (item = prte_list_remove_first(&obj->netstats))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&obj->netstats);
}
PRTE_CLASS_INSTANCE(prte_node_stats_t, prte_object_t,
                   prte_node_stats_construct,
                   prte_node_stats_destruct);


static void prte_envar_construct(prte_envar_t *obj)
{
    obj->envar = NULL;
    obj->value = NULL;
    obj->separator = '\0';
}
static void prte_envar_destruct(prte_envar_t *obj)
{
    if (NULL != obj->envar) {
        free(obj->envar);
    }
    if (NULL != obj->value) {
        free(obj->value);
    }
}
PRTE_CLASS_INSTANCE(prte_envar_t,
                   prte_list_item_t,
                   prte_envar_construct,
                   prte_envar_destruct);

int prte_dss_register_vars (void)
{
    prte_mca_base_var_enum_t *new_enum;
    char *enviro_val;
    int ret;

    enviro_val = getenv("PRTE_dss_debug");
    if (NULL != enviro_val) {  /* debug requested */
        prte_dss_verbose = 0;
    }

    prte_dss_group_id = prte_mca_base_var_group_register ("prte", "dss", NULL, NULL);

    /** set the default buffer type. If we are in debug mode, then we default
     * to fully described buffers. Otherwise, we default to non-described for brevity
     * and performance
     */
#if PRTE_ENABLE_DEBUG
    default_buf_type = PRTE_DSS_BUFFER_FULLY_DESC;
#else
    default_buf_type = PRTE_DSS_BUFFER_NON_DESC;
#endif

    ret = prte_mca_base_var_enum_create ("buffer types", buffer_type_values, &new_enum);
    if (PRTE_SUCCESS != ret) {
        fprintf (stderr, "Fail A\n");
        return ret;
    }

    ret = prte_mca_base_var_register ("prte", "dss", NULL, "buffer_type",
                                 "Set the default mode for PRTE buffers (0=non-described, 1=described)",
                                 PRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRTE_INFO_LVL_8, PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &default_buf_type);
    PRTE_RELEASE(new_enum);
    if (0 > ret) {
        return ret;
    }

    /* setup the initial size of the buffer. */
    prte_dss_initial_size = PRTE_DSS_DEFAULT_INITIAL_SIZE;
    ret = prte_mca_base_var_register ("prte", "dss", NULL, "buffer_initial_size", NULL,
                                 PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRTE_INFO_LVL_8, PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prte_dss_initial_size);
    if (0 > ret) {
        return ret;
    }

    /* the threshold as to where to stop doubling the size of the buffer
     * allocated memory and start doing additive increases */
    prte_dss_threshold_size = PRTE_DSS_DEFAULT_THRESHOLD_SIZE;
    ret = prte_mca_base_var_register ("prte", "dss", NULL, "buffer_threshold_size", NULL,
                                 PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                 PRTE_INFO_LVL_8, PRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                 &prte_dss_threshold_size);

    return (0 > ret) ? ret : PRTE_SUCCESS;
}

int prte_dss_open(void)
{
    int rc;
    prte_data_type_t tmp;

    if (prte_dss_initialized) {
        return PRTE_SUCCESS;
    }

    /* Lock DSS MCA variables */
    prte_mca_base_var_group_set_var_flag (prte_dss_group_id, PRTE_MCA_BASE_VAR_FLAG_SETTABLE, false);

    /* Setup the types array */
    PRTE_CONSTRUCT(&prte_dss_types, prte_pointer_array_t);
    if (PRTE_SUCCESS != (rc = prte_pointer_array_init(&prte_dss_types,
                                                      PRTE_DSS_ID_DYNAMIC,
                                                      PRTE_DSS_ID_MAX,
                                                      PRTE_DSS_ID_MAX))) {
        return rc;
    }
    prte_dss_num_reg_types = 0;

    /* Register all the intrinsic types */

    tmp = PRTE_NULL;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_null,
                                          prte_dss_unpack_null,
                                          (prte_dss_copy_fn_t)prte_dss_copy_null,
                                          (prte_dss_compare_fn_t)prte_dss_compare_null,
                                          (prte_dss_print_fn_t)prte_dss_print_null,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_NULL", &tmp))) {
        return rc;
    }
    tmp = PRTE_BYTE;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_byte,
                                          prte_dss_unpack_byte,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_byte,
                                          (prte_dss_print_fn_t)prte_dss_print_byte,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_BYTE", &tmp))) {
        return rc;
    }
    tmp = PRTE_BOOL;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_bool,
                                          prte_dss_unpack_bool,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_bool,
                                          (prte_dss_print_fn_t)prte_dss_print_bool,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_BOOL", &tmp))) {
        return rc;
    }
    tmp = PRTE_INT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int,
                                          prte_dss_unpack_int,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_int,
                                          (prte_dss_print_fn_t)prte_dss_print_int,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_INT", &tmp))) {
        return rc;
    }
    tmp = PRTE_UINT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int,
                                          prte_dss_unpack_int,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_uint,
                                          (prte_dss_print_fn_t)prte_dss_print_uint,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_UINT", &tmp))) {
        return rc;
    }
    tmp = PRTE_INT8;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_byte,
                                          prte_dss_unpack_byte,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_int8,
                                          (prte_dss_print_fn_t)prte_dss_print_int8,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_INT8", &tmp))) {
        return rc;
    }
    tmp = PRTE_UINT8;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_byte,
                                          prte_dss_unpack_byte,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_uint8,
                                          (prte_dss_print_fn_t)prte_dss_print_uint8,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_UINT8", &tmp))) {
        return rc;
    }
    tmp = PRTE_INT16;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int16,
                                          prte_dss_unpack_int16,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_int16,
                                          (prte_dss_print_fn_t)prte_dss_print_int16,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_INT16", &tmp))) {
        return rc;
    }
    tmp = PRTE_UINT16;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int16,
                                          prte_dss_unpack_int16,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_uint16,
                                          (prte_dss_print_fn_t)prte_dss_print_uint16,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_UINT16", &tmp))) {
        return rc;
    }
    tmp = PRTE_INT32;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int32,
                                          prte_dss_unpack_int32,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_int32,
                                          (prte_dss_print_fn_t)prte_dss_print_int32,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_INT32", &tmp))) {
        return rc;
    }
    tmp = PRTE_UINT32;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int32,
                                          prte_dss_unpack_int32,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_uint32,
                                          (prte_dss_print_fn_t)prte_dss_print_uint32,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_UINT32", &tmp))) {
        return rc;
    }
    tmp = PRTE_INT64;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int64,
                                          prte_dss_unpack_int64,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_int64,
                                          (prte_dss_print_fn_t)prte_dss_print_int64,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_INT64", &tmp))) {
        return rc;
    }
    tmp = PRTE_UINT64;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_int64,
                                          prte_dss_unpack_int64,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_uint64,
                                          (prte_dss_print_fn_t)prte_dss_print_uint64,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_UINT64", &tmp))) {
        return rc;
    }
    tmp = PRTE_SIZE;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_sizet,
                                          prte_dss_unpack_sizet,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_size,
                                          (prte_dss_print_fn_t)prte_dss_print_size,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_SIZE", &tmp))) {
        return rc;
    }
    tmp = PRTE_PID;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_pid,
                                          prte_dss_unpack_pid,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_pid,
                                          (prte_dss_print_fn_t)prte_dss_print_pid,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_PID", &tmp))) {
        return rc;
    }
    tmp = PRTE_STRING;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_string,
                                          prte_dss_unpack_string,
                                          (prte_dss_copy_fn_t)prte_dss_copy_string,
                                          (prte_dss_compare_fn_t)prte_dss_compare_string,
                                          (prte_dss_print_fn_t)prte_dss_print_string,
                                          PRTE_DSS_STRUCTURED,
                                          "PRTE_STRING", &tmp))) {
        return rc;
    }
    tmp = PRTE_DATA_TYPE;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_data_type,
                                          prte_dss_unpack_data_type,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_dt,
                                          (prte_dss_print_fn_t)prte_dss_print_data_type,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_DATA_TYPE", &tmp))) {
        return rc;
    }

    tmp = PRTE_BYTE_OBJECT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_byte_object,
                                          prte_dss_unpack_byte_object,
                                          (prte_dss_copy_fn_t)prte_dss_copy_byte_object,
                                          (prte_dss_compare_fn_t)prte_dss_compare_byte_object,
                                          (prte_dss_print_fn_t)prte_dss_print_byte_object,
                                          PRTE_DSS_STRUCTURED,
                                          "PRTE_BYTE_OBJECT", &tmp))) {
        return rc;
    }

    tmp = PRTE_PSTAT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_pstat,
                                                     prte_dss_unpack_pstat,
                                                     (prte_dss_copy_fn_t)prte_dss_copy_pstat,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_pstat,
                                                     (prte_dss_print_fn_t)prte_dss_print_pstat,
                                                     PRTE_DSS_STRUCTURED,
                                                     "PRTE_PSTAT", &tmp))) {
        return rc;
    }

    tmp = PRTE_NODE_STAT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_node_stat,
                                                     prte_dss_unpack_node_stat,
                                                     (prte_dss_copy_fn_t)prte_dss_copy_node_stat,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_node_stat,
                                                     (prte_dss_print_fn_t)prte_dss_print_node_stat,
                                                     PRTE_DSS_STRUCTURED,
                                                     "PRTE_NODE_STAT", &tmp))) {
        return rc;
    }
    tmp = PRTE_VALUE;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_value,
                                                     prte_dss_unpack_value,
                                                     (prte_dss_copy_fn_t)prte_dss_copy_value,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_value,
                                                     (prte_dss_print_fn_t)prte_dss_print_value,
                                                     PRTE_DSS_STRUCTURED,
                                                     "PRTE_VALUE", &tmp))) {
        return rc;
    }
    tmp = PRTE_BUFFER;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_buffer_contents,
                                                     prte_dss_unpack_buffer_contents,
                                                     (prte_dss_copy_fn_t)prte_dss_copy_buffer_contents,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_buffer_contents,
                                                     (prte_dss_print_fn_t)prte_dss_print_buffer_contents,
                                                     PRTE_DSS_STRUCTURED,
                                                     "PRTE_BUFFER", &tmp))) {
        return rc;
    }
    tmp = PRTE_FLOAT;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_float,
                                                     prte_dss_unpack_float,
                                                     (prte_dss_copy_fn_t)prte_dss_std_copy,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_float,
                                                     (prte_dss_print_fn_t)prte_dss_print_float,
                                                     PRTE_DSS_UNSTRUCTURED,
                                                     "PRTE_FLOAT", &tmp))) {
        return rc;
    }
    tmp = PRTE_DOUBLE;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_double,
                                                     prte_dss_unpack_double,
                                                     (prte_dss_copy_fn_t)prte_dss_std_copy,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_double,
                                                     (prte_dss_print_fn_t)prte_dss_print_double,
                                                     PRTE_DSS_UNSTRUCTURED,
                                                     "PRTE_DOUBLE", &tmp))) {
        return rc;
    }
    tmp = PRTE_TIMEVAL;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_timeval,
                                                     prte_dss_unpack_timeval,
                                                     (prte_dss_copy_fn_t)prte_dss_std_copy,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_timeval,
                                                     (prte_dss_print_fn_t)prte_dss_print_timeval,
                                                     PRTE_DSS_UNSTRUCTURED,
                                                     "PRTE_TIMEVAL", &tmp))) {
        return rc;
    }
     tmp = PRTE_TIME;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_time,
                                                     prte_dss_unpack_time,
                                                     (prte_dss_copy_fn_t)prte_dss_std_copy,
                                                     (prte_dss_compare_fn_t)prte_dss_compare_time,
                                                     (prte_dss_print_fn_t)prte_dss_print_time,
                                                     PRTE_DSS_UNSTRUCTURED,
                                                     "PRTE_TIME", &tmp))) {
        return rc;
    }

    tmp = PRTE_NAME;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_name,
                                          prte_dss_unpack_name,
                                          (prte_dss_copy_fn_t)prte_dss_copy_name,
                                          (prte_dss_compare_fn_t)prte_dss_compare_name,
                                          (prte_dss_print_fn_t)prte_dss_print_name,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_NAME", &tmp))) {
        return rc;
    }

    tmp = PRTE_JOBID;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_jobid,
                                          prte_dss_unpack_jobid,
                                          (prte_dss_copy_fn_t)prte_dss_copy_jobid,
                                          (prte_dss_compare_fn_t)prte_dss_compare_jobid,
                                          (prte_dss_print_fn_t)prte_dss_print_jobid,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_JOBID", &tmp))) {
        return rc;
    }

    tmp = PRTE_VPID;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_vpid,
                                          prte_dss_unpack_vpid,
                                          (prte_dss_copy_fn_t)prte_dss_copy_vpid,
                                          (prte_dss_compare_fn_t)prte_dss_compare_vpid,
                                          (prte_dss_print_fn_t)prte_dss_print_vpid,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_VPID", &tmp))) {
        return rc;
    }


    tmp = PRTE_STATUS;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_status,
                                          prte_dss_unpack_status,
                                          (prte_dss_copy_fn_t)prte_dss_std_copy,
                                          (prte_dss_compare_fn_t)prte_dss_compare_status,
                                          (prte_dss_print_fn_t)prte_dss_print_status,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_STATUS", &tmp))) {
        return rc;
    }

    tmp = PRTE_ENVAR;
    if (PRTE_SUCCESS != (rc = prte_dss.register_type(prte_dss_pack_envar,
                                          prte_dss_unpack_envar,
                                          (prte_dss_copy_fn_t)prte_dss_copy_envar,
                                          (prte_dss_compare_fn_t)prte_dss_compare_envar,
                                          (prte_dss_print_fn_t)prte_dss_print_envar,
                                          PRTE_DSS_UNSTRUCTURED,
                                          "PRTE_ENVAR", &tmp))) {
        return rc;
    }
    /* All done */

    prte_dss_initialized = true;
    return PRTE_SUCCESS;
}


int prte_dss_close(void)
{
    int32_t i;

    if (!prte_dss_initialized) {
        return PRTE_SUCCESS;
    }
    prte_dss_initialized = false;

    for (i = 0 ; i < prte_pointer_array_get_size(&prte_dss_types) ; ++i) {
        prte_dss_type_info_t *info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, i);
        if (NULL != info) {
            prte_pointer_array_set_item(&prte_dss_types, i, NULL);
            PRTE_RELEASE(info);
        }
    }

    PRTE_DESTRUCT(&prte_dss_types);

    return PRTE_SUCCESS;
}

bool prte_dss_structured(prte_data_type_t type)
{
    int i;

    /* find the type */
    for (i = 0 ; i < prte_dss_types.size ; ++i) {
        prte_dss_type_info_t *info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, i);
        if (NULL != info && info->odti_type == type) {
            return info->odti_structured;
        }
    }

    /* default to false */
    return false;
}
