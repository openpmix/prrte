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
 * Copyright (c) 2006-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/util/argv.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/proc_info.h"
#include "src/class/prrte_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/util/show_help.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"
#include "src/runtime/prrte_globals.h"

static int prrte_rmaps_rf_map(prrte_job_t *jdata);

prrte_rmaps_base_module_t prrte_rmaps_rank_file_module = {
    .map_job = prrte_rmaps_rf_map
};


static int prrte_rmaps_rank_file_parse(const char *);
static char *prrte_rmaps_rank_file_parse_string_or_int(void);
static const char *prrte_rmaps_rank_file_name_cur = NULL;
char *prrte_rmaps_rank_file_slot_list = NULL;

/*
 * Local variable
 */
static prrte_pointer_array_t rankmap;
static int num_ranks=0;

/*
 * Create a rank_file  mapping for the job.
 */
static int prrte_rmaps_rf_map(prrte_job_t *jdata)
{
    prrte_job_map_t *map;
    prrte_app_context_t *app=NULL;
    prrte_std_cntr_t i, k;
    prrte_list_t node_list;
    prrte_list_item_t *item;
    prrte_node_t *node, *nd, *root_node;
    prrte_vpid_t rank, vpid_start;
    prrte_std_cntr_t num_slots;
    prrte_rmaps_rank_file_map_t *rfmap;
    prrte_std_cntr_t relative_index, tmp_cnt;
    int rc;
    prrte_proc_t *proc;
    prrte_mca_base_component_t *c = &prrte_rmaps_rank_file_component.super.base_version;
    char *slots;
    bool initial_map=true;
    prrte_hwloc_resource_type_t rtype;

    /* only handle initial launch of rf job */
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s being restarted - rank_file cannot map",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper &&
        0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
        /* a mapper has been specified, and it isn't me */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rf: job %s not using rank_file mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRRTE_MAPPING_BYUSER != PRRTE_GET_MAPPING_POLICY(prrte_rmaps_base.mapping)) {
        /* NOT FOR US */
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRRTE_BIND_ORDERED_REQUESTED(jdata->map->binding)) {
        /* NOT FOR US */
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:rank_file: mapping job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* convenience def */
    map = jdata->map;

    /* default to LOGICAL processors */
    if (prrte_rmaps_rank_file_component.physical) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rank_file: using PHYSICAL processors");
        rtype = PRRTE_HWLOC_PHYSICAL;
    } else {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:rank_file: using LOGICAL processors");
        rtype = PRRTE_HWLOC_LOGICAL;
    }

    /* setup the node list */
    PRRTE_CONSTRUCT(&node_list, prrte_list_t);

    /* pickup the first app - there must be at least one */
    if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0))) {
        rc = PRRTE_ERR_SILENT;
        goto error;
    }

    /* SANITY CHECKS */

    /* if the number of processes wasn't specified, then we know there can be only
     * one app_context allowed in the launch, and that we are to launch it across
     * all available slots.
     */
    if (0 == app->num_procs && 1 < jdata->num_apps) {
        prrte_show_help("help-rmaps_rank_file.txt", "prrte-rmaps-rf:multi-apps-and-zero-np",
                       true, jdata->num_apps, NULL);
        rc = PRRTE_ERR_SILENT;
        goto error;
    }

    /* END SANITY CHECKS */

    /* start at the beginning... */
    vpid_start = 0;
    jdata->num_procs = 0;
    PRRTE_CONSTRUCT(&rankmap, prrte_pointer_array_t);

    /* parse the rankfile, storing its results in the rankmap */
    if ( NULL != prrte_rankfile ) {
        if ( PRRTE_SUCCESS != (rc = prrte_rmaps_rank_file_parse(prrte_rankfile))) {
            PRRTE_ERROR_LOG(rc);
            goto error;
        }
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for(i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        /* for each app_context, we have to get the list of nodes that it can
         * use since that can now be modified with a hostfile and/or -host
         * option
         */
        if(PRRTE_SUCCESS != (rc = prrte_rmaps_base_get_target_nodes(&node_list, &num_slots, app,
                                                                  map->mapping, initial_map, false))) {
            PRRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* we already checked for sanity, so it's okay to just do here */
        if (0 == app->num_procs) {
            if (NULL != prrte_rankfile) {
                /* if we were given a rankfile, then we set the number of procs
                 * to the number of entries in that rankfile
                 */
                app->num_procs = num_ranks;
            } else {
                /** set the num_procs to equal the number of slots on these mapped nodes */
                app->num_procs = num_slots;
            }
        }
        for (k=0; k < app->num_procs; k++) {
            rank = vpid_start + k;
            /* get the rankfile entry for this rank */
            if (NULL == (rfmap = (prrte_rmaps_rank_file_map_t*)prrte_pointer_array_get_item(&rankmap, rank))) {
                /* if we were give a default slot-list, then use it */
                if (NULL != prrte_hwloc_base_cpu_list) {
                    slots = prrte_hwloc_base_cpu_list;
                    /* take the next node off of the available list */
                    node = NULL;
                    PRRTE_LIST_FOREACH(nd, &node_list, prrte_node_t) {
                        /* if adding one to this node would oversubscribe it, then try
                         * the next one */
                        if (nd->slots <= (int)nd->num_procs) {
                            continue;
                        }
                        /* take this one */
                        node = nd;
                        break;
                    }
                    if (NULL == node) {
                        /* all would be oversubscribed, so take the least loaded one */
                        k = UINT32_MAX;
                        PRRTE_LIST_FOREACH(nd, &node_list, prrte_node_t) {
                            if (nd->num_procs < (prrte_vpid_t)k) {
                                k = nd->num_procs;
                                node = nd;
                            }
                        }
                    }
                    /* if we still have nothing, then something is very wrong */
                    if (NULL == node) {
                        rc = PRRTE_ERR_OUT_OF_RESOURCE;
                        goto error;
                    }
                } else {
                    /* all ranks must be specified */
                    prrte_show_help("help-rmaps_rank_file.txt", "missing-rank", true, rank, prrte_rankfile);
                    rc = PRRTE_ERR_SILENT;
                    goto error;
                }
            } else {
                if (0 == strlen(rfmap->slot_list)) {
                    /* rank was specified but no slot list given - that's an error */
                    prrte_show_help("help-rmaps_rank_file.txt","no-slot-list", true, rank, rfmap->node_name);
                    rc = PRRTE_ERR_SILENT;
                    goto error;
                }
                slots = rfmap->slot_list;
                /* find the node where this proc was assigned */
                node = NULL;
                PRRTE_LIST_FOREACH(nd, &node_list, prrte_node_t) {
                    if (NULL != rfmap->node_name &&
                        0 == strcmp(nd->name, rfmap->node_name)) {
                        node = nd;
                        break;
                    } else if (NULL != rfmap->node_name &&
                               (('+' == rfmap->node_name[0]) &&
                                (('n' == rfmap->node_name[1]) ||
                                 ('N' == rfmap->node_name[1])))) {

                        relative_index=atoi(strtok(rfmap->node_name,"+n"));
                        if ( relative_index >= (int)prrte_list_get_size (&node_list) || ( 0 > relative_index)){
                            prrte_show_help("help-rmaps_rank_file.txt","bad-index", true,rfmap->node_name);
                            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                            return PRRTE_ERR_BAD_PARAM;
                        }
                        root_node = (prrte_node_t*) prrte_list_get_first(&node_list);
                        for(tmp_cnt=0; tmp_cnt<relative_index; tmp_cnt++) {
                            root_node = (prrte_node_t*) prrte_list_get_next(root_node);
                        }
                        node = root_node;
                        break;
                    }
                }
            }
            if (NULL == node) {
                prrte_show_help("help-rmaps_rank_file.txt","bad-host", true, rfmap->node_name);
                rc = PRRTE_ERR_SILENT;
                goto error;
            }
            /* ensure the node is in the map */
            if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_RETAIN(node);
                prrte_pointer_array_add(map->nodes, node);
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_MAPPED);
                ++(jdata->map->num_nodes);
            }
            if (NULL == (proc = prrte_rmaps_base_setup_proc(jdata, node, i))) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            if ((node->slots < (int)node->num_procs) ||
                (0 < node->slots_max && node->slots_max < (int)node->num_procs)) {
                if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                   true, node->num_procs, app->app);
                    PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                    rc = PRRTE_ERR_SILENT;
                    goto error;
                }
                /* flag the node as oversubscribed so that sched-yield gets
                 * properly set
                 */
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_OVERSUBSCRIBED);
                PRRTE_FLAG_SET(jdata, PRRTE_JOB_FLAG_OVERSUBSCRIBED);
            }
            /* set the vpid */
            proc->name.vpid = rank;

            if (NULL != slots) {
                /* setup the bitmap */
                hwloc_cpuset_t bitmap;
                char *cpu_bitmap;
                if (NULL == node->topology || NULL == node->topology->topo) {
                    /* not allowed - for rank-file, we must have
                     * the topology info
                     */
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-topology", true, node->name);
                    rc = PRRTE_ERR_SILENT;
                    goto error;
                }
                bitmap = hwloc_bitmap_alloc();
                /* parse the slot_list to find the socket and core */
                if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_cpu_list_parse(slots, node->topology->topo, rtype, bitmap))) {
                    PRRTE_ERROR_LOG(rc);
                    hwloc_bitmap_free(bitmap);
                    goto error;
                }
                /* note that we cannot set the proc locale to any specific object
                 * as the slot list may have assigned it to more than one - so
                 * leave that field NULL
                 */
                /* set the proc to the specified map */
                hwloc_bitmap_list_asprintf(&cpu_bitmap, bitmap);
                prrte_set_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, PRRTE_ATTR_GLOBAL, cpu_bitmap, PRRTE_STRING);
                /* cleanup */
                free(cpu_bitmap);
                hwloc_bitmap_free(bitmap);
            }

            /* insert the proc into the proper place */
            if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs,
                                                                  proc->name.vpid, proc))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            jdata->num_procs++;
        }
        /* update the starting point */
        vpid_start += app->num_procs;
        /* cleanup the node list - it can differ from one app_context
         * to another, so we have to get it every time
         */
        while (NULL != (item = prrte_list_remove_first(&node_list))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&node_list);
        PRRTE_CONSTRUCT(&node_list, prrte_list_t);
    }
    PRRTE_DESTRUCT(&node_list);

    /* cleanup the rankmap */
    for (i=0; i < rankmap.size; i++) {
        if (NULL != (rfmap = prrte_pointer_array_get_item(&rankmap, i))) {
            PRRTE_RELEASE(rfmap);
        }
    }
    PRRTE_DESTRUCT(&rankmap);
    /* mark the job as fully described */
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);

    return rc;

 error:
    PRRTE_LIST_DESTRUCT(&node_list);

    return rc;
}

static int prrte_rmaps_rank_file_parse(const char *rankfile)
{
    int token;
    int rc = PRRTE_SUCCESS;
    int cnt;
    char* node_name = NULL;
    char** argv;
    char buff[64];
    char* value;
    int rank=-1;
    int i;
    prrte_node_t *hnp_node;
    prrte_rmaps_rank_file_map_t *rfmap=NULL;
    prrte_pointer_array_t *assigned_ranks_array;
    char tmp_rank_assignment[64];

    /* keep track of rank assignments */
    assigned_ranks_array = PRRTE_NEW(prrte_pointer_array_t);

    /* get the hnp node's info */
    hnp_node = (prrte_node_t*)(prrte_node_pool->addr[0]);

    prrte_rmaps_rank_file_name_cur = rankfile;
    prrte_rmaps_rank_file_done = false;
    prrte_rmaps_rank_file_in = fopen(rankfile, "r");

    if (NULL == prrte_rmaps_rank_file_in) {
        prrte_show_help("help-rmaps_rank_file.txt", "no-rankfile", true, rankfile);
        rc = PRRTE_ERR_NOT_FOUND;
        PRRTE_ERROR_LOG(rc);
        goto unlock;
    }

    while (!prrte_rmaps_rank_file_done) {
        token = prrte_rmaps_rank_file_lex();

        switch (token) {
            case PRRTE_RANKFILE_ERROR:
                prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRRTE_ERR_BAD_PARAM;
                PRRTE_ERROR_LOG(rc);
                goto unlock;
                break;
            case PRRTE_RANKFILE_QUOTED_STRING:
                prrte_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true, "QUOTED_STRING", rankfile);
                rc = PRRTE_ERR_BAD_PARAM;
                PRRTE_ERROR_LOG(rc);
                goto unlock;
            case PRRTE_RANKFILE_NEWLINE:
                rank = -1;
                if (NULL != node_name) {
                    free(node_name);
                }
                node_name = NULL;
                rfmap = NULL;
                break;
            case PRRTE_RANKFILE_RANK:
                token = prrte_rmaps_rank_file_lex();
                if (PRRTE_RANKFILE_INT == token) {
                    rank = prrte_rmaps_rank_file_value.ival;
                    rfmap = PRRTE_NEW(prrte_rmaps_rank_file_map_t);
                    prrte_pointer_array_set_item(&rankmap, rank, rfmap);
                    num_ranks++;  // keep track of number of provided ranks
                } else {
                    prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRRTE_ERR_BAD_PARAM;
                    PRRTE_ERROR_LOG(rc);
                    goto unlock;
                }
                break;
            case PRRTE_RANKFILE_USERNAME:
                prrte_show_help("help-rmaps_rank_file.txt", "not-supported-rankfile", true, "USERNAME", rankfile);
                rc = PRRTE_ERR_BAD_PARAM;
                PRRTE_ERROR_LOG(rc);
                goto unlock;
                break;
            case PRRTE_RANKFILE_EQUAL:
                if (rank < 0) {
                    prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRRTE_ERR_BAD_PARAM;
                    PRRTE_ERROR_LOG(rc);
                    goto unlock;
                }
                token = prrte_rmaps_rank_file_lex();
                switch (token) {
                    case PRRTE_RANKFILE_HOSTNAME:
                    case PRRTE_RANKFILE_IPV4:
                    case PRRTE_RANKFILE_IPV6:
                    case PRRTE_RANKFILE_STRING:
                    case PRRTE_RANKFILE_INT:
                    case PRRTE_RANKFILE_RELATIVE:
                        if(PRRTE_RANKFILE_INT == token) {
                            sprintf(buff,"%d", prrte_rmaps_rank_file_value.ival);
                            value = buff;
                        } else {
                            value = prrte_rmaps_rank_file_value.sval;
                        }
                        argv = prrte_argv_split (value, '@');
                        cnt = prrte_argv_count (argv);
                        if (NULL != node_name) {
                            free(node_name);
                        }
                        if (1 == cnt) {
                            node_name = strdup(argv[0]);
                        } else if (2 == cnt) {
                            node_name = strdup(argv[1]);
                        } else {
                            prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                            rc = PRRTE_ERR_BAD_PARAM;
                            PRRTE_ERROR_LOG(rc);
                            prrte_argv_free(argv);
                            node_name = NULL;
                            goto unlock;
                        }
                        prrte_argv_free (argv);

                        // Strip off the FQDN if present, ignore IP addresses
                        if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(node_name) ) {
                            char *ptr;
                            if (NULL != (ptr = strchr(node_name, '.'))) {
                                *ptr = '\0';
                            }
                        }

                        /* check the rank item */
                        if (NULL == rfmap) {
                            prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                            rc = PRRTE_ERR_BAD_PARAM;
                            PRRTE_ERROR_LOG(rc);
                            goto unlock;
                        }
                        /* check if this is the local node */
                        if (prrte_check_host_is_local(node_name)) {
                            rfmap->node_name = strdup(hnp_node->name);
                        } else {
                            rfmap->node_name = strdup(node_name);
                        }
                }
                break;
            case PRRTE_RANKFILE_SLOT:
                if (NULL == node_name || rank < 0 ||
                    NULL == (value = prrte_rmaps_rank_file_parse_string_or_int())) {
                    prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRRTE_ERR_BAD_PARAM;
                    PRRTE_ERROR_LOG(rc);
                    goto unlock;
                }

                /* check for a duplicate rank assignment */
                if (NULL != prrte_pointer_array_get_item(assigned_ranks_array, rank)) {
                    prrte_show_help("help-rmaps_rank_file.txt", "bad-assign", true, rank,
                                   prrte_pointer_array_get_item(assigned_ranks_array, rank), rankfile);
                    rc = PRRTE_ERR_BAD_PARAM;
                    free(value);
                    goto unlock;
                } else {
                    /* prepare rank assignment string for the help message in case of a bad-assign */
                    sprintf(tmp_rank_assignment, "%s slot=%s", node_name, value);
                    prrte_pointer_array_set_item(assigned_ranks_array, 0, tmp_rank_assignment);
                }

                /* check the rank item */
                if (NULL == rfmap) {
                    prrte_show_help("help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRRTE_ERR_BAD_PARAM;
                    PRRTE_ERROR_LOG(rc);
                    free(value);
                    goto unlock;
                }
                for (i=0; i < 64 && '\0' != value[i]; i++) {
                    rfmap->slot_list[i] = value[i];
                }
                free(value);
                break;
        }
    }
    fclose(prrte_rmaps_rank_file_in);
    prrte_rmaps_rank_file_lex_destroy ();

unlock:
    if (NULL != node_name) {
        free(node_name);
    }
    PRRTE_RELEASE(assigned_ranks_array);
    prrte_rmaps_rank_file_name_cur = NULL;
    return rc;
}


static char *prrte_rmaps_rank_file_parse_string_or_int(void)
{
    int rc;
    char tmp_str[64];

    if (PRRTE_RANKFILE_EQUAL != prrte_rmaps_rank_file_lex()){
        return NULL;
    }

    rc = prrte_rmaps_rank_file_lex();
    switch (rc) {
        case PRRTE_RANKFILE_STRING:
            return strdup(prrte_rmaps_rank_file_value.sval);
        case PRRTE_RANKFILE_INT:
            sprintf(tmp_str,"%d",prrte_rmaps_rank_file_value.ival);
            return strdup(tmp_str);
        default:
            return NULL;

    }

}
