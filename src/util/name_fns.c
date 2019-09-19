/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
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
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "types.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>

#include "src/util/printf.h"
#include "src/util/string_copy.h"
#include "src/threads/tsd.h"

#include "src/mca/errmgr/errmgr.h"

#include "src/util/name_fns.h"

#define PRRTE_PRINT_NAME_ARGS_MAX_SIZE   50
#define PRRTE_PRINT_NAME_ARG_NUM_BUFS    16

#define PRRTE_SCHEMA_DELIMITER_CHAR      '.'
#define PRRTE_SCHEMA_WILDCARD_CHAR       '*'
#define PRRTE_SCHEMA_WILDCARD_STRING     "*"
#define PRRTE_SCHEMA_INVALID_CHAR        '$'
#define PRRTE_SCHEMA_INVALID_STRING      "$"

/* constructor - used to initialize namelist instance */
static void prrte_namelist_construct(prrte_namelist_t* list)
{
    list->name.jobid = PRRTE_JOBID_INVALID;
    list->name.vpid = PRRTE_VPID_INVALID;
}

/* destructor - used to free any resources held by instance */
static void prrte_namelist_destructor(prrte_namelist_t* list)
{
}

/* define instance of prrte_class_t */
PRRTE_CLASS_INSTANCE(prrte_namelist_t,              /* type name */
                   prrte_list_item_t,             /* parent "class" name */
                   prrte_namelist_construct,      /* constructor */
                   prrte_namelist_destructor);    /* destructor */

static bool fns_init=false;

static prrte_tsd_key_t print_args_tsd_key;
char* prrte_print_args_null = "NULL";
typedef struct {
    char *buffers[PRRTE_PRINT_NAME_ARG_NUM_BUFS];
    int cntr;
} prrte_print_args_buffers_t;

static void
buffer_cleanup(void *value)
{
    int i;
    prrte_print_args_buffers_t *ptr;

    if (NULL != value) {
        ptr = (prrte_print_args_buffers_t*)value;
        for (i=0; i < PRRTE_PRINT_NAME_ARG_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
        free (ptr);
    }
}

static prrte_print_args_buffers_t*
get_print_name_buffer(void)
{
    prrte_print_args_buffers_t *ptr;
    int ret, i;

    if (!fns_init) {
        /* setup the print_args function */
        if (PRRTE_SUCCESS != (ret = prrte_tsd_key_create(&print_args_tsd_key, buffer_cleanup))) {
            PRRTE_ERROR_LOG(ret);
            return NULL;
        }
        fns_init = true;
    }

    ret = prrte_tsd_getspecific(print_args_tsd_key, (void**)&ptr);
    if (PRRTE_SUCCESS != ret) return NULL;

    if (NULL == ptr) {
        ptr = (prrte_print_args_buffers_t*)malloc(sizeof(prrte_print_args_buffers_t));
        for (i=0; i < PRRTE_PRINT_NAME_ARG_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((PRRTE_PRINT_NAME_ARGS_MAX_SIZE+1) * sizeof(char));
        }
        ptr->cntr = 0;
        ret = prrte_tsd_setspecific(print_args_tsd_key, (void*)ptr);
    }

    return (prrte_print_args_buffers_t*) ptr;
}

char* prrte_util_print_name_args(const prrte_process_name_t *name)
{
    prrte_print_args_buffers_t *ptr;
    char *job, *vpid;

    /* protect against NULL names */
    if (NULL == name) {
        /* get the next buffer */
        ptr = get_print_name_buffer();
        if (NULL == ptr) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return prrte_print_args_null;
        }
        /* cycle around the ring */
        if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
            ptr->cntr = 0;
        }
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "[NO-NAME]");
        return ptr->buffers[ptr->cntr-1];
    }

    /* get the jobid, vpid strings first - this will protect us from
     * stepping on each other's buffer. This also guarantees
     * that the print_args function has been initialized, so
     * we don't need to duplicate that here
     */
    job = prrte_util_print_jobids(name->jobid);
    vpid = prrte_util_print_vpids(name->vpid);

    /* get the next buffer */
    ptr = get_print_name_buffer();

    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_print_args_null;
    }

    /* cycle around the ring */
    if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    snprintf(ptr->buffers[ptr->cntr++],
             PRRTE_PRINT_NAME_ARGS_MAX_SIZE,
             "[%s,%s]", job, vpid);

    return ptr->buffers[ptr->cntr-1];
}

char* prrte_util_print_jobids(const prrte_jobid_t job)
{
    prrte_print_args_buffers_t *ptr;
    unsigned long tmp1, tmp2;

    ptr = get_print_name_buffer();

    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_print_args_null;
    }

    /* cycle around the ring */
    if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    if (PRRTE_JOBID_INVALID == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "[INVALID]");
    } else if (PRRTE_JOBID_WILDCARD == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "[WILDCARD]");
    } else {
        tmp1 = PRRTE_JOB_FAMILY((unsigned long)job);
        tmp2 = PRRTE_LOCAL_JOBID((unsigned long)job);
        snprintf(ptr->buffers[ptr->cntr++],
                 PRRTE_PRINT_NAME_ARGS_MAX_SIZE,
                 "[%lu,%lu]", tmp1, tmp2);
    }
    return ptr->buffers[ptr->cntr-1];
}

char* prrte_util_print_job_family(const prrte_jobid_t job)
{
    prrte_print_args_buffers_t *ptr;
    unsigned long tmp1;

    ptr = get_print_name_buffer();

    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_print_args_null;
    }

    /* cycle around the ring */
    if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    if (PRRTE_JOBID_INVALID == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "INVALID");
    } else if (PRRTE_JOBID_WILDCARD == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "WILDCARD");
    } else {
        tmp1 = PRRTE_JOB_FAMILY((unsigned long)job);
        snprintf(ptr->buffers[ptr->cntr++],
                 PRRTE_PRINT_NAME_ARGS_MAX_SIZE,
                 "%lu", tmp1);
    }
    return ptr->buffers[ptr->cntr-1];
}

char* prrte_util_print_local_jobid(const prrte_jobid_t job)
{
    prrte_print_args_buffers_t *ptr;
    unsigned long tmp1;

    ptr = get_print_name_buffer();

    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_print_args_null;
    }

    /* cycle around the ring */
    if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    if (PRRTE_JOBID_INVALID == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "INVALID");
    } else if (PRRTE_JOBID_WILDCARD == job) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "WILDCARD");
    } else {
        tmp1 = (unsigned long)job & 0x0000ffff;
        snprintf(ptr->buffers[ptr->cntr++],
                 PRRTE_PRINT_NAME_ARGS_MAX_SIZE,
                 "%lu", tmp1);
    }
    return ptr->buffers[ptr->cntr-1];
}

char* prrte_util_print_vpids(const prrte_vpid_t vpid)
{
    prrte_print_args_buffers_t *ptr;

    ptr = get_print_name_buffer();

    if (NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return prrte_print_args_null;
    }

    /* cycle around the ring */
    if (PRRTE_PRINT_NAME_ARG_NUM_BUFS == ptr->cntr) {
        ptr->cntr = 0;
    }

    if (PRRTE_VPID_INVALID == vpid) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "INVALID");
    } else if (PRRTE_VPID_WILDCARD == vpid) {
        snprintf(ptr->buffers[ptr->cntr++], PRRTE_PRINT_NAME_ARGS_MAX_SIZE, "WILDCARD");
    } else {
        snprintf(ptr->buffers[ptr->cntr++],
                 PRRTE_PRINT_NAME_ARGS_MAX_SIZE,
                 "%ld", (long)vpid);
    }
    return ptr->buffers[ptr->cntr-1];
}



/***   STRING FUNCTIONS   ***/

int prrte_snprintf_jobid(char *jobid_string, size_t size, const prrte_jobid_t jobid)
{
    int rc;

    /* check for wildcard value - handle appropriately */
    if (PRRTE_JOBID_WILDCARD == jobid) {
        (void)prrte_string_copy(jobid_string, PRRTE_SCHEMA_WILDCARD_STRING, size);
    } else {
        rc = snprintf(jobid_string, size, "%ld", (long) jobid);
        if (0 > rc) {
            return PRRTE_ERROR;
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_util_convert_jobid_to_string(char **jobid_string, const prrte_jobid_t jobid)
{
    int rc;
    char str[256];

    rc = prrte_snprintf_jobid(str, 255, jobid);
    if (0 > rc) {
        *jobid_string = NULL;
        return rc;
    }
    *jobid_string = strdup(str);
    if (NULL == *jobid_string) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    return PRRTE_SUCCESS;
}


int prrte_util_convert_string_to_jobid(prrte_jobid_t *jobid, const char* jobidstring)
{
    if (NULL == jobidstring) {  /* got an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        *jobid = PRRTE_JOBID_INVALID;
        return PRRTE_ERR_BAD_PARAM;
    }

    /** check for wildcard character - handle appropriately */
    if (0 == strcmp(PRRTE_SCHEMA_WILDCARD_STRING, jobidstring)) {
        *jobid = PRRTE_JOBID_WILDCARD;
        return PRRTE_SUCCESS;
    }

    /* check for invalid value */
    if (0 == strcmp(PRRTE_SCHEMA_INVALID_STRING, jobidstring)) {
        *jobid = PRRTE_JOBID_INVALID;
        return PRRTE_SUCCESS;
    }

    *jobid = strtoul(jobidstring, NULL, 10);

    return PRRTE_SUCCESS;
}

int prrte_util_convert_vpid_to_string(char **vpid_string, const prrte_vpid_t vpid)
{
    /* check for wildcard value - handle appropriately */
    if (PRRTE_VPID_WILDCARD == vpid) {
        *vpid_string = strdup(PRRTE_SCHEMA_WILDCARD_STRING);
        return PRRTE_SUCCESS;
    }

    /* check for invalid value - handle appropriately */
    if (PRRTE_VPID_INVALID == vpid) {
        *vpid_string = strdup(PRRTE_SCHEMA_INVALID_STRING);
        return PRRTE_SUCCESS;
    }

    if (0 > prrte_asprintf(vpid_string, "%ld", (long) vpid)) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    return PRRTE_SUCCESS;
}


int prrte_util_convert_string_to_vpid(prrte_vpid_t *vpid, const char* vpidstring)
{
    if (NULL == vpidstring) {  /* got an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        *vpid = PRRTE_VPID_INVALID;
        return PRRTE_ERR_BAD_PARAM;
    }

    /** check for wildcard character - handle appropriately */
    if (0 == strcmp(PRRTE_SCHEMA_WILDCARD_STRING, vpidstring)) {
        *vpid = PRRTE_VPID_WILDCARD;
        return PRRTE_SUCCESS;
    }

    /* check for invalid value */
    if (0 == strcmp(PRRTE_SCHEMA_INVALID_STRING, vpidstring)) {
        *vpid = PRRTE_VPID_INVALID;
        return PRRTE_SUCCESS;
    }

    *vpid = strtol(vpidstring, NULL, 10);

    return PRRTE_SUCCESS;
}

int prrte_util_convert_string_to_process_name(prrte_process_name_t *name,
                                             const char* name_string)
{
    char *temp, *token;
    prrte_jobid_t job;
    prrte_vpid_t vpid;
    int return_code=PRRTE_SUCCESS;

    /* set default */
    name->jobid = PRRTE_JOBID_INVALID;
    name->vpid = PRRTE_VPID_INVALID;

    /* check for NULL string - error */
    if (NULL == name_string) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    temp = strdup(name_string);  /** copy input string as the strtok process is destructive */
    token = strchr(temp, PRRTE_SCHEMA_DELIMITER_CHAR); /** get first field -> jobid */

    /* check for error */
    if (NULL == token) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        free(temp);
        return PRRTE_ERR_BAD_PARAM;
    }
    *token = '\0';
    token++;

    /* check for WILDCARD character - assign
     * value accordingly, if found
     */
    if (0 == strcmp(temp, PRRTE_SCHEMA_WILDCARD_STRING)) {
        job = PRRTE_JOBID_WILDCARD;
    } else if (0 == strcmp(temp, PRRTE_SCHEMA_INVALID_STRING)) {
        job = PRRTE_JOBID_INVALID;
    } else {
        job = strtoul(temp, NULL, 10);
    }

    /* check for WILDCARD character - assign
     * value accordingly, if found
     */
    if (0 == strcmp(token, PRRTE_SCHEMA_WILDCARD_STRING)) {
        vpid = PRRTE_VPID_WILDCARD;
    } else if (0 == strcmp(token, PRRTE_SCHEMA_INVALID_STRING)) {
        vpid = PRRTE_VPID_INVALID;
    } else {
        vpid = strtoul(token, NULL, 10);
    }

    name->jobid = job;
    name->vpid = vpid;

    free(temp);

    return return_code;
}

int prrte_util_convert_process_name_to_string(char **name_string,
                                             const prrte_process_name_t* name)
{
    char *tmp, *tmp2;

    if (NULL == name) { /* got an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* check for wildcard and invalid values - where encountered, insert the
     * corresponding string so we can correctly parse the name string when
     * it is passed back to us later
     */
    if (PRRTE_JOBID_WILDCARD == name->jobid) {
        prrte_asprintf(&tmp, "%s", PRRTE_SCHEMA_WILDCARD_STRING);
    } else if (PRRTE_JOBID_INVALID == name->jobid) {
        prrte_asprintf(&tmp, "%s", PRRTE_SCHEMA_INVALID_STRING);
    } else {
        prrte_asprintf(&tmp, "%lu", (unsigned long)name->jobid);
    }

    if (PRRTE_VPID_WILDCARD == name->vpid) {
        prrte_asprintf(&tmp2, "%s%c%s", tmp, PRRTE_SCHEMA_DELIMITER_CHAR, PRRTE_SCHEMA_WILDCARD_STRING);
    } else if (PRRTE_VPID_INVALID == name->vpid) {
        prrte_asprintf(&tmp2, "%s%c%s", tmp, PRRTE_SCHEMA_DELIMITER_CHAR, PRRTE_SCHEMA_INVALID_STRING);
    } else {
        prrte_asprintf(&tmp2, "%s%c%lu", tmp, PRRTE_SCHEMA_DELIMITER_CHAR, (unsigned long)name->vpid);
    }

    prrte_asprintf(name_string, "%s", tmp2);

    free(tmp);
    free(tmp2);

    return PRRTE_SUCCESS;
}


/****    CREATE PROCESS NAME    ****/
int prrte_util_create_process_name(prrte_process_name_t **name,
                                  prrte_jobid_t job,
                                  prrte_vpid_t vpid
                                  )
{
    *name = NULL;

    *name = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    if (NULL == *name) { /* got an error */
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    (*name)->jobid = job;
    (*name)->vpid = vpid;

    return PRRTE_SUCCESS;
}

/****    COMPARE NAME FIELDS     ****/
int prrte_util_compare_name_fields(prrte_ns_cmp_bitmask_t fields,
                                  const prrte_process_name_t* name1,
                                  const prrte_process_name_t* name2)
{
    /* handle the NULL pointer case */
    if (NULL == name1 && NULL == name2) {
        return PRRTE_EQUAL;
    } else if (NULL == name1) {
        return PRRTE_VALUE2_GREATER;
    } else if (NULL == name2) {
        return PRRTE_VALUE1_GREATER;
    }

    /* in this comparison function, we check for exact equalities.
     * In the case of wildcards, we check to ensure that the fields
     * actually match those values - thus, a "wildcard" in this
     * function does not actually stand for a wildcard value, but
     * rather a specific value - UNLESS the CMP_WILD bitmask value
     * is set
     */

    /* check job id */
    if (PRRTE_NS_CMP_JOBID & fields) {
        if (PRRTE_NS_CMP_WILD & fields &&
            (PRRTE_JOBID_WILDCARD == name1->jobid ||
             PRRTE_JOBID_WILDCARD == name2->jobid)) {
            goto check_vpid;
        }
        if (name1->jobid < name2->jobid) {
            return PRRTE_VALUE2_GREATER;
        } else if (name1->jobid > name2->jobid) {
            return PRRTE_VALUE1_GREATER;
        }
    }

    /* get here if jobid's are equal, or not being checked
     * now check vpid
     */
 check_vpid:
    if (PRRTE_NS_CMP_VPID & fields) {
        if (PRRTE_NS_CMP_WILD & fields &&
            (PRRTE_VPID_WILDCARD == name1->vpid ||
             PRRTE_VPID_WILDCARD == name2->vpid)) {
            return PRRTE_EQUAL;
        }
        if (name1->vpid < name2->vpid) {
            return PRRTE_VALUE2_GREATER;
        } else if (name1->vpid > name2->vpid) {
            return PRRTE_VALUE1_GREATER;
        }
    }

    /* only way to get here is if all fields are being checked and are equal,
    * or jobid not checked, but vpid equal,
    * only vpid being checked, and equal
    * return that fact
    */
    return PRRTE_EQUAL;
}

/* hash a vpid based on Robert Jenkin's algorithm - note
 * that the precise values of the constants in the algo are
 * irrelevant.
 */
uint32_t  prrte_util_hash_vpid(prrte_vpid_t vpid) {
    uint32_t hash;

    hash = vpid;
    hash = (hash + 0x7ed55d16) + (hash<<12);
    hash = (hash ^ 0xc761c23c) ^ (hash>>19);
    hash = (hash + 0x165667b1) + (hash<<5);
    hash = (hash + 0xd3a2646c) ^ (hash<<9);
    hash = (hash + 0xfd7046c5) + (hash<<3);
    hash = (hash ^ 0xb55a4f09) ^ (hash>>16);
    return hash;
}

/* sysinfo conversion to and from string */
int prrte_util_convert_string_to_sysinfo(char **cpu_type, char **cpu_model,
                                        const char* sysinfo_string)
{
    char *temp, *token;
    int return_code=PRRTE_SUCCESS;

    /* check for NULL string - error */
    if (NULL == sysinfo_string) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    temp = strdup(sysinfo_string);  /** copy input string as the strtok process is destructive */
    token = strchr(temp, PRRTE_SCHEMA_DELIMITER_CHAR); /** get first field -> cpu_type */

    /* check for error */
    if (NULL == token) {
        free(temp);
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    *token = '\0';
    token++;

    /* If type is a valid string get the value otherwise leave cpu_type untouched.
     */
    if (0 != strcmp(temp, PRRTE_SCHEMA_INVALID_STRING)) {
        *cpu_type = strdup(temp);
    }

    /* If type is a valid string get the value otherwise leave cpu_type untouched.
     */
    if (0 != strcmp(token, PRRTE_SCHEMA_INVALID_STRING)) {
        *cpu_model = strdup(token);
    }

    free(temp);

    return return_code;
}

int prrte_util_convert_sysinfo_to_string(char **sysinfo_string,
                                        const char *cpu_type, const char *cpu_model)
{
    char *tmp;

    /* check for no sysinfo values (like empty cpu_type) - where encountered, insert the
     * invalid string so we can correctly parse the name string when
     * it is passed back to us later
     */
    if (NULL == cpu_type) {
        prrte_asprintf(&tmp, "%s", PRRTE_SCHEMA_INVALID_STRING);
    } else {
        prrte_asprintf(&tmp, "%s", cpu_type);
    }

    if (NULL == cpu_model) {
        prrte_asprintf(sysinfo_string, "%s%c%s", tmp, PRRTE_SCHEMA_DELIMITER_CHAR, PRRTE_SCHEMA_INVALID_STRING);
    } else {
        prrte_asprintf(sysinfo_string, "%s%c%s", tmp, PRRTE_SCHEMA_DELIMITER_CHAR, cpu_model);
    }
    free(tmp);
    return PRRTE_SUCCESS;
}

char *prrte_pretty_print_timing(int64_t secs, int64_t usecs)
{
    unsigned long minutes, seconds;
    float fsecs;
    char *timestring;

    seconds = secs + (usecs / 1000000l);
    minutes = seconds / 60l;
    seconds = seconds % 60l;
    if (0 == minutes && 0 == seconds) {
        fsecs = ((float)(secs)*1000000.0 + (float)usecs) / 1000.0;
        prrte_asprintf(&timestring, "%8.2f millisecs", fsecs);
    } else {
        prrte_asprintf(&timestring, "%3lu:%02lu min:sec", minutes, seconds);
    }

    return timestring;
}
