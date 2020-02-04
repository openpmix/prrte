/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2020 IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <pmix_server.h>

#include "prrte_stdint.h"
#include "types.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/os_dirpath.h"
#include "src/util/error.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"

#include "pmix_server_internal.h"
#include "pmix_server.h"

static void opcbfunc(pmix_status_t status, void *cbdata);

/* stuff proc attributes for sending back to a proc */
int prrte_pmix_server_register_nspace(prrte_job_t *jdata)
{
    int rc;
    prrte_proc_t *pptr;
    int i, k, n, p;
    prrte_list_t *info, *pmap;
    prrte_info_item_t *kv, *kptr;
    prrte_node_t *node, *mynode;
    prrte_vpid_t vpid;
    char **list, **procs, **micro, *tmp, *regex;
    prrte_job_t *dmns;
    prrte_job_map_t *map;
    prrte_app_context_t *app;
    uid_t uid;
    gid_t gid;
    prrte_list_t *cache;
    hwloc_obj_t machine;
    pmix_proc_t pproc;
    pmix_status_t ret;
    pmix_info_t *pinfo;
    size_t ninfo;
    prrte_pmix_lock_t lock;
    prrte_list_t local_procs;
    prrte_namelist_t *nm;
    size_t nmsize;
    pmix_server_pset_t *pset;
    prrte_value_t *val;
    uint32_t ui32;

    prrte_output_verbose(2, prrte_pmix_server_globals.output,
                        "%s register nspace for %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* setup the info list */
    info = PRRTE_NEW(prrte_list_t);
    uid = geteuid();
    gid = getegid();

    /* pass our nspace/rank */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_LOAD_KEY(kv->info.key, PMIX_SERVER_NSPACE);
    kv->info.value.type = PMIX_PROC;
    /* have to stringify the jobid */
    PMIX_PROC_CREATE(kv->info.value.data.proc, 1);
    PRRTE_PMIX_CONVERT_JOBID(kv->info.value.data.proc->nspace, PRRTE_PROC_MY_NAME->jobid);
    prrte_list_append(info, &kv->super);

    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_RANK, &PRRTE_PROC_MY_NAME->vpid, PMIX_PROC_RANK);
    prrte_list_append(info, &kv->super);

    /* jobid */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_LOAD_KEY(kv->info.key, PMIX_JOBID);
    kv->info.value.type = PMIX_PROC;
    /* have to stringify the jobid */
    PMIX_PROC_CREATE(kv->info.value.data.proc, 1);
    PRRTE_PMIX_CONVERT_JOBID(kv->info.value.data.proc->nspace, jdata->jobid);
    prrte_list_append(info, &kv->super);

    /* offset */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NPROC_OFFSET, &jdata->offset, PMIX_PROC_RANK);
    prrte_list_append(info, &kv->super);

    /* check for cached values to add to the job info */
    cache = NULL;
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE, (void**)&cache, PRRTE_PTR) &&
        NULL != cache) {
        while (NULL != (val = (prrte_value_t*)prrte_list_remove_first(cache))) {
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_LOAD_KEY(kv->info.key, val->key);
            prrte_pmix_value_load(&kv->info.value, val);
            prrte_list_append(info, &kv->super);
            PRRTE_RELEASE(val);
        }
        prrte_remove_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE);
        PRRTE_RELEASE(cache);
    }

    /* assemble the node and proc map info */
    list = NULL;
    procs = NULL;
    map = jdata->map;
    for (i=0; i < map->nodes->size; i++) {
        micro = NULL;
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            prrte_argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k=0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, k))) {
                    if (jdata->jobid == pptr->name.jobid) {
                        prrte_argv_append_nosize(&micro, PRRTE_VPID_PRINT(pptr->name.vpid));
                    }
                }
            }
            /* assemble the rank/node map */
            if (NULL != micro) {
                tmp = prrte_argv_join(micro, ',');
                prrte_argv_free(micro);
                prrte_argv_append_nosize(&procs, tmp);
                free(tmp);
            }
        }
    }
    /* let the PMIx server generate the nodemap regex */
    if (NULL != list) {
        tmp = prrte_argv_join(list, ',');
        prrte_argv_free(list);
        list = NULL;
        if (PRRTE_SUCCESS != (rc = PMIx_generate_regex(tmp, &regex))) {
            PRRTE_ERROR_LOG(rc);
            free(tmp);
            PRRTE_LIST_RELEASE(info);
            return rc;
        }
        free(tmp);
        kv = PRRTE_NEW(prrte_info_item_t);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&kv->info, PMIX_NODE_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_NODE_MAP, regex, PMIX_STRING);
#endif
        free(regex);
        prrte_list_append(info, &kv->super);
    }

    /* let the PMIx server generate the procmap regex */
    if (NULL != procs) {
        tmp = prrte_argv_join(procs, ';');
        prrte_argv_free(procs);
        procs = NULL;
        if (PRRTE_SUCCESS != (rc = PMIx_generate_ppn(tmp, &regex))) {
            PRRTE_ERROR_LOG(rc);
            free(tmp);
            PRRTE_LIST_RELEASE(info);
            return rc;
        }
        free(tmp);
        kv = PRRTE_NEW(prrte_info_item_t);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&kv->info, PMIX_PROC_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_PROC_MAP, regex, PMIX_STRING);
#endif
        free(regex);
        prrte_list_append(info, &kv->super);
    }

    /* get our local node */
    if (NULL == (dmns = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_LIST_RELEASE(info);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(dmns->procs, PRRTE_PROC_MY_NAME->vpid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_LIST_RELEASE(info);
        return PRRTE_ERR_NOT_FOUND;
    }
    mynode = pptr->node;
    if (NULL == mynode) {
        /* cannot happen */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_LIST_RELEASE(info);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* pass our hostname */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, prrte_process_info.nodename, PMIX_STRING);
    prrte_list_append(info, &kv->super);

    /* pass our node ID */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NODEID, &mynode->index, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* pass our node size */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NODE_SIZE, &mynode->num_procs, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* pass the number of nodes in the job */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NUM_NODES, &map->num_nodes, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* univ size */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_UNIV_SIZE, &jdata->total_slots_alloc, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* job size */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &jdata->num_procs, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* number of apps in this job */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_JOB_NUM_APPS, &jdata->num_apps, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* local size */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_SIZE, &jdata->num_local_procs, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* max procs */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_MAX_PROCS, &jdata->total_slots_alloc, PMIX_UINT32);
    prrte_list_append(info, &kv->super);

    /* topology signature */
    kv = PRRTE_NEW(prrte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, prrte_topo_signature, PMIX_STRING);
#else
    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, prrte_topo_signature, PMIX_STRING);
#endif
    prrte_list_append(info, &kv->super);

    /* total available physical memory */
    machine = hwloc_get_next_obj_by_type (prrte_hwloc_topology, HWLOC_OBJ_MACHINE, NULL);
    if (NULL != machine) {
        kv = PRRTE_NEW(prrte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->memory.total_memory, PMIX_UINT64);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->total_memory, PMIX_UINT64);
#endif
        prrte_list_append(info, &kv->super);
    }

    /* pass the mapping policy used for this job */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_MAPBY, prrte_rmaps_base_print_mapping(jdata->map->mapping), PMIX_STRING);
    prrte_list_append(info, &kv->super);

    /* pass the ranking policy used for this job */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_RANKBY, prrte_rmaps_base_print_ranking(jdata->map->ranking), PMIX_STRING);
    prrte_list_append(info, &kv->super);

    /* pass the binding policy used for this job */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_BINDTO, prrte_hwloc_base_print_binding(jdata->map->binding), PMIX_STRING);
    prrte_list_append(info, &kv->super);

    /* register any psets for this job */
    for (i=0; i < (int)jdata->num_apps; i++) {
        app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, pptr->app_idx);
        tmp = NULL;
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_PSET_NAME, (void**)&tmp, PRRTE_STRING) &&
            NULL != tmp) {
            pset = PRRTE_NEW(pmix_server_pset_t);
            pset->name = strdup(tmp);
            prrte_list_append(&prrte_pmix_server_globals.psets, &pset->super);
        }
    }

    /* pass the top-level session directory - this is our jobfam session dir */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_TMPDIR, prrte_process_info.jobfam_session_dir, PMIX_STRING);
    prrte_list_append(info, &kv->super);

    /* create and pass a job-level session directory */
    if (0 > prrte_asprintf(&tmp, "%s/%d", prrte_process_info.jobfam_session_dir, PRRTE_LOCAL_JOBID(jdata->jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(prrte_process_info.jobfam_session_dir, S_IRWXU))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NSDIR, tmp, PMIX_STRING);
    free(tmp);
    prrte_list_append(info, &kv->super);

    /* register any local clients */
    vpid = PRRTE_VPID_MAX;
    PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, jdata->jobid);
    micro = NULL;
    PRRTE_CONSTRUCT(&local_procs, prrte_list_t);
    for (i=0; i < mynode->procs->size; i++) {
        if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(mynode->procs, i))) {
            continue;
        }
        /* track all procs on the node */
        nm = PRRTE_NEW(prrte_namelist_t);
        nm->name.jobid = pptr->name.jobid;
        nm->name.vpid = pptr->name.vpid;
        prrte_list_append(&local_procs, &nm->super);
        /* see if this is a peer - i.e., from the same jobid */
        if (pptr->name.jobid == jdata->jobid) {
            prrte_argv_append_nosize(&micro, PRRTE_VPID_PRINT(pptr->name.vpid));
            if (pptr->name.vpid < vpid) {
                vpid = pptr->name.vpid;
            }
            /* go ahead and register this client - since we are going to wait
             * for register_nspace to complete and the PMIx library serializes
             * the registration requests, we don't need to wait here */
            PRRTE_PMIX_CONVERT_VPID(pproc.rank,  pptr->name.vpid);
            ret = PMIx_server_register_client(&pproc, uid, gid, (void*)pptr, NULL, NULL);
            if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
                PMIX_ERROR_LOG(ret);
            }
        }
    }
    if (NULL != micro) {
        /* pass the local peers */
        kv = PRRTE_NEW(prrte_info_item_t);
        tmp = prrte_argv_join(micro, ',');
        PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_PEERS, tmp, PMIX_STRING);
        free(tmp);
        prrte_argv_free(micro);
        prrte_list_append(info, &kv->super);
    }

    /* pass the local ldr */
    kv = PRRTE_NEW(prrte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALLDR, &vpid, PMIX_PROC_RANK);
    prrte_list_append(info, &kv->super);

    /* for each proc in this job, create an object that
     * includes the info describing the proc so the recipient has a complete
     * picture. This allows procs to connect to each other without
     * any further info exchange, assuming the underlying transports
     * support it. We also pass all the proc-specific data here so
     * that each proc can lookup info about every other proc in the job */

    for (n=0; n < map->nodes->size; n++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, n))) {
            continue;
        }
        /* cycle across each proc on this node, passing all data that
         * varies by proc */
        for (i=0; i < node->procs->size; i++) {
            if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* only consider procs from this job */
            if (pptr->name.jobid != jdata->jobid) {
                continue;
            }
            /* setup the proc map object */
            pmap = PRRTE_NEW(prrte_list_t);

            /* must start with rank */
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_RANK, &pptr->name.vpid, PMIX_PROC_RANK);
            prrte_list_append(pmap, &kv->super);

            /* location, for local procs */
            if (node == mynode) {
                tmp = NULL;
                if (prrte_get_attribute(&pptr->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&tmp, PRRTE_STRING) &&
                    NULL != tmp) {
                    kv = PRRTE_NEW(prrte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, prrte_hwloc_base_get_locality_string(prrte_hwloc_topology, tmp), PMIX_STRING);
                    prrte_list_append(pmap, &kv->super);
                    free(tmp);
                } else {
                    /* the proc is not bound */
                    kv = PRRTE_NEW(prrte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, NULL, PMIX_STRING);
                    prrte_list_append(pmap, &kv->super);
                }
                /* create and pass a proc-level session directory */
                if (0 > prrte_asprintf(&tmp, "%s/%d/%d",
                                       prrte_process_info.jobfam_session_dir,
                                       PRRTE_LOCAL_JOBID(jdata->jobid), pptr->name.vpid)) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                    return PRRTE_ERR_OUT_OF_RESOURCE;
                }
                if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(tmp, S_IRWXU))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PROCDIR, tmp, PMIX_STRING);
                free(tmp);
                prrte_list_append(pmap, &kv->super);
            }

            /* global/univ rank */
            kv = PRRTE_NEW(prrte_info_item_t);
            vpid = pptr->name.vpid + jdata->offset;
            PMIX_INFO_LOAD(&kv->info, PMIX_GLOBAL_RANK, &vpid, PMIX_PROC_RANK);
            prrte_list_append(pmap, &kv->super);

            if (1 < jdata->num_apps) {
                /* appnum */
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_APPNUM, &pptr->app_idx, PMIX_UINT32);
                prrte_list_append(pmap, &kv->super);

                /* app ldr */
                app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, pptr->app_idx);
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_APPLDR, &app->first_rank, PMIX_PROC_RANK);
                prrte_list_append(pmap, &kv->super);

                /* app rank */
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_APP_RANK, &pptr->app_rank, PMIX_PROC_RANK);
                prrte_list_append(pmap, &kv->super);

                /* app size */
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_APP_SIZE, &app->num_procs, PMIX_UINT32);
                prrte_list_append(info, &kv->super);

#if PMIX_NUMERIC_VERSION >= 0x00040000
                app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, pptr->app_idx);
                tmp = NULL;
                if (prrte_get_attribute(&app->attributes, PRRTE_APP_PSET_NAME, (void**)&tmp, PRRTE_STRING) &&
                    NULL != tmp) {
                    kv = PRRTE_NEW(prrte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_PSET_NAME, tmp, PMIX_STRING);
                    free(tmp);
                    prrte_list_append(pmap, &kv->super);
                }
            } else {
                app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0);
                tmp = NULL;
                if (prrte_get_attribute(&app->attributes, PRRTE_APP_PSET_NAME, (void**)&tmp, PRRTE_STRING) &&
                    NULL != tmp) {
                    kv = PRRTE_NEW(prrte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_PSET_NAME, tmp, PMIX_STRING);
                    free(tmp);
                    prrte_list_append(pmap, &kv->super);
                }
#endif
            }

            /* local rank */
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_RANK, &pptr->local_rank, PMIX_UINT16);
            prrte_list_append(pmap, &kv->super);

            /* node rank */
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_NODE_RANK, &pptr->node_rank, PMIX_UINT16);
            prrte_list_append(pmap, &kv->super);

            /* node ID */
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_NODEID, &pptr->node->index, PMIX_UINT32);
            prrte_list_append(pmap, &kv->super);

#if PMIX_NUMERIC_VERSION >= 0x00040000
            /* reincarnation number */
            ui32 = 0;  // we are starting this proc for the first time
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_REINCARNATION, &ui32, PMIX_UINT32);
            prrte_list_append(pmap, &kv->super);
#endif

            if (map->num_nodes < prrte_hostname_cutoff) {
                kv = PRRTE_NEW(prrte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, pptr->node->name, PMIX_STRING);
                prrte_list_append(pmap, &kv->super);
            }
            kv = PRRTE_NEW(prrte_info_item_t);
            PMIX_LOAD_KEY(kv->info.key, PMIX_PROC_DATA);
            kv->info.value.type = PMIX_DATA_ARRAY;
            ninfo = prrte_list_get_size(pmap);
            if (0 < ninfo) {
                PMIX_DATA_ARRAY_CREATE(kv->info.value.data.darray, ninfo, PMIX_INFO);
                pinfo = (pmix_info_t*)kv->info.value.data.darray->array;
                p = 0;
                while (NULL != (kptr = (prrte_info_item_t*)prrte_list_remove_first(pmap))) {
                    PMIX_INFO_XFER(&pinfo[p], &kptr->info);
                    PRRTE_RELEASE(kptr);
                    ++p;
                }
            }
            prrte_list_append(info, &kv->super);
        }
    }

    /* mark the job as registered */
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NSPACE_REGISTERED, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);

    /* pass it down */
    ninfo = prrte_list_get_size(info);
    /* if there are local procs, then we add that here */
    if (0 < (nmsize = prrte_list_get_size(&local_procs))) {
        ++ninfo;
    }
    PMIX_INFO_CREATE(pinfo, ninfo);

#if PMIX_NUMERIC_VERSION >= 0x00040000
    /* first add the local procs, if they are defined */
    if (0 < nmsize) {
        pmix_proc_t *procs;
        PMIX_LOAD_KEY(pinfo[0].key, PMIX_LOCAL_PROCS);
        pinfo[0].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[0].value.data.darray, nmsize, PMIX_PROC);
        procs = (pmix_proc_t*)pinfo[0].value.data.darray->array;
        n = 0;
        PRRTE_LIST_FOREACH(nm, &local_procs, prrte_namelist_t) {
            PRRTE_PMIX_CONVERT_JOBID(procs[n].nspace, nm->name.jobid);
            PRRTE_PMIX_CONVERT_VPID(procs[n].rank, nm->name.vpid);
            ++n;
        }
    }
#endif

    PRRTE_LIST_DESTRUCT(&local_procs);

    /* now load the rest of the list */
    if (0 < nmsize) {
        n = 1;
    } else {
        n = 0;
    }
    PRRTE_LIST_FOREACH(kv, info, prrte_info_item_t) {
        PMIX_INFO_XFER(&pinfo[n], &kv->info);
        ++n;
    }
    PRRTE_LIST_RELEASE(info);

    /* register it */
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(pproc.nspace,
                                      jdata->num_local_procs,
                                      pinfo, ninfo, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prrte_pmix_convert_status(ret);
        PMIX_INFO_FREE(pinfo, ninfo);
        PRRTE_LIST_RELEASE(info);
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
        return rc;
    }
    PRRTE_PMIX_WAIT_THREAD(&lock);
    rc = lock.status;
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    if (PRRTE_SUCCESS != rc) {
        PMIX_INFO_FREE(pinfo, ninfo);
        return rc;
    }

    /* if the user has connected us to an external server, then we must
     * assume there is going to be some cross-mpirun exchange, and so
     * we protect against that situation by publishing the job info
     * for this job - this allows any subsequent "connect" to retrieve
     * the job info */
    if (NULL != prrte_data_server_uri) {
        pmix_data_buffer_t pbkt;
        pmix_byte_object_t pbo;
        uid_t euid;
        pmix_data_range_t range = PMIX_RANGE_SESSION;
        pmix_persistence_t persist = PMIX_PERSIST_APP;

        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
        ret = PMIx_Data_pack(&pproc, &pbkt, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prrte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            return rc;
        }
        ret = PMIx_Data_pack(&pproc, &pbkt, pinfo, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prrte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }
        PMIX_INFO_FREE(pinfo, ninfo);
        PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);

        ninfo = 4;
        PMIX_INFO_CREATE(pinfo, ninfo);

        /* first pass the packed values with a key of the nspace */
        n=0;
        PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, PRRTE_PROC_MY_NAME->jobid);
        PMIX_INFO_LOAD(&pinfo[n], pproc.nspace, &pbo, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        ++n;

        /* set the range to be session */
        PMIX_INFO_LOAD(&pinfo[n], PMIX_RANGE, &range, PMIX_DATA_RANGE);
        ++n;

        /* set the persistence to be app */
        PMIX_INFO_LOAD(&pinfo[n], PMIX_PERSISTENCE, &persist, PMIX_PERSIST);
        ++n;

        /* add our effective userid to the directives */
        euid = geteuid();
        PMIX_INFO_LOAD(&pinfo[n], PMIX_USERID, &euid, PMIX_UINT32);
        ++n;

        /* now publish it */
        PRRTE_PMIX_CONVERT_NAME(&pproc, PRRTE_PROC_MY_NAME);
        PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
        if (PMIX_SUCCESS != (ret = pmix_server_publish_fn(&pproc, pinfo, ninfo, opcbfunc, &lock))) {
            PMIX_ERROR_LOG(ret);
            rc = prrte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            PRRTE_LIST_RELEASE(info);
            PRRTE_PMIX_DESTRUCT_LOCK(&lock);
            return rc;
        }
        PRRTE_PMIX_WAIT_THREAD(&lock);
        rc = lock.status;
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    }
    PMIX_INFO_FREE(pinfo, ninfo);

    return rc;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;

    lock->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}
