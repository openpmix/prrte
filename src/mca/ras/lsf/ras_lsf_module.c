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
 * Copyright (c) 2007-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "constants.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#define SR1_PJOBS
#include <lsf/lsbatch.h>

#include "src/util/argv.h"
#include "src/util/net.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"

#include "src/mca/ras/base/ras_private.h"
#include "src/mca/ras/base/base.h"
#include "ras_lsf.h"


/*
 * Local functions
 */
static int allocate(prrte_job_t *jdata, prrte_list_t *nodes);
static int finalize(void);


/*
 * Global variable
 */
prrte_ras_base_module_t prrte_ras_lsf_module = {
    NULL,
    allocate,
    NULL,
    finalize
};


static int allocate(prrte_job_t *jdata, prrte_list_t *nodes)
{
    char **nodelist;
    prrte_node_t *node;
    int i, num_nodes;
    char *affinity_file;
    struct stat buf;
    char *ptr;
    bool directives_given = false;

    /* get the list of allocated nodes */
    if ((num_nodes = lsb_getalloc(&nodelist)) < 0) {
        prrte_show_help("help-ras-lsf.txt", "nodelist-failed", true);
        return PRRTE_ERR_NOT_AVAILABLE;
    }

    node = NULL;

    /* step through the list */
    for (i = 0; i < num_nodes; i++) {
        if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(nodelist[i]) ) {
            if (NULL != (ptr = strchr(nodelist[i], '.'))) {
                *ptr = '\0';
            }
        }

        /* is this a repeat of the current node? */
        if (NULL != node && 0 == strcmp(nodelist[i], node->name)) {
            /* it is a repeat - just bump the slot count */
            ++node->slots;
            prrte_output_verbose(10, prrte_ras_base_framework.framework_output,
                                "ras/lsf: +++ Node (%s) [slots=%d]", node->name, node->slots);
            continue;
        }

        /* not a repeat - create a node entry for it */
        node = PRRTE_NEW(prrte_node_t);
        node->name = strdup(nodelist[i]);
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = 1;
        node->state = PRRTE_NODE_STATE_UP;
        prrte_list_append(nodes, &node->super);

        prrte_output_verbose(10, prrte_ras_base_framework.framework_output,
                            "ras/lsf: New Node (%s) [slots=%d]", node->name, node->slots);
    }

    /* release the nodelist from lsf */
    prrte_argv_free(nodelist);

    /* check to see if any mapping or binding directives were given */
    if (NULL != jdata && NULL != jdata->map) {
        if ((PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) ||
            PRRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            directives_given = true;
        }
    } else if ((PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) ||
               PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy)) {
            directives_given = true;
    }

    /* check for an affinity file */
    if (!directives_given && NULL != (affinity_file = getenv("LSB_AFFINITY_HOSTFILE"))) {
        /* check to see if the file is empty - if it is,
         * then affinity wasn't actually set for this job */
        if (0 != stat(affinity_file, &buf)) {
            prrte_show_help("help-ras-lsf.txt", "affinity-file-not-found", true, affinity_file);
            return PRRTE_ERR_SILENT;
        }
        if (0 == buf.st_size) {
            /* no affinity, so just return */
            return PRRTE_SUCCESS;
        }
        /* the affinity file sequentially lists rank locations, with
         * cpusets given as physical cpu-ids. Setup the job object
         * so it knows to process this accordingly */
        if (NULL == jdata->map) {
            jdata->map = PRRTE_NEW(prrte_job_map_t);
        }
        PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_SEQ);
        jdata->map->req_mapper = strdup("seq"); // need sequential mapper
        /* tell the sequential mapper that all cpusets are to be treated as "physical" */
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_PHYSICAL_CPUIDS, true, NULL, PRRTE_BOOL);
        /* LSF provides its info as hwthreads, so set the hwthread-as-cpus flag */
        prrte_hwloc_use_hwthreads_as_cpus = true;
        /* don't override something provided by the user, but default to bind-to hwthread */
        if (!PRRTE_BINDING_POLICY_IS_SET(prrte_hwloc_binding_policy)) {
            PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_HWTHREAD);
        }
        /*
         * Do not set the hostfile attribute on each app_context since that
         * would confuse the sequential mapper when it tries to assign bindings
         * when running an MPMD job.
         * Instead just overwrite the prrte_default_hostfile so it will be
         * general for all of the app_contexts.
         */
        if( NULL != prrte_default_hostfile ) {
            free(prrte_default_hostfile);
            prrte_default_hostfile = NULL;
        }
        prrte_default_hostfile = strdup(affinity_file);
        prrte_output_verbose(10, prrte_ras_base_framework.framework_output,
                            "ras/lsf: Set default_hostfile to %s",prrte_default_hostfile);

        return PRRTE_SUCCESS;
    }

    return PRRTE_SUCCESS;
}

static int finalize(void)
{
    return PRRTE_SUCCESS;
}
