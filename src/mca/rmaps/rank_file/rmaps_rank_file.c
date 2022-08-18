/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/class/pmix_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/proc_info.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"

static int prte_rmaps_rf_map(prte_job_t *jdata,
                             prte_rmaps_options_t *options);

prte_rmaps_base_module_t prte_rmaps_rank_file_module = {
    .map_job = prte_rmaps_rf_map
};

static int prte_rmaps_rank_file_parse(const char *);
static char *prte_rmaps_rank_file_parse_string_or_int(void);
char *prte_rmaps_rank_file_slot_list = NULL;

/*
 * Local variable
 */
static pmix_pointer_array_t rankmap;
static int num_ranks = 0;

/*
 * Create a rank_file  mapping for the job.
 */
static int prte_rmaps_rf_map(prte_job_t *jdata,
                             prte_rmaps_options_t *options)
{
    prte_job_map_t *map;
    prte_app_context_t *app = NULL;
    int32_t i, k;
    pmix_list_t node_list;
    pmix_list_item_t *item;
    prte_node_t *node, *nd, *root_node;
    pmix_rank_t rank, vpid_start;
    int32_t num_slots;
    prte_rmaps_rank_file_map_t *rfmap;
    int32_t relative_index, tmp_cnt;
    int rc;
    prte_proc_t *proc;
    prte_mca_base_component_t *c = &prte_rmaps_rank_file_component.super.base_version;
    char *slots;
    bool initial_map = true;
    char *rankfile = NULL;
    prte_binding_policy_t bind;

    /* only handle initial launch of rf job */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s being restarted - rank_file cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper
        && 0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s not using rank_file mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRTE_MAPPING_BYUSER != PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* NOT FOR US */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s not using rankfile policy",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (options->ordered) {
        /* NOT FOR US */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s binding order requested - rank_file cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &rankfile, PMIX_STRING)
        || NULL == rankfile) {
        /* we cannot do it */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s no rankfile specified",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_BAD_PARAM;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_file: mapping job %s", PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* convenience def */
    map = jdata->map;

    /* setup the node list */
    PMIX_CONSTRUCT(&node_list, pmix_list_t);

    /* pickup the first app - there must be at least one */
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    if (NULL == app) {
        rc = PRTE_ERR_SILENT;
        goto error;
    }

    /* SANITY CHECKS */

    /* if the number of processes wasn't specified, then we know there can be only
     * one app_context allowed in the launch, and that we are to launch it across
     * all available slots.
     */
    if (0 == app->num_procs && 1 < jdata->num_apps) {
        pmix_show_help("help-rmaps_rank_file.txt", "prte-rmaps-rf:multi-apps-and-zero-np", true,
                       jdata->num_apps, NULL);
        rc = PRTE_ERR_SILENT;
        goto error;
    }

    /* END SANITY CHECKS */

    /* start at the beginning... */
    vpid_start = 0;
    jdata->num_procs = 0;
    PMIX_CONSTRUCT(&rankmap, pmix_pointer_array_t);

    /* parse the rankfile, storing its results in the rankmap */
    if (PRTE_SUCCESS != (rc = prte_rmaps_rank_file_parse(rankfile))) {
        rc = PRTE_ERR_SILENT;
        goto error;
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL == app) {
            continue;
        }

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        rc = prte_rmaps_base_get_target_nodes(&node_list, &num_slots, jdata, app,
                                              options->map, initial_map, false);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* we already checked for sanity, so it's okay to just do here */
        if (0 == app->num_procs) {
            /* set the number of procs to the number of entries in that rankfile */
            app->num_procs = num_ranks;
        }
        if (0 == app->num_procs) {
            pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
            rc = PRTE_ERR_SILENT;
            goto error;
        }
        for (k = 0; k < app->num_procs; k++) {
            rank = vpid_start + k;
            /* get the rankfile entry for this rank */
            rfmap = (prte_rmaps_rank_file_map_t *) pmix_pointer_array_get_item(&rankmap, rank);
            if (NULL == rfmap) {
                /* if this job was given a slot-list, then use it */
                if (NULL != options->cpuset) {
                    slots = options->cpuset;
                } else if (NULL != prte_hwloc_default_cpu_list) {
                    /* if we were give a default slot-list, then use it */
                    slots = prte_hwloc_default_cpu_list;
                } else {
                    /* all ranks must be specified */
                    pmix_show_help("help-rmaps_rank_file.txt", "missing-rank", true, rank,
                                   rankfile);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                /* take the next node off of the available list */
                node = NULL;
                PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                {
                    /* if adding one to this node would oversubscribe it, then try
                     * the next one */
                    if (nd->slots <= (int) nd->num_procs) {
                        continue;
                    }
                    /* take this one */
                    node = nd;
                    break;
                }
                if (NULL == node) {
                    /* all would be oversubscribed, so take the least loaded one */
                    k = (int32_t) UINT32_MAX;
                    PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                    {
                        if (nd->num_procs < (pmix_rank_t) k) {
                            k = nd->num_procs;
                            node = nd;
                        }
                    }
                }
                /* if we still have nothing, then something is very wrong */
                if (NULL == node) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto error;
                }
            } else {
                if (0 == strlen(rfmap->slot_list)) {
                    /* rank was specified but no slot list given - that's an error */
                    pmix_show_help("help-rmaps_rank_file.txt", "no-slot-list", true, rank,
                                   rfmap->node_name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                slots = rfmap->slot_list;
                /* find the node where this proc was assigned */
                node = NULL;
                PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                {
                    if (NULL != rfmap->node_name && 0 == strcmp(nd->name, rfmap->node_name)) {
                        node = nd;
                        break;
                    } else if (NULL != rfmap->node_name
                               && (('+' == rfmap->node_name[0])
                                   && (('n' == rfmap->node_name[1])
                                       || ('N' == rfmap->node_name[1])))) {

                        relative_index = atoi(strtok(rfmap->node_name, "+n"));
                        if (relative_index >= (int) pmix_list_get_size(&node_list)
                            || (0 > relative_index)) {
                            pmix_show_help("help-rmaps_rank_file.txt", "bad-index", true,
                                           rfmap->node_name);
                            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        root_node = (prte_node_t *) pmix_list_get_first(&node_list);
                        for (tmp_cnt = 0; tmp_cnt < relative_index; tmp_cnt++) {
                            root_node = (prte_node_t *) pmix_list_get_next(root_node);
                        }
                        node = root_node;
                        break;
                    }
                }
            }
            if (NULL == node) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-host", true, rfmap->node_name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            if (!options->donotlaunch) {
                rc = prte_rmaps_base_check_support(jdata, node, options);
                if (PRTE_SUCCESS != rc) {
                    return rc;
                }
            }
            prte_rmaps_base_get_cpuset(jdata, node, options);
            if (!prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, options)) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-host", true, rfmap->node_name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            /* check if we are oversubscribed */
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_SUCCESS != rc) {
                goto error;
            }
          /* set the vpid */
            proc->name.rank = rank;
            /* insert the proc into the proper place */
            rc = pmix_pointer_array_set_item(jdata->procs, proc->name.rank, proc);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                goto error;
            }
            jdata->num_procs++;
        }
        /* update the starting point */
        vpid_start += app->num_procs;
        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        PMIX_LIST_DESTRUCT(&node_list);
        PMIX_CONSTRUCT(&node_list, pmix_list_t);
    }
    PMIX_LIST_DESTRUCT(&node_list);

    /* cleanup the rankmap */
    for (i = 0; i < rankmap.size; i++) {
        if (NULL != (rfmap = pmix_pointer_array_get_item(&rankmap, i))) {
            PMIX_RELEASE(rfmap);
        }
    }
    PMIX_DESTRUCT(&rankmap);
    if (NULL != rankfile) {
        free(rankfile);
    }
    /* compute local/app ranks */
    rc = prte_rmaps_base_compute_vpids(jdata, options);
    return rc;

error:
    PMIX_LIST_DESTRUCT(&node_list);
    if (NULL != rankfile) {
        free(rankfile);
    }

    return rc;
}

static int prte_rmaps_rank_file_parse(const char *rankfile)
{
    int token;
    int rc = PRTE_SUCCESS;
    int cnt;
    char *node_name = NULL;
    char **argv;
    char buff[64];
    char *value;
    int rank = -1;
    int i;
    prte_node_t *hnp_node;
    prte_rmaps_rank_file_map_t *rfmap = NULL;
    pmix_pointer_array_t *assigned_ranks_array;
    char tmp_rank_assignment[64];

    /* keep track of rank assignments */
    assigned_ranks_array = PMIX_NEW(pmix_pointer_array_t);

    /* get the hnp node's info */
    hnp_node = (prte_node_t *) (prte_node_pool->addr[0]);

    prte_rmaps_rank_file_done = false;
    prte_rmaps_rank_file_in = fopen(rankfile, "r");

    if (NULL == prte_rmaps_rank_file_in) {
        pmix_show_help("help-rmaps_rank_file.txt", "no-rankfile", true,
                       prte_tool_basename, rankfile, prte_tool_basename);
        rc = PRTE_ERR_NOT_FOUND;
        goto unlock;
    }

    while (!prte_rmaps_rank_file_done) {
        token = prte_rmaps_rank_file_lex();

        switch (token) {
        case PRTE_RANKFILE_ERROR:
            pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_QUOTED_STRING:
            pmix_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true,
                           "QUOTED_STRING", rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_NEWLINE:
            rank = -1;
            if (NULL != node_name) {
                free(node_name);
            }
            node_name = NULL;
            rfmap = NULL;
            break;
        case PRTE_RANKFILE_RANK:
            token = prte_rmaps_rank_file_lex();
            if (PRTE_RANKFILE_INT == token) {
                rank = prte_rmaps_rank_file_value.ival;
                rfmap = PMIX_NEW(prte_rmaps_rank_file_map_t);
                pmix_pointer_array_set_item(&rankmap, rank, rfmap);
                num_ranks++; // keep track of number of provided ranks
            } else {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            break;
        case PRTE_RANKFILE_USERNAME:
            pmix_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true, "USERNAME",
                           rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_EQUAL:
            if (rank < 0) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            token = prte_rmaps_rank_file_lex();
            switch (token) {
            case PRTE_RANKFILE_HOSTNAME:
            case PRTE_RANKFILE_IPV4:
            case PRTE_RANKFILE_IPV6:
            case PRTE_RANKFILE_STRING:
            case PRTE_RANKFILE_INT:
            case PRTE_RANKFILE_RELATIVE:
                if (PRTE_RANKFILE_INT == token) {
                    sprintf(buff, "%d", prte_rmaps_rank_file_value.ival);
                    value = buff;
                } else {
                    value = prte_rmaps_rank_file_value.sval;
                }
                argv = pmix_argv_split(value, '@');
                cnt = pmix_argv_count(argv);
                if (NULL != node_name) {
                    free(node_name);
                }
                if (1 == cnt) {
                    node_name = strdup(argv[0]);
                } else if (2 == cnt) {
                    node_name = strdup(argv[1]);
                } else {
                    pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    pmix_argv_free(argv);
                    node_name = NULL;
                    goto unlock;
                }
                pmix_argv_free(argv);

                // Strip off the FQDN if present, ignore IP addresses
                if (!prte_keep_fqdn_hostnames && !pmix_net_isaddr(node_name)) {
                    char *ptr;
                    if (NULL != (ptr = strchr(node_name, '.'))) {
                        *ptr = '\0';
                    }
                }

                /* check the rank item */
                if (NULL == rfmap) {
                    pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    goto unlock;
                }
                /* check if this is the local node */
                if (prte_check_host_is_local(node_name)) {
                    rfmap->node_name = strdup(hnp_node->name);
                } else {
                    rfmap->node_name = strdup(node_name);
                }
            }
            break;
        case PRTE_RANKFILE_SLOT:
            if (NULL == node_name || rank < 0
                || NULL == (value = prte_rmaps_rank_file_parse_string_or_int())) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }

            /* check for a duplicate rank assignment */
            if (NULL != pmix_pointer_array_get_item(assigned_ranks_array, rank)) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-assign", true, rank,
                               pmix_pointer_array_get_item(assigned_ranks_array, rank), rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                free(value);
                goto unlock;
            } else {
                /* prepare rank assignment string for the help message in case of a bad-assign */
                sprintf(tmp_rank_assignment, "%s slot=%s", node_name, value);
                pmix_pointer_array_set_item(assigned_ranks_array, 0, tmp_rank_assignment);
            }

            /* check the rank item */
            if (NULL == rfmap) {
                pmix_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                free(value);
                goto unlock;
            }
            for (i = 0; i < 64 && '\0' != value[i]; i++) {
                rfmap->slot_list[i] = value[i];
            }
            free(value);
            break;
        }
    }
    fclose(prte_rmaps_rank_file_in);
    prte_rmaps_rank_file_lex_destroy();

unlock:
    if (NULL != node_name) {
        free(node_name);
    }
    PMIX_RELEASE(assigned_ranks_array);
    return rc;
}

static char *prte_rmaps_rank_file_parse_string_or_int(void)
{
    int rc;
    char tmp_str[64];

    if (PRTE_RANKFILE_EQUAL != prte_rmaps_rank_file_lex()) {
        return NULL;
    }

    rc = prte_rmaps_rank_file_lex();
    switch (rc) {
    case PRTE_RANKFILE_STRING:
        return strdup(prte_rmaps_rank_file_value.sval);
    case PRTE_RANKFILE_INT:
        sprintf(tmp_str, "%d", prte_rmaps_rank_file_value.ival);
        return strdup(tmp_str);
    default:
        return NULL;
    }
}
