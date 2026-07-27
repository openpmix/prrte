/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <ctype.h>

#include "src/class/pmix_list.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/hostfile/hostfile.h"
#include "ras_hosts.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static pmix_status_t modify(prte_pmix_server_req_t *req);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_hosts_module = {
    .init = NULL,
    .allocate = allocate,
    .modify = modify,
    .finalize = finalize
};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int rc, i, j;
    char *hosts, **hostlist = NULL;
    bool check;
    prte_app_context_t *app;

    /* We first see if we were given a rank/seqfile - if so, use it
     * as the hosts will be taken from the mapping */
    hosts = NULL;
    check = prte_get_attribute(&jdata->attributes, PRTE_JOB_FILE, (void **) &hosts, PMIX_STRING);
    if (check && NULL != hosts) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:hosts:allocate parsing rank/seqfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

        /* a rank/seqfile was provided - parse it */
        rc = prte_util_add_hostfile_nodes(nodes, hosts);
        if (PRTE_SUCCESS != rc) {
            free(hosts);
            return rc;
        }
        free(hosts);
    }

    /* if something was found in the rankfile, then we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        /* Record that the rankfile mapping policy has been selected */
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_GIVEN);
        PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYUSER);
        /* rankfile is considered equivalent to an RM allocation */
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        }
        return PRTE_SUCCESS;
    }

    /* if a dash-host has been provided, aggregate across all the
     * app_contexts. Any hosts the user wants to add via comm_spawn
     * can be done so using the add_host option */
    for (i = 0; i < jdata->apps->size; i++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i);
        if (NULL ==  app) {
            continue;
        }
        hosts = NULL;
        check = prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, (void **) &hosts, PMIX_STRING);
        if (check && NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding dash_hosts",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            rc = prte_util_add_dash_host_nodes(nodes, hosts, true);
            if (PRTE_SUCCESS != rc) {
                free(hosts);
                return rc;
            }
            free(hosts);
        }
    }

    /* if something was found in the dash-host(s), then we are done */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
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
     * in hosthosts from across all app_contexts
     *
     * Note that any relative node syntax found in the hosthosts will
     * generate an error in this scenario, so only non-relative syntax
     * can be present
     */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        hosts = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_HOSTFILE, (void **) &hosts, PMIX_STRING) &&
            NULL != hosts) {
            PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                                 "%s ras:base:allocate adding hostfile %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hosts));

            /* hostfile was specified - parse it and add it to the list */
            hostlist = PMIx_Argv_split(hosts, ',');
            free(hosts);
            for (j=0; NULL != hostlist[j]; j++) {
                if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(nodes, hostlist[j]))) {
                    PMIx_Argv_free(hostlist);
                    /* set an error event */
                    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
                    return rc;
                }
            }
            PMIx_Argv_free(hostlist);
        }
    }

    /* if something was found in the hosthosts(s), then we are done
     */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
    }

    /* if nothing was found so far, then look for a default hostfile */
    if (NULL != prte_default_hostfile) {
        PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                             "%s ras:base:allocate parsing default hostfile %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_default_hostfile));

        /* a default hostfile was provided - parse it */
        if (PRTE_SUCCESS != (rc = prte_util_add_hostfile_nodes(nodes, prte_default_hostfile))) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_ALLOC_FAILED);
            return rc;
        }
    }

    /* if something was found in the default hostfile, then we are done */
    if (!pmix_list_is_empty(nodes)) {
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((5, prte_ras_base_framework.framework_output,
                         "%s ras:hosts:allocate nothing found in hosts",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}

/* Apply a "slots=+N" / "slots=-N" adjustment to a node that is already in the
 * global pool.
 *
 * Two things the open-coded version got wrong. It clamped at zero but not at
 * slots_max, so an adjustment could push a node above the ceiling the
 * allocation gave it - unlike every other slot adjustment in the framework
 * (prte_ras_base_node_insert clamps both ends for PRTE_NODE_ADD_SLOTS). And it
 * never told prte_ras_base.total_slots_alloc, which is what a managed
 * allocation reports to applications as PMIX_UNIV_SIZE / PMIX_MAX_PROCS, so
 * the DVM's idea of its own size drifted from the pool it was describing. */
static void adjust_slots(prte_node_t *nptr, int slots)
{
    int before = nptr->slots;

    nptr->slots += slots;
    if (0 > nptr->slots) {
        nptr->slots = 0;
    } else if (0 < nptr->slots_max && nptr->slots > nptr->slots_max) {
        nptr->slots = nptr->slots_max;
    }
    prte_ras_base.total_slots_alloc += (nptr->slots - before);
}

static pmix_status_t process_hostfile(char *hostfile, pmix_list_t *nodes)
{
    FILE *fp;
    char *line, *cptr, *ptr, *nm;
    bool addslots, found;
    int slots, m, n;
    prte_node_t *nptr, *node;

    /* We don't use the hostfile parsing code in src/util because it
     * uses flex and that has problems handling the range of allowed
     * syntax here */
    fp = fopen(hostfile, "r");
    if (NULL == fp) {
        pmix_show_help("help-ras-base.txt", "ras-base:addhost-not-found", true, hostfile);
        return PMIX_ERR_SILENT;
    }

    while (NULL != (line = pmix_getline(fp))) {
        // ignore comments and blank lines
        if (0 == strlen(line)) {
            free(line);
            continue;
        }
        // remove leading whitespace. NOTE: isspace() takes an int whose
        // value must be representable as unsigned char (or EOF); passing a
        // plain char is undefined for any byte with the high bit set, so
        // every ctype call here casts.
        cptr = line;
        while (isspace((unsigned char) *cptr)) {
            ++cptr;
        }
        if ('#' == *cptr) {
            free(line);
            continue;
        }
        addslots = false;
        // because there can be arbitrary whitespace around keywords,
        // we manually parse the line to get the directives
        ptr = cptr;
        while ('\0' != *ptr && !isspace((unsigned char) *ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // end of the line - just the node name was given
            slots = -1;
            goto process;
        }
        *ptr = '\0'; // terminate the name
        // find the '=' sign
        ++ptr;
        while ('\0' != *ptr && '=' != *ptr) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // didn't specify slots - use the default value
            slots = -1;
            goto process;
        }
        // find the value
        ++ptr;
        while ('\0' != *ptr && isspace((unsigned char) *ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            // bad syntax
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            fclose(fp);
            free(line);
            return PMIX_ERR_SILENT;
        }
        // if it is a '+' or '-', then we are adjusting
        // the #slots
        if ('+' == *ptr || '-' == *ptr) {
            addslots = true;
        }
        slots = strtol(ptr, NULL, 10);

process:
        // see if we have this node
        found = false;
        // does the name refer to me?
        if (prte_check_host_is_local(cptr)) {
            nm = prte_process_info.nodename;
        } else {
            nm = cptr;
        }

        for (n = 0; !found && n < prte_node_pool->size; n++) {
            nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
            if (NULL == nptr) {
                continue;
            }
            if (0 == strcmp(nm, nptr->name)) {
                // we have the node
                if (addslots) {
                    adjust_slots(nptr, slots);
                }
                found = true;
                break;
            } else if (NULL != nptr->aliases) {
                /* no choice but an exhaustive search - fortunately, these lists are short! */
                for (m = 0; NULL != nptr->aliases[m]; m++) {
                    /* match on nm, not cptr: if the name given refers to this
                     * host it was resolved to our canonical nodename above, and
                     * that is the spelling the pool's aliases carry */
                    if (0 == strcmp(nm, nptr->aliases[m])) {
                        if (addslots) {
                            adjust_slots(nptr, slots);
                        }
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            // this is a new node - add it
            node = PMIX_NEW(prte_node_t);
            node->name = strdup(cptr);
            node->state = PRTE_NODE_STATE_ADDED;
            if (0 <= slots) {
                /* they gave us the number of slots, so set it - including an
                 * explicit "slots=0", which means this node contributes none
                 * and must NOT be silently re-sized from its core count.
                 * Only the -1 marker (no slots clause at all) leaves the
                 * count to be computed when the daemon reports its topology. */
                node->slots = slots;
                PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
            } else if (0 > slots && -1 != slots) {
                // cannot have a new node with negative slots - the -1
                // is a marker for a node without slots being specified
                pmix_show_help("help-ras-base.txt", "negative-slots", true,
                               hostfile, cptr);
                PMIX_RELEASE(node);
                free(line);
                fclose(fp);
                return PMIX_ERR_BAD_PARAM;
            }
            pmix_list_append(nodes, &node->super);
        }
        free(line);
    }
    fclose(fp);
    return PMIX_SUCCESS;
}

static pmix_status_t modify(prte_pmix_server_req_t *req)
{
    int rc;
    pmix_list_t nodes;
    size_t n, k;
    char **hostfiles;
    bool handled = false;

    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    // look for applicable directives
    for (n=0; n < req->ninfo; n++) {
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOSTFILE)) {
            /* the value has to be a string we can split - a request that
             * arrived over the wire may carry anything */
            if (PMIX_STRING != req->info[n].value.type ||
                NULL == req->info[n].value.data.string) {
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = PMIX_ERR_BAD_PARAM;
                return req->pstatus;
            }
            // comma-delimited list of hostfiles to add or delete
            hostfiles = PMIx_Argv_split(req->info[n].value.data.string, ',');
            if (NULL == hostfiles) {
                continue;
            }
            for (k=0; NULL != hostfiles[k]; k++) {
                rc = process_hostfile(hostfiles[k], &nodes);
                if (PMIX_SUCCESS != rc) {
                    PMIX_LIST_DESTRUCT(&nodes);
                    PMIx_Argv_free(hostfiles);
                    req->pstatus = rc;
                    return rc;
                }
            }
            PMIx_Argv_free(hostfiles);
            handled = true;
        }
        if (PMIx_Check_key(req->info[n].key, PMIX_ADD_HOST)) {
            pmix_list_t dhnodes;
            prte_node_t *nd;

            if (PMIX_STRING != req->info[n].value.type ||
                NULL == req->info[n].value.data.string) {
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = PMIX_ERR_BAD_PARAM;
                return req->pstatus;
            }
            // comma-delimited list of hosts to add or delete
            PMIX_CONSTRUCT(&dhnodes, pmix_list_t);
            rc = prte_util_add_dash_host_nodes(&dhnodes, req->info[n].value.data.string, true);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&dhnodes);
                PMIX_LIST_DESTRUCT(&nodes);
                req->pstatus = prte_pmix_convert_rc(rc);
                return req->pstatus;
            }
            /* mark these as newly added so the DVM extension will
             * include them despite any static -host filter given
             * when the DVM was started - the hostfile parser above
             * already does this for its new nodes */
            while (NULL != (nd = (prte_node_t *) pmix_list_remove_first(&dhnodes))) {
                nd->state = PRTE_NODE_STATE_ADDED;
                pmix_list_append(&nodes, &nd->super);
            }
            PMIX_DESTRUCT(&dhnodes);
            handled = true;
        }
    }

    if (0 < pmix_list_get_size(&nodes)) {
        /* mark that an updated nidmap must be communicated to existing daemons */
        prte_nidmap_communicated = false;
        rc = prte_ras_base_node_insert(&nodes, req->jdata);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            /* node_insert drains what it consumed; destruct so whatever it
             * did not reach is not leaked along with the list itself */
            PMIX_LIST_DESTRUCT(&nodes);
            req->pstatus = prte_pmix_convert_rc(rc);
            return req->pstatus;
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);

    /* When no external scheduler is present, this component is the DVM's local
     * resource authority for elastic operations.  Claim the size-change
     * directives so the base prte_ras_base_complete_request() logic runs with
     * the ORIGINAL request info intact:
     *   - PMIX_ALLOC_NEW / PMIX_ALLOC_EXTEND carrying PMIX_ALLOC_NODE_LIST add
     *     the named nodes and extend the DVM;
     *   - PMIX_ALLOC_RELEASE removes the named nodes (PMIX_ALLOC_NODE_LIST) or
     *     tears down a whole reservation (PMIX_ALLOC_ID).
     * Keeping the original request info intact preserves the node list and the
     * allocation ids, so a release can target a specific reservation. */
    switch (req->allocdir) {
    case PMIX_ALLOC_NEW:
    case PMIX_ALLOC_EXTEND:
    case PMIX_ALLOC_RELEASE:
        handled = true;
        break;
    default:
        break;
    }

    /* If we satisfied something, let the base layer finish it; otherwise defer
     * to the next module (this component is the lowest-priority RAS). */
    if (handled) {
        return PMIX_OPERATION_SUCCEEDED;
    }
    return PMIX_ERR_TAKE_NEXT_OPTION;
}
