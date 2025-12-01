/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2010 Oracle and/or its affiliates.  All rights reserved
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file:
 * Resource Allocation for Grid Engine
 */
#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/ras_private.h"
#include "src/mca/ras/gridengine/ras_gridengine.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

/*
 * Local functions
 */
static int prte_ras_gridengine_allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int prte_ras_gridengine_finalize(void);
#if 0
static int get_slot_count(char* node_name, int* slot_cnt);
#endif

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_gridengine_module = {
    .allocate = prte_ras_gridengine_allocate,
    .finalize = prte_ras_gridengine_finalize
};

/**
 *  Discover available (pre-allocated) nodes. Allocate the
 *  requested number of nodes/process slots to the job.
 *
 */
static int prte_ras_gridengine_allocate(prte_job_t *jdata, pmix_list_t *nodelist)
{
    char *pe_hostfile;
    char *job_id;
    char buf[1024], *tok, *num, *queue, *arch, *ptr;
    int rc;
    FILE *fp;
    prte_node_t *node;
    bool found;
    PRTE_HIDE_UNUSED_PARAMS(jdata);

    pe_hostfile = getenv("PE_HOSTFILE");
    job_id = getenv("JOB_ID");

    if (NULL == pe_hostfile || NULL == job_id) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* show the Grid Engine's JOB_ID */
    if (prte_mca_ras_gridengine_component.show_jobid ||
        prte_mca_ras_gridengine_component.verbose != -1) {
        pmix_output(0, "ras:gridengine: JOB_ID: %s", job_id);
    }

    /* check the PE_HOSTFILE before continuing on */
    fp = fopen(pe_hostfile, "r");
    if (NULL == fp) {
        pmix_show_help("help-ras-gridengine.txt", "cannot-read-pe-hostfile", true, pe_hostfile,
                       strerror(errno));
        rc = PRTE_ERROR;
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* parse the pe_hostfile for hostname, slots, etc, then compare the
     * current node with a list of hosts in the nodelist, if the current
     * node is not found in nodelist, add it in */
    pmix_output(prte_mca_ras_gridengine_component.verbose,
                "ras:gridengine: PE_HOSTFILE: %s",
                pe_hostfile);

    while (fgets(buf, sizeof(buf), fp)) {
        ptr = strtok_r(buf, " \n", &tok);
        num = strtok_r(NULL, " \n", &tok);
        queue = strtok_r(NULL, " \n", &tok);
        arch = strtok_r(NULL, " \n", &tok);

        /* see if we already have this node */
        found = false;
        PMIX_LIST_FOREACH(node, nodelist, prte_node_t) {
            if (0 == strcmp(ptr, node->name)) {
                /* just add the slots */
                node->slots += (int) strtol(num, (char **) NULL, 10);
                found = true;
                break;
            }
        }
        if (!found) {
            /* create a new node entry */
            node = PMIX_NEW(prte_node_t);
            if (NULL == node) {
                fclose(fp);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            node->name = strdup(ptr);
            node->state = PRTE_NODE_STATE_UP;
            node->slots_inuse = 0;
            node->slots_max = 0;
            node->slots = (int) strtol(num, (char **) NULL, 10);
            pmix_output(prte_mca_ras_gridengine_component.verbose,
                        "ras:gridengine: %s: PE_HOSTFILE shows slots=%d queue=%s arch=%s",
                        node->name, node->slots, queue, arch);
            pmix_list_append(nodelist, &node->super);
        }
    } /* finished reading the $PE_HOSTFILE */

cleanup:
    if (NULL != fp) {
        fclose(fp);
    }

    /* in gridengine, if we didn't find anything, then something
     * is wrong. The user may not have indicated this was a parallel
     * job, or may not have an allocation at all. In any case, this
     * is considered an unrecoverable error and we need to report it
     */
    if (pmix_list_is_empty(nodelist)) {
        pmix_show_help("help-ras-gridengine.txt", "no-nodes-found", true);
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}


/**
 * finalize
 */
static int prte_ras_gridengine_finalize(void)
{
    /* Nothing to do */
    pmix_output(prte_mca_ras_gridengine_component.verbose,
                "ras:gridengine:finalize: success (nothing to do)");
    return PRTE_SUCCESS;
}
