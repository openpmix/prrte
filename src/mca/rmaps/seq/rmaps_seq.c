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
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include <ctype.h>
#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/util/if.h"
#include "src/util/net.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "rmaps_seq.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

static int prte_rmaps_seq_map(prte_job_t *jdata);

/* define the module */
prte_rmaps_base_module_t prte_rmaps_seq_module = {.map_job = prte_rmaps_seq_map};

/* local object for tracking rank locations */
typedef struct {
    prte_list_item_t super;
    char *hostname;
    char *cpuset;
} seq_node_t;
static void sn_con(seq_node_t *p)
{
    p->hostname = NULL;
    p->cpuset = NULL;
}
static void sn_des(seq_node_t *p)
{
    if (NULL != p->hostname) {
        free(p->hostname);
        p->hostname = NULL;
    }
    if (NULL != p->cpuset) {
        free(p->cpuset);
        p->cpuset = NULL;
    }
}
PRTE_CLASS_INSTANCE(seq_node_t, prte_list_item_t, sn_con, sn_des);

static char *prte_getline(FILE *fp);
static int process_file(char *path, prte_list_t *list);

/*
 * Sequentially map the ranks according to the placement in the
 * specified hostfile
 */
static int prte_rmaps_seq_map(prte_job_t *jdata)
{
    prte_job_map_t *map;
    prte_app_context_t *app;
    int i, n;
    int32_t j;
    prte_list_item_t *item;
    prte_node_t *node, *nd;
    seq_node_t *sq, *save = NULL, *seq;
    ;
    pmix_rank_t vpid;
    int32_t num_nodes;
    int rc;
    prte_list_t default_seq_list;
    prte_list_t node_list, *seq_list, sq_list;
    prte_proc_t *proc;
    prte_mca_base_component_t *c = &prte_rmaps_seq_component.base_version;
    char *hosts = NULL;
    bool use_hwthread_cpus, match;

    PRTE_OUTPUT_VERBOSE((1, prte_rmaps_base_framework.framework_output,
                         "%s rmaps:seq called on job %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace)));

    /* this mapper can only handle initial launch
     * when seq mapping is desired - allow
     * restarting of failed apps
     */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s is being restarted - seq cannot map",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper) {
        if (0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
            /* a mapper has been specified, and it isn't me */
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: job %s not using sequential mapper",
                                PRTE_JOBID_PRINT(jdata->nspace));
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        /* we need to process it */
        goto process;
    }
    if (PRTE_MAPPING_SEQ != PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* I don't know how to do these - defer */
        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s not using seq mapper",
                            PRTE_JOBID_PRINT(jdata->nspace));
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

process:
    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps:seq: mapping job %s", PRTE_JOBID_PRINT(jdata->nspace));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* convenience def */
    map = jdata->map;

    /* if there is a default hostfile, go and get its ordered list of nodes */
    PRTE_CONSTRUCT(&default_seq_list, prte_list_t);
    if (NULL != prte_default_hostfile) {
        rc = process_file(prte_default_hostfile, &default_seq_list);
        if (PRTE_SUCCESS != rc) {
            PRTE_LIST_DESTRUCT(&default_seq_list);
            return rc;
        }
    }

    /* check for type of cpu being used */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)
        && PRTE_BIND_TO_HWTHREAD == PRTE_GET_BINDING_POLICY(jdata->map->binding)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }

    /* start at the beginning... */
    vpid = 0;
    jdata->num_procs = 0;
    if (0 < prte_list_get_size(&default_seq_list)) {
        save = (seq_node_t *) prte_list_get_first(&default_seq_list);
    }

    /* initialize all the nodes as not included in this job map */
    for (j = 0; j < prte_node_pool->size; j++) {
        if (NULL != (node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, j))) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        /* specified seq file trumps all */
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING)) {
            if (NULL == hosts) {
                rc = PRTE_ERR_NOT_FOUND;
                goto error;
            }
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using hostfile %s nodes on app %s", hosts,
                                app->app);
            PRTE_CONSTRUCT(&sq_list, prte_list_t);
            rc = process_file(hosts, &sq_list);
            free(hosts);
            if (PRTE_SUCCESS != rc) {
                PRTE_LIST_DESTRUCT(&sq_list);
                goto error;
            }
            seq_list = &sq_list;
        } else if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                                      PMIX_STRING)) {
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using dash-host nodes on app %s", app->app);
            PRTE_CONSTRUCT(&node_list, prte_list_t);
            /* dash host entries cannot specify cpusets, so used the std function to retrieve the
             * list */
            if (PRTE_SUCCESS != (rc = prte_util_get_ordered_dash_host_list(&node_list, hosts))) {
                PRTE_ERROR_LOG(rc);
                free(hosts);
                goto error;
            }
            free(hosts);
            /* transfer the list to a seq_node_t list */
            PRTE_CONSTRUCT(&sq_list, prte_list_t);
            while (NULL != (nd = (prte_node_t *) prte_list_remove_first(&node_list))) {
                sq = PRTE_NEW(seq_node_t);
                sq->hostname = strdup(nd->name);
                prte_list_append(&sq_list, &sq->super);
                PRTE_RELEASE(nd);
            }
            PRTE_DESTRUCT(&node_list);
            seq_list = &sq_list;
        } else if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts,
                                      PMIX_STRING)) {
            if (NULL == hosts) {
                rc = PRTE_ERR_NOT_FOUND;
                goto error;
            }
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using hostfile %s nodes on app %s", hosts,
                                app->app);
            PRTE_CONSTRUCT(&sq_list, prte_list_t);
            rc = process_file(hosts, &sq_list);
            free(hosts);
            if (PRTE_SUCCESS != rc) {
                PRTE_LIST_DESTRUCT(&sq_list);
                goto error;
            }
            seq_list = &sq_list;
        } else if (0 < prte_list_get_size(&default_seq_list)) {
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using default hostfile nodes on app %s", app->app);
            seq_list = &default_seq_list;
        } else {
            /* can't do anything - no nodes available! */
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                           true);
            rc = PRTE_ERR_SILENT;
            goto error;
        }

        /* check for nolocal and remove the head node, if required */
        if (PRTE_GET_MAPPING_DIRECTIVE(map->mapping) & PRTE_MAPPING_NO_USE_LOCAL) {
            for (item = prte_list_get_first(seq_list); item != prte_list_get_end(seq_list);
                 item = prte_list_get_next(item)) {
                seq = (seq_node_t *) item;
                /* need to check ifislocal because the name in the
                 * hostfile may not have been FQDN, while name returned
                 * by gethostname may have been (or vice versa)
                 */
                if (prte_check_host_is_local(seq->hostname)) {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:seq: removing head node %s", seq->hostname);
                    prte_list_remove_item(seq_list, item);
                    PRTE_RELEASE(item); /* "un-retain" it */
                }
            }
        }

        if (NULL == seq_list || 0 == (num_nodes = (int32_t) prte_list_get_size(seq_list))) {
            prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:no-available-resources",
                           true);
            return PRTE_ERR_SILENT;
        }

        /* set #procs to the number of entries */
        if (0 == app->num_procs) {
            app->num_procs = num_nodes;
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: setting num procs to %s for app %s",
                                PRTE_VPID_PRINT(app->num_procs), app->app);
        } else if (num_nodes < app->num_procs) {
            prte_show_help("help-prte-rmaps-seq.txt", "seq:not-enough-resources", true,
                           app->num_procs, num_nodes);
            return PRTE_ERR_SILENT;
        }

        if (seq_list == &default_seq_list) {
            sq = save;
        } else {
            sq = (seq_node_t *) prte_list_get_first(seq_list);
        }
        for (n = 0; n < app->num_procs; n++) {
            /* find this node on the global array - this is necessary so
             * that our mapping gets saved on that array as the objects
             * returned by the hostfile function are -not- on the array
             */
            match = false;
            for (j = 0; j < prte_node_pool->size; j++) {
                if (NULL
                    == (node = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, j))) {
                    continue;
                }
                if (prte_node_match(node, sq->hostname)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                /* wasn't found - that is an error */
                prte_show_help("help-prte-rmaps-seq.txt", "prte-rmaps-seq:resource-not-found", true,
                               sq->hostname);
                rc = PRTE_ERR_SILENT;
                goto error;
            }
            /* ensure the node is in the map */
            if (!PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
                PRTE_RETAIN(node);
                prte_pointer_array_add(map->nodes, node);
                jdata->map->num_nodes++;
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_MAPPED);
            }
            proc = prte_rmaps_base_setup_proc(jdata, node, i);
            if ((node->slots < (int) node->num_procs)
                || (0 < node->slots_max && node->slots_max < (int) node->num_procs)) {
                if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                    & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error", true,
                                   node->num_procs, app->app);
                    PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                /* flag the node as oversubscribed so that sched-yield gets
                 * properly set
                 */
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_OVERSUBSCRIBED);
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED);
                /* check for permission */
                if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                    /* if we weren't given a directive either way, then we will error out
                     * as the #slots were specifically given, either by the host RM or
                     * via hostfile/dash-host */
                    if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN
                          & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRTE_ERR_SILENT;
                    } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE
                               & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        /* if we were explicitly told not to oversubscribe, then don't */
                        prte_show_help("help-prte-rmaps-base.txt", "prte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app);
                        PRTE_UPDATE_EXIT_STATUS(PRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRTE_ERR_SILENT;
                    }
                }
            }
            /* assign the vpid */
            proc->name.rank = vpid++;
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: assign proc %s to node %s for app %s",
                                PRTE_VPID_PRINT(proc->name.rank), sq->hostname, app->app);

            /* record the cpuset, if given */
            if (NULL != sq->cpuset) {
                hwloc_cpuset_t bitmap;
                char *cpu_bitmap;
                if (NULL == node->topology || NULL == node->topology->topo) {
                    /* not allowed - for sequential cpusets, we must have
                     * the topology info
                     */
                    prte_show_help("help-prte-rmaps-base.txt", "rmaps:no-topology", true,
                                   node->name);
                    rc = PRTE_ERR_SILENT;
                    goto error;
                }
                /* if we are using hwthreads as cpus and binding to hwthreads, then
                 * we can just copy the cpuset across as it already specifies things
                 * at that level */
                if (use_hwthread_cpus) {
                    cpu_bitmap = strdup(sq->cpuset);
                } else {
                    /* setup the bitmap */
                    bitmap = hwloc_bitmap_alloc();
                    /* parse the slot_list to find the package and core */
                    if (PRTE_SUCCESS
                        != (rc = prte_hwloc_base_cpu_list_parse(sq->cpuset, node->topology->topo,
                                                                bitmap))) {
                        PRTE_ERROR_LOG(rc);
                        hwloc_bitmap_free(bitmap);
                        goto error;
                    }
                    /* note that we cannot set the proc locale to any specific object
                     * as the slot list may have assigned it to more than one - so
                     * leave that field NULL
                     */
                    /* set the proc to the specified map */
                    hwloc_bitmap_list_asprintf(&cpu_bitmap, bitmap);
                    hwloc_bitmap_free(bitmap);
                }
                prte_set_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP, PRTE_ATTR_GLOBAL,
                                   cpu_bitmap, PMIX_STRING);
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:seq: binding proc %s to cpuset %s bitmap %s",
                                    PRTE_VPID_PRINT(proc->name.rank), sq->cpuset, cpu_bitmap);
                /* note that the user specified the mapping */
                PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYUSER);
                PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_GIVEN);
                /* cleanup */
                free(cpu_bitmap);
            } else {
                hwloc_obj_t locale;

                /* assign the locale - okay for the topo to be null as
                 * it just means it wasn't returned
                 */
                if (NULL != node->topology && NULL != node->topology->topo) {
                    locale = hwloc_get_root_obj(node->topology->topo);
                    prte_set_attribute(&proc->attributes, PRTE_PROC_HWLOC_LOCALE, PRTE_ATTR_LOCAL,
                                       locale, PMIX_POINTER);
                }
            }

            /* add to the jdata proc array */
            if (PRTE_SUCCESS
                != (rc = prte_pointer_array_set_item(jdata->procs, proc->name.rank, proc))) {
                PRTE_ERROR_LOG(rc);
                goto error;
            }
            /* move to next node */
            sq = (seq_node_t *) prte_list_get_next(&sq->super);
        }

        /** track the total number of processes we mapped */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list if it came from this app_context */
        if (seq_list != &default_seq_list) {
            PRTE_LIST_DESTRUCT(seq_list);
        } else {
            save = sq;
        }
    }

    /* mark that this job is to be fully
     * described in the launch msg */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_FULLY_DESCRIBED, PRTE_ATTR_GLOBAL, NULL,
                       PMIX_BOOL);

    return PRTE_SUCCESS;

error:
    PRTE_LIST_DESTRUCT(&default_seq_list);
    return rc;
}

static char *prte_getline(FILE *fp)
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

static int process_file(char *path, prte_list_t *list)
{
    char *hstname = NULL;
    FILE *fp;
    seq_node_t *sq;
    char *sep, *eptr;
    char *ptr;

    /* open the file */
    fp = fopen(path, "r");
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    while (NULL != (hstname = prte_getline(fp))) {
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
        sq = PRTE_NEW(seq_node_t);
        if (NULL != (sep = strchr(hstname, ' '))) {
            *sep = '\0';
            sep++;
            /* remove any trailing space */
            eptr = sep + strlen(sep) - 1;
            while (eptr > sep && isspace(*eptr)) {
                eptr--;
            }
            *(eptr + 1) = 0;
            sq->cpuset = strdup(sep);
        }

        // Strip off the FQDN if present, ignore IP addresses
        if (!prte_keep_fqdn_hostnames && !prte_net_isaddr(hstname)) {
            if (NULL != (ptr = strchr(hstname, '.'))) {
                *ptr = '\0';
            }
        }

        sq->hostname = hstname;
        prte_list_append(list, &sq->super);
    }
    fclose(fp);
    return PRTE_SUCCESS;
}
