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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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

#ifndef _PRRTE_NAME_FNS_H_
#define _PRRTE_NAME_FNS_H_

#include "prrte_config.h"

#ifdef HAVE_STDINT_h
#include <stdint.h>
#endif

#include "types.h"

#include "src/class/prrte_list.h"

BEGIN_C_DECLS

typedef uint8_t  prrte_ns_cmp_bitmask_t;  /**< Bit mask for comparing process names */
#define PRRTE_NS_CMP_NONE       0x00
#define PRRTE_NS_CMP_JOBID      0x02
#define PRRTE_NS_CMP_VPID       0x04
#define PRRTE_NS_CMP_ALL        0x0f
#define PRRTE_NS_CMP_WILD       0x10

/* useful define to print name args in output messages */
PRRTE_EXPORT char* prrte_util_print_name_args(const prrte_process_name_t *name);
#define PRRTE_NAME_PRINT(n) \
    prrte_util_print_name_args(n)

PRRTE_EXPORT char* prrte_util_print_jobids(const prrte_jobid_t job);
#define PRRTE_JOBID_PRINT(n) \
    prrte_util_print_jobids(n)

PRRTE_EXPORT char* prrte_util_print_vpids(const prrte_vpid_t vpid);
#define PRRTE_VPID_PRINT(n) \
    prrte_util_print_vpids(n)

PRRTE_EXPORT char* prrte_util_print_job_family(const prrte_jobid_t job);
#define PRRTE_JOB_FAMILY_PRINT(n) \
    prrte_util_print_job_family(n)

PRRTE_EXPORT char* prrte_util_print_local_jobid(const prrte_jobid_t job);
#define PRRTE_LOCAL_JOBID_PRINT(n) \
    prrte_util_print_local_jobid(n)

PRRTE_EXPORT char *prrte_pretty_print_timing(int64_t secs, int64_t usecs);

/* a macro for identifying the job family - i.e., for
 * extracting the mpirun-specific id field of the jobid
 */
#define PRRTE_JOB_FAMILY(n) \
    (((n) >> 16) & 0x0000ffff)

/* a macro for discovering the HNP name of a proc given its jobid */
#define PRRTE_HNP_NAME_FROM_JOB(n, job)     \
    do {                                   \
        (n)->jobid = (job) & 0xffff0000;   \
        (n)->vpid = 0;                     \
    } while(0);

/* a macro for extracting the local jobid from the jobid - i.e.,
 * the non-mpirun-specific id field of the jobid
 */
#define PRRTE_LOCAL_JOBID(n) \
    ( (n) & 0x0000ffff)

#define PRRTE_CONSTRUCT_JOB_FAMILY(n) \
    ( ((n) << 16) & 0xffff0000)

#define PRRTE_CONSTRUCT_LOCAL_JOBID(local, job) \
    ( ((local) & 0xffff0000) | ((job) & 0x0000ffff) )

#define PRRTE_CONSTRUCT_JOBID(family, local) \
    PRRTE_CONSTRUCT_LOCAL_JOBID(PRRTE_CONSTRUCT_JOB_FAMILY(family), local)

/* a macro for identifying that a proc is a daemon */
#define PRRTE_JOBID_IS_DAEMON(n)  \
    !((n) & 0x0000ffff)

/* a macro for obtaining the daemon jobid */
#define PRRTE_DAEMON_JOBID(n) \
    ((n) & 0xffff0000)

/* List of names for general use */
struct prrte_namelist_t {
    prrte_list_item_t super;      /**< Allows this item to be placed on a list */
    prrte_process_name_t name;   /**< Name of a process */
};
typedef struct prrte_namelist_t prrte_namelist_t;

PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_namelist_t);

PRRTE_EXPORT int prrte_snprintf_jobid(char *jobid_string, size_t size, const prrte_jobid_t jobid);
PRRTE_EXPORT int prrte_util_convert_jobid_to_string(char **jobid_string, const prrte_jobid_t jobid);
PRRTE_EXPORT int prrte_util_convert_string_to_jobid(prrte_jobid_t *jobid, const char* jobidstring);
PRRTE_EXPORT int prrte_util_convert_vpid_to_string(char **vpid_string, const prrte_vpid_t vpid);
PRRTE_EXPORT int prrte_util_convert_string_to_vpid(prrte_vpid_t *vpid, const char* vpidstring);
PRRTE_EXPORT int prrte_util_convert_string_to_process_name(prrte_process_name_t *name,
                                             const char* name_string);
PRRTE_EXPORT int prrte_util_convert_process_name_to_string(char** name_string,
                                             const prrte_process_name_t *name);
PRRTE_EXPORT int prrte_util_create_process_name(prrte_process_name_t **name,
                                  prrte_jobid_t job,
                                  prrte_vpid_t vpid);

PRRTE_EXPORT int prrte_util_compare_name_fields(prrte_ns_cmp_bitmask_t fields,
                                  const prrte_process_name_t* name1,
                                  const prrte_process_name_t* name2);
/** This funtion returns a guaranteed unique hash value for the passed process name */
PRRTE_EXPORT uint32_t prrte_util_hash_vpid(prrte_vpid_t vpid);
PRRTE_EXPORT int prrte_util_convert_string_to_sysinfo(char **cpu_type, char **cpu_model,
                                             const char* sysinfo_string);
PRRTE_EXPORT int prrte_util_convert_sysinfo_to_string(char** sysinfo_string,
						      const char *cpu_model, const char *cpu_type);

END_C_DECLS
#endif
