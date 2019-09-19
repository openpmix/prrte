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
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>

#include "constants.h"
#include "types.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/class/prrte_list.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/dss/dss.h"
#include "src/util/argv.h"
#include "src/mca/if/if.h"
#include "src/pmix/pmix-internal.h"

#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/proc_info.h"
#include "src/util/error_strings.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_quit.h"

#include "src/mca/ras/base/ras_private.h"

/* function to display allocation */
void prrte_ras_base_display_alloc(void)
{
    char *tmp=NULL, *tmp2, *tmp3;
    int i, istart;
    prrte_node_t *alloc;

    if (prrte_xml_output) {
        prrte_asprintf(&tmp, "<allocation>\n");
    } else {
        prrte_asprintf(&tmp, "\n======================   ALLOCATED NODES   ======================\n");
    }
    if (prrte_hnp_is_allocated) {
            istart = 0;
    } else {
        istart = 1;
    }
    for (i=istart; i < prrte_node_pool->size; i++) {
        if (NULL == (alloc = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
            continue;
        }
        if (prrte_xml_output) {
            /* need to create the output in XML format */
            prrte_asprintf(&tmp2, "\t<host name=\"%s\" slots=\"%d\" max_slots=\"%d\" slots_inuse=\"%d\">\n",
                     (NULL == alloc->name) ? "UNKNOWN" : alloc->name,
                     (int)alloc->slots, (int)alloc->slots_max, (int)alloc->slots_inuse);
        } else {
            prrte_asprintf(&tmp2, "\t%s: flags=0x%02x slots=%d max_slots=%d slots_inuse=%d state=%s\n",
                     (NULL == alloc->name) ? "UNKNOWN" : alloc->name, alloc->flags,
                     (int)alloc->slots, (int)alloc->slots_max, (int)alloc->slots_inuse,
                     prrte_node_state_to_str(alloc->state));
        }
        if (NULL == tmp) {
            tmp = tmp2;
        } else {
            prrte_asprintf(&tmp3, "%s%s", tmp, tmp2);
            free(tmp);
            free(tmp2);
            tmp = tmp3;
        }
    }
    if (prrte_xml_output) {
        fprintf(prrte_xml_fp, "%s</allocation>\n", tmp);
        fflush(prrte_xml_fp);
    } else {
        prrte_output(prrte_clean_output, "%s=================================================================\n", tmp);
    }
    free(tmp);
}

/*
 * Function for selecting one component from all those that are
 * available.
 */
void prrte_ras_base_allocate(int fd, short args, void *cbdata)
{
    int rc;
    prrte_job_t *jdata;
    prrte_list_t nodes;
    prrte_node_t *node;
    prrte_std_cntr_t i;
    prrte_app_context_t *app;
    prrte_state_caddy_t *caddy = (prrte_state_caddy_t*)cbdata;
    char *hosts=NULL;
    pmix_status_t ret;

    PRRTE_ACQUIRE_OBJECT(caddy);

    PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                         "%s ras:base:allocate",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* convenience */
    jdata = caddy->jdata;

    /* if we already did this, don't do it again - the pool of
     * global resources is set.
     */
    if (prrte_ras_base.allocation_read) {

        PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                             "%s ras:base:allocate allocation already read",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        goto next_state;
    }
    prrte_ras_base.allocation_read = true;

    /* Otherwise, we have to create
     * the initial set of resources that will delineate all
     * further operations serviced by this HNP. This list will
     * contain ALL nodes that can be used by any subsequent job.
     *
     * In other words, if a node isn't found in this step, then
     * no job launched by this HNP will be able to utilize it.
     */

    /* construct a list to hold the results */
    PRRTE_CONSTRUCT(&nodes, prrte_list_t);

    /* if a component was selected, then we know we are in a managed
     * environment.  - the active module will return a list of what it found
     */
    if (NULL != prrte_ras_base.active_module)  {
        /* read the allocation */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base.active_module->allocate(jdata, &nodes))) {
            if (PRRTE_ERR_ALLOCATION_PENDING == rc) {
                /* an allocation request is underway, so just do nothing */
                PRRTE_DESTRUCT(&nodes);
                PRRTE_RELEASE(caddy);
                return;
            }
            if (PRRTE_ERR_SYSTEM_WILL_BOOTSTRAP == rc) {
                /* this module indicates that nodes will be discovered
                 * on a bootstrap basis, so all we do here is add our
                 * own node to the list
                 */
                goto addlocal;
            }
            if (PRRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* we have an active module, but it is unable to
                 * allocate anything for this job - this indicates
                 * that it isn't a fatal error, but could be if
                 * an allocation is required
                 */
                if (prrte_allocation_required) {
                    /* an allocation is required, so this is fatal */
                    PRRTE_DESTRUCT(&nodes);
                    prrte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
                    PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                    PRRTE_RELEASE(caddy);
                    return;
                } else {
                    /* an allocation is not required, so we can just
                     * run on the local node - go add it
                     */
                    goto addlocal;
                }
            }
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&nodes);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
    }
    /* If something came back, save it and we are done */
    if (!prrte_list_is_empty(&nodes)) {
        /* flag that the allocation is managed */
        prrte_managed_allocation = true;
        /* since it is managed, we do not attempt to resolve
         * the nodenames */
        prrte_if_do_not_resolve = true;
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&nodes);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
        PRRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    } else if (prrte_allocation_required) {
        /* if nothing was found, and an allocation is
         * required, then error out
         */
        PRRTE_DESTRUCT(&nodes);
        prrte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                         "%s ras:base:allocate nothing found in module - proceeding to hostfile",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* nothing was found, or no active module was alive. We first see
     * if we were given a rankfile - if so, use it as the hosts will be
     * taken from the mapping */
    if (NULL != prrte_rankfile) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing rankfile %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             prrte_rankfile));

        /* a rankfile was provided - parse it */
        if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&nodes,
                                                               prrte_rankfile))) {
            PRRTE_DESTRUCT(&nodes);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
    }

    /* if something was found in the rankfile, we use that as our global
     * pool - set it and we are done
     */
    if (!prrte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
        /* rankfile is considered equivalent to an RM allocation */
        if (!(PRRTE_MAPPING_SUBSCRIBE_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping))) {
            PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        /* cleanup */
        PRRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    /* if a dash-host has been provided, aggregate across all the
     * app_contexts. Any hosts the user wants to add via comm_spawn
     * can be done so using the add_host option */
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (!prrte_soft_locations &&
            prrte_get_attribute(&app->attributes, PRRTE_APP_DASH_HOST, (void**)&hosts, PRRTE_STRING)) {
            /* if we are using soft locations, then any dash-host would
             * just include desired nodes and not required. We don't want
             * to pick them up here as this would mean the request was
             * always satisfied - instead, we want to allow the request
             * to fail later on and use whatever nodes are actually
             * available
             */
            PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding dash_hosts",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            if (PRRTE_SUCCESS != (rc = prrte_util_add_dash_host_nodes(&nodes, hosts, true))) {
                free(hosts);
                PRRTE_DESTRUCT(&nodes);
                PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                PRRTE_RELEASE(caddy);
                return;
            }
            free(hosts);
        }
    }

    /* if something was found in the dash-host(s), we use that as our global
     * pool - set it and we are done
     */
    if (!prrte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    /* Our next option is to look for a hostfile and assign our global
     * pool from there.
     *
     * Individual hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in hostfiles from across all app_contexts
     *
     * Note that any relative node syntax found in the hostfiles will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding hostfile %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&nodes, hosts))) {
                free(hosts);
                PRRTE_DESTRUCT(&nodes);
                /* set an error event */
                PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                PRRTE_RELEASE(caddy);
                return;
            }
            free(hosts);
        }
    }

    /* if something was found in the hostfiles(s), we use that as our global
     * pool - set it and we are done
     */
    if (!prrte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    /* if nothing was found so far, then look for a default hostfile */
    if (NULL != prrte_default_hostfile) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             prrte_default_hostfile));

        /* a default hostfile was provided - parse it */
        if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&nodes,
                                                               prrte_default_hostfile))) {
            PRRTE_DESTRUCT(&nodes);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
    }

    /* if something was found in the default hostfile, we use that as our global
     * pool - set it and we are done
     */
    if (!prrte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                         "%s ras:base:allocate nothing found in hostfiles - inserting current node",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

  addlocal:
    /* if nothing was found by any of the above methods, then we have no
     * earthly idea what to do - so just add the local host
     */
    node = PRRTE_NEW(prrte_node_t);
    if (NULL == node) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        PRRTE_DESTRUCT(&nodes);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    /* use the same name we got in prrte_process_info so we avoid confusion in
     * the session directories
     */
    node->name = strdup(prrte_process_info.nodename);
    node->state = PRRTE_NODE_STATE_UP;
    node->slots_inuse = 0;
    node->slots_max = 0;
    node->slots = 1;
    prrte_list_append(&nodes, &node->super);
    /* mark the HNP as "allocated" since we have nothing else to use */
    prrte_hnp_is_allocated = true;

    /* store the results in the global resource pool - this removes the
     * list items
     */
    if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_DESTRUCT(&nodes);
        PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
        PRRTE_RELEASE(caddy);
        return;
    }
    PRRTE_DESTRUCT(&nodes);

  DISPLAY:
    /* shall we display the results? */
    if (4 < prrte_output_get_verbosity(prrte_ras_base_framework.framework_output)) {
        prrte_ras_base_display_alloc();
    }

  next_state:
    /* are we to report this event? */
    if (prrte_report_events) {
        if (PMIX_SUCCESS != (ret = PMIx_Notify_event(PMIX_NOTIFY_ALLOC_COMPLETE,
                                                     NULL, PMIX_GLOBAL, NULL, 0,
                                                     NULL, NULL))) {
            PMIX_ERROR_LOG(ret);
            PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
            PRRTE_RELEASE(caddy);
        }
    }

    /* set total slots alloc */
    jdata->total_slots_alloc = prrte_ras_base.total_slots_alloc;

    /* set the job state to the next position */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_ALLOCATION_COMPLETE);

    /* cleanup */
    PRRTE_RELEASE(caddy);
}

int prrte_ras_base_add_hosts(prrte_job_t *jdata)
{
    int rc;
    prrte_list_t nodes;
    int i, n;
    prrte_app_context_t *app;
    prrte_node_t *node, *next, *nptr;
    char *hosts;

    /* construct a list to hold the results */
    PRRTE_CONSTRUCT(&nodes, prrte_list_t);

    /* Individual add-hostfile names, if given, are included
     * in the app_contexts for this job. We therefore need to
     * retrieve the app_contexts for the job, and then cycle
     * through them to see if anything is there. The parser will
     * add the nodes found in each add-hostfile to our list - i.e.,
     * the resulting list contains the UNION of all nodes specified
     * in add-hostfiles from across all app_contexts
     *
     * Note that any relative node syntax found in the add-hostfiles will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */

    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_ADD_HOSTFILE, (void**)&hosts, PRRTE_STRING)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_ras_base_framework.framework_output,
                                 "%s ras:base:add_hosts checking add-hostfile %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            if (PRRTE_SUCCESS != (rc = prrte_util_add_hostfile_nodes(&nodes, hosts))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&nodes);
                free(hosts);
                return rc;
            }
            /* now indicate that this app is to run across it */
            prrte_set_attribute(&app->attributes, PRRTE_APP_HOSTFILE, PRRTE_ATTR_LOCAL, (void**)hosts, PRRTE_STRING);
            prrte_remove_attribute(&app->attributes, PRRTE_APP_ADD_HOSTFILE);
            free(hosts);
        }
    }

    /* We next check for and add any add-host options. Note this is
     * a -little- different than dash-host in that (a) we add these
     * nodes to the global pool regardless of what may already be there,
     * and (b) as a result, any job and/or app_context can access them.
     *
     * Note that any relative node syntax found in the add-host lists will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_ADD_HOST, (void**)&hosts, PRRTE_STRING)) {
            prrte_output_verbose(5, prrte_ras_base_framework.framework_output,
                                "%s ras:base:add_hosts checking add-host %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hosts);
            if (PRRTE_SUCCESS != (rc = prrte_util_add_dash_host_nodes(&nodes, hosts, true))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&nodes);
                free(hosts);
                return rc;
            }
            /* now indicate that this app is to run across them */
            prrte_set_attribute(&app->attributes, PRRTE_APP_DASH_HOST, PRRTE_ATTR_LOCAL, hosts, PRRTE_STRING);
            prrte_remove_attribute(&app->attributes, PRRTE_APP_ADD_HOST);
            free(hosts);
        }
    }

    /* if something was found, we add that to our global pool */
    if (!prrte_list_is_empty(&nodes)) {
        /* the node insert code doesn't check for uniqueness, so we will
         * do so here - yes, this is an ugly, non-scalable loop, but this
         * is the exception case and so we can do it here */
        PRRTE_LIST_FOREACH_SAFE(node, next, &nodes, prrte_node_t) {
            node->state = PRRTE_NODE_STATE_ADDED;
            for (n=0; n < prrte_node_pool->size; n++) {
                if (NULL == (nptr = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, n))) {
                    continue;
                }
                if (0 == strcmp(node->name, nptr->name)) {
                    prrte_list_remove_item(&nodes, &node->super);
                    PRRTE_RELEASE(node);
                    break;
                }
            }
        }
        if (!prrte_list_is_empty(&nodes)) {
            /* store the results in the global resource pool - this removes the
             * list items
             */
            if (PRRTE_SUCCESS != (rc = prrte_ras_base_node_insert(&nodes, jdata))) {
                PRRTE_ERROR_LOG(rc);
            }
            /* mark that an updated nidmap must be communicated to existing daemons */
            prrte_nidmap_communicated = false;
        }
    }
    /* cleanup */
    PRRTE_LIST_DESTRUCT(&nodes);

    /* shall we display the results? */
    if (0 < prrte_output_get_verbosity(prrte_ras_base_framework.framework_output)) {
        prrte_ras_base_display_alloc();
    }

    return PRRTE_SUCCESS;
}
