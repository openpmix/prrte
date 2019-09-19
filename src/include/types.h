/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file */

#ifndef PRRTE_TYPES_H
#define PRRTE_TYPES_H

#include "prrte_config.h"

#include <stdint.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#if PRRTE_ENABLE_DEBUG
#include "src/util/output.h"
#endif

#if PRRTE_ENABLE_HETEROGENEOUS_SUPPORT
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include "src/dss/dss_types.h"

/**
 * Supported datatypes for messaging and storage operations.
 */

typedef int32_t prrte_std_cntr_t;  /** standard counters used in PRRTE */
#define PRRTE_STD_CNTR_T         PRRTE_INT32
#define PRRTE_STD_CNTR_MAX       INT32_MAX
#define PRRTE_STD_CNTR_MIN       INT32_MIN
#define PRRTE_STD_CNTR_INVALID   -1

/** rank on node, used for both local and node rank. We
 * don't send these around on their own, so don't create
 * dedicated type support for them - we are defining them
 * here solely for readability in the code and so we have
 * one place where any future changes can be made
 */
typedef uint16_t prrte_local_rank_t;
typedef uint16_t prrte_node_rank_t;
#define PRRTE_LOCAL_RANK         PRRTE_UINT16
#define PRRTE_NODE_RANK          PRRTE_UINT16
#define PRRTE_LOCAL_RANK_MAX     UINT16_MAX-1
#define PRRTE_NODE_RANK_MAX      UINT16_MAX-1
#define PRRTE_LOCAL_RANK_INVALID UINT16_MAX
#define PRRTE_NODE_RANK_INVALID  UINT16_MAX

/* index for app_contexts */
typedef uint32_t prrte_app_idx_t;
#define PRRTE_APP_IDX        PRRTE_UINT32
#define PRRTE_APP_IDX_MAX    UINT32_MAX

/* general typedefs & structures */

#if PRRTE_ENABLE_HETEROGENEOUS_SUPPORT && !defined(WORDS_BIGENDIAN)
#define PRRTE_PROCESS_NAME_NTOH(guid) prrte_process_name_ntoh_intr(&(guid))
static inline __prrte_attribute_always_inline__ void
prrte_process_name_ntoh_intr(prrte_process_name_t *name)
{
    name->jobid = ntohl(name->jobid);
    name->vpid = ntohl(name->vpid);
}
#define PRRTE_PROCESS_NAME_HTON(guid) prrte_process_name_hton_intr(&(guid))
static inline __prrte_attribute_always_inline__ void
prrte_process_name_hton_intr(prrte_process_name_t *name)
{
    name->jobid = htonl(name->jobid);
    name->vpid = htonl(name->vpid);
}
#else
#define PRRTE_PROCESS_NAME_NTOH(guid)
#define PRRTE_PROCESS_NAME_HTON(guid)
#endif

/*
 * portable assignment of pointer to int
 */

typedef union {
   uint64_t lval;
   uint32_t ival;
   void*    pval;
   struct {
       uint32_t uval;
       uint32_t lval;
   } sval;
} prrte_ptr_t;

/*
 * handle differences in iovec
 */

#if defined(__APPLE__) || defined(__WINDOWS__)
typedef char* prrte_iov_base_ptr_t;
#define PRRTE_IOVBASE char
#else
#define PRRTE_IOVBASE void
typedef void* prrte_iov_base_ptr_t;
#endif

/*
 * handle differences in socklen_t
 */

#if defined(HAVE_SOCKLEN_T)
typedef socklen_t prrte_socklen_t;
#else
typedef int prrte_socklen_t;
#endif

/*
 * Convert a 64 bit value to network byte order.
 */
static inline uint64_t prrte_hton64(uint64_t val) __prrte_attribute_const__;
static inline uint64_t prrte_hton64(uint64_t val)
{
#ifdef HAVE_UNIX_BYTESWAP
    union { uint64_t ll;
            uint32_t l[2];
    } w, r;

    /* platform already in network byte order? */
    if(htonl(1) == 1L)
        return val;
    w.ll = val;
    r.l[0] = htonl(w.l[1]);
    r.l[1] = htonl(w.l[0]);
    return r.ll;
#else
    return val;
#endif
}

/*
 * Convert a 64 bit value from network to host byte order.
 */

static inline uint64_t prrte_ntoh64(uint64_t val) __prrte_attribute_const__;
static inline uint64_t prrte_ntoh64(uint64_t val)
{
#ifdef HAVE_UNIX_BYTESWAP
    union { uint64_t ll;
            uint32_t l[2];
    } w, r;

    /* platform already in network byte order? */
    if(htonl(1) == 1L)
        return val;
    w.ll = val;
    r.l[0] = ntohl(w.l[1]);
    r.l[1] = ntohl(w.l[0]);
    return r.ll;
#else
    return val;
#endif
}


/**
 * Convert between a local representation of pointer and a 64 bits value.
 */
static inline uint64_t prrte_ptr_ptol( void* ptr ) __prrte_attribute_const__;
static inline uint64_t prrte_ptr_ptol( void* ptr )
{
    return (uint64_t)(uintptr_t) ptr;
}

static inline void* prrte_ptr_ltop( uint64_t value ) __prrte_attribute_const__;
static inline void* prrte_ptr_ltop( uint64_t value )
{
#if SIZEOF_VOID_P == 4 && PRRTE_ENABLE_DEBUG
    if (value > ((1ULL << 32) - 1ULL)) {
        prrte_output(0, "Warning: truncating value in prrte_ptr_ltop");
    }
#endif
    return (void*)(uintptr_t) value;
}

#if defined(WORDS_BIGENDIAN) || !defined(HAVE_UNIX_BYTESWAP)
static inline uint16_t prrte_swap_bytes2(uint16_t val) __prrte_attribute_const__;
static inline uint16_t prrte_swap_bytes2(uint16_t val)
{
    union { uint16_t bigval;
            uint8_t  arrayval[2];
    } w, r;

    w.bigval = val;
    r.arrayval[0] = w.arrayval[1];
    r.arrayval[1] = w.arrayval[0];

    return r.bigval;
}

static inline uint32_t prrte_swap_bytes4(uint32_t val) __prrte_attribute_const__;
static inline uint32_t prrte_swap_bytes4(uint32_t val)
{
    union { uint32_t bigval;
            uint8_t  arrayval[4];
    } w, r;

    w.bigval = val;
    r.arrayval[0] = w.arrayval[3];
    r.arrayval[1] = w.arrayval[2];
    r.arrayval[2] = w.arrayval[1];
    r.arrayval[3] = w.arrayval[0];

    return r.bigval;
}

static inline uint64_t prrte_swap_bytes8(uint64_t val) __prrte_attribute_const__;
static inline uint64_t prrte_swap_bytes8(uint64_t val)
{
    union { uint64_t bigval;
            uint8_t  arrayval[8];
    } w, r;

    w.bigval = val;
    r.arrayval[0] = w.arrayval[7];
    r.arrayval[1] = w.arrayval[6];
    r.arrayval[2] = w.arrayval[5];
    r.arrayval[3] = w.arrayval[4];
    r.arrayval[4] = w.arrayval[3];
    r.arrayval[5] = w.arrayval[2];
    r.arrayval[6] = w.arrayval[1];
    r.arrayval[7] = w.arrayval[0];

    return r.bigval;
}

#else
#define prrte_swap_bytes2 htons
#define prrte_swap_bytes4 htonl
#define prrte_swap_bytes8 prrte_hton64
#endif /* WORDS_BIGENDIAN || !HAVE_UNIX_BYTESWAP */

#define PRRTE_NAME_ARGS(n) \
    (unsigned long) ((NULL == n) ? (unsigned long)PRRTE_JOBID_INVALID : (unsigned long)(n)->jobid), \
    (unsigned long) ((NULL == n) ? (unsigned long)PRRTE_VPID_INVALID : (unsigned long)(n)->vpid) \

/*
 * define invalid values
 */
#define PRRTE_JOBID_INVALID          (PRRTE_JOBID_MAX + 2)
#define PRRTE_VPID_INVALID           (PRRTE_VPID_MAX + 2)
#define PRRTE_LOCAL_JOBID_INVALID    (PRRTE_JOBID_INVALID & 0x0000FFFF)

/*
 * define wildcard values
 */
#define PRRTE_JOBID_WILDCARD         (PRRTE_JOBID_MAX + 1)
#define PRRTE_VPID_WILDCARD          (PRRTE_VPID_MAX + 1)
#define PRRTE_LOCAL_JOBID_WILDCARD   (PRRTE_JOBID_WILDCARD & 0x0000FFFF)

/* PRRTE attribute */
typedef uint16_t prrte_attribute_key_t;
#define PRRTE_ATTR_KEY_T   PRRTE_UINT16
typedef struct {
    prrte_list_item_t super;             /* required for this to be on lists */
    prrte_attribute_key_t key;           /* key identifier */
    prrte_data_type_t type;              /* the type of value stored */
    bool local;                         // whether or not to pack/send this value
    union {
        bool flag;
        uint8_t byte;
        char *string;
        size_t size;
        pid_t pid;
        int integer;
        int8_t int8;
        int16_t int16;
        int32_t int32;
        int64_t int64;
        unsigned int uint;
        uint8_t uint8;
        uint16_t uint16;
        uint32_t uint32;
        uint64_t uint64;
        prrte_byte_object_t bo;
        prrte_buffer_t buf;
        float fval;
        struct timeval tv;
        void *ptr;  // never packed or passed anywhere
        prrte_vpid_t vpid;
        prrte_jobid_t jobid;
        prrte_process_name_t name;
        prrte_envar_t envar;
    } data;
} prrte_attribute_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_attribute_t);

#endif
