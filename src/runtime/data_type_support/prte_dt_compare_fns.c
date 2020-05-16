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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>

#include <sys/types.h>

#include "src/mca/grpcomm/grpcomm.h"

#include "src/runtime/data_type_support/prte_dt_support.h"

int prte_dt_compare_std_cntr(prte_std_cntr_t *value1, prte_std_cntr_t *value2, prte_data_type_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/**
 * JOB
 */
int prte_dt_compare_job(prte_job_t *value1, prte_job_t *value2, prte_data_type_t type)
{
    /** check jobids */
    if (value1->jobid > value2->jobid) return PRTE_VALUE1_GREATER;
    if (value1->jobid < value2->jobid) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/**
* NODE
 */
int prte_dt_compare_node(prte_node_t *value1, prte_node_t *value2, prte_data_type_t type)
{
    int test;

    /** check node names */
    test = strcmp(value1->name, value2->name);
    if (0 == test) return PRTE_EQUAL;
    if (0 < test) return PRTE_VALUE2_GREATER;

    return PRTE_VALUE1_GREATER;
}

/**
* PROC
 */
int prte_dt_compare_proc(prte_proc_t *value1, prte_proc_t *value2, prte_data_type_t type)
{
    prte_ns_cmp_bitmask_t mask;

    /** check vpids */
    mask = PRTE_NS_CMP_VPID;

    return prte_util_compare_name_fields(mask, &value1->name, &value2->name);
}

/*
 * APP CONTEXT
 */
int prte_dt_compare_app_context(prte_app_context_t *value1, prte_app_context_t *value2, prte_data_type_t type)
{
    if (value1->idx > value2->idx) return PRTE_VALUE1_GREATER;
    if (value2->idx > value1->idx) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/*
 * EXIT CODE
 */
int prte_dt_compare_exit_code(prte_exit_code_t *value1,
                                    prte_exit_code_t *value2,
                                    prte_data_type_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/*
 * NODE STATE
 */
int prte_dt_compare_node_state(prte_node_state_t *value1,
                                     prte_node_state_t *value2,
                                     prte_node_state_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/*
 * PROC STATE
 */
int prte_dt_compare_proc_state(prte_proc_state_t *value1,
                                     prte_proc_state_t *value2,
                                     prte_proc_state_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/*
 * JOB STATE
 */
int prte_dt_compare_job_state(prte_job_state_t *value1,
                                    prte_job_state_t *value2,
                                    prte_job_state_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/*
 * JOB_MAP
 */
int prte_dt_compare_map(prte_job_map_t *value1, prte_job_map_t *value2, prte_data_type_t type)
{
    return PRTE_EQUAL;
}

/*
 * RML tags
 */
int prte_dt_compare_tags(prte_rml_tag_t *value1, prte_rml_tag_t *value2, prte_data_type_t type)
{
    if (*value1 > *value2) {
        return PRTE_VALUE1_GREATER;
    } else if (*value1 < *value2) {
        return PRTE_VALUE2_GREATER;
    } else {
        return PRTE_EQUAL;
    }
}

/* PRTE_DAEMON_CMD */
int prte_dt_compare_daemon_cmd(prte_daemon_cmd_flag_t *value1, prte_daemon_cmd_flag_t *value2, prte_data_type_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/* PRTE_IOF_TAG */
int prte_dt_compare_iof_tag(prte_iof_tag_t *value1, prte_iof_tag_t *value2, prte_data_type_t type)
{
    if (*value1 > *value2) return PRTE_VALUE1_GREATER;

    if (*value2 > *value1) return PRTE_VALUE2_GREATER;

    return PRTE_EQUAL;
}

/* PRTE_ATTR */
int prte_dt_compare_attr(prte_attribute_t *value1, prte_attribute_t *value2, prte_data_type_t type)
{
    if (value1->key > value2->key) {
        return PRTE_VALUE1_GREATER;
    }
    if (value2->key > value1->key) {
        return PRTE_VALUE2_GREATER;
    }

    return PRTE_EQUAL;
}

/* PRTE_SIGNATURE */
int prte_dt_compare_sig(prte_grpcomm_signature_t *value1, prte_grpcomm_signature_t *value2, prte_data_type_t type)
{
    if (value1->sz > value2->sz) {
        return PRTE_VALUE1_GREATER;
    }
    if (value2->sz > value1->sz) {
        return PRTE_VALUE2_GREATER;
    }
    /* same size - check contents */
    if (0 == memcmp(value1->signature, value2->signature, value1->sz*sizeof(prte_process_name_t))) {
        return PRTE_EQUAL;
    }
    return PRTE_VALUE2_GREATER;
}
