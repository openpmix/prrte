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
 * Copyright (c) 2006-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
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
#include <ctype.h>

#include "src/util/if.h"
#include "src/util/net.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_seq.h"

static int prrte_rmaps_seq_map(prrte_job_t *jdata);

/* define the module */
prrte_rmaps_base_module_t prrte_rmaps_seq_module = {
    .map_job = prrte_rmaps_seq_map
};

/* local object for tracking rank locations */
typedef struct {
    prrte_list_item_t super;
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
PRRTE_CLASS_INSTANCE(seq_node_t,
                   prrte_list_item_t,
                   sn_con, sn_des);

static char *prrte_getline(FILE *fp);

/*
 * Sequentially map the ranks according to the placement in the
 * specified hostfile
 */
static int prrte_rmaps_seq_map(prrte_job_t *jdata)
{
    prrte_job_map_t *map;
    prrte_app_context_t *app;
    int i, n;
    prrte_std_cntr_t j;
    prrte_list_item_t *item;
    prrte_node_t *node, *nd;
    seq_node_t *sq, *save=NULL, *seq;;
    prrte_vpid_t vpid;
    prrte_std_cntr_t num_nodes;
    int rc;
    prrte_list_t default_seq_list;
    prrte_list_t node_list, *seq_list, sq_list;
    prrte_proc_t *proc;
    prrte_mca_base_component_t *c = &prrte_rmaps_seq_component.base_version;
    char *hosts = NULL, *sep, *eptr;
    FILE *fp;
    prrte_hwloc_resource_type_t rtype;

    PRRTE_OUTPUT_VERBOSE((1, prrte_rmaps_base_framework.framework_output,
                         "%s rmaps:seq called on job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* this mapper can only handle initial launch
     * when seq mapping is desired - allow
     * restarting of failed apps
     */
    if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s is being restarted - seq cannot map",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL != jdata->map->req_mapper) {
        if (0 != strcasecmp(jdata->map->req_mapper, c->mca_component_name)) {
            /* a mapper has been specified, and it isn't me */
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: job %s not using sequential mapper",
                                PRRTE_JOBID_PRINT(jdata->jobid));
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
        /* we need to process it */
        goto process;
    }
    if (PRRTE_MAPPING_SEQ != PRRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* I don't know how to do these - defer */
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: job %s not using seq mapper",
                            PRRTE_JOBID_PRINT(jdata->jobid));
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

 process:
    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps:seq: mapping job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* flag that I did the mapping */
    if (NULL != jdata->map->last_mapper) {
        free(jdata->map->last_mapper);
    }
    jdata->map->last_mapper = strdup(c->mca_component_name);

    /* convenience def */
    map = jdata->map;

    /* if there is a default hostfile, go and get its ordered list of nodes */
    PRRTE_CONSTRUCT(&default_seq_list, prrte_list_t);
    if (NULL != prrte_default_hostfile) {
        char *hstname = NULL;
        /* open the file */
        fp = fopen(prrte_default_hostfile, "r");
        if (NULL == fp) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            rc = PRRTE_ERR_NOT_FOUND;
            goto error;
        }
        while (NULL != (hstname = prrte_getline(fp))) {
            if (0 == strlen(hstname)) {
                free(hstname);
                /* blank line - ignore */
                continue;
            }
            if( '#' == hstname[0] ) {
                free(hstname);
                /* Comment line - ignore */
                continue;
            }
            sq = PRRTE_NEW(seq_node_t);
            if (NULL != (sep = strchr(hstname, ' '))) {
                *sep = '\0';
                sep++;
                /* remove any trailing space */
                eptr = sep + strlen(sep) - 1;
                while (eptr > sep && isspace(*eptr)) {
                    eptr--;
                }
                *(eptr+1) = 0;
                sq->cpuset = strdup(sep);
            }

            // Strip off the FQDN if present, ignore IP addresses
            if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(hstname) ) {
                char *ptr;
                if (NULL != (ptr = strchr(hstname, '.'))) {
                    *ptr = '\0';
                }
            }

            sq->hostname = hstname;
            prrte_list_append(&default_seq_list, &sq->super);
        }
        fclose(fp);
    }

    /* start at the beginning... */
    vpid = 0;
    jdata->num_procs = 0;
    if (0 < prrte_list_get_size(&default_seq_list)) {
        save = (seq_node_t*)prrte_list_get_first(&default_seq_list);
    }

    /* default to LOGICAL processors */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_PHYSICAL_CPUIDS, NULL, PRRTE_BOOL)) {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: using PHYSICAL processors");
        rtype = PRRTE_HWLOC_PHYSICAL;
    } else {
        prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                            "mca:rmaps:seq: using LOGICAL processors");
        rtype = PRRTE_HWLOC_LOGICAL;
    }

    /* initialize all the nodes as not included in this job map */
    for (j=0; j < prrte_node_pool->size; j++) {
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, j))) {
            PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
        }
    }

    /* cycle through the app_contexts, mapping them sequentially */
    for(i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        /* dash-host trumps hostfile */
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using dash-host nodes on app %s", app->app);
            PRRTE_CONSTRUCT(&node_list, prrte_list_t);
            /* dash host entries cannot specify cpusets, so used the std function to retrieve the list */
            if (PRRTE_SUCCESS != (rc = prrte_util_get_ordered_dash_host_list(&node_list, hosts))) {
                PRRTE_ERROR_LOG(rc);
                free(hosts);
                goto error;
            }
            free(hosts);
            /* transfer the list to a seq_node_t list */
            PRRTE_CONSTRUCT(&sq_list, prrte_list_t);
            while (NULL != (nd = (prrte_node_t*)prrte_list_remove_first(&node_list))) {
                sq = PRRTE_NEW(seq_node_t);
                sq->hostname = strdup(nd->name);
                prrte_list_append(&sq_list, &sq->super);
                PRRTE_RELEASE(nd);
            }
            PRRTE_DESTRUCT(&node_list);
            seq_list = &sq_list;
        } else if (prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
            char *hstname;
            if (NULL == hosts) {
                rc = PRRTE_ERR_NOT_FOUND;
                goto error;
            }
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using hostfile %s nodes on app %s", hosts, app->app);
            PRRTE_CONSTRUCT(&sq_list, prrte_list_t);
            /* open the file */
            fp = fopen(hosts, "r");
            if (NULL == fp) {
                PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                rc = PRRTE_ERR_NOT_FOUND;
                PRRTE_DESTRUCT(&sq_list);
                goto error;
            }
            while (NULL != (hstname = prrte_getline(fp))) {
                if (0 == strlen(hstname)) {
                    free(hstname);
                    /* blank line - ignore */
                    continue;
                }
                if( '#' == hstname[0] ) {
                    free(hstname);
                    /* Comment line - ignore */
                    continue;
                }
                sq = PRRTE_NEW(seq_node_t);
                if (NULL != (sep = strchr(hstname, ' '))) {
                    *sep = '\0';
                    sep++;
                    /* remove any trailing space */
                    eptr = sep + strlen(sep) - 1;
                    while (eptr > sep && isspace(*eptr)) {
                        eptr--;
                    }
                    *(eptr+1) = 0;
                    sq->cpuset = strdup(sep);
                }

                // Strip off the FQDN if present, ignore IP addresses
                if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(hstname) ) {
                    char *ptr;
                    if (NULL != (ptr = strchr(hstname, '.'))) {
                        (*ptr) = '\0';
                    }
                }

                sq->hostname = hstname;
                prrte_list_append(&sq_list, &sq->super);
            }
            fclose(fp);
            free(hosts);
            seq_list = &sq_list;
        } else if (0 < prrte_list_get_size(&default_seq_list)) {
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: using default hostfile nodes on app %s", app->app);
            seq_list = &default_seq_list;
        } else {
            /* can't do anything - no nodes available! */
            prrte_show_help("help-prrte-rmaps-base.txt",
                           "prrte-rmaps-base:no-available-resources",
                           true);
            return PRRTE_ERR_SILENT;
        }

        /* check for nolocal and remove the head node, if required */
        if (map->mapping & PRRTE_MAPPING_NO_USE_LOCAL) {
            for (item  = prrte_list_get_first(seq_list);
                 item != prrte_list_get_end(seq_list);
                 item  = prrte_list_get_next(item) ) {
                seq = (seq_node_t*)item;
                /* need to check ifislocal because the name in the
                 * hostfile may not have been FQDN, while name returned
                 * by gethostname may have been (or vice versa)
                 */
                if (prrte_check_host_is_local(seq->hostname)) {
                    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                        "mca:rmaps:seq: removing head node %s", seq->hostname);
                    prrte_list_remove_item(seq_list, item);
                    PRRTE_RELEASE(item);  /* "un-retain" it */
                }
            }
        }

        if (NULL == seq_list || 0 == (num_nodes = (prrte_std_cntr_t)prrte_list_get_size(seq_list))) {
            prrte_show_help("help-prrte-rmaps-base.txt",
                           "prrte-rmaps-base:no-available-resources",
                           true);
            return PRRTE_ERR_SILENT;
        }

        /* if num_procs wasn't specified, set it now */
        if (0 == app->num_procs) {
            app->num_procs = num_nodes;
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: setting num procs to %s for app %s",
                                PRRTE_VPID_PRINT(app->num_procs), app->app);
        } else if (num_nodes < app->num_procs) {
            prrte_show_help("help-prrte-rmaps-base.txt", "seq:not-enough-resources", true,
                           app->num_procs, num_nodes);
            return PRRTE_ERR_SILENT;
        }

        if (seq_list == &default_seq_list) {
            sq = save;
        } else {
            sq = (seq_node_t*)prrte_list_get_first(seq_list);
        }
        for (n=0; n < app->num_procs; n++) {
            /* find this node on the global array - this is necessary so
             * that our mapping gets saved on that array as the objects
             * returned by the hostfile function are -not- on the array
             */
            node = NULL;
            for (j=0; j < prrte_node_pool->size; j++) {
                if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, j))) {
                    continue;
                }
                if (0 == strcmp(sq->hostname, node->name)) {
                    break;
                }
            }
            if (NULL == node) {
                /* wasn't found - that is an error */
                prrte_show_help("help-prrte-rmaps-seq.txt",
                               "prrte-rmaps-seq:resource-not-found",
                               true, sq->hostname);
                rc = PRRTE_ERR_SILENT;
                goto error;
            }
            /* ensure the node is in the map */
            if (!PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_RETAIN(node);
                prrte_pointer_array_add(map->nodes, node);
                jdata->map->num_nodes++;
                PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_MAPPED);
            }
            proc = prrte_rmaps_base_setup_proc(jdata, node, i);
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
                /* check for permission */
                if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
                    /* if we weren't given a directive either way, then we will error out
                     * as the #slots were specifically given, either by the host RM or
                     * via hostfile/dash-host */
                    if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
                        prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app);
                        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRRTE_ERR_SILENT;
                    } else if (PRRTE_MAPPING_NO_OVERSUBSCRIBE & PRRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                        /* if we were explicitly told not to oversubscribe, then don't */
                        prrte_show_help("help-prrte-rmaps-base.txt", "prrte-rmaps-base:alloc-error",
                                       true, app->num_procs, app->app);
                        PRRTE_UPDATE_EXIT_STATUS(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                        return PRRTE_ERR_SILENT;
                    }
                }
            }
            /* assign the vpid */
            proc->name.vpid = vpid++;
            prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                "mca:rmaps:seq: assign proc %s to node %s for app %s",
                                PRRTE_VPID_PRINT(proc->name.vpid), sq->hostname, app->app);

            /* record the cpuset, if given */
            if (NULL != sq->cpuset) {
                hwloc_cpuset_t bitmap;
                char *cpu_bitmap;
                if (NULL == node->topology || NULL == node->topology->topo) {
                    /* not allowed - for sequential cpusets, we must have
                     * the topology info
                     */
                    prrte_show_help("help-prrte-rmaps-base.txt", "rmaps:no-topology", true, node->name);
                    rc = PRRTE_ERR_SILENT;
                    goto error;
                }
                /* if we are using hwthreads as cpus and binding to hwthreads, then
                 * we can just copy the cpuset across as it already specifies things
                 * at that level */
                if (prrte_hwloc_use_hwthreads_as_cpus &&
                    PRRTE_BIND_TO_HWTHREAD == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy)) {
                    cpu_bitmap = strdup(sq->cpuset);
                } else {
                    /* setup the bitmap */
                    bitmap = hwloc_bitmap_alloc();
                    /* parse the slot_list to find the socket and core */
                    if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_cpu_list_parse(sq->cpuset, node->topology->topo, rtype, bitmap))) {
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
                    hwloc_bitmap_free(bitmap);
                }
                prrte_set_attribute(&proc->attributes, PRRTE_PROC_CPU_BITMAP, PRRTE_ATTR_GLOBAL, cpu_bitmap, PRRTE_STRING);
                prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                                    "mca:rmaps:seq: binding proc %s to cpuset %s bitmap %s",
                                    PRRTE_VPID_PRINT(proc->name.vpid), sq->cpuset, cpu_bitmap);
                /* we are going to bind to cpuset since the user is specifying the cpus */
                PRRTE_SET_BINDING_POLICY(jdata->map->binding, PRRTE_BIND_TO_CPUSET);
                /* note that the user specified the mapping */
                PRRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRRTE_MAPPING_BYUSER);
                PRRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRRTE_MAPPING_GIVEN);
                /* cleanup */
                free(cpu_bitmap);
            } else {
                hwloc_obj_t locale;

                /* assign the locale - okay for the topo to be null as
                 * it just means it wasn't returned
                 */
                if (NULL != node->topology && NULL != node->topology->topo) {
                    locale = hwloc_get_root_obj(node->topology->topo);
                    prrte_set_attribute(&proc->attributes, PRRTE_PROC_HWLOC_LOCALE,
                                       PRRTE_ATTR_LOCAL, locale, PRRTE_PTR);
                }
            }

            /* add to the jdata proc array */
            if (PRRTE_SUCCESS != (rc = prrte_pointer_array_set_item(jdata->procs, proc->name.vpid, proc))) {
                PRRTE_ERROR_LOG(rc);
                goto error;
            }
            /* move to next node */
            sq = (seq_node_t*)prrte_list_get_next(&sq->super);
        }

        /** track the total number of processes we mapped */
        jdata->num_procs += app->num_procs;

        /* cleanup the node list if it came from this app_context */
        if (seq_list != &default_seq_list) {
            PRRTE_LIST_DESTRUCT(seq_list);
        } else {
            save = sq;
        }
    }

    /* mark that this job is to be fully
     * described in the launch msg */
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);

    return PRRTE_SUCCESS;

 error:
    PRRTE_LIST_DESTRUCT(&default_seq_list);
    return rc;
}

static char *prrte_getline(FILE *fp)
{
    char *ret, *buff;
    char input[1024];

    ret = fgets(input, 1024, fp);
    if (NULL != ret) {
           input[strlen(input)-1] = '\0';  /* remove newline */
           buff = strdup(input);
           return buff;
    }

    return NULL;
}
