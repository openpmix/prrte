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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>

#include "constants.h"
#include "types.h"

#include "src/class/prte_list.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/mca/prteif/prteif.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_quit.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/error_strings.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/mca/ras/base/ras_private.h"

char *prte_ras_base_flag_string(prte_node_t *node)
{
    char *tmp, *t2;

    if (0 == node->flags) {
        tmp = strdup("flags: NONE");
        return tmp;
    }

    tmp = strdup("flags: ");
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
        prte_asprintf(&t2, "%sDAEMON_LAUNCHED:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_LOC_VERIFIED)) {
        prte_asprintf(&t2, "%sLOCATION_VERIFIED:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_OVERSUBSCRIBED)) {
        prte_asprintf(&t2, "%sOVERSUBSCRIBED:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_MAPPED)) {
        prte_asprintf(&t2, "%sMAPPED:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
        prte_asprintf(&t2, "%sSLOTS_GIVEN:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (PRTE_FLAG_TEST(node, PRTE_NODE_NON_USABLE)) {
        prte_asprintf(&t2, "%sNONUSABLE:", tmp);
        free(tmp);
        tmp = t2;
    }
    if (':' == tmp[strlen(tmp) - 1]) {
        tmp[strlen(tmp) - 1] = '\0';
    } else {
        free(tmp);
        tmp = strdup("flags: NONE");
    }
    return tmp;
}

/* function to display allocation */
void prte_ras_base_display_alloc(prte_job_t *jdata)
{
    char *tmp = NULL, *tmp2, *tmp3;
    int i, istart;
    prte_node_t *alloc;
    char *flgs;

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, NULL, PMIX_BOOL)) {
        prte_asprintf(&tmp, "<allocation>\n");
    } else {
        prte_asprintf(&tmp,
                      "\n======================   ALLOCATED NODES   ======================\n");
    }
    if (prte_hnp_is_allocated) {
        istart = 0;
    } else {
        istart = 1;
    }
    for (i = istart; i < prte_node_pool->size; i++) {
        if (NULL == (alloc = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, i))) {
            continue;
        }
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, NULL, PMIX_BOOL)) {
            /* need to create the output in XML format */
            prte_asprintf(&tmp2,
                          "\t<host name=\"%s\" slots=\"%d\" max_slots=\"%d\" slots_inuse=\"%d\">\n",
                          (NULL == alloc->name) ? "UNKNOWN" : alloc->name, (int) alloc->slots,
                          (int) alloc->slots_max, (int) alloc->slots_inuse);
        } else {
            /* build the flags string */
            flgs = prte_ras_base_flag_string(alloc);
            prte_asprintf(&tmp2, "\t%s: slots=%d max_slots=%d slots_inuse=%d state=%s\n\t%s\n",
                          (NULL == alloc->name) ? "UNKNOWN" : alloc->name, (int) alloc->slots,
                          (int) alloc->slots_max, (int) alloc->slots_inuse,
                          prte_node_state_to_str(alloc->state), flgs);
            free(flgs);
        }
        if (NULL == tmp) {
            tmp = tmp2;
        } else {
            prte_asprintf(&tmp3, "%s%s", tmp, tmp2);
            free(tmp);
            free(tmp2);
            tmp = tmp3;
        }
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, NULL, PMIX_BOOL)) {
        prte_output(prte_clean_output, "%s</allocation>\n", tmp);
    } else {
        prte_output(prte_clean_output,
                    "%s=================================================================\n", tmp);
    }
    free(tmp);
}

/*
 * Function for selecting one component from all those that are
 * available.
 */
void prte_ras_base_allocate(int fd, short args, void *cbdata)
{
    int rc;
    prte_job_t *jdata;
    prte_list_t nodes;
    prte_node_t *node;
    int32_t i;
    prte_app_context_t *app;
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    char *hosts = NULL;
    pmix_status_t ret;

    PRTE_ACQUIRE_OBJECT(caddy);

    PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output, "%s ras:base:allocate",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* convenience */
    jdata = caddy->jdata;

    /* if we already did this, don't do it again - the pool of
     * global resources is set.
     */
    if (prte_ras_base.allocation_read) {

        PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate allocation already read",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto next_state;
    }
    prte_ras_base.allocation_read = true;

    /* Otherwise, we have to create
     * the initial set of resources that will delineate all
     * further operations serviced by this HNP. This list will
     * contain ALL nodes that can be used by any subsequent job.
     *
     * In other words, if a node isn't found in this step, then
     * no job launched by this HNP will be able to utilize it.
     */

    /* construct a list to hold the results */
    PRTE_CONSTRUCT(&nodes, prte_list_t);

    /* if a component was selected, then we know we are in a managed
     * environment.  - the active module will return a list of what it found
     */
    if (NULL != prte_ras_base.active_module) {
        /* read the allocation */
        if (PRTE_SUCCESS != (rc = prte_ras_base.active_module->allocate(jdata, &nodes))) {
            if (PRTE_ERR_ALLOCATION_PENDING == rc) {
                /* an allocation request is underway, so just do nothing */
                PRTE_DESTRUCT(&nodes);
                PRTE_RELEASE(caddy);
                return;
            }
            if (PRTE_ERR_SYSTEM_WILL_BOOTSTRAP == rc) {
                /* this module indicates that nodes will be discovered
                 * on a bootstrap basis, so all we do here is add our
                 * own node to the list
                 */
                goto addlocal;
            }
            if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
                /* we have an active module, but it is unable to
                 * allocate anything for this job - this indicates
                 * that it isn't a fatal error, but could be if
                 * an allocation is required
                 */
                if (prte_allocation_required) {
                    /* an allocation is required, so this is fatal */
                    PRTE_DESTRUCT(&nodes);
                    prte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
                    PRTE_RELEASE(caddy);
                    return;
                } else {
                    /* an allocation is not required, so we can just
                     * run on the local node - go add it
                     */
                    goto addlocal;
                }
            }
            PRTE_ERROR_LOG(rc);
            PRTE_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
    }
    /* If something came back, save it and we are done */
    if (!prte_list_is_empty(&nodes)) {
        /* flag that the allocation is managed */
        prte_managed_allocation = true;
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
        PRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    } else if (prte_allocation_required) {
        /* if nothing was found, and an allocation is
         * required, then error out
         */
        PRTE_DESTRUCT(&nodes);
        prte_show_help("help-ras-base.txt", "ras-base:no-allocation", true);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        PRTE_RELEASE(caddy);
        return;
    }

    PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:base:allocate nothing found in module - proceeding to hostfile",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* nothing was found, or no active module was alive. We first see
     * if we were given a rank/seqfile - if so, use it as the hosts will be
     * taken from the mapping */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING)) {
        PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing rank/seqfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

        /* a rank/seqfile was provided - parse it */
        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&nodes, hosts))) {
            PRTE_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            free(hosts);
            return;
        }
        free(hosts);
    }

    /* if something was found in the rankfile, we use that as our global
     * pool - set it and we are done
     */
    if (!prte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
        /* Record that the rankfile mapping policy has been selected */
        PRTE_SET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping, PRTE_MAPPING_GIVEN);
        PRTE_SET_MAPPING_POLICY(prte_rmaps_base.mapping, PRTE_MAPPING_BYUSER);
        /* rankfile is considered equivalent to an RM allocation */
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        /* cleanup */
        PRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    /* if a dash-host has been provided, aggregate across all the
     * app_contexts. Any hosts the user wants to add via comm_spawn
     * can be done so using the add_host option */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts,
                               PMIX_STRING)) {
            PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding dash_hosts",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&nodes, hosts, true))) {
                free(hosts);
                PRTE_DESTRUCT(&nodes);
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
                PRTE_RELEASE(caddy);
                return;
            }
            free(hosts);
        }
    }

    /* if something was found in the dash-host(s), we use that as our global
     * pool - set it and we are done
     */
    if (!prte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRTE_DESTRUCT(&nodes);
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
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts,
                               PMIX_STRING)) {
            PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&nodes, hosts))) {
                free(hosts);
                PRTE_DESTRUCT(&nodes);
                /* set an error event */
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
                PRTE_RELEASE(caddy);
                return;
            }
            free(hosts);
        }
    }

    /* if something was found in the hostfiles(s), we use that as our global
     * pool - set it and we are done
     */
    if (!prte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    /* if nothing was found so far, then look for a default hostfile */
    if (NULL != prte_default_hostfile) {
        PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_default_hostfile));

        /* a default hostfile was provided - parse it */
        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&nodes, prte_default_hostfile))) {
            PRTE_DESTRUCT(&nodes);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
    }

    /* if something was found in the default hostfile, we use that as our global
     * pool - set it and we are done
     */
    if (!prte_list_is_empty(&nodes)) {
        /* store the results in the global resource pool - this removes the
         * list items
         */
        if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
            PRTE_ERROR_LOG(rc);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
            return;
        }
        /* cleanup */
        PRTE_DESTRUCT(&nodes);
        goto DISPLAY;
    }

    PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:base:allocate nothing found in hostfiles - inserting current node",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

addlocal:
    /* if nothing was found by any of the above methods, then we have no
     * earthly idea what to do - so just add the local host
     */
    node = PRTE_NEW(prte_node_t);
    if (NULL == node) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PRTE_DESTRUCT(&nodes);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        PRTE_RELEASE(caddy);
        return;
    }
    /* use the same name we got in prte_process_info so we avoid confusion in
     * the session directories
     */
    node->name = strdup(prte_process_info.nodename);
    node->state = PRTE_NODE_STATE_UP;
    node->slots_inuse = 0;
    node->slots_max = 0;
    node->slots = 1;
    prte_list_append(&nodes, &node->super);
    /* mark the HNP as "allocated" since we have nothing else to use */
    prte_hnp_is_allocated = true;

    /* store the results in the global resource pool - this removes the
     * list items
     */
    if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&nodes);
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
        PRTE_RELEASE(caddy);
        return;
    }
    PRTE_DESTRUCT(&nodes);

DISPLAY:
    /* shall we display the results? */
    if (4 < prte_output_get_verbosity(prte_ras_base_framework.framework_output)) {
        prte_ras_base_display_alloc(jdata);
    }

next_state:
    /* are we to report this event? */
    if (prte_report_events) {
        if (PMIX_SUCCESS
            != (ret = PMIx_Notify_event(PMIX_NOTIFY_ALLOC_COMPLETE, NULL, PMIX_GLOBAL, NULL, 0,
                                        NULL, NULL))) {
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            PRTE_RELEASE(caddy);
        }
    }

    /* set total slots alloc */
    jdata->total_slots_alloc = prte_ras_base.total_slots_alloc;

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOCATION_COMPLETE);

    /* cleanup */
    PRTE_RELEASE(caddy);
}

int prte_ras_base_add_hosts(prte_job_t *jdata)
{
    int rc;
    prte_list_t nodes;
    int i, n;
    prte_app_context_t *app;
    prte_node_t *node, *next, *nptr;
    char *hosts;

    /* construct a list to hold the results */
    PRTE_CONSTRUCT(&nodes, prte_list_t);

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

    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE, (void **) &hosts,
                               PMIX_STRING)) {
            PRTE_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:add_hosts checking add-hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(&nodes, hosts))) {
                PRTE_ERROR_LOG(rc);
                PRTE_DESTRUCT(&nodes);
                free(hosts);
                return rc;
            }
            /* now indicate that this app is to run across it */
            prte_set_attribute(&app->attributes, PRTE_APP_HOSTFILE, PRTE_ATTR_LOCAL,
                               (void **) hosts, PMIX_STRING);
            prte_remove_attribute(&app->attributes, PRTE_APP_ADD_HOSTFILE);
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
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_ADD_HOST, (void **) &hosts,
                               PMIX_STRING)) {
            prte_output_verbose(5, prte_ras_base_framework.framework_output,
                                "%s ras:base:add_hosts checking add-host %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts);
            if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&nodes, hosts, true))) {
                PRTE_ERROR_LOG(rc);
                PRTE_DESTRUCT(&nodes);
                free(hosts);
                return rc;
            }
            /* now indicate that this app is to run across them */
            prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST, PRTE_ATTR_LOCAL, hosts,
                               PMIX_STRING);
            prte_remove_attribute(&app->attributes, PRTE_APP_ADD_HOST);
            free(hosts);
        }
    }

    /* if something was found, we add that to our global pool */
    if (!prte_list_is_empty(&nodes)) {
        /* the node insert code doesn't check for uniqueness, so we will
         * do so here - yes, this is an ugly, non-scalable loop, but this
         * is the exception case and so we can do it here */
        PRTE_LIST_FOREACH_SAFE(node, next, &nodes, prte_node_t)
        {
            node->state = PRTE_NODE_STATE_ADDED;
            for (n = 0; n < prte_node_pool->size; n++) {
                if (NULL
                    == (nptr = (prte_node_t *) prte_pointer_array_get_item(prte_node_pool, n))) {
                    continue;
                }
                if (0 == strcmp(node->name, nptr->name)) {
                    prte_list_remove_item(&nodes, &node->super);
                    PRTE_RELEASE(node);
                    break;
                }
            }
        }
        if (!prte_list_is_empty(&nodes)) {
            /* store the results in the global resource pool - this removes the
             * list items
             */
            if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, jdata))) {
                PRTE_ERROR_LOG(rc);
            }
            /* mark that an updated nidmap must be communicated to existing daemons */
            prte_nidmap_communicated = false;
        }
    }
    /* cleanup */
    PRTE_LIST_DESTRUCT(&nodes);

    /* shall we display the results? */
    if (0 < prte_output_get_verbosity(prte_ras_base_framework.framework_output)) {
        prte_ras_base_display_alloc(jdata);
    }

    return PRTE_SUCCESS;
}
