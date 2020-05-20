/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights
 *                         reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Buffer management types.
 */

#ifndef PRTE_DSS_TYPES_H_
#define PRTE_DSS_TYPES_H_

#include "prte_config.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/prte_object.h"
#include "src/class/prte_pointer_array.h"
#include "src/class/prte_list.h"

typedef uint32_t prte_jobid_t;
#define PRTE_JOBID_T        PRTE_UINT32
#define PRTE_JOBID_MAX      UINT32_MAX-2
#define PRTE_JOBID_MIN      0

typedef uint32_t prte_vpid_t;
#define PRTE_VPID_T         PRTE_UINT32
#define PRTE_VPID_MAX       UINT32_MAX-2
#define PRTE_VPID_MIN       0

typedef struct {
    prte_jobid_t jobid;
    prte_vpid_t vpid;
} prte_process_name_t;
#define PRTE_SIZEOF_PROCESS_NAME_T 8

BEGIN_C_DECLS

typedef uint8_t prte_data_type_t;  /** data type indicators */
#define PRTE_DATA_TYPE_T    PRTE_UINT8
#define PRTE_DSS_ID_MAX     UINT8_MAX
#define PRTE_DSS_ID_INVALID PRTE_DSS_ID_MAX

/* define a structure to hold generic byte objects */
typedef struct {
    int32_t size;
    uint8_t *bytes;
} prte_byte_object_t;

/* Type defines for packing and unpacking */
#define    PRTE_UNDEF               (prte_data_type_t)    0 /**< type hasn't been defined yet */
#define    PRTE_BYTE                (prte_data_type_t)    1 /**< a byte of data */
#define    PRTE_BOOL                (prte_data_type_t)    2 /**< boolean */
#define    PRTE_STRING              (prte_data_type_t)    3 /**< a NULL terminated string */
#define    PRTE_SIZE                (prte_data_type_t)    4 /**< the generic size_t */
#define    PRTE_PID                 (prte_data_type_t)    5 /**< process pid */
    /* all the integer flavors */
#define    PRTE_INT                 (prte_data_type_t)    6 /**< generic integer */
#define    PRTE_INT8                (prte_data_type_t)    7 /**< an 8-bit integer */
#define    PRTE_INT16               (prte_data_type_t)    8 /**< a 16-bit integer */
#define    PRTE_INT32               (prte_data_type_t)    9 /**< a 32-bit integer */
#define    PRTE_INT64               (prte_data_type_t)   10 /**< a 64-bit integer */
    /* all the unsigned integer flavors */
#define    PRTE_UINT                (prte_data_type_t)   11 /**< generic unsigned integer */
#define    PRTE_UINT8               (prte_data_type_t)   12 /**< an 8-bit unsigned integer */
#define    PRTE_UINT16              (prte_data_type_t)   13 /**< a 16-bit unsigned integer */
#define    PRTE_UINT32              (prte_data_type_t)   14 /**< a 32-bit unsigned integer */
#define    PRTE_UINT64              (prte_data_type_t)   15 /**< a 64-bit unsigned integer */
    /* floating point types */
#define    PRTE_FLOAT               (prte_data_type_t)   16
#define    PRTE_DOUBLE              (prte_data_type_t)   17
    /* system types */
#define    PRTE_TIMEVAL             (prte_data_type_t)   18
#define    PRTE_TIME                (prte_data_type_t)   19
    /* PRTE types */
#define    PRTE_BYTE_OBJECT         (prte_data_type_t)   20 /**< byte object structure */
#define    PRTE_DATA_TYPE           (prte_data_type_t)   21 /**< data type */
#define    PRTE_NULL                (prte_data_type_t)   22 /**< don't interpret data type */
#define    PRTE_PSTAT               (prte_data_type_t)   23 /**< process statistics */
#define    PRTE_NODE_STAT           (prte_data_type_t)   24 /**< node statistics */
#define    PRTE_HWLOC_TOPO          (prte_data_type_t)   25 /**< hwloc topology */
#define    PRTE_VALUE               (prte_data_type_t)   26 /**< prte value structure */
#define    PRTE_BUFFER              (prte_data_type_t)   27 /**< pack the remaining contents of a buffer as an object */
#define    PRTE_PTR                 (prte_data_type_t)   28 /**< pointer to void* */
#define    PRTE_NAME                (prte_data_type_t)   29
#define    PRTE_JOBID               (prte_data_type_t)   30
#define    PRTE_VPID                (prte_data_type_t)   31
#define    PRTE_STATUS              (prte_data_type_t)   32
#define    PRTE_PERSIST             (prte_data_type_t)   33 /**< corresponds to PMIx persistence type (uint8_t) */
#define    PRTE_SCOPE               (prte_data_type_t)   34 /**< corresponds to PMIx scope type (uint8_t) */
#define    PRTE_DATA_RANGE          (prte_data_type_t)   35 /**< corresponds to PMIx data range type (uint8_t) */
#define    PRTE_INFO_DIRECTIVES     (prte_data_type_t)   36 /**< corresponds to PMIx info directives type (uint32_t) */
#define    PRTE_PROC_STATE          (prte_data_type_t)   37 /**< corresponds to PMIx proc state type (uint8_t) */
#define    PRTE_PROC_INFO           (prte_data_type_t)   38 /**< corresponds to PMIx proc_info type */
#define    PRTE_ENVAR               (prte_data_type_t)   39 /**< corresponds to PMIx envar type */
#define    PRTE_LIST                (prte_data_type_t)   40 /**< an prte list */

    /* State-related types */
#define    PRTE_NODE_STATE          (prte_data_type_t)   42 /**< node status flag */
#define    PRTE_JOB_STATE           (prte_data_type_t)   43 /**< job status flag */
#define    PRTE_EXIT_CODE           (prte_data_type_t)   44 /**< process exit code */
    /* Resource types */
#define    PRTE_APP_CONTEXT         (prte_data_type_t)   45 /**< argv and enviro arrays */
#define    PRTE_NODE_DESC           (prte_data_type_t)   46 /**< describes capabilities of nodes */
#define    PRTE_SLOT_DESC           (prte_data_type_t)   47 /**< describes slot allocations/reservations */
#define    PRTE_JOB                 (prte_data_type_t)   48 /**< job information */
#define    PRTE_NODE                (prte_data_type_t)   49 /**< node information */
#define    PRTE_PROC                (prte_data_type_t)   50 /**< process information */
#define    PRTE_JOB_MAP             (prte_data_type_t)   51 /**< map of process locations */

/* RML types */
#define    PRTE_RML_TAG             (prte_data_type_t)   52 /**< tag for sending/receiving messages */
/* DAEMON command type */
#define    PRTE_DAEMON_CMD          (prte_data_type_t)   53 /**< command flag for communicating with the daemon */

/* IOF types */
#define    PRTE_IOF_TAG             (prte_data_type_t)   54

/* Attribute */
#define    PRTE_ATTRIBUTE           (prte_data_type_t)   55
/* Grpcomm signature */
#define    PRTE_SIGNATURE           (prte_data_type_t)   56

/* PRTE Dynamic */
#define    PRTE_DSS_ID_DYNAMIC      (prte_data_type_t)  100

/* define the results values for comparisons so we can change them in only one place */
#define PRTE_VALUE1_GREATER  +1
#define PRTE_VALUE2_GREATER  -1
#define PRTE_EQUAL            0

/* define some types so we can store the generic
 * values and still *know* how to convert it for PMIx */
typedef int prte_status_t;
typedef uint32_t prte_proc_state_t;  // assigned values in src/mca/plm/plm_types.h
#define PRTE_PROC_STATE_T   PRTE_UINT32

/* define an prte_proc_info_t for transferring info to/from PMIx */
typedef struct {
    prte_list_item_t super;
    prte_process_name_t name;
    char *hostname;
    char *executable_name;
    pid_t pid;
    prte_status_t exit_code;
    prte_proc_state_t state;
} prte_proc_info_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_proc_info_t);

/* defaine a struct for envar directives */
typedef struct {
    prte_list_item_t super;
    char *envar;
    char *value;
    char separator;
} prte_envar_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_envar_t);

/* Data value object */
typedef struct {
    prte_list_item_t super;             /* required for this to be on lists */
    char *key;                          /* key string */
    prte_data_type_t type;              /* the type of value stored */
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
        prte_byte_object_t bo;
        float fval;
        double dval;
        struct timeval tv;
        time_t time;
        prte_status_t status;
        prte_process_name_t name;
        prte_proc_info_t pinfo;
        void *ptr;  // never packed or passed anywhere
        prte_envar_t envar;
    } data;
} prte_value_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_value_t);

/* Process statistics object */
#define PRTE_PSTAT_MAX_STRING_LEN   32
typedef struct {
    prte_list_item_t super;                /* required for this to be on a list */
    /* process ident info */
    char node[PRTE_PSTAT_MAX_STRING_LEN];
    int32_t rank;
    pid_t pid;
    char cmd[PRTE_PSTAT_MAX_STRING_LEN];
    /* process stats */
    char state[2];
    struct timeval time;
    float percent_cpu;
    int32_t priority;
    int16_t num_threads;
    float pss;   /* in MBytes */
    float vsize;  /* in MBytes */
    float rss;  /* in MBytes */
    float peak_vsize;  /* in MBytes */
    int16_t processor;
    /* time at which sample was taken */
    struct timeval sample_time;
} prte_pstats_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_pstats_t);
typedef struct {
    prte_list_item_t super;
    char *disk;
    unsigned long num_reads_completed;
    unsigned long num_reads_merged;
    unsigned long num_sectors_read;
    unsigned long milliseconds_reading;
    unsigned long num_writes_completed;
    unsigned long num_writes_merged;
    unsigned long num_sectors_written;
    unsigned long milliseconds_writing;
    unsigned long num_ios_in_progress;
    unsigned long milliseconds_io;
    unsigned long weighted_milliseconds_io;
} prte_diskstats_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_diskstats_t);
typedef struct {
    prte_list_item_t super;
    char *net_interface;
    unsigned long num_bytes_recvd;
    unsigned long num_packets_recvd;
    unsigned long num_recv_errs;
    unsigned long num_bytes_sent;
    unsigned long num_packets_sent;
    unsigned long num_send_errs;
} prte_netstats_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_netstats_t);
typedef struct {
    prte_object_t super;
    /* node-level load averages */
    float la;
    float la5;
    float la15;
    /* memory usage */
    float total_mem;  /* in MBytes */
    float free_mem;  /* in MBytes */
    float buffers;  /* in MBytes */
    float cached;   /* in MBytes */
    float swap_cached;  /* in MBytes */
    float swap_total;   /* in MBytes */
    float swap_free;    /* in MBytes */
    float mapped;       /* in MBytes */
    /* time at which sample was taken */
    struct timeval sample_time;
    /* list of disk stats, one per disk */
    prte_list_t diskstats;
    /* list of net stats, one per interface */
    prte_list_t netstats;

} prte_node_stats_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_node_stats_t);

/* structured-unstructured data flags */
#define PRTE_DSS_STRUCTURED     true
#define PRTE_DSS_UNSTRUCTURED   false

/**
 * buffer type
 */
enum prte_dss_buffer_type_t {
    PRTE_DSS_BUFFER_NON_DESC   = 0x00,
    PRTE_DSS_BUFFER_FULLY_DESC = 0x01
};

typedef enum prte_dss_buffer_type_t prte_dss_buffer_type_t;

#define PRTE_DSS_BUFFER_TYPE_HTON(h);
#define PRTE_DSS_BUFFER_TYPE_NTOH(h);

/**
 * Structure for holding a buffer to be used with the RML or OOB
 * subsystems.
 */
struct prte_buffer_t {
    /** First member must be the object's parent */
    prte_object_t parent;
    /** type of buffer */
    prte_dss_buffer_type_t type;
    /** Start of my memory */
    char *base_ptr;
    /** Where the next data will be packed to (within the allocated
        memory starting at base_ptr) */
    char *pack_ptr;
    /** Where the next data will be unpacked from (within the
        allocated memory starting as base_ptr) */
    char *unpack_ptr;

    /** Number of bytes allocated (starting at base_ptr) */
    size_t bytes_allocated;
    /** Number of bytes used by the buffer (i.e., amount of data --
        including overhead -- packed in the buffer) */
    size_t bytes_used;
};
/**
 * Convenience typedef
 */
typedef struct prte_buffer_t prte_buffer_t;

/** formalize the declaration */
PRTE_EXPORT PRTE_CLASS_DECLARATION (prte_buffer_t);

END_C_DECLS

#endif /* PRTE_DSS_TYPES_H */
