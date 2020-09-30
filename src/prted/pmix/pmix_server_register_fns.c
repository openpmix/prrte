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
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
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

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <pmix_server.h>

#include "prte_stdint.h"
#include "types.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/os_dirpath.h"
#include "src/util/error.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/util/name_fns.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/prted/pmix/pmix_server.h"

static void opcbfunc(pmix_status_t status, void *cbdata);

/* stuff proc attributes for sending back to a proc */
int prte_pmix_server_register_nspace(prte_job_t *jdata)
{
    int rc;
    prte_proc_t *pptr;
    int i, k, n, p;
    prte_list_t *info, *pmap, nodeinfo, appinfo;
    prte_info_item_t *kv, *kptr;
    prte_info_array_item_t *iarray;
    prte_node_t *node;
    prte_vpid_t vpid;
    char **list, **procs, **micro, *tmp, *regex;
    prte_job_map_t *map;
    prte_app_context_t *app;
    uid_t uid;
    gid_t gid;
    prte_list_t *cache;
    hwloc_obj_t machine;
    pmix_proc_t pproc;
    pmix_status_t ret;
    pmix_info_t *pinfo, *iptr;
    size_t ninfo;
    prte_pmix_lock_t lock;
    prte_list_t local_procs;
    prte_namelist_t *nm;
    size_t nmsize;
#if PMIX_NUMERIC_VERSION >= 0x00040000
    pmix_server_pset_t *pset;
    pmix_cpuset_t cpuset;
#endif
    prte_value_t *val;
    uint32_t ui32;

    prte_output_verbose(2, prte_pmix_server_globals.output,
                        "%s register nspace for %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_JOBID_PRINT(jdata->jobid));

    /* setup the info list */
    info = PRTE_NEW(prte_list_t);
    PRTE_CONSTRUCT(&nodeinfo, prte_list_t);
    PRTE_CONSTRUCT(&appinfo, prte_list_t);
    uid = geteuid();
    gid = getegid();

    /* pass our nspace/rank */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_LOAD_KEY(kv->info.key, PMIX_SERVER_NSPACE);
    kv->info.value.type = PMIX_PROC;
    /* have to stringify the jobid */
    PMIX_PROC_CREATE(kv->info.value.data.proc, 1);
    PMIX_LOAD_NSPACE(kv->info.value.data.proc->nspace, prte_process_info.myproc.nspace);
    prte_list_append(info, &kv->super);

    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_SERVER_RANK, &prte_process_info.myproc.rank, PMIX_PROC_RANK);
    prte_list_append(info, &kv->super);

    /* jobid */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_LOAD_KEY(kv->info.key, PMIX_JOBID);
    kv->info.value.type = PMIX_PROC;
    PMIX_PROC_CREATE(kv->info.value.data.proc, 1);
    PMIX_LOAD_NSPACE(kv->info.value.data.proc->nspace, jdata->nspace);
    prte_list_append(info, &kv->super);

    /* offset */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NPROC_OFFSET, &jdata->offset, PMIX_PROC_RANK);
    prte_list_append(info, &kv->super);

    /* check for cached values to add to the job info */
    cache = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, (void**)&cache, PRTE_PTR) &&
        NULL != cache) {
        while (NULL != (val = (prte_value_t*)prte_list_remove_first(cache))) {
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_LOAD_KEY(kv->info.key, val->key);
            prte_pmix_value_load(&kv->info.value, val);
            prte_list_append(info, &kv->super);
            PRTE_RELEASE(val);
        }
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE);
        PRTE_RELEASE(cache);
    }

    /* assemble the node and proc map info */
    list = NULL;
    procs = NULL;
    map = jdata->map;
    PMIX_LOAD_NSPACE(pproc.nspace, jdata->nspace);
    PRTE_CONSTRUCT(&local_procs, prte_list_t);
    for (i=0; i < map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, i))) {
            micro = NULL;
            tmp = NULL;
            vpid = PRTE_VPID_MAX;
            ui32 = 0;
            prte_argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k=0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, k))) {
                    if (jdata->jobid == pptr->name.jobid) {
                        prte_argv_append_nosize(&micro, PRTE_VPID_PRINT(pptr->name.vpid));
                        if (pptr->name.vpid < vpid) {
                            vpid = pptr->name.vpid;
                        }
                        ++ui32;
                    }
                    if (PRTE_PROC_MY_NAME->vpid == node->daemon->name.vpid) {
                        /* track all procs on our node */
                        nm = PRTE_NEW(prte_namelist_t);
                        nm->name.jobid = pptr->name.jobid;
                        nm->name.vpid = pptr->name.vpid;
                        prte_list_append(&local_procs, &nm->super);
                        if (jdata->jobid == pptr->name.jobid) {
                            /* go ahead and register this client - since we are going to wait
                             * for register_nspace to complete and the PMIx library serializes
                             * the registration requests, we don't need to wait here */
                            PRTE_PMIX_CONVERT_VPID(pproc.rank,  pptr->name.vpid);
                            ret = PMIx_server_register_client(&pproc, uid, gid, (void*)pptr, NULL, NULL);
                            if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
                                PMIX_ERROR_LOG(ret);
                            }
                        }
                    }
                }
            }
            /* assemble the rank/node map */
            if (NULL != micro) {
                tmp = prte_argv_join(micro, ',');
                prte_argv_free(micro);
                prte_argv_append_nosize(&procs, tmp);
            }
            /* construct the node info array */
            iarray = PRTE_NEW(prte_info_array_item_t);
            /* start with the hostname */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, node->name, PMIX_STRING);
            prte_list_append(&iarray->infolist, &kv->super);
#ifdef PMIX_HOSTNAME_ALIASES
            /* add any aliases */
            if (prte_get_attribute(&node->attributes, PRTE_NODE_ALIAS, (void**)&regex, PRTE_STRING) &&
                NULL != regex) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_ALIASES, regex, PMIX_STRING);
                prte_list_append(&iarray->infolist, &kv->super);
                free(regex);
            }
#endif
            /* pass the node ID */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_NODEID, &node->index, PMIX_UINT32);
            prte_list_append(&iarray->infolist, &kv->super);
            /* add node size */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_NODE_SIZE, &node->num_procs, PMIX_UINT32);
            prte_list_append(&iarray->infolist, &kv->super);
            /* add local size for this job */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_SIZE, &ui32, PMIX_UINT32);
            prte_list_append(&iarray->infolist, &kv->super);
            /* pass the local ldr */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_LOCALLDR, &vpid, PMIX_PROC_RANK);
            prte_list_append(&iarray->infolist, &kv->super);
            /* add the local peers */
            if (NULL != tmp) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_PEERS, tmp, PMIX_STRING);
                prte_list_append(&iarray->infolist, &kv->super);
                free(tmp);
            }
            /* add to the overall payload */
            prte_list_append(&nodeinfo, &iarray->super);
        }
    }
    /* let the PMIx server generate the nodemap regex */
    if (NULL != list) {
        tmp = prte_argv_join(list, ',');
        prte_argv_free(list);
        list = NULL;
        if (PRTE_SUCCESS != (rc = PMIx_generate_regex(tmp, &regex))) {
            PRTE_ERROR_LOG(rc);
            free(tmp);
            PRTE_LIST_RELEASE(info);
            return rc;
        }
        free(tmp);
        kv = PRTE_NEW(prte_info_item_t);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&kv->info, PMIX_NODE_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_NODE_MAP, regex, PMIX_STRING);
#endif
        free(regex);
        prte_list_append(info, &kv->super);
    }

    /* let the PMIx server generate the procmap regex */
    if (NULL != procs) {
        tmp = prte_argv_join(procs, ';');
        prte_argv_free(procs);
        procs = NULL;
        if (PRTE_SUCCESS != (rc = PMIx_generate_ppn(tmp, &regex))) {
            PRTE_ERROR_LOG(rc);
            free(tmp);
            PRTE_LIST_RELEASE(info);
            return rc;
        }
        free(tmp);
        kv = PRTE_NEW(prte_info_item_t);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&kv->info, PMIX_PROC_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_PROC_MAP, regex, PMIX_STRING);
#endif
        free(regex);
        prte_list_append(info, &kv->super);
    }

    /* pass the number of nodes in the job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NUM_NODES, &map->num_nodes, PMIX_UINT32);
    prte_list_append(info, &kv->super);

    /* univ size */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_UNIV_SIZE, &jdata->total_slots_alloc, PMIX_UINT32);
    prte_list_append(info, &kv->super);

    /* job size */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_JOB_SIZE, &jdata->num_procs, PMIX_UINT32);
    prte_list_append(info, &kv->super);

    /* number of apps in this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_JOB_NUM_APPS, &jdata->num_apps, PMIX_UINT32);
    prte_list_append(info, &kv->super);

    /* max procs */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_MAX_PROCS, &jdata->total_slots_alloc, PMIX_UINT32);
    prte_list_append(info, &kv->super);

    /* topology signature */
    kv = PRTE_NEW(prte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V1, prte_topo_signature, PMIX_STRING);
#else
    PMIX_INFO_LOAD(&kv->info, PMIX_HWLOC_XML_V2, prte_topo_signature, PMIX_STRING);
#endif
    prte_list_append(info, &kv->super);

    /* total available physical memory */
    machine = hwloc_get_next_obj_by_type (prte_hwloc_topology, HWLOC_OBJ_MACHINE, NULL);
    if (NULL != machine) {
        kv = PRTE_NEW(prte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->memory.total_memory, PMIX_UINT64);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->total_memory, PMIX_UINT64);
#endif
        prte_list_append(info, &kv->super);
    }

    /* pass the mapping policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_MAPBY, prte_rmaps_base_print_mapping(jdata->map->mapping), PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* pass the ranking policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_RANKBY, prte_rmaps_base_print_ranking(jdata->map->ranking), PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* pass the binding policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_BINDTO, prte_hwloc_base_print_binding(jdata->map->binding), PMIX_STRING);
    prte_list_append(info, &kv->super);

#ifdef PMIX_HOSTNAME_KEEP_FQDN
    /* tell the user what we did with FQDN */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_KEEP_FQDN, &prte_keep_fqdn_hostnames, PMIX_BOOL);
    prte_list_append(info, &kv->super);
#endif

    /* pass the top-level session directory - this is our jobfam session dir */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_TMPDIR, prte_process_info.jobfam_session_dir, PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* create and pass a job-level session directory */
    if (0 > prte_asprintf(&tmp, "%s/%d", prte_process_info.jobfam_session_dir, PRTE_LOCAL_JOBID(jdata->jobid))) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (PRTE_SUCCESS != (rc = prte_os_dirpath_create(prte_process_info.jobfam_session_dir, S_IRWXU))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NSDIR, tmp, PMIX_STRING);
    free(tmp);
    prte_list_append(info, &kv->super);

    /* for each app in the job, create an app-array */
    for (n=0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        iarray = PRTE_NEW(prte_info_array_item_t);
        /* start with the app number */
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_APPNUM, &n, PMIX_UINT32);
        prte_list_append(&iarray->infolist, &kv->super);
        /* add the app size */
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_APP_SIZE, &app->num_procs, PMIX_UINT32);
        prte_list_append(&iarray->infolist, &kv->super);
        /* add the app leader */
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_APPLDR, &app->first_rank, PMIX_PROC_RANK);
        prte_list_append(&iarray->infolist, &kv->super);
        /* add the wdir */
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_WDIR, app->cwd, PMIX_STRING);
        prte_list_append(&iarray->infolist, &kv->super);
#if PMIX_NUMERIC_VERSION >= 0x00040000
        /* add the argv */
        tmp = prte_argv_join(app->argv, ' ');
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_APP_ARGV, tmp, PMIX_STRING);
        free(tmp);
        prte_list_append(&iarray->infolist, &kv->super);
        /* add the pset name */
        tmp = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_PSET_NAME, (void**)&tmp, PRTE_STRING) &&
            NULL != tmp) {
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_PSET_NAME, tmp, PMIX_STRING);
            prte_list_append(&iarray->infolist, &kv->super);
            /* register it */
            pset = PRTE_NEW(pmix_server_pset_t);
            pset->name = strdup(tmp);
            prte_list_append(&prte_pmix_server_globals.psets, &pset->super);
            free(tmp);
        }
#endif
        /* add to the main payload */
        prte_list_append(&appinfo, &iarray->super);
    }
    /* for each proc in this job, create an object that
     * includes the info describing the proc so the recipient has a complete
     * picture. This allows procs to connect to each other without
     * any further info exchange, assuming the underlying transports
     * support it. We also pass all the proc-specific data here so
     * that each proc can lookup info about every other proc in the job */

    for (n=0; n < map->nodes->size; n++) {
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, n))) {
            continue;
        }
        /* cycle across each proc on this node, passing all data that
         * varies by proc */
        for (i=0; i < node->procs->size; i++) {
            if (NULL == (pptr = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* only consider procs from this job */
            if (pptr->name.jobid != jdata->jobid) {
                continue;
            }
            /* setup the proc map object */
            pmap = PRTE_NEW(prte_list_t);

            /* must start with rank */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_RANK, &pptr->name.vpid, PMIX_PROC_RANK);
            prte_list_append(pmap, &kv->super);

            /* location, for local procs */
            if (PRTE_PROC_MY_NAME->vpid == node->daemon->name.vpid) {
                tmp = NULL;
                if (prte_get_attribute(&pptr->attributes, PRTE_PROC_CPU_BITMAP, (void**)&tmp, PRTE_STRING) &&
                    NULL != tmp) {
#if PMIX_NUMERIC_VERSION >= 0x00040000
                    /* provide the cpuset string for this proc */
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_CPUSET, tmp, PMIX_STRING);
                    prte_list_append(pmap, &kv->super);
                    /* let PMIx generate the locality string */
                    PMIX_CPUSET_CONSTRUCT(&cpuset);
                    cpuset.source = "hwloc";
                    cpuset.bitmap = hwloc_bitmap_alloc();
                    hwloc_bitmap_list_sscanf(cpuset.bitmap, tmp);
                    free(tmp);
                    ret = PMIx_server_generate_locality_string(&cpuset, &tmp);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PRTE_LIST_RELEASE(pmap);
                        PRTE_LIST_DESTRUCT(&appinfo);
                        PRTE_LIST_RELEASE(info);
                        return prte_pmix_convert_status(ret);
                    }
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, tmp, PMIX_STRING);
                    prte_list_append(pmap, &kv->super);
                    free(tmp);
#else
                    /* generate the locality string ourselves */
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, prte_hwloc_base_get_locality_string(prte_hwloc_topology, tmp), PMIX_STRING);
                    prte_list_append(pmap, &kv->super);
                    /* and also provide the cpuset string for this proc */
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_CPUSET, tmp, PMIX_STRING);
                    prte_list_append(pmap, &kv->super);
                    free(tmp);
#endif
                } else {
                    /* the proc is not bound */
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, NULL, PMIX_STRING);
                    prte_list_append(pmap, &kv->super);
                }
                /* debugger daemons and tools don't get session directories */
                if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
                    !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                    /* create and pass a proc-level session directory */
                    if (0 > prte_asprintf(&tmp, "%s/%d/%d",
                                           prte_process_info.jobfam_session_dir,
                                           PRTE_LOCAL_JOBID(jdata->jobid), pptr->name.vpid)) {
                        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                        return PRTE_ERR_OUT_OF_RESOURCE;
                    }
                    if (PRTE_SUCCESS != (rc = prte_os_dirpath_create(tmp, S_IRWXU))) {
                        PRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    kv = PRTE_NEW(prte_info_item_t);
                    PMIX_INFO_LOAD(&kv->info, PMIX_PROCDIR, tmp, PMIX_STRING);
                    free(tmp);
                    prte_list_append(pmap, &kv->super);
                }
            }

            /* global/univ rank */
            kv = PRTE_NEW(prte_info_item_t);
            vpid = pptr->name.vpid + jdata->offset;
            PMIX_INFO_LOAD(&kv->info, PMIX_GLOBAL_RANK, &vpid, PMIX_PROC_RANK);
            prte_list_append(pmap, &kv->super);

            /* appnum */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_APPNUM, &pptr->app_idx, PMIX_UINT32);
            prte_list_append(pmap, &kv->super);

            /* app rank */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_APP_RANK, &pptr->app_rank, PMIX_PROC_RANK);
            prte_list_append(pmap, &kv->super);

            /* local rank */
            if (PRTE_LOCAL_RANK_INVALID != pptr->local_rank) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_LOCAL_RANK, &pptr->local_rank, PMIX_UINT16);
                prte_list_append(pmap, &kv->super);
            }

            /* node rank */
            if (PRTE_NODE_RANK_INVALID != pptr->node_rank) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_NODE_RANK, &pptr->node_rank, PMIX_UINT16);
                prte_list_append(pmap, &kv->super);
            }

            /* node ID */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_NODEID, &pptr->node->index, PMIX_UINT32);
            prte_list_append(pmap, &kv->super);

#if PMIX_NUMERIC_VERSION >= 0x00040000
            /* reincarnation number */
            ui32 = 0;  // we are starting this proc for the first time
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_REINCARNATION, &ui32, PMIX_UINT32);
            prte_list_append(pmap, &kv->super);
#endif

            if (map->num_nodes < prte_hostname_cutoff) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME, pptr->node->name, PMIX_STRING);
                prte_list_append(pmap, &kv->super);
            }
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_LOAD_KEY(kv->info.key, PMIX_PROC_DATA);
            kv->info.value.type = PMIX_DATA_ARRAY;
            ninfo = prte_list_get_size(pmap);
            if (0 < ninfo) {
                PMIX_DATA_ARRAY_CREATE(kv->info.value.data.darray, ninfo, PMIX_INFO);
                pinfo = (pmix_info_t*)kv->info.value.data.darray->array;
                p = 0;
                while (NULL != (kptr = (prte_info_item_t*)prte_list_remove_first(pmap))) {
                    PMIX_INFO_XFER(&pinfo[p], &kptr->info);
                    PRTE_RELEASE(kptr);
                    ++p;
                }
            }
            prte_list_append(info, &kv->super);
        }
    }

    /* mark the job as registered */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_NSPACE_REGISTERED, PRTE_ATTR_LOCAL, NULL, PRTE_BOOL);

    /* pass it down */
    ninfo = prte_list_get_size(info) + prte_list_get_size(&nodeinfo) + prte_list_get_size(&appinfo);
    /* if there are local procs, then we add that here */
    if (0 < (nmsize = prte_list_get_size(&local_procs))) {
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
        PRTE_LIST_FOREACH(nm, &local_procs, prte_namelist_t) {
            PRTE_PMIX_CONVERT_JOBID(rc, procs[n].nspace, nm->name.jobid);
            PRTE_PMIX_CONVERT_VPID(procs[n].rank, nm->name.vpid);
            ++n;
        }
    }
#endif

    PRTE_LIST_DESTRUCT(&local_procs);

    /* now load the rest of the job info */
    if (0 < nmsize) {
        n = 1;
    } else {
        n = 0;
    }
    PRTE_LIST_FOREACH(kv, info, prte_info_item_t) {
        PMIX_INFO_XFER(&pinfo[n], &kv->info);
        ++n;
    }
    PRTE_LIST_RELEASE(info);

    /* now load the node info */
    PRTE_LIST_FOREACH(iarray, &nodeinfo, prte_info_array_item_t) {
        nmsize = prte_list_get_size(&iarray->infolist);
        PMIX_LOAD_KEY(pinfo[n].key, PMIX_NODE_INFO_ARRAY);
        pinfo[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[n].value.data.darray, nmsize, PMIX_INFO);
        iptr = (pmix_info_t*)pinfo[n].value.data.darray->array;
        k=0;
        PRTE_LIST_FOREACH(kv, &iarray->infolist, prte_info_item_t) {
            PMIX_INFO_XFER(&iptr[k], &kv->info);
            ++k;
        }
        ++n;
    }
    PRTE_LIST_DESTRUCT(&nodeinfo);

    /* now load the app info */
    PRTE_LIST_FOREACH(iarray, &appinfo, prte_info_array_item_t) {
        nmsize = prte_list_get_size(&iarray->infolist);
        PMIX_LOAD_KEY(pinfo[n].key, PMIX_APP_INFO_ARRAY);
        pinfo[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[n].value.data.darray, nmsize, PMIX_INFO);
        iptr = (pmix_info_t*)pinfo[n].value.data.darray->array;
        k=0;
        PRTE_LIST_FOREACH(kv, &iarray->infolist, prte_info_item_t) {
            PMIX_INFO_XFER(&iptr[k], &kv->info);
            ++k;
        }
        ++n;
    }
    PRTE_LIST_DESTRUCT(&appinfo);

    /* register it */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(pproc.nspace,
                                      jdata->num_local_procs,
                                      pinfo, ninfo, opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        PMIX_INFO_FREE(pinfo, ninfo);
        PRTE_LIST_RELEASE(info);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        return rc;
    }
    PRTE_PMIX_WAIT_THREAD(&lock);
    rc = lock.status;
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    if (PRTE_SUCCESS != rc) {
        PMIX_INFO_FREE(pinfo, ninfo);
        return rc;
    }

    /* if the user has connected us to an external server, then we must
     * assume there is going to be some cross-mpirun exchange, and so
     * we protect against that situation by publishing the job info
     * for this job - this allows any subsequent "connect" to retrieve
     * the job info */
    if (NULL != prte_data_server_uri) {
        pmix_data_buffer_t pbkt;
        pmix_byte_object_t pbo;
        uid_t euid;
        pmix_data_range_t range = PMIX_RANGE_SESSION;
        pmix_persistence_t persist = PMIX_PERSIST_APP;

        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
        ret = PMIx_Data_pack(&pproc, &pbkt, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            return rc;
        }
        ret = PMIx_Data_pack(&pproc, &pbkt, pinfo, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
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
        PMIX_INFO_LOAD(&pinfo[n], prte_process_info.myproc.nspace, &pbo, PMIX_BYTE_OBJECT);
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
        PRTE_PMIX_CONSTRUCT_LOCK(&lock);
        if (PMIX_SUCCESS != (ret = pmix_server_publish_fn(&prte_process_info.myproc, pinfo, ninfo, opcbfunc, &lock))) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            PRTE_LIST_RELEASE(info);
            PRTE_PMIX_DESTRUCT_LOCK(&lock);
            return rc;
        }
        PRTE_PMIX_WAIT_THREAD(&lock);
        rc = lock.status;
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
    }
    PMIX_INFO_FREE(pinfo, ninfo);

    return rc;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;

    lock->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}
