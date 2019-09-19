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
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PRRTE_DSS_INTERNAL_H_
#define PRRTE_DSS_INTERNAL_H_

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/prrte_pointer_array.h"

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
#define PRRTE_DSS_DEFAULT_INITIAL_SIZE  2048
/*
 * The default threshold size when we switch from doubling the
 * buffer size to addatively increasing it
 */
#define PRRTE_DSS_DEFAULT_THRESHOLD_SIZE 4096

/*
 * Internal type corresponding to size_t.  Do not use this in
 * interface calls - use PRRTE_SIZE instead.
 */
#if SIZEOF_SIZE_T == 1
#define DSS_TYPE_SIZE_T PRRTE_UINT8
#elif SIZEOF_SIZE_T == 2
#define DSS_TYPE_SIZE_T PRRTE_UINT16
#elif SIZEOF_SIZE_T == 4
#define DSS_TYPE_SIZE_T PRRTE_UINT32
#elif SIZEOF_SIZE_T == 8
#define DSS_TYPE_SIZE_T PRRTE_UINT64
#else
#error Unsupported size_t size!
#endif

/*
 * Internal type corresponding to bool.  Do not use this in interface
 * calls - use PRRTE_BOOL instead.
 */
#if SIZEOF__BOOL == 1
#define DSS_TYPE_BOOL PRRTE_UINT8
#elif SIZEOF__BOOL == 2
#define DSS_TYPE_BOOL PRRTE_UINT16
#elif SIZEOF__BOOL == 4
#define DSS_TYPE_BOOL PRRTE_UINT32
#elif SIZEOF__BOOL == 8
#define DSS_TYPE_BOOL PRRTE_UINT64
#else
#error Unsupported bool size!
#endif

/*
 * Internal type corresponding to int and unsigned int.  Do not use
 * this in interface calls - use PRRTE_INT / PRRTE_UINT instead.
 */
#if SIZEOF_INT == 1
#define DSS_TYPE_INT PRRTE_INT8
#define DSS_TYPE_UINT PRRTE_UINT8
#elif SIZEOF_INT == 2
#define DSS_TYPE_INT PRRTE_INT16
#define DSS_TYPE_UINT PRRTE_UINT16
#elif SIZEOF_INT == 4
#define DSS_TYPE_INT PRRTE_INT32
#define DSS_TYPE_UINT PRRTE_UINT32
#elif SIZEOF_INT == 8
#define DSS_TYPE_INT PRRTE_INT64
#define DSS_TYPE_UINT PRRTE_UINT64
#else
#error Unsupported int size!
#endif

/*
 * Internal type corresponding to pid_t.  Do not use this in interface
 * calls - use PRRTE_PID instead.
 */
#if SIZEOF_PID_T == 1
#define DSS_TYPE_PID_T PRRTE_UINT8
#elif SIZEOF_PID_T == 2
#define DSS_TYPE_PID_T PRRTE_UINT16
#elif SIZEOF_PID_T == 4
#define DSS_TYPE_PID_T PRRTE_UINT32
#elif SIZEOF_PID_T == 8
#define DSS_TYPE_PID_T PRRTE_UINT64
#else
#error Unsupported pid_t size!
#endif

/* Unpack generic size macros */
#define UNPACK_SIZE_MISMATCH(unpack_type, remote_type, ret)                 \
    do {                                                                    \
        switch(remote_type) {                                               \
        case PRRTE_UINT8:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint8_t, remote_type);  \
            break;                                                          \
        case PRRTE_INT8:                                                     \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int8_t, remote_type);   \
            break;                                                          \
        case PRRTE_UINT16:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint16_t, remote_type); \
            break;                                                          \
        case PRRTE_INT16:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int16_t, remote_type);  \
            break;                                                          \
        case PRRTE_UINT32:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint32_t, remote_type); \
            break;                                                          \
        case PRRTE_INT32:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int32_t, remote_type);  \
            break;                                                          \
        case PRRTE_UINT64:                                                   \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, uint64_t, remote_type); \
            break;                                                          \
        case PRRTE_INT64:                                                    \
            UNPACK_SIZE_MISMATCH_FOUND(unpack_type, int64_t, remote_type);  \
            break;                                                          \
        default:                                                            \
            ret = PRRTE_ERR_NOT_FOUND;                                       \
        }                                                                   \
    } while (0)

/* NOTE: do not need to deal with endianness here, as the unpacking of
   the underling sender-side type will do that for us.  Repeat: the
   data in tmpbuf[] is already in host byte order. */
#define UNPACK_SIZE_MISMATCH_FOUND(unpack_type, tmptype, tmpdsstype)        \
    do {                                                                    \
        int32_t i;                                                          \
        tmptype *tmpbuf = (tmptype*)malloc(sizeof(tmptype) * (*num_vals));  \
        ret = prrte_dss_unpack_buffer(buffer, tmpbuf, num_vals, tmpdsstype); \
        for (i = 0 ; i < *num_vals ; ++i) {                                 \
            ((unpack_type*) dest)[i] = (unpack_type)(tmpbuf[i]);            \
        }                                                                   \
        free(tmpbuf);                                                       \
    } while (0)


/**
 * Internal struct used for holding registered dss functions
 */
struct prrte_dss_type_info_t {
    prrte_object_t super;
    /* type identifier */
    prrte_data_type_t odti_type;
    /** Debugging string name */
    char *odti_name;
    /** Pack function */
    prrte_dss_pack_fn_t odti_pack_fn;
    /** Unpack function */
    prrte_dss_unpack_fn_t odti_unpack_fn;
    /** copy function */
    prrte_dss_copy_fn_t odti_copy_fn;
    /** compare function */
    prrte_dss_compare_fn_t odti_compare_fn;
    /** print function */
    prrte_dss_print_fn_t odti_print_fn;
    /** flag to indicate structured data */
    bool odti_structured;
};
/**
 * Convenience typedef
 */
typedef struct prrte_dss_type_info_t prrte_dss_type_info_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_dss_type_info_t);

/*
 * globals needed within dss
 */
extern bool prrte_dss_initialized;
extern bool prrte_dss_debug;
extern int prrte_dss_verbose;
extern int prrte_dss_initial_size;
extern int prrte_dss_threshold_size;
extern prrte_pointer_array_t prrte_dss_types;
extern prrte_data_type_t prrte_dss_num_reg_types;

/*
 * Implementations of API functions
 */

int prrte_dss_pack(prrte_buffer_t *buffer, const void *src,
                  int32_t num_vals,
                  prrte_data_type_t type);
int prrte_dss_unpack(prrte_buffer_t *buffer, void *dest,
                    int32_t *max_num_vals,
                    prrte_data_type_t type);

int prrte_dss_copy(void **dest, void *src, prrte_data_type_t type);

int prrte_dss_compare(const void *value1, const void *value2,
                     prrte_data_type_t type);

int prrte_dss_print(char **output, char *prefix, void *src, prrte_data_type_t type);

int prrte_dss_dump(int output_stream, void *src, prrte_data_type_t type);

int prrte_dss_peek(prrte_buffer_t *buffer, prrte_data_type_t *type,
                  int32_t *number);

int prrte_dss_peek_type(prrte_buffer_t *buffer, prrte_data_type_t *type);

int prrte_dss_unload(prrte_buffer_t *buffer, void **payload,
                    int32_t *bytes_used);
int prrte_dss_load(prrte_buffer_t *buffer, void *payload, int32_t bytes_used);

int prrte_dss_copy_payload(prrte_buffer_t *dest, prrte_buffer_t *src);

int prrte_dss_register(prrte_dss_pack_fn_t pack_fn,
                      prrte_dss_unpack_fn_t unpack_fn,
                      prrte_dss_copy_fn_t copy_fn,
                      prrte_dss_compare_fn_t compare_fn,
                      prrte_dss_print_fn_t print_fn,
                      bool structured,
                      const char *name, prrte_data_type_t *type);

bool prrte_dss_structured(prrte_data_type_t type);

char *prrte_dss_lookup_data_type(prrte_data_type_t type);

void prrte_dss_dump_data_types(int output);

/*
 * Specialized functions
 */
PRRTE_EXPORT    int prrte_dss_pack_buffer(prrte_buffer_t *buffer, const void *src,
                                          int32_t num_vals, prrte_data_type_t type);

PRRTE_EXPORT    int prrte_dss_unpack_buffer(prrte_buffer_t *buffer, void *dst,
                                            int32_t *num_vals, prrte_data_type_t type);

/*
 * Internal pack functions
 */

int prrte_dss_pack_null(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type);
int prrte_dss_pack_byte(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_bool(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_int(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);
int prrte_dss_pack_int16(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);
int prrte_dss_pack_int32(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);
int prrte_dss_pack_int64(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_sizet(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_pid(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_string(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_data_type(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_byte_object(prrte_buffer_t *buffer, const void *src,
                              int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_pstat(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_node_stat(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_value(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_buffer_contents(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_float(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_double(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_timeval(prrte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_time(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_name(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_jobid(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_vpid(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type);

int prrte_dss_pack_status(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type);
int prrte_dss_pack_envar(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type);

/*
 * Internal unpack functions
 */

int prrte_dss_unpack_null(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type);
int prrte_dss_unpack_byte(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_bool(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_int(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);
int prrte_dss_unpack_int16(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);
int prrte_dss_unpack_int32(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);
int prrte_dss_unpack_int64(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_sizet(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_pid(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_string(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_data_type(prrte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_byte_object(prrte_buffer_t *buffer, void *dest,
                                int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_pstat(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_node_stat(prrte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_value(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_buffer_contents(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_float(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_double(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_timeval(prrte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_time(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_name(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_jobid(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_vpid(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_status(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type);

int prrte_dss_unpack_envar(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type);

/*
 * Internal copy functions
 */

int prrte_dss_std_copy(void **dest, void *src, prrte_data_type_t type);

int prrte_dss_copy_null(char **dest, char *src, prrte_data_type_t type);

int prrte_dss_copy_string(char **dest, char *src, prrte_data_type_t type);

int prrte_dss_copy_byte_object(prrte_byte_object_t **dest, prrte_byte_object_t *src,
                              prrte_data_type_t type);

int prrte_dss_copy_pstat(prrte_pstats_t **dest, prrte_pstats_t *src,
                        prrte_data_type_t type);

int prrte_dss_copy_node_stat(prrte_node_stats_t **dest, prrte_node_stats_t *src,
                            prrte_data_type_t type);

int prrte_dss_copy_value(prrte_value_t **dest, prrte_value_t *src,
                        prrte_data_type_t type);

int prrte_dss_copy_buffer_contents(prrte_buffer_t **dest, prrte_buffer_t *src,
                                  prrte_data_type_t type);

int prrte_dss_copy_name(prrte_process_name_t **dest, prrte_process_name_t *src, prrte_data_type_t type);

int prrte_dss_copy_jobid(prrte_jobid_t **dest, prrte_jobid_t *src, prrte_data_type_t type);

int prrte_dss_copy_vpid(prrte_vpid_t **dest, prrte_vpid_t *src, prrte_data_type_t type);

int prrte_dss_copy_envar(prrte_envar_t **dest, prrte_envar_t *src, prrte_data_type_t type);


/*
 * Internal compare functions
 */

int prrte_dss_compare_bool(bool *value1, bool *value2, prrte_data_type_t type);

int prrte_dss_compare_int(int *value1, int *value2, prrte_data_type_t type);
int prrte_dss_compare_uint(unsigned int *value1, unsigned int *value2, prrte_data_type_t type);

int prrte_dss_compare_size(size_t *value1, size_t *value2, prrte_data_type_t type);

int prrte_dss_compare_pid(pid_t *value1, pid_t *value2, prrte_data_type_t type);

int prrte_dss_compare_byte(char *value1, char *value2, prrte_data_type_t type);
int prrte_dss_compare_char(char *value1, char *value2, prrte_data_type_t type);
int prrte_dss_compare_int8(int8_t *value1, int8_t *value2, prrte_data_type_t type);
int prrte_dss_compare_uint8(uint8_t *value1, uint8_t *value2, prrte_data_type_t type);

int prrte_dss_compare_int16(int16_t *value1, int16_t *value2, prrte_data_type_t type);
int prrte_dss_compare_uint16(uint16_t *value1, uint16_t *value2, prrte_data_type_t type);

int prrte_dss_compare_int32(int32_t *value1, int32_t *value2, prrte_data_type_t type);
int prrte_dss_compare_uint32(uint32_t *value1, uint32_t *value2, prrte_data_type_t type);

int prrte_dss_compare_int64(int64_t *value1, int64_t *value2, prrte_data_type_t type);
int prrte_dss_compare_uint64(uint64_t *value1, uint64_t *value2, prrte_data_type_t type);

int prrte_dss_compare_null(char *value1, char *value2, prrte_data_type_t type);

int prrte_dss_compare_string(char *value1, char *value2, prrte_data_type_t type);

int prrte_dss_compare_dt(prrte_data_type_t *value1, prrte_data_type_t *value2, prrte_data_type_t type);

int prrte_dss_compare_byte_object(prrte_byte_object_t *value1, prrte_byte_object_t *value2, prrte_data_type_t type);

int prrte_dss_compare_pstat(prrte_pstats_t *value1, prrte_pstats_t *value2, prrte_data_type_t type);

int prrte_dss_compare_node_stat(prrte_node_stats_t *value1, prrte_node_stats_t *value2, prrte_data_type_t type);

int prrte_dss_compare_value(prrte_value_t *value1, prrte_value_t *value2, prrte_data_type_t type);

int prrte_dss_compare_buffer_contents(prrte_buffer_t *value1, prrte_buffer_t *value2, prrte_data_type_t type);

int prrte_dss_compare_float(float *value1, float *value2, prrte_data_type_t type);

int prrte_dss_compare_double(double *value1, double *value2, prrte_data_type_t type);

int prrte_dss_compare_timeval(struct timeval *value1, struct timeval *value2, prrte_data_type_t type);

int prrte_dss_compare_time(time_t *value1, time_t *value2, prrte_data_type_t type);

int prrte_dss_compare_name(prrte_process_name_t *value1,
                          prrte_process_name_t *value2,
                          prrte_data_type_t type);

int prrte_dss_compare_vpid(prrte_vpid_t *value1,
                          prrte_vpid_t *value2,
                          prrte_data_type_t type);

int prrte_dss_compare_jobid(prrte_jobid_t *value1,
                           prrte_jobid_t *value2,
                           prrte_data_type_t type);

int prrte_dss_compare_status(int *value1, int *value2, prrte_data_type_t type);
int prrte_dss_compare_envar(prrte_envar_t *value1, prrte_envar_t *value2, prrte_data_type_t type);

/*
 * Internal print functions
 */
int prrte_dss_print_byte(char **output, char *prefix, uint8_t *src, prrte_data_type_t type);

int prrte_dss_print_string(char **output, char *prefix, char *src, prrte_data_type_t type);

int prrte_dss_print_size(char **output, char *prefix, size_t *src, prrte_data_type_t type);
int prrte_dss_print_pid(char **output, char *prefix, pid_t *src, prrte_data_type_t type);
int prrte_dss_print_bool(char **output, char *prefix, bool *src, prrte_data_type_t type);
int prrte_dss_print_int(char **output, char *prefix, int *src, prrte_data_type_t type);
int prrte_dss_print_uint(char **output, char *prefix, int *src, prrte_data_type_t type);
int prrte_dss_print_uint8(char **output, char *prefix, uint8_t *src, prrte_data_type_t type);
int prrte_dss_print_uint16(char **output, char *prefix, uint16_t *src, prrte_data_type_t type);
int prrte_dss_print_uint32(char **output, char *prefix, uint32_t *src, prrte_data_type_t type);
int prrte_dss_print_int8(char **output, char *prefix, int8_t *src, prrte_data_type_t type);
int prrte_dss_print_int16(char **output, char *prefix, int16_t *src, prrte_data_type_t type);
int prrte_dss_print_int32(char **output, char *prefix, int32_t *src, prrte_data_type_t type);
#ifdef HAVE_INT64_T
int prrte_dss_print_uint64(char **output, char *prefix, uint64_t *src, prrte_data_type_t type);
int prrte_dss_print_int64(char **output, char *prefix, int64_t *src, prrte_data_type_t type);
#else
int prrte_dss_print_uint64(char **output, char *prefix, void *src, prrte_data_type_t type);
int prrte_dss_print_int64(char **output, char *prefix, void *src, prrte_data_type_t type);
#endif
int prrte_dss_print_null(char **output, char *prefix, void *src, prrte_data_type_t type);
int prrte_dss_print_data_type(char **output, char *prefix, prrte_data_type_t *src, prrte_data_type_t type);
int prrte_dss_print_byte_object(char **output, char *prefix, prrte_byte_object_t *src, prrte_data_type_t type);
int prrte_dss_print_pstat(char **output, char *prefix, prrte_pstats_t *src, prrte_data_type_t type);
int prrte_dss_print_node_stat(char **output, char *prefix, prrte_node_stats_t *src, prrte_data_type_t type);
int prrte_dss_print_value(char **output, char *prefix, prrte_value_t *src, prrte_data_type_t type);
int prrte_dss_print_buffer_contents(char **output, char *prefix, prrte_buffer_t *src, prrte_data_type_t type);
int prrte_dss_print_float(char **output, char *prefix, float *src, prrte_data_type_t type);
int prrte_dss_print_double(char **output, char *prefix, double *src, prrte_data_type_t type);
int prrte_dss_print_timeval(char **output, char *prefix, struct timeval *src, prrte_data_type_t type);
int prrte_dss_print_time(char **output, char *prefix, time_t *src, prrte_data_type_t type);
int prrte_dss_print_name(char **output, char *prefix, prrte_process_name_t *name, prrte_data_type_t type);
int prrte_dss_print_jobid(char **output, char *prefix, prrte_process_name_t *src, prrte_data_type_t type);
int prrte_dss_print_vpid(char **output, char *prefix, prrte_process_name_t *src, prrte_data_type_t type);
int prrte_dss_print_status(char **output, char *prefix, int *src, prrte_data_type_t type);
int prrte_dss_print_envar(char **output, char *prefix,
                         prrte_envar_t *src, prrte_data_type_t type);


/*
 * Internal helper functions
 */

char* prrte_dss_buffer_extend(prrte_buffer_t *bptr, size_t bytes_to_add);

bool prrte_dss_too_small(prrte_buffer_t *buffer, size_t bytes_reqd);

prrte_dss_type_info_t* prrte_dss_find_type(prrte_data_type_t type);

int prrte_dss_store_data_type(prrte_buffer_t *buffer, prrte_data_type_t type);

int prrte_dss_get_data_type(prrte_buffer_t *buffer, prrte_data_type_t *type);

END_C_DECLS

#endif
