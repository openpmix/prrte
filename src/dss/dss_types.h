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
 * Copyright (c) 2007-2011 Cisco Systems, Inc.  All rights reserved.
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

#ifndef PRRTE_DSS_TYPES_H_
#define PRRTE_DSS_TYPES_H_

#include "prrte_config.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/prrte_object.h"
#include "src/class/prrte_pointer_array.h"
#include "src/class/prrte_list.h"

typedef uint32_t prrte_jobid_t;
#define PRRTE_JOBID_T        PRRTE_UINT32
#define PRRTE_JOBID_MAX      UINT32_MAX-2
#define PRRTE_JOBID_MIN      0

typedef uint32_t prrte_vpid_t;
#define PRRTE_VPID_T         PRRTE_UINT32
#define PRRTE_VPID_MAX       UINT32_MAX-2
#define PRRTE_VPID_MIN       0

typedef struct {
    prrte_jobid_t jobid;
    prrte_vpid_t vpid;
} prrte_process_name_t;
#define PRRTE_SIZEOF_PROCESS_NAME_T 8

BEGIN_C_DECLS

typedef uint8_t prrte_data_type_t;  /** data type indicators */
#define PRRTE_DATA_TYPE_T    PRRTE_UINT8
#define PRRTE_DSS_ID_MAX     UINT8_MAX
#define PRRTE_DSS_ID_INVALID PRRTE_DSS_ID_MAX

/* define a structure to hold generic byte objects */
typedef struct {
    int32_t size;
    uint8_t *bytes;
} prrte_byte_object_t;

/* Type defines for packing and unpacking */
#define    PRRTE_UNDEF               (prrte_data_type_t)    0 /**< type hasn't been defined yet */
#define    PRRTE_BYTE                (prrte_data_type_t)    1 /**< a byte of data */
#define    PRRTE_BOOL                (prrte_data_type_t)    2 /**< boolean */
#define    PRRTE_STRING              (prrte_data_type_t)    3 /**< a NULL terminated string */
#define    PRRTE_SIZE                (prrte_data_type_t)    4 /**< the generic size_t */
#define    PRRTE_PID                 (prrte_data_type_t)    5 /**< process pid */
    /* all the integer flavors */
#define    PRRTE_INT                 (prrte_data_type_t)    6 /**< generic integer */
#define    PRRTE_INT8                (prrte_data_type_t)    7 /**< an 8-bit integer */
#define    PRRTE_INT16               (prrte_data_type_t)    8 /**< a 16-bit integer */
#define    PRRTE_INT32               (prrte_data_type_t)    9 /**< a 32-bit integer */
#define    PRRTE_INT64               (prrte_data_type_t)   10 /**< a 64-bit integer */
    /* all the unsigned integer flavors */
#define    PRRTE_UINT                (prrte_data_type_t)   11 /**< generic unsigned integer */
#define    PRRTE_UINT8               (prrte_data_type_t)   12 /**< an 8-bit unsigned integer */
#define    PRRTE_UINT16              (prrte_data_type_t)   13 /**< a 16-bit unsigned integer */
#define    PRRTE_UINT32              (prrte_data_type_t)   14 /**< a 32-bit unsigned integer */
#define    PRRTE_UINT64              (prrte_data_type_t)   15 /**< a 64-bit unsigned integer */
    /* floating point types */
#define    PRRTE_FLOAT               (prrte_data_type_t)   16
#define    PRRTE_DOUBLE              (prrte_data_type_t)   17
    /* system types */
#define    PRRTE_TIMEVAL             (prrte_data_type_t)   18
#define    PRRTE_TIME                (prrte_data_type_t)   19
    /* PRRTE types */
#define    PRRTE_BYTE_OBJECT         (prrte_data_type_t)   20 /**< byte object structure */
#define    PRRTE_DATA_TYPE           (prrte_data_type_t)   21 /**< data type */
#define    PRRTE_NULL                (prrte_data_type_t)   22 /**< don't interpret data type */
#define    PRRTE_PSTAT               (prrte_data_type_t)   23 /**< process statistics */
#define    PRRTE_NODE_STAT           (prrte_data_type_t)   24 /**< node statistics */
#define    PRRTE_HWLOC_TOPO          (prrte_data_type_t)   25 /**< hwloc topology */
#define    PRRTE_VALUE               (prrte_data_type_t)   26 /**< prrte value structure */
#define    PRRTE_BUFFER              (prrte_data_type_t)   27 /**< pack the remaining contents of a buffer as an object */
#define    PRRTE_PTR                 (prrte_data_type_t)   28 /**< pointer to void* */
#define    PRRTE_NAME                (prrte_data_type_t)   29
#define    PRRTE_JOBID               (prrte_data_type_t)   30
#define    PRRTE_VPID                (prrte_data_type_t)   31
#define    PRRTE_STATUS              (prrte_data_type_t)   32
#define    PRRTE_PERSIST             (prrte_data_type_t)   33 /**< corresponds to PMIx persistence type (uint8_t) */
#define    PRRTE_SCOPE               (prrte_data_type_t)   34 /**< corresponds to PMIx scope type (uint8_t) */
#define    PRRTE_DATA_RANGE          (prrte_data_type_t)   35 /**< corresponds to PMIx data range type (uint8_t) */
#define    PRRTE_INFO_DIRECTIVES     (prrte_data_type_t)   36 /**< corresponds to PMIx info directives type (uint32_t) */
#define    PRRTE_PROC_STATE          (prrte_data_type_t)   37 /**< corresponds to PMIx proc state type (uint8_t) */
#define    PRRTE_PROC_INFO           (prrte_data_type_t)   38 /**< corresponds to PMIx proc_info type */
#define    PRRTE_ENVAR               (prrte_data_type_t)   39 /**< corresponds to PMIx envar type */
#define    PRRTE_LIST                (prrte_data_type_t)   40 /**< an prrte list */

/* General PRRTE types - support handled within DSS */
#define    PRRTE_STD_CNTR            (prrte_data_type_t)   41 /**< standard counter type */
    /* State-related types */
#define    PRRTE_NODE_STATE          (prrte_data_type_t)   42 /**< node status flag */
#define    PRRTE_JOB_STATE           (prrte_data_type_t)   43 /**< job status flag */
#define    PRRTE_EXIT_CODE           (prrte_data_type_t)   44 /**< process exit code */
    /* Resource types */
#define    PRRTE_APP_CONTEXT         (prrte_data_type_t)   45 /**< argv and enviro arrays */
#define    PRRTE_NODE_DESC           (prrte_data_type_t)   46 /**< describes capabilities of nodes */
#define    PRRTE_SLOT_DESC           (prrte_data_type_t)   47 /**< describes slot allocations/reservations */
#define    PRRTE_JOB                 (prrte_data_type_t)   48 /**< job information */
#define    PRRTE_NODE                (prrte_data_type_t)   49 /**< node information */
#define    PRRTE_PROC                (prrte_data_type_t)   50 /**< process information */
#define    PRRTE_JOB_MAP             (prrte_data_type_t)   51 /**< map of process locations */

/* RML types */
#define    PRRTE_RML_TAG             (prrte_data_type_t)   52 /**< tag for sending/receiving messages */
/* DAEMON command type */
#define    PRRTE_DAEMON_CMD          (prrte_data_type_t)   53 /**< command flag for communicating with the daemon */

/* IOF types */
#define    PRRTE_IOF_TAG             (prrte_data_type_t)   54

/* Attribute */
#define    PRRTE_ATTRIBUTE           (prrte_data_type_t)   55
/* Grpcomm signature */
#define    PRRTE_SIGNATURE           (prrte_data_type_t)   56

/* PRRTE Dynamic */
#define    PRRTE_DSS_ID_DYNAMIC      (prrte_data_type_t)  100

/* define the results values for comparisons so we can change them in only one place */
#define PRRTE_VALUE1_GREATER  +1
#define PRRTE_VALUE2_GREATER  -1
#define PRRTE_EQUAL            0

/* define some types so we can store the generic
 * values and still *know* how to convert it for PMIx */
typedef int prrte_status_t;
typedef uint32_t prrte_proc_state_t;  // assigned values in src/mca/plm/plm_types.h
#define PRRTE_PROC_STATE_T   PRRTE_UINT32

/* define an prrte_proc_info_t for transferring info to/from PMIx */
typedef struct {
    prrte_list_item_t super;
    prrte_process_name_t name;
    char *hostname;
    char *executable_name;
    pid_t pid;
    prrte_status_t exit_code;
    prrte_proc_state_t state;
} prrte_proc_info_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_proc_info_t);

/* defaine a struct for envar directives */
typedef struct {
    prrte_list_item_t super;
    char *envar;
    char *value;
    char separator;
} prrte_envar_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_envar_t);

/* Data value object */
typedef struct {
    prrte_list_item_t super;             /* required for this to be on lists */
    char *key;                          /* key string */
    prrte_data_type_t type;              /* the type of value stored */
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
        float fval;
        double dval;
        struct timeval tv;
        time_t time;
        prrte_status_t status;
        prrte_process_name_t name;
        prrte_proc_info_t pinfo;
        void *ptr;  // never packed or passed anywhere
        prrte_envar_t envar;
    } data;
} prrte_value_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_value_t);

/* Process statistics object */
#define PRRTE_PSTAT_MAX_STRING_LEN   32
typedef struct {
    prrte_list_item_t super;                /* required for this to be on a list */
    /* process ident info */
    char node[PRRTE_PSTAT_MAX_STRING_LEN];
    int32_t rank;
    pid_t pid;
    char cmd[PRRTE_PSTAT_MAX_STRING_LEN];
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
} prrte_pstats_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_pstats_t);
typedef struct {
    prrte_list_item_t super;
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
} prrte_diskstats_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_diskstats_t);
typedef struct {
    prrte_list_item_t super;
    char *net_interface;
    unsigned long num_bytes_recvd;
    unsigned long num_packets_recvd;
    unsigned long num_recv_errs;
    unsigned long num_bytes_sent;
    unsigned long num_packets_sent;
    unsigned long num_send_errs;
} prrte_netstats_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_netstats_t);
typedef struct {
    prrte_object_t super;
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
    prrte_list_t diskstats;
    /* list of net stats, one per interface */
    prrte_list_t netstats;

} prrte_node_stats_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_node_stats_t);

/* structured-unstructured data flags */
#define PRRTE_DSS_STRUCTURED     true
#define PRRTE_DSS_UNSTRUCTURED   false

/**
 * buffer type
 */
enum prrte_dss_buffer_type_t {
    PRRTE_DSS_BUFFER_NON_DESC   = 0x00,
    PRRTE_DSS_BUFFER_FULLY_DESC = 0x01
};

typedef enum prrte_dss_buffer_type_t prrte_dss_buffer_type_t;

#define PRRTE_DSS_BUFFER_TYPE_HTON(h);
#define PRRTE_DSS_BUFFER_TYPE_NTOH(h);

/**
 * Structure for holding a buffer to be used with the RML or OOB
 * subsystems.
 */
struct prrte_buffer_t {
    /** First member must be the object's parent */
    prrte_object_t parent;
    /** type of buffer */
    prrte_dss_buffer_type_t type;
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
typedef struct prrte_buffer_t prrte_buffer_t;

/** formalize the declaration */
PRRTE_EXPORT PRRTE_CLASS_DECLARATION (prrte_buffer_t);

END_C_DECLS

#endif /* PRRTE_DSS_TYPES_H */
