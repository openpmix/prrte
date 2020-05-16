/* -*- C -*-
 *
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PRTE_DSS_INTERNAL_H_
#define PRTE_DSS_INTERNAL_H_

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/prte_pointer_array.h"

#include "src/dss/dss.h"
#include "src/util/output.h"

#if !defined(STDC_HEADERS) && HAVE_MEMORY_H
#    include <memory.h>
#endif
#include <string.h>

BEGIN_C_DECLS

/*
 * The default starting chunk size
 */
#define PRTE_DSS_DEFAULT_INITIAL_SIZE  2048
/*
 * The default threshold size when we switch from doubling the
 * buffer size to addatively increasing it
 */
#define PRTE_DSS_DEFAULT_THRESHOLD_SIZE 4096

/*
 * Internal type corresponding to size_t.  Do not use this in
 * interface calls - use PRTE_SIZE instead.
 */
#if SIZEOF_SIZE_T == 1
#define DSS_TYPE_SIZE_T PRTE_UINT8
#elif SIZEOF_SIZE_T == 2
#define DSS_TYPE_SIZE_T PRTE_UINT16
#elif SIZEOF_SIZE_T == 4
#define DSS_TYPE_SIZE_T PRTE_UINT32
#elif SIZEOF_SIZE_T == 8
#define DSS_TYPE_SIZE_T PRTE_UINT64
#else
#error Unsupported size_t size!
#endif

/*
 * Internal type corresponding to bool.  Do not use this in interface
 * calls - use PRTE_BOOL instead.
 */
#if SIZEOF__BOOL == 1
#define DSS_TYPE_BOOL PRTE_UINT8
#elif SIZEOF__BOOL == 2
#define DSS_TYPE_BOOL PRTE_UINT16
#elif SIZEOF__BOOL == 4
#define DSS_TYPE_BOOL PRTE_UINT32
#elif SIZEOF__BOOL == 8
#define DSS_TYPE_BOOL PRTE_UINT64
#else
#error Unsupported bool size!
#endif

/*
 * Internal type corresponding to int and unsigned int.  Do not use
 * this in interface calls - use PRTE_INT / PRTE_UINT instead.
 */
#if SIZEOF_INT == 1
#define DSS_TYPE_INT PRTE_INT8
#define DSS_TYPE_UINT PRTE_UINT8
#elif SIZEOF_INT == 2
#define DSS_TYPE_INT PRTE_INT16
#define DSS_TYPE_UINT PRTE_UINT16
#elif SIZEOF_INT == 4
#define DSS_TYPE_INT PRTE_INT32
#define DSS_TYPE_UINT PRTE_UINT32
#elif SIZEOF_INT == 8
#define DSS_TYPE_INT PRTE_INT64
#define DSS_TYPE_UINT PRTE_UINT64
#else
#error Unsupported int size!
#endif

/*
 * Internal type corresponding to pid_t.  Do not use this in interface
 * calls - use PRTE_PID instead.
 */
#if SIZEOF_PID_T == 1
#define DSS_TYPE_PID_T PRTE_UINT8
#elif SIZEOF_PID_T == 2
#define DSS_TYPE_PID_T PRTE_UINT16
#elif SIZEOF_PID_T == 4
#define DSS_TYPE_PID_T PRTE_UINT32
#elif SIZEOF_PID_T == 8
#define DSS_TYPE_PID_T PRTE_UINT64
#else
#error Unsupported pid_t size!
#endif

/* Unpack generic size macros */
#define UNPACK_SIZE_MISMATCH(unpack_type, remote_type, ret)                 \
    do {                                                                    \
        switch(remote_type) {                                               \
        case PRTE_UINT8:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint8_t, remote_type);  \
            break;                                                          \
        case PRTE_INT8:                                                     \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int8_t, remote_type);   \
            break;                                                          \
        case PRTE_UINT16:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint16_t, remote_type); \
            break;                                                          \
        case PRTE_INT16:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int16_t, remote_type);  \
            break;                                                          \
        case PRTE_UINT32:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint32_t, remote_type); \
            break;                                                          \
        case PRTE_INT32:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int32_t, remote_type);  \
            break;                                                          \
        case PRTE_UINT64:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint64_t, remote_type); \
            break;                                                          \
        case PRTE_INT64:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int64_t, remote_type);  \
            break;                                                          \
        default:                                                            \
            ret = PRTE_ERR_NOT_FOUND;                                       \
        }                                                                   \
    } while (0)

/* NOTE: do not need to deal with endianness here, as the unpacking of
   the underling sender-side type will do that for us.  Repeat: the
   data in tmpbuf[] is already in host byte order. */
#define UNPACK_SIZE_MISMATCH_FOUND(unpack_type, tmptype, tmpdsstype)        \
    do {                                                                    \
        int32_t i;                                                          \
        tmptype *tmpbuf = (tmptype*)malloc(sizeof(tmptype) * (*num_vals));  \
        ret = prte_dss_unpack_buffer(buffer, tmpbuf, num_vals, tmpdsstype); \
        for (i = 0 ; i < *num_vals ; ++i) {                                 \
            ((unpack_type*) dest)[i] = (unpack_type)(tmpbuf[i]);            \
        }                                                                   \
        free(tmpbuf);                                                       \
    } while (0)


/**
 * Internal struct used for holding registered dss functions
 */
struct prte_dss_type_info_t {
    prte_object_t super;
    /* type identifier */
    prte_data_type_t odti_type;
    /** Debugging string name */
    char *odti_name;
    /** Pack function */
    prte_dss_pack_fn_t odti_pack_fn;
    /** Unpack function */
    prte_dss_unpack_fn_t odti_unpack_fn;
    /** copy function */
    prte_dss_copy_fn_t odti_copy_fn;
    /** compare function */
    prte_dss_compare_fn_t odti_compare_fn;
    /** print function */
    prte_dss_print_fn_t odti_print_fn;
    /** flag to indicate structured data */
    bool odti_structured;
};
/**
 * Convenience typedef
 */
typedef struct prte_dss_type_info_t prte_dss_type_info_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_dss_type_info_t);

/*
 * globals needed within dss
 */
extern bool prte_dss_initialized;
extern bool prte_dss_debug;
extern int prte_dss_verbose;
extern int prte_dss_initial_size;
extern int prte_dss_threshold_size;
extern prte_pointer_array_t prte_dss_types;
extern prte_data_type_t prte_dss_num_reg_types;

/*
 * Implementations of API functions
 */

int prte_dss_pack(prte_buffer_t *buffer, const void *src,
                  int32_t num_vals,
                  prte_data_type_t type);
int prte_dss_unpack(prte_buffer_t *buffer, void *dest,
                    int32_t *max_num_vals,
                    prte_data_type_t type);

int prte_dss_copy(void **dest, void *src, prte_data_type_t type);

int prte_dss_compare(const void *value1, const void *value2,
                     prte_data_type_t type);

int prte_dss_print(char **output, char *prefix, void *src, prte_data_type_t type);

int prte_dss_dump(int output_stream, void *src, prte_data_type_t type);

int prte_dss_peek(prte_buffer_t *buffer, prte_data_type_t *type,
                  int32_t *number);

int prte_dss_peek_type(prte_buffer_t *buffer, prte_data_type_t *type);

int prte_dss_unload(prte_buffer_t *buffer, void **payload,
                    int32_t *bytes_used);
int prte_dss_load(prte_buffer_t *buffer, void *payload, int32_t bytes_used);

int prte_dss_copy_payload(prte_buffer_t *dest, prte_buffer_t *src);

int prte_dss_register(prte_dss_pack_fn_t pack_fn,
                      prte_dss_unpack_fn_t unpack_fn,
                      prte_dss_copy_fn_t copy_fn,
                      prte_dss_compare_fn_t compare_fn,
                      prte_dss_print_fn_t print_fn,
                      bool structured,
                      const char *name, prte_data_type_t *type);

bool prte_dss_structured(prte_data_type_t type);

char *prte_dss_lookup_data_type(prte_data_type_t type);

void prte_dss_dump_data_types(int output);

/*
 * Specialized functions
 */
PRTE_EXPORT    int prte_dss_pack_buffer(prte_buffer_t *buffer, const void *src,
                                          int32_t num_vals, prte_data_type_t type);

PRTE_EXPORT    int prte_dss_unpack_buffer(prte_buffer_t *buffer, void *dst,
                                            int32_t *num_vals, prte_data_type_t type);

/*
 * Internal pack functions
 */

int prte_dss_pack_null(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type);
int prte_dss_pack_byte(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_bool(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_int(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);
int prte_dss_pack_int16(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);
int prte_dss_pack_int32(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);
int prte_dss_pack_int64(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_sizet(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_pid(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_string(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_data_type(prte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_byte_object(prte_buffer_t *buffer, const void *src,
                              int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_pstat(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_node_stat(prte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_value(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_buffer_contents(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_float(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_double(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_timeval(prte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_time(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_name(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_jobid(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_vpid(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type);

int prte_dss_pack_status(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type);
int prte_dss_pack_envar(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type);

/*
 * Internal unpack functions
 */

int prte_dss_unpack_null(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type);
int prte_dss_unpack_byte(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_bool(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_int(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);
int prte_dss_unpack_int16(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);
int prte_dss_unpack_int32(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);
int prte_dss_unpack_int64(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_sizet(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_pid(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_string(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_data_type(prte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_byte_object(prte_buffer_t *buffer, void *dest,
                                int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_pstat(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_node_stat(prte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_value(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_buffer_contents(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_float(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_double(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_timeval(prte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_time(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_name(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_jobid(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_vpid(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_status(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type);

int prte_dss_unpack_envar(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type);

/*
 * Internal copy functions
 */

int prte_dss_std_copy(void **dest, void *src, prte_data_type_t type);

int prte_dss_copy_null(char **dest, char *src, prte_data_type_t type);

int prte_dss_copy_string(char **dest, char *src, prte_data_type_t type);

int prte_dss_copy_byte_object(prte_byte_object_t **dest, prte_byte_object_t *src,
                              prte_data_type_t type);

int prte_dss_copy_pstat(prte_pstats_t **dest, prte_pstats_t *src,
                        prte_data_type_t type);

int prte_dss_copy_node_stat(prte_node_stats_t **dest, prte_node_stats_t *src,
                            prte_data_type_t type);

int prte_dss_copy_value(prte_value_t **dest, prte_value_t *src,
                        prte_data_type_t type);

int prte_dss_copy_buffer_contents(prte_buffer_t **dest, prte_buffer_t *src,
                                  prte_data_type_t type);

int prte_dss_copy_name(prte_process_name_t **dest, prte_process_name_t *src, prte_data_type_t type);

int prte_dss_copy_jobid(prte_jobid_t **dest, prte_jobid_t *src, prte_data_type_t type);

int prte_dss_copy_vpid(prte_vpid_t **dest, prte_vpid_t *src, prte_data_type_t type);

int prte_dss_copy_envar(prte_envar_t **dest, prte_envar_t *src, prte_data_type_t type);


/*
 * Internal compare functions
 */

int prte_dss_compare_bool(bool *value1, bool *value2, prte_data_type_t type);

int prte_dss_compare_int(int *value1, int *value2, prte_data_type_t type);
int prte_dss_compare_uint(unsigned int *value1, unsigned int *value2, prte_data_type_t type);

int prte_dss_compare_size(size_t *value1, size_t *value2, prte_data_type_t type);

int prte_dss_compare_pid(pid_t *value1, pid_t *value2, prte_data_type_t type);

int prte_dss_compare_byte(char *value1, char *value2, prte_data_type_t type);
int prte_dss_compare_char(char *value1, char *value2, prte_data_type_t type);
int prte_dss_compare_int8(int8_t *value1, int8_t *value2, prte_data_type_t type);
int prte_dss_compare_uint8(uint8_t *value1, uint8_t *value2, prte_data_type_t type);

int prte_dss_compare_int16(int16_t *value1, int16_t *value2, prte_data_type_t type);
int prte_dss_compare_uint16(uint16_t *value1, uint16_t *value2, prte_data_type_t type);

int prte_dss_compare_int32(int32_t *value1, int32_t *value2, prte_data_type_t type);
int prte_dss_compare_uint32(uint32_t *value1, uint32_t *value2, prte_data_type_t type);

int prte_dss_compare_int64(int64_t *value1, int64_t *value2, prte_data_type_t type);
int prte_dss_compare_uint64(uint64_t *value1, uint64_t *value2, prte_data_type_t type);

int prte_dss_compare_null(char *value1, char *value2, prte_data_type_t type);

int prte_dss_compare_string(char *value1, char *value2, prte_data_type_t type);

int prte_dss_compare_dt(prte_data_type_t *value1, prte_data_type_t *value2, prte_data_type_t type);

int prte_dss_compare_byte_object(prte_byte_object_t *value1, prte_byte_object_t *value2, prte_data_type_t type);

int prte_dss_compare_pstat(prte_pstats_t *value1, prte_pstats_t *value2, prte_data_type_t type);

int prte_dss_compare_node_stat(prte_node_stats_t *value1, prte_node_stats_t *value2, prte_data_type_t type);

int prte_dss_compare_value(prte_value_t *value1, prte_value_t *value2, prte_data_type_t type);

int prte_dss_compare_buffer_contents(prte_buffer_t *value1, prte_buffer_t *value2, prte_data_type_t type);

int prte_dss_compare_float(float *value1, float *value2, prte_data_type_t type);

int prte_dss_compare_double(double *value1, double *value2, prte_data_type_t type);

int prte_dss_compare_timeval(struct timeval *value1, struct timeval *value2, prte_data_type_t type);

int prte_dss_compare_time(time_t *value1, time_t *value2, prte_data_type_t type);

int prte_dss_compare_name(prte_process_name_t *value1,
                          prte_process_name_t *value2,
                          prte_data_type_t type);

int prte_dss_compare_vpid(prte_vpid_t *value1,
                          prte_vpid_t *value2,
                          prte_data_type_t type);

int prte_dss_compare_jobid(prte_jobid_t *value1,
                           prte_jobid_t *value2,
                           prte_data_type_t type);

int prte_dss_compare_status(int *value1, int *value2, prte_data_type_t type);
int prte_dss_compare_envar(prte_envar_t *value1, prte_envar_t *value2, prte_data_type_t type);

/*
 * Internal print functions
 */
int prte_dss_print_byte(char **output, char *prefix, uint8_t *src, prte_data_type_t type);

int prte_dss_print_string(char **output, char *prefix, char *src, prte_data_type_t type);

int prte_dss_print_size(char **output, char *prefix, size_t *src, prte_data_type_t type);
int prte_dss_print_pid(char **output, char *prefix, pid_t *src, prte_data_type_t type);
int prte_dss_print_bool(char **output, char *prefix, bool *src, prte_data_type_t type);
int prte_dss_print_int(char **output, char *prefix, int *src, prte_data_type_t type);
int prte_dss_print_uint(char **output, char *prefix, int *src, prte_data_type_t type);
int prte_dss_print_uint8(char **output, char *prefix, uint8_t *src, prte_data_type_t type);
int prte_dss_print_uint16(char **output, char *prefix, uint16_t *src, prte_data_type_t type);
int prte_dss_print_uint32(char **output, char *prefix, uint32_t *src, prte_data_type_t type);
int prte_dss_print_int8(char **output, char *prefix, int8_t *src, prte_data_type_t type);
int prte_dss_print_int16(char **output, char *prefix, int16_t *src, prte_data_type_t type);
int prte_dss_print_int32(char **output, char *prefix, int32_t *src, prte_data_type_t type);
#ifdef HAVE_INT64_T
int prte_dss_print_uint64(char **output, char *prefix, uint64_t *src, prte_data_type_t type);
int prte_dss_print_int64(char **output, char *prefix, int64_t *src, prte_data_type_t type);
#else
int prte_dss_print_uint64(char **output, char *prefix, void *src, prte_data_type_t type);
int prte_dss_print_int64(char **output, char *prefix, void *src, prte_data_type_t type);
#endif
int prte_dss_print_null(char **output, char *prefix, void *src, prte_data_type_t type);
int prte_dss_print_data_type(char **output, char *prefix, prte_data_type_t *src, prte_data_type_t type);
int prte_dss_print_byte_object(char **output, char *prefix, prte_byte_object_t *src, prte_data_type_t type);
int prte_dss_print_pstat(char **output, char *prefix, prte_pstats_t *src, prte_data_type_t type);
int prte_dss_print_node_stat(char **output, char *prefix, prte_node_stats_t *src, prte_data_type_t type);
int prte_dss_print_value(char **output, char *prefix, prte_value_t *src, prte_data_type_t type);
int prte_dss_print_buffer_contents(char **output, char *prefix, prte_buffer_t *src, prte_data_type_t type);
int prte_dss_print_float(char **output, char *prefix, float *src, prte_data_type_t type);
int prte_dss_print_double(char **output, char *prefix, double *src, prte_data_type_t type);
int prte_dss_print_timeval(char **output, char *prefix, struct timeval *src, prte_data_type_t type);
int prte_dss_print_time(char **output, char *prefix, time_t *src, prte_data_type_t type);
int prte_dss_print_name(char **output, char *prefix, prte_process_name_t *name, prte_data_type_t type);
int prte_dss_print_jobid(char **output, char *prefix, prte_process_name_t *src, prte_data_type_t type);
int prte_dss_print_vpid(char **output, char *prefix, prte_process_name_t *src, prte_data_type_t type);
int prte_dss_print_status(char **output, char *prefix, int *src, prte_data_type_t type);
int prte_dss_print_envar(char **output, char *prefix,
                         prte_envar_t *src, prte_data_type_t type);


/*
 * Internal helper functions
 */

char* prte_dss_buffer_extend(prte_buffer_t *bptr, size_t bytes_to_add);

bool prte_dss_too_small(prte_buffer_t *buffer, size_t bytes_reqd);

prte_dss_type_info_t* prte_dss_find_type(prte_data_type_t type);

int prte_dss_store_data_type(prte_buffer_t *buffer, prte_data_type_t type);

int prte_dss_get_data_type(prte_buffer_t *buffer, prte_data_type_t *type);

END_C_DECLS

#endif
