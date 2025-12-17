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
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#include <sys/stat.h>
#include <sys/types.h>

#include "src/class/pmix_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/lsf/rmaps_lsf.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_show_help.h"

static int lsf_map(prte_job_t *jdata,
                            prte_rmaps_options_t *options);

prte_rmaps_base_module_t prte_rmaps_lsf_module = {
    .map_job = lsf_map
};

static int file_parse(const char *);

#if PMIX_NUMERIC_VERSION < 0x00040205
static char *pmix_getline(FILE *fp)
{
    char *ret, *buff;
    char input[1024];

    ret = fgets(input, 1024, fp);
    if (NULL != ret) {
        input[strlen(input) - 1] = '\0'; /* remove newline */
        buff = strdup(input);
        return buff;
    }

    return NULL;
}
#endif

/*
 * Local variable
 */
static pmix_pointer_array_t rankmap;
static int num_ranks = 0;

/*
 * Create a rank_file  mapping for the job.
 */
static int lsf_map(prte_job_t *jdata,
                   prte_rmaps_options_t *options)
{
    prte_app_context_t *app = NULL;
    int32_t i, k;
    pmix_list_t node_list;
    prte_node_t *node, *nd, *root_node;
    pmix_rank_t rank, vpid_start;
    int32_t num_slots;
    prte_rmaps_lsf_map_t *rfmap;
    int32_t relative_index, tmp_cnt;
    int rc;
    prte_proc_t *proc;
    pmix_mca_base_component_t *c = &prte_mca_rmaps_lsf_component.super;
    char *slots = NULL;
    bool initial_map = true;
    char *affinity_file = NULL;
    hwloc_cpuset_t proc_bitmap, bitmap;
    char *cpu_bitmap;
    char *avail_bitmap = NULL;
    char *overlap_bitmap = NULL;
    bool physical;

    /* only handle initial launch of rf job */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: job %s being restarted - lsf cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    affinity_file = getenv("LSB_AFFINITY_HOSTFILE");
    if (NULL == affinity_file) {
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: affinity file not given in environment");
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
        // user gave a mapping directive, so it cannot be us
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: mapping directive given - skipping lsf");
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (options->ordered) {
        /* NOT FOR US */
        pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: job %s binding order requested - lsf cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    physical = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);
    /* LSF provides its info as hwthreads, so set the hwthread-as-cpus flag */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
    options->use_hwthreads = true;
    /* don't override something provided by the user, but default to bind-to hwthread */
    if (!PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
        PRTE_SET_BINDING_POLICY(prte_hwloc_default_binding_policy, PRTE_BIND_TO_HWTHREAD);
    }

    pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:lsf: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->pmix_mca_component_name);

    /* setup the node list */
    PMIX_CONSTRUCT(&node_list, pmix_list_t);

    /* pickup the first app - there must be at least one */
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    if (NULL == app) {
        rc = PRTE_ERR_SILENT;
        goto error;
    }

    /* start at the beginning... */
    vpid_start = 0;
    jdata->num_procs = 0;
    PMIX_CONSTRUCT(&rankmap, pmix_pointer_array_t);
    rc = pmix_pointer_array_init(&rankmap,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                 PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&rankmap);
        return PRTE_ERROR;
    }

    /* parse the affinity, storing its results in the rankmap */
    if (PRTE_SUCCESS != (rc = file_parse(affinity_file))) {
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
                                              options->map, initial_map, false, true);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            goto error;
        }
        /* flag that all subsequent requests should not reset the node->mapped flag */
        initial_map = false;

        /* set the number of procs to the number of entries in that affinity_file */
        if (PRTE_FLAG_TEST(app, PRTE_APP_FLAG_COMPUTED)) {
            app->num_procs = num_ranks;
            if (0 == app->num_procs) {
                pmix_show_help("help-rmaps_lsf.txt", "bad-syntax", true, affinity_file);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
        }

        for (k = 0; k < app->num_procs; k++) {
            rank = vpid_start + k;
            /* get the rankfile entry for this rank */
            rfmap = (prte_rmaps_lsf_map_t *) pmix_pointer_array_get_item(&rankmap, rank);
            if (NULL == rfmap) {
                /* if this job was given a slot-list, then use it */
                if (NULL != options->cpuset) {
                    slots = options->cpuset;
                } else if (NULL != prte_hwloc_default_cpu_list) {
                    /* if we were give a default slot-list, then use it */
                    slots = prte_hwloc_default_cpu_list;
                } else {
                    /* all ranks must be specified */
                    pmix_show_help("help-rmaps_lsf.txt", "missing-rank", true, rank,
                                   affinity_file);
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
                    slots = NULL;
                } else {
                    slots = rfmap->slot_list;
                }
                /* find the node where this proc was assigned */
                node = NULL;
                PMIX_LIST_FOREACH(nd, &node_list, prte_node_t)
                {
                    if (NULL != rfmap->node_name && prte_quickmatch(nd, rfmap->node_name)) {
                        node = nd;
                        break;
                    } else if (NULL != rfmap->node_name
                               && (('+' == rfmap->node_name[0])
                                   && (('n' == rfmap->node_name[1])
                                       || ('N' == rfmap->node_name[1])))) {

                        relative_index = atoi(strtok(rfmap->node_name, "+n"));
                        if (relative_index >= (int) pmix_list_get_size(&node_list)
                            || (0 > relative_index)) {
                            pmix_show_help("help-rmaps_lsf.txt", "bad-index", true,
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
                pmix_show_help("help-rmaps_lsf.txt", "resource-not-found", true, rfmap->node_name);
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
            if (NULL == options->job_cpuset) {
                // the prior function will have printed out the error
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            if (!prte_rmaps_base_check_avail(jdata, app, node, &node_list, NULL, options)) {
                pmix_show_help("help-rmaps_lsf.txt", "bad-host", true, rfmap->node_name);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* check if we are oversubscribed */
            rc = prte_rmaps_base_check_oversubscribed(jdata, app, node, options);
            if (PRTE_SUCCESS != rc) {
                goto error;
            }
            options->map = PRTE_MAPPING_BYUSER;
            proc = prte_rmaps_base_setup_proc(jdata, app->idx, node, NULL, options);
            if (NULL == proc) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            /* set the vpid */
            proc->name.rank = rank;
            /* Assign this process to these slots
             * Allow for overload in cases where different ranks are assigned to
             * the same PU, but it must be requested by the user.
             */
            if (NULL != slots &&
                (PRTE_BIND_TO_NONE != PRTE_GET_BINDING_POLICY(jdata->map->binding) || options->overload) ) {
                if (NULL == node->topology || NULL == node->topology->topo) {
                    // Not allowed - for rank-file, we must have the topology
                    pmix_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true,
                                   node->name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                proc_bitmap = hwloc_bitmap_alloc();

                /* parse the slot_list to find the package and core */
                rc = prte_hwloc_base_cpu_list_parse(slots, node->topology->topo, options->use_hwthreads, proc_bitmap);
                if (PRTE_ERR_NOT_FOUND == rc) {
                    char *tmp = prte_hwloc_base_cset2str(hwloc_topology_get_allowed_cpuset(node->topology->topo),
                                                         false, physical, node->topology->topo);
                    pmix_show_help("help-rmaps_lsf.txt", "missing-cpu", true,
                                   prte_tool_basename, slots, tmp);
                    free(tmp);
                    rc = PRTE_ERR_SILENT;
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                } else if (PRTE_ERROR == rc) {
                    pmix_show_help("help-rmaps_lsf.txt", "bad-syntax", true, affinity_file);
                    rc = PRTE_ERR_SILENT;
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                } else if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    hwloc_bitmap_free(proc_bitmap);
                    goto error;
                }
                /* note that we cannot set the proc locale to any specific object
                 * as the slot list may have assigned it to more than one - so
                 * leave that field NULL
                 */

                /* set the proc to the specified map */
                hwloc_bitmap_list_asprintf(&cpu_bitmap, proc_bitmap);
                proc->cpuset = strdup(cpu_bitmap);

                pmix_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:rank_file: convert slots from <%s> to <%s>",
                                    slots, cpu_bitmap);

                /* Check to see if these slots are available on this node */
                if (!hwloc_bitmap_isincluded(proc_bitmap, node->available) && !options->overload) {
                    bitmap = hwloc_bitmap_alloc();
                    hwloc_bitmap_list_asprintf(&avail_bitmap, node->available);

                    hwloc_bitmap_andnot(bitmap, proc_bitmap, node->available);
                    hwloc_bitmap_list_asprintf(&overlap_bitmap, bitmap);

                    pmix_show_help("help-rmaps_lsf.txt", "rmaps:proc-slots-overloaded", true,
                                   PRTE_NAME_PRINT(&proc->name),
                                   node->name,
                                   proc->cpuset,
                                   avail_bitmap,
                                   overlap_bitmap);

                    hwloc_bitmap_free(bitmap);
                    hwloc_bitmap_free(proc_bitmap);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto error;
                }

                /* Mark these slots as taken on this node */
                hwloc_bitmap_andnot(node->available, node->available, proc_bitmap);

                /* cleanup */
                free(cpu_bitmap);
                hwloc_bitmap_free(proc_bitmap);
            }
            /* insert the proc into the proper place */
            PMIX_RETAIN(proc);
            rc = pmix_pointer_array_set_item(jdata->procs, proc->name.rank, proc);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_RELEASE(proc);
                goto error;
            }
            jdata->num_procs++;
            PMIX_RELEASE(proc);
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
    /* compute local/app ranks */
    rc = prte_rmaps_base_compute_vpids(jdata, options);
    return rc;

error:
    PMIX_LIST_DESTRUCT(&node_list);

    return rc;
}

static int file_parse(const char *affinity_file)
{
    int rc = PRTE_SUCCESS;
    int i, j;
    prte_rmaps_lsf_map_t *rfmap = NULL;
    pmix_pointer_array_t *assigned_ranks_array;
    struct stat buf;
    FILE *fp;
    char *hstname, *membind_opt;
    char *sep, *eptr, **cpus, *ptr;
    prte_node_t *nptr, *node;
    hwloc_obj_t obj;

    /* keep track of rank assignments */
    assigned_ranks_array = PMIX_NEW(pmix_pointer_array_t);
    rc = pmix_pointer_array_init(assigned_ranks_array,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                 PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(assigned_ranks_array);
        return PRTE_ERROR;
    }

    /* check to see if the file is empty - if it is,
     * then affinity wasn't actually set for this job */
    if (0 != stat(affinity_file, &buf)) {
        pmix_show_help("help-rmaps_lsf.txt", "lsf-affinity-file-not-found", true, affinity_file);
        return PRTE_ERR_SILENT;
    }
    if (0 == buf.st_size) {
        /* no affinity, so just return */
        return PRTE_SUCCESS;
    }

    /* open the file */
    fp = fopen(affinity_file, "r");
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    while (NULL != (hstname = pmix_getline(fp))) {
        if (0 == strlen(hstname)) {
            free(hstname);
            /* blank line - ignore */
            continue;
        }
        if ('#' == hstname[0]) {
            free(hstname);
            /* Comment line - ignore */
            continue;
        }
        if (NULL != (sep = strchr(hstname, ' '))) {
            *sep = '\0';
            sep++;
            /* remove any trailing space */
            eptr = sep + strlen(sep) - 1;
            while (eptr > sep && isspace(*eptr)) {
                eptr--;
            }
            *(eptr + 1) = 0;
            /*
             * If the submitted LSF job has memory binding related resource requirement, after
             * the cpu id list there are memory binding options.
             *
             * The following is the format of LSB_AFFINITY_HOSTFILE file:
             *
             * Host1 0,1,2,3 0 2
             * Host1 4,5,6,7 1 2
             *
             * Each line includes: host_name, cpu_id_list, NUMA_node_id_list, and memory_policy.
             * In this fix we will drop the last two sections (NUMA_node_id_list and memory_policy)
             * of each line and keep them in 'membind_opt' for future use.
             */
            if (NULL != (membind_opt = strchr(sep, ' '))) {
                *membind_opt = '\0';
                membind_opt++;
            }
        }

        // Convert the Physical CPU set from LSF to a Hwloc logical CPU set
        pmix_output_verbose(20, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: (lsf) Convert Physical CPUSET from <%s>", sep);

        // if we are not keeping fqdn, remove the domain name here
        if (!prte_keep_fqdn_hostnames) {
            if (!pmix_net_isaddr(hstname) &&
                NULL != (ptr = strchr(hstname, '.'))) {
                *ptr = '\0';
            }
        }

        // find the named host
        nptr = NULL;
        for (j = 0; j < prte_node_pool->size; j++) {
            node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, j);
            if (NULL == node) {
                continue;
            }
            if (prte_quickmatch(node, hstname)) {
                nptr = node;
                break;
            }
        }
        if (NULL == nptr) {
            /* wasn't found - that is an error */
            pmix_show_help("help-rmaps_lsf.txt",
                           "resource-not-found", true,
                           hstname);
            fclose(fp);
            free(hstname);
            return PRTE_ERROR;
        }

        if (NULL != sep) {
            cpus = PMIX_ARGV_SPLIT_COMPAT(sep, ',');
            for(i = 0; NULL != cpus[i]; ++i) {
                // get the specified object
                obj = hwloc_get_pu_obj_by_os_index(nptr->topology->topo, strtol(cpus[i], NULL, 10)) ;
                if (NULL == obj) {
                    PMIX_ARGV_FREE_COMPAT(cpus);
                    fclose(fp);
                    free(hstname);
                    return PRTE_ERROR;
                }
                free(cpus[i]);
                // 10 max number of digits in an int
                cpus[i] = (char*)malloc(sizeof(char) * 10);
                snprintf(cpus[i], 10, "%d", obj->logical_index);
            }
            sep = PMIX_ARGV_JOIN_COMPAT(cpus, ',');
            PMIX_ARGV_FREE_COMPAT(cpus);
            pmix_output_verbose(20, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:lsf: (lsf) Convert Physical CPUSET to   <%s>", sep);
        }

        rfmap = PMIX_NEW(prte_rmaps_lsf_map_t);
        rfmap->node_name = hstname;
        if (NULL != sep) {
            snprintf(rfmap->slot_list, RMAPS_LSF_MAX_SLOTS, "%s", sep);
        }
        pmix_pointer_array_set_item(&rankmap, num_ranks, rfmap);
        num_ranks++; // keep track of number of provided ranks
        pmix_output_verbose(20, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:lsf: Adding node %s cpus %s",
                            rfmap->node_name, rfmap->slot_list);

    }
    fclose(fp);

    return PRTE_SUCCESS;
}
