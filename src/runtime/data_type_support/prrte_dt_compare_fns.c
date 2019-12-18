/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>

#include <sys/types.h>

#include "src/mca/grpcomm/grpcomm.h"

#include "src/runtime/data_type_support/prrte_dt_support.h"

int prrte_dt_compare_std_cntr(prrte_std_cntr_t *value1, prrte_std_cntr_t *value2, prrte_data_type_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/**
 * JOB
 */
int prrte_dt_compare_job(prrte_job_t *value1, prrte_job_t *value2, prrte_data_type_t type)
{
    /** check jobids */
    if (value1->jobid > value2->jobid) return PRRTE_VALUE1_GREATER;
    if (value1->jobid < value2->jobid) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/**
* NODE
 */
int prrte_dt_compare_node(prrte_node_t *value1, prrte_node_t *value2, prrte_data_type_t type)
{
    int test;

    /** check node names */
    test = strcmp(value1->name, value2->name);
    if (0 == test) return PRRTE_EQUAL;
    if (0 < test) return PRRTE_VALUE2_GREATER;

    return PRRTE_VALUE1_GREATER;
}

/**
* PROC
 */
int prrte_dt_compare_proc(prrte_proc_t *value1, prrte_proc_t *value2, prrte_data_type_t type)
{
    prrte_ns_cmp_bitmask_t mask;

    /** check vpids */
    mask = PRRTE_NS_CMP_VPID;

    return prrte_util_compare_name_fields(mask, &value1->name, &value2->name);
}

/*
 * APP CONTEXT
 */
int prrte_dt_compare_app_context(prrte_app_context_t *value1, prrte_app_context_t *value2, prrte_data_type_t type)
{
    if (value1->idx > value2->idx) return PRRTE_VALUE1_GREATER;
    if (value2->idx > value1->idx) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/*
 * EXIT CODE
 */
int prrte_dt_compare_exit_code(prrte_exit_code_t *value1,
                                    prrte_exit_code_t *value2,
                                    prrte_data_type_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/*
 * NODE STATE
 */
int prrte_dt_compare_node_state(prrte_node_state_t *value1,
                                     prrte_node_state_t *value2,
                                     prrte_node_state_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/*
 * PROC STATE
 */
int prrte_dt_compare_proc_state(prrte_proc_state_t *value1,
                                     prrte_proc_state_t *value2,
                                     prrte_proc_state_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/*
 * JOB STATE
 */
int prrte_dt_compare_job_state(prrte_job_state_t *value1,
                                    prrte_job_state_t *value2,
                                    prrte_job_state_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/*
 * JOB_MAP
 */
int prrte_dt_compare_map(prrte_job_map_t *value1, prrte_job_map_t *value2, prrte_data_type_t type)
{
    return PRRTE_EQUAL;
}

/*
 * RML tags
 */
int prrte_dt_compare_tags(prrte_rml_tag_t *value1, prrte_rml_tag_t *value2, prrte_data_type_t type)
{
    if (*value1 > *value2) {
        return PRRTE_VALUE1_GREATER;
    } else if (*value1 < *value2) {
        return PRRTE_VALUE2_GREATER;
    } else {
        return PRRTE_EQUAL;
    }
}

/* PRRTE_DAEMON_CMD */
int prrte_dt_compare_daemon_cmd(prrte_daemon_cmd_flag_t *value1, prrte_daemon_cmd_flag_t *value2, prrte_data_type_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/* PRRTE_IOF_TAG */
int prrte_dt_compare_iof_tag(prrte_iof_tag_t *value1, prrte_iof_tag_t *value2, prrte_data_type_t type)
{
    if (*value1 > *value2) return PRRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRRTE_VALUE2_GREATER;

    return PRRTE_EQUAL;
}

/* PRRTE_ATTR */
int prrte_dt_compare_attr(prrte_attribute_t *value1, prrte_attribute_t *value2, prrte_data_type_t type)
{
    if (value1->key > value2->key) {
        return PRRTE_VALUE1_GREATER;
    }
    if (value2->key > value1->key) {
        return PRRTE_VALUE2_GREATER;
    }

    return PRRTE_EQUAL;
}

/* PRRTE_SIGNATURE */
int prrte_dt_compare_sig(prrte_grpcomm_signature_t *value1, prrte_grpcomm_signature_t *value2, prrte_data_type_t type)
{
    if (value1->sz > value2->sz) {
        return PRRTE_VALUE1_GREATER;
    }
    if (value2->sz > value1->sz) {
        return PRRTE_VALUE2_GREATER;
    }
    /* same size - check contents */
    if (0 == memcmp(value1->signature, value2->signature, value1->sz*sizeof(prrte_process_name_t))) {
        return PRRTE_EQUAL;
    }
    return PRRTE_VALUE2_GREATER;
}
