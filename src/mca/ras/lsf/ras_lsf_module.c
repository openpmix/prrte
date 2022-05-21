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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SR1_PJOBS
#include <lsf/lsbatch.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_net.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"

#include "ras_lsf.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/ras/base/ras_private.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_lsf_module = {NULL, allocate, NULL, finalize};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    char **nodelist;
    prte_node_t *node;
    int i, num_nodes;
    char *affinity_file;
    struct stat buf;
    char *ptr;
    bool directives_given = false;

    /* get the list of allocated nodes */
    if ((num_nodes = lsb_getalloc(&nodelist)) < 0) {
        pmix_show_help("help-ras-lsf.txt", "nodelist-failed", true);
        return PRTE_ERR_NOT_AVAILABLE;
    }

    node = NULL;

    /* step through the list */
    for (i = 0; i < num_nodes; i++) {
        if (!prte_keep_fqdn_hostnames && !pmix_net_isaddr(nodelist[i])) {
            if (NULL != (ptr = strchr(nodelist[i], '.'))) {
                *ptr = '\0';
            }
        }

        /* is this a repeat of the current node? */
        if (NULL != node && 0 == strcmp(nodelist[i], node->name)) {
            /* it is a repeat - just bump the slot count */
            ++node->slots;
            prte_output_verbose(10, prte_ras_base_framework.framework_output,
                                "ras/lsf: +++ Node (%s) [slots=%d]", node->name, node->slots);
            continue;
        }

        /* not a repeat - create a node entry for it */
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(nodelist[i]);
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = 1;
        node->state = PRTE_NODE_STATE_UP;
        pmix_list_append(nodes, &node->super);

        prte_output_verbose(10, prte_ras_base_framework.framework_output,
                            "ras/lsf: New Node (%s) [slots=%d]", node->name, node->slots);
    }

    /* release the nodelist from lsf */
    pmix_argv_free(nodelist);

    /* check to see if any mapping or binding directives were given */
    if (NULL != jdata && NULL != jdata->map) {
        if ((PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))
            || PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            directives_given = true;
        }
    } else if ((PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))
               || PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
        directives_given = true;
    }

    /* check for an affinity file */
    if (!prte_ras_lsf_skip_affinity_file && !directives_given
        && NULL != (affinity_file = getenv("LSB_AFFINITY_HOSTFILE"))) {
        /* check to see if the file is empty - if it is,
         * then affinity wasn't actually set for this job */
        if (0 != stat(affinity_file, &buf)) {
            pmix_show_help("help-ras-lsf.txt", "affinity-file-not-found", true, affinity_file);
            return PRTE_ERR_SILENT;
        }
        if (0 == buf.st_size) {
            /* no affinity, so just return */
            return PRTE_SUCCESS;
        }
#if 1
        // Phsical CPU IDs are no longer supported. See the Issue below:
        //   https://github.com/openpmix/prrte/issues/791
        // Until that is resolved throw an error if we detect that the user is
        // trying to use LSF level affinity options.
        if (NULL != affinity_file) { // Always true
            pmix_show_help("help-ras-lsf.txt", "affinity-file-found-not-used", true, affinity_file,
                           "Physical CPU ID mapping is not supported");
            return PRTE_ERR_SILENT;
        }
#else
        /* the affinity file sequentially lists rank locations, with
         * cpusets given as physical cpu-ids. Setup the job object
         * so it knows to process this accordingly */
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_SEQ);
        jdata->map->req_mapper = strdup("seq"); // need sequential mapper
        /* tell the sequential mapper that all cpusets are to be treated as "physical" */
        // TODO - Physical CPUs are no longer supported by PRRTE. Need a fix the following
        //        attribute is no longer valid.
        // prte_set_attribute(&jdata->attributes, PRTE_JOB_PHYSICAL_CPUIDS, true, NULL, PMIX_BOOL);
        /* LSF provides its info as hwthreads, so set the hwthread-as-cpus flag */
        prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, true, NULL, PMIX_BOOL);
        /* don't override something provided by the user, but default to bind-to hwthread */
        if (!PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
            PRTE_SET_BINDING_POLICY(prte_hwloc_default_binding_policy, PRTE_BIND_TO_HWTHREAD);
        }
        /*
         * Do not set the hostfile attribute on each app_context since that
         * would confuse the sequential mapper when it tries to assign bindings
         * when running an MPMD job.
         * Instead just overwrite the prte_default_hostfile so it will be
         * general for all of the app_contexts.
         */
        if (NULL != prte_default_hostfile) {
            free(prte_default_hostfile);
            prte_default_hostfile = NULL;
        }
        prte_default_hostfile = strdup(affinity_file);
        prte_output_verbose(10, prte_ras_base_framework.framework_output,
                            "ras/lsf: Set default_hostfile to %s", prte_default_hostfile);
#endif
        return PRTE_SUCCESS;
    }

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    return PRTE_SUCCESS;
}
