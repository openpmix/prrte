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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2025-2026 Triad National Security, LLC. All rights
 *                         reserved.
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
#include <string.h>
#include <unistd.h>
#include <flux/core.h>
#include <flux/hostlist.h>
#include <flux/idset.h>
#include <jansson.h>
#include <stdio.h>

#include "src/util/pmix_net.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_flux.h"
#include "src/mca/ras/base/base.h"

/*
 * Local functions
 */
static int init(void);
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);
static void modify(prte_pmix_server_req_t *req);
static void deallocate(prte_job_t *jdata, prte_app_context_t *app);
static struct hostlist *hostlist_from_R_nodelist (json_t *nodelist);
static struct R_hostinfo *hostinfo_array_create (struct hostlist *hl);
static void hostinfo_array_destroy (struct R_hostinfo *hostinfo, int n);
static int hostinfo_append_ranks (struct R_hostinfo *hostinfo, int nnodes,
                                  int start, const char *rankstr,
                                  const char *corestr, char **error_str);
static int parse_json_payload(json_t *root,  pmix_list_t *prte_nodelist);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_flux_module = {
    .init = init,
    .allocate = allocate,
    .deallocate = deallocate,
    .modify = modify,
    .finalize = finalize
};

/*
 * helper structs
 */

struct R_hostinfo {
    char *hostname;
    int broker_rank;
    int nslots;
};

static struct hostlist *hostlist_from_R_nodelist (json_t *nodelist)
{   
    size_t i;
    json_t *val;
    struct hostlist *hl = NULL;

    if (!(hl = hostlist_create ())) {
        return NULL;
    }
    json_array_foreach (nodelist, i, val) {
        const char *host = json_string_value (val);
        if (!host)
            goto error;
        if (hostlist_append (hl, host) < 0) {
            goto error;
        }
    }
    return hl;
error:
    hostlist_destroy (hl);
    return NULL;
}

static struct R_hostinfo *hostinfo_array_create (struct hostlist *hl)
{   
    int i;
    int n;
    const char *host;
    struct R_hostinfo *hostinfo;
        
    n = hostlist_count (hl);
    if (!(hostinfo = calloc (n, sizeof (struct R_hostinfo))))
        return NULL;     
                          
    i = 0;  
    host = hostlist_first (hl);
    while (host) {
        if (!(hostinfo[i++].hostname = strdup (host)))
            goto error;                  
        host = hostlist_next (hl);       
    }                                    
    return hostinfo;                     
error:                                   
    hostinfo_array_destroy (hostinfo, n);
    return NULL; 
}  

static void hostinfo_array_destroy (struct R_hostinfo *hostinfo, int n)
{
    if (hostinfo) {
        for (int i = 0; i < n; i++)
            free (hostinfo[i].hostname);
        free (hostinfo);
    }
}

static int hostinfo_append_ranks (struct R_hostinfo *hostinfo,
                                  int nnodes,
                                  int start,
                                  const char *rankstr,
                                  const char *corestr,
                                  char **error_str)
{                        
    unsigned int i;       
    struct idset *ranks = NULL;
    struct idset *cores = NULL;
    int ncores;
    int count = 0;
                                         
    if (!(ranks = idset_decode (rankstr))
        || !(cores = idset_decode (corestr))) {
        *error_str = strdup("failed to decode ranks/core idset");
        goto out;                        
    }       
        
    if (idset_count (ranks) <= 0
        || (ncores = idset_count (cores)) <= 0) {
        *error_str = strdup("invalid rank or core count in Rv1");
        goto out;
    }
    
    i = idset_first (ranks);
    while (i != IDSET_INVALID_ID) {
        if (start + count > nnodes - 1) {
            *error_str = strdup("Rlite ranks exceeds nodelist entries");
            goto out;
        }
        hostinfo[start + count].broker_rank = i;
        hostinfo[start + count].nslots = ncores;
        count++;
        i = idset_next (ranks, i);
    }
out:
    idset_destroy (ranks);
    idset_destroy (cores);
    return count;
}

static int parse_json_payload(json_t *root,  pmix_list_t *prte_nodelist)
{
    int i, version, start, nnodes, ret = PRTE_SUCCESS;
    char *error_str = NULL;
    json_t *entry = NULL;
    json_t *R_lite = NULL;
    json_t *nodelist = NULL;
    json_t *scheduling = NULL;
    json_t *properties = NULL;
    struct hostlist *hl = NULL;
    struct R_hostinfo *hostinfo = NULL;
    json_error_t error;

    /*
     * unpack the json
    */
    if (json_unpack_ex (root, &error, 0,
                        "{s:i s?O s:{s:o s:o s?o}}",
                        "version", &version,
                        "scheduling", &scheduling,
                        "execution",
                          "R_lite", &R_lite,
                          "nodelist", &nodelist,
                          "properties", &properties) < 0) {

        error_str = "error unpacking R object";
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }

    /*
     * we only know how to parse versions 1 of resource.R
     */
    if (version != 1) {
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                             "%s ras:flux:allocate: unexpected resource.R version %d expected 1",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), version));
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }

    if (NULL == nodelist) {
        error_str = "'nodelist' is missing";
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }

    if (NULL == R_lite) {
        error_str = "R_lite is missing";
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }

    hl = hostlist_from_R_nodelist (nodelist);
    if (NULL == hl) {
        error_str = "unable to generate hostlist from R nodelist";
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }

    nnodes = hostlist_count (hl);
    if (!(hostinfo = hostinfo_array_create (hl))) {
        error_str = "unable to generate hostinfo from hostlist";
        ret = PRTE_ERR_NOT_AVAILABLE;
        goto err;
    }
 
    start = 0;
    json_array_foreach (R_lite, i, entry) {
        const char *ranks;
        const char *cores;
        int rc;
        if (json_unpack (entry,
                         "{s:s s:{s:s}}",
                         "rank", &ranks,
                         "children",
                          "core", &cores) < 0) {
            error_str = "failed to unpack R_lite entry";
            ret = PRTE_ERR_NOT_AVAILABLE;
            goto err;
        }
        if ((rc = hostinfo_append_ranks (hostinfo,
                                         nnodes,
                                         start,
                                         ranks,
                                         cores,
                                         &error_str)) <= 0)
            goto err;
        start += rc;
    }

    for(i = 0; i < nnodes; i++) {
        prte_node_t *node;

        node = PMIX_NEW(prte_node_t);
        if (NULL == node) {
            ret = PRTE_ERR_OUT_OF_RESOURCE;
            goto err;
        }
        node->name = strdup(hostinfo[i].hostname);
        assert(NULL != node->name);
        node->state = PRTE_NODE_STATE_UP;
        node->slots_inuse = 0;
        node->slots_max = 0;
        node->slots = hostinfo[i].nslots;
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                             "%s ras:flux:allocate:discover: adding node %s (%d slots)",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, hostinfo[i].nslots));
        pmix_list_append(prte_nodelist, &node->super);
    }

err:
    if (PRTE_SUCCESS != ret) {
        if (NULL != error_str) {
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                 "%s ras:flux:allocate: %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), error_str));
        } else {
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                 "%s ras:flux:allocate: some error happened parsing R data ret = %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ret));
       }
    }

    if(NULL != hostinfo) {
        hostinfo_array_destroy(hostinfo, nnodes);
    }
    if(NULL != hl) {
        hostlist_destroy(hl);
    }
    return ret;
}

/* init the module */
static int init(void)
{
    return PRTE_SUCCESS;
}

/**
 * Discover available (pre-allocated) nodes and report
 * them back to the caller.
 *
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int ret = PRTE_SUCCESS;
    flux_t *h = NULL;
    flux_future_t *f = NULL;
    flux_error_t flux_error;
    json_error_t json_err;
    char *return_str=NULL;
    const char *flux_job_id=NULL;

    if ((jdata == NULL) || (nodes == NULL)) {
        return PRTE_ERR_BAD_PARAM;
    }

    h = flux_open_ex(NULL, 0, &flux_error);
    if(NULL == h) {
        pmix_show_help("help-ras-flux.txt", "flux-broker-not-found", 1, flux_error.text);
        ret = PRTE_ERR_NOT_FOUND;
        goto err;
    }

    /*
     * first get job id attribute from local broker 
     */

    flux_job_id = flux_attr_get (h, "jobid");
    if (NULL == flux_job_id) {
        ret = PRTE_ERR_NOT_FOUND;
        goto err;
    }

    PMIX_OUTPUT_VERBOSE((10, prte_ras_base_framework.framework_output,
                         "%s ras:flux:allocate: flux job id is %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), flux_job_id));

    /*
     * not sure what good this does but here goes
     */
    prte_job_ident = strdup(flux_job_id);

    /*
     * Lookup the "resource.R" KVS key
     * see https://flux-framework.readthedocs.io/projects/flux-rfc/en/latest/spec_20.html
     * The cores data from the R is used to determine slots on each node.
     */

    f = flux_kvs_lookup(h, NULL, 0, "resource.R");
    if (NULL == f) {
        int errno_l = errno;
        pmix_show_help("help-ras-flux.txt", "flux-kvs-lookup-failure", 1, strerror(errno_l));
        ret = PRTE_ERR_NOT_FOUND;
        goto err;
    }

    if(flux_kvs_lookup_get (f, (const char **)&return_str) < 0){
        int errno_l = errno;
        pmix_show_help("help-ras-flux.txt", "flux-kvs-lookup-get-failure", 1, strerror(errno_l));
        return PRTE_ERR_NOT_FOUND;
        goto err;
    }

    PMIX_OUTPUT_VERBOSE((10, prte_ras_base_framework.framework_output,
                         "%s ras:flux:allocate: flux_kvs_lookup_get returned %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),return_str));

    /*
     *  Parse the returned JSON payload
     */
    json_t *root = json_loads(return_str, JSON_DECODE_ANY, &json_err);
    if (NULL == root) {
        pmix_show_help("help-ras-flux.txt", "flux-json-parse-failure", 1, json_err.text);
        ret = PRTE_ERR_UNPACK_FAILURE;
        goto err;
    }

    ret = parse_json_payload(root, nodes);

err:
    if (NULL != root) {
        json_decref(root);
    }
    if (NULL != f) {
        flux_future_destroy(f);
        f = NULL;
    }
    if (NULL != h) {
        flux_close(h);
        h = NULL;
    }

    return ret;
}

/*
 * This method is not currently available using Flux
 */
static void deallocate(prte_job_t *jdata, prte_app_context_t *app)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata, app);
    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s ras:flux:deallocate: not implemented",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
}

/*
 * This method is not currently available using Flux
 */
static void modify(prte_pmix_server_req_t *req)
{
    PRTE_HIDE_UNUSED_PARAMS(req);
    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s ras:flux:modify: not implemented",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
}


/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s ras:flux:finalize: success (nothing to do)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    return PRTE_SUCCESS;
}
