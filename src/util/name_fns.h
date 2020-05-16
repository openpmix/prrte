/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 * Populates global structure with system-specific information.
 *
 * Notes: add limits.h, compute size of integer and other types via sizeof(type)*CHAR_BIT
 *
 */

#ifndef _PRTE_NAME_FNS_H_
#define _PRTE_NAME_FNS_H_

#include "prte_config.h"

#ifdef HAVE_STDINT_h
#include <stdint.h>
#endif

#include "types.h"

#include "src/class/prte_list.h"

BEGIN_C_DECLS

typedef uint8_t  prte_ns_cmp_bitmask_t;  /**< Bit mask for comparing process names */
#define PRTE_NS_CMP_NONE       0x00
#define PRTE_NS_CMP_JOBID      0x02
#define PRTE_NS_CMP_VPID       0x04
#define PRTE_NS_CMP_ALL        0x0f
#define PRTE_NS_CMP_WILD       0x10

/* useful define to print name args in output messages */
PRTE_EXPORT char* prte_util_print_name_args(const prte_process_name_t *name);
#define PRTE_NAME_PRINT(n) \
    prte_util_print_name_args(n)

PRTE_EXPORT char* prte_util_print_jobids(const prte_jobid_t job);
#define PRTE_JOBID_PRINT(n) \
    prte_util_print_jobids(n)

PRTE_EXPORT char* prte_util_print_vpids(const prte_vpid_t vpid);
#define PRTE_VPID_PRINT(n) \
    prte_util_print_vpids(n)

PRTE_EXPORT char* prte_util_print_job_family(const prte_jobid_t job);
#define PRTE_JOB_FAMILY_PRINT(n) \
    prte_util_print_job_family(n)

PRTE_EXPORT char* prte_util_print_local_jobid(const prte_jobid_t job);
#define PRTE_LOCAL_JOBID_PRINT(n) \
    prte_util_print_local_jobid(n)

PRTE_EXPORT char *prte_pretty_print_timing(int64_t secs, int64_t usecs);

/* a macro for identifying the job family - i.e., for
 * extracting the mpirun-specific id field of the jobid
 */
#define PRTE_JOB_FAMILY(n) \
    (((n) >> 16) & 0x0000ffff)

/* a macro for discovering the HNP name of a proc given its jobid */
#define PRTE_HNP_NAME_FROM_JOB(n, job)     \
    do {                                   \
        (n)->jobid = (job) & 0xffff0000;   \
        (n)->vpid = 0;                     \
    } while(0);

/* a macro for extracting the local jobid from the jobid - i.e.,
 * the non-mpirun-specific id field of the jobid
 */
#define PRTE_LOCAL_JOBID(n) \
    ( (n) & 0x0000ffff)

#define PRTE_CONSTRUCT_JOB_FAMILY(n) \
    ( ((n) << 16) & 0xffff0000)

#define PRTE_CONSTRUCT_LOCAL_JOBID(local, job) \
    ( ((local) & 0xffff0000) | ((job) & 0x0000ffff) )

#define PRTE_CONSTRUCT_JOBID(family, local) \
    PRTE_CONSTRUCT_LOCAL_JOBID(PRTE_CONSTRUCT_JOB_FAMILY(family), local)

/* a macro for identifying that a proc is a daemon */
#define PRTE_JOBID_IS_DAEMON(n)  \
    !((n) & 0x0000ffff)

/* a macro for obtaining the daemon jobid */
#define PRTE_DAEMON_JOBID(n) \
    ((n) & 0xffff0000)

/* List of names for general use */
struct prte_namelist_t {
    prte_list_item_t super;      /**< Allows this item to be placed on a list */
    prte_process_name_t name;   /**< Name of a process */
};
typedef struct prte_namelist_t prte_namelist_t;

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_namelist_t);

PRTE_EXPORT int prte_util_convert_vpid_to_string(char **vpid_string, const prte_vpid_t vpid);
PRTE_EXPORT int prte_util_convert_string_to_vpid(prte_vpid_t *vpid, const char* vpidstring);
PRTE_EXPORT int prte_util_convert_string_to_process_name(prte_process_name_t *name,
                                             const char* name_string);
PRTE_EXPORT int prte_util_convert_process_name_to_string(char** name_string,
                                             const prte_process_name_t *name);
PRTE_EXPORT int prte_util_create_process_name(prte_process_name_t **name,
                                  prte_jobid_t job,
                                  prte_vpid_t vpid);

PRTE_EXPORT int prte_util_compare_name_fields(prte_ns_cmp_bitmask_t fields,
                                  const prte_process_name_t* name1,
                                  const prte_process_name_t* name2);
/** This funtion returns a guaranteed unique hash value for the passed process name */
PRTE_EXPORT uint32_t prte_util_hash_vpid(prte_vpid_t vpid);
PRTE_EXPORT int prte_util_convert_string_to_sysinfo(char **cpu_type, char **cpu_model,
                                             const char* sysinfo_string);
PRTE_EXPORT int prte_util_convert_sysinfo_to_string(char** sysinfo_string,
						      const char *cpu_model, const char *cpu_type);

END_C_DECLS
#endif
