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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <fcntl.h>
#include <pmix_server.h>

#include "prte_stdint.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "types.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

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
    pmix_rank_t vpid;
    char **list, **procs, **micro, *tmp, *regex;
    prte_job_map_t *map;
    prte_app_context_t *app;
    uid_t uid;
    gid_t gid;
    prte_list_t *cache;
    hwloc_obj_t machine;
    pmix_proc_t pproc, *parentproc;
    pmix_status_t ret;
    pmix_info_t *pinfo, *iptr, devinfo[2];
    size_t ninfo;
    prte_pmix_lock_t lock;
    prte_list_t local_procs;
    prte_namelist_t *nm;
    size_t nmsize;
    pmix_server_pset_t *pset;
    pmix_cpuset_t cpuset;
    uint32_t ui32;
    prte_job_t *parent = NULL;
    pmix_device_distance_t *distances;
    size_t ndist;
    pmix_topology_t topo;

    prte_output_verbose(2, prte_pmix_server_globals.output, "%s register nspace for %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace));

    /* setup the info list */
    info = PRTE_NEW(prte_list_t);
    PRTE_CONSTRUCT(&nodeinfo, prte_list_t);
    PRTE_CONSTRUCT(&appinfo, prte_list_t);
    uid = geteuid();
    gid = getegid();
    topo.source = "hwloc";

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
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, (void **) &cache, PMIX_POINTER)
        && NULL != cache) {
        while (NULL != (kv = (prte_info_item_t *) prte_list_remove_first(cache))) {
            prte_list_append(info, &kv->super);
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
    for (i = 0; i < map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, i))) {
            micro = NULL;
            tmp = NULL;
            vpid = PMIX_RANK_VALID;
            ui32 = 0;
            prte_argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k = 0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prte_proc_t *) prte_pointer_array_get_item(node->procs, k))) {
                    if (PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                        prte_argv_append_nosize(&micro, PRTE_VPID_PRINT(pptr->name.rank));
                        if (pptr->name.rank < vpid) {
                            vpid = pptr->name.rank;
                        }
                        ++ui32;
                    }
                    if (PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                        /* track all procs on our node */
                        nm = PRTE_NEW(prte_namelist_t);
                        PMIX_LOAD_PROCID(&nm->name, pptr->name.nspace, pptr->name.rank);
                        prte_list_append(&local_procs, &nm->super);
                        if (PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                            /* go ahead and register this client - since we are going to wait
                             * for register_nspace to complete and the PMIx library serializes
                             * the registration requests, we don't need to wait here */
                            ret = PMIx_server_register_client(&pptr->name, uid, gid,
                                                              (void*)pptr, NULL, NULL);
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
            /* add any aliases */
            if (NULL != node->aliases) {
                regex = pmix_argv_join(node->aliases, ',');
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_ALIASES, regex, PMIX_STRING);
                prte_list_append(&iarray->infolist, &kv->super);
                free(regex);
            }
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
            /* if oversubscribed, mark it */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_OVERSUBSCRIBED)) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_NODE_OVERSUBSCRIBED, NULL, PMIX_BOOL);
                prte_list_append(&iarray->infolist, &kv->super);
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
        PMIX_INFO_LOAD(&kv->info, PMIX_NODE_MAP, regex, PMIX_REGEX);
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
        PMIX_INFO_LOAD(&kv->info, PMIX_PROC_MAP, regex, PMIX_REGEX);
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
    machine = hwloc_get_next_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_MACHINE, NULL);
    if (NULL != machine) {
        kv = PRTE_NEW(prte_info_item_t);
#if HWLOC_API_VERSION < 0x20000
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->memory.total_memory,
                       PMIX_UINT64);
#else
        PMIX_INFO_LOAD(&kv->info, PMIX_AVAIL_PHYS_MEMORY, &machine->total_memory, PMIX_UINT64);
#endif
        prte_list_append(info, &kv->super);
    }

    /* pass the mapping policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_MAPBY, prte_rmaps_base_print_mapping(jdata->map->mapping),
                   PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* pass the ranking policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_RANKBY, prte_rmaps_base_print_ranking(jdata->map->ranking),
                   PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* pass the binding policy used for this job */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_BINDTO, prte_hwloc_base_print_binding(jdata->map->binding),
                   PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* tell the user what we did with FQDN */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_HOSTNAME_KEEP_FQDN, &prte_keep_fqdn_hostnames, PMIX_BOOL);
    prte_list_append(info, &kv->super);

    /* pass the top-level session directory - this is our jobfam session dir */
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_TMPDIR, prte_process_info.jobfam_session_dir, PMIX_STRING);
    prte_list_append(info, &kv->super);

    /* create and pass a job-level session directory */
    if (0 > prte_asprintf(&tmp, "%s/%u", prte_process_info.jobfam_session_dir,
                          PRTE_LOCAL_JOBID(jdata->nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (PRTE_SUCCESS
        != (rc = prte_os_dirpath_create(prte_process_info.jobfam_session_dir, S_IRWXU))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    kv = PRTE_NEW(prte_info_item_t);
    PMIX_INFO_LOAD(&kv->info, PMIX_NSDIR, tmp, PMIX_STRING);
    free(tmp);
    prte_list_append(info, &kv->super);

    /* check for output directives */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_IOF_TAG_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_RANK_OUTPUT, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_IOF_RANK_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_IOF_TIMESTAMP_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_IOF_XML_OUTPUT, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }
    tmp = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_FILE, (void **) &tmp, PMIX_STRING)
        && NULL != tmp) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_OUTPUT_TO_FILE, tmp, PMIX_STRING);
        prte_list_append(info, &kv->super);
        free(tmp);
    }
    tmp = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_DIRECTORY, (void **) &tmp, PMIX_STRING)
        && NULL != tmp) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_OUTPUT_TO_DIRECTORY, tmp, PMIX_STRING);
        prte_list_append(info, &kv->super);
        free(tmp);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_NOCOPY, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_OUTPUT_NOCOPY, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, NULL, PMIX_BOOL)) {
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_MERGE_STDERR_STDOUT, NULL, PMIX_BOOL);
        prte_list_append(info, &kv->super);
    }

    /* for each app in the job, create an app-array */
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) prte_pointer_array_get_item(jdata->apps, n))) {
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
        /* add the argv */
        tmp = prte_argv_join(app->argv, ' ');
        kv = PRTE_NEW(prte_info_item_t);
        PMIX_INFO_LOAD(&kv->info, PMIX_APP_ARGV, tmp, PMIX_STRING);
        free(tmp);
        prte_list_append(&iarray->infolist, &kv->super);
        /* add the pset name */
        tmp = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_PSET_NAME, (void **) &tmp, PMIX_STRING)
            && NULL != tmp) {
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_PSET_NAME, tmp, PMIX_STRING);
            prte_list_append(&iarray->infolist, &kv->super);
            /* register it */
            pset = PRTE_NEW(pmix_server_pset_t);
            pset->name = strdup(tmp);
            prte_list_append(&prte_pmix_server_globals.psets, &pset->super);
            free(tmp);
        }
        /* add to the main payload */
        prte_list_append(&appinfo, &iarray->super);
    }

    /* get the parent job that spawned this one */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &parentproc, PMIX_PROC)) {
        parent = prte_get_job_data_object(parentproc->nspace);
        if (NULL != parent && PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, parent->nspace)) {
            PMIX_PROC_RELEASE(parentproc);
            parent = NULL;
        }
    }

    /* for each proc in this job, create an object that
     * includes the info describing the proc so the recipient has a complete
     * picture. This allows procs to connect to each other without
     * any further info exchange, assuming the underlying transports
     * support it. We also pass all the proc-specific data here so
     * that each proc can lookup info about every other proc in the job */
    if (0 != prte_pmix_server_globals.generate_dist) {
        PMIX_INFO_LOAD(&devinfo[0], PMIX_DEVICE_TYPE, &prte_pmix_server_globals.generate_dist, PMIX_DEVTYPE);
        PMIX_INFO_LOAD(&devinfo[1], PMIX_HOSTNAME, NULL, PMIX_STRING);
    }

    for (n = 0; n < map->nodes->size; n++) {
        if (NULL == (node = (prte_node_t *) prte_pointer_array_get_item(map->nodes, n))) {
            continue;
        }
        /* cycle across each proc on this node, passing all data that
         * varies by proc */
        for (i = 0; i < node->procs->size; i++) {
            if (NULL == (pptr = (prte_proc_t *) prte_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* only consider procs from this job */
            if (!PMIX_CHECK_NSPACE(pptr->name.nspace, jdata->nspace)) {
                continue;
            }
            /* setup the proc map object */
            pmap = PRTE_NEW(prte_list_t);

            /* must start with rank */
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_RANK, &pptr->name.rank, PMIX_PROC_RANK);
            prte_list_append(pmap, &kv->super);

            /* location, for local procs */
            tmp = NULL;
            if (prte_get_attribute(&pptr->attributes, PRTE_PROC_CPU_BITMAP,
                                   (void **) &tmp, PMIX_STRING)
                && NULL != tmp) {
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
                    hwloc_bitmap_free(cpuset.bitmap);
                    return prte_pmix_convert_status(ret);
                }
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, tmp, PMIX_STRING);
                prte_list_append(pmap, &kv->super);
                free(tmp);
                if (0 != prte_pmix_server_globals.generate_dist) {
                    /* compute the device distances for this proc */
                    topo.topology = node->topology->topo;
                    devinfo[1].value.data.string = node->name;
                    ret = PMIx_Compute_distances(&topo, &cpuset,
                                                 devinfo, 2, &distances, &ndist);
                    devinfo[1].value.data.string = NULL;
                    if (PMIX_SUCCESS == ret) {
                        if (4 < prte_output_get_verbosity(prte_pmix_server_globals.output)) {
                            size_t f;
                            for (f=0; f < ndist; f++) {
                                prte_output(0, "UUID: %s OSNAME: %s TYPE: %s MIND: %u MAXD: %u",
                                            distances[f].uuid, distances[f].osname,
                                            PMIx_Device_type_string(distances[f].type),
                                            distances[f].mindist, distances[f].maxdist);
                            }
                        }
                        kv = PRTE_NEW(prte_info_item_t);
                        PMIX_LOAD_KEY(kv->info.key, PMIX_DEVICE_DISTANCES);
                        kv->info.value.type = PMIX_DATA_ARRAY;
                        PMIX_DATA_ARRAY_CREATE(kv->info.value.data.darray, ndist, PMIX_DEVICE_DIST);
                        kv->info.value.data.darray->array = distances;
                        prte_list_append(pmap, &kv->super);
                    }
                }
                hwloc_bitmap_free(cpuset.bitmap);
            } else {
                /* the proc is not bound */
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_LOCALITY_STRING, NULL, PMIX_STRING);
                prte_list_append(pmap, &kv->super);
            }
            if (PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                /* create and pass a proc-level session directory */
                if (0 > prte_asprintf(&tmp, "%s/%u/%u", prte_process_info.jobfam_session_dir,
                                      PRTE_LOCAL_JOBID(jdata->nspace), pptr->name.rank)) {
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

            /* global/univ rank */
            kv = PRTE_NEW(prte_info_item_t);
            vpid = pptr->name.rank + jdata->offset;
            PMIX_INFO_LOAD(&kv->info, PMIX_GLOBAL_RANK, &vpid, PMIX_PROC_RANK);
            prte_list_append(pmap, &kv->super);

            /* parent ID, if we were spawned by a non-tool */
            if (NULL != parent) {
                kv = PRTE_NEW(prte_info_item_t);
                PMIX_INFO_LOAD(&kv->info, PMIX_PARENT_ID, parentproc, PMIX_PROC);
                prte_list_append(pmap, &kv->super);
            }

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

            /* reincarnation number */
            ui32 = 0; // we are starting this proc for the first time
            kv = PRTE_NEW(prte_info_item_t);
            PMIX_INFO_LOAD(&kv->info, PMIX_REINCARNATION, &ui32, PMIX_UINT32);
            prte_list_append(pmap, &kv->super);

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
                pinfo = (pmix_info_t *) kv->info.value.data.darray->array;
                p = 0;
                while (NULL != (kptr = (prte_info_item_t *) prte_list_remove_first(pmap))) {
                    PMIX_INFO_XFER(&pinfo[p], &kptr->info);
                    PRTE_RELEASE(kptr);
                    ++p;
                }
            }
            prte_list_append(info, &kv->super);
        }
    }
    if (NULL != parent) {
        PMIX_PROC_RELEASE(parentproc);
    }
    PMIX_INFO_DESTRUCT(&devinfo[0]);
    PMIX_INFO_DESTRUCT(&devinfo[1]);

    /* mark the job as registered */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_NSPACE_REGISTERED, PRTE_ATTR_LOCAL, NULL,
                       PMIX_BOOL);

    /* pass it down */
    ninfo = prte_list_get_size(info) + prte_list_get_size(&nodeinfo) + prte_list_get_size(&appinfo);
    /* if there are local procs, then we add that here */
    if (0 < (nmsize = prte_list_get_size(&local_procs))) {
        ++ninfo;
    }
    PMIX_INFO_CREATE(pinfo, ninfo);

    /* first add the local procs, if they are defined */
    if (0 < nmsize) {
        pmix_proc_t *procs_tmp;
        PMIX_LOAD_KEY(pinfo[0].key, PMIX_LOCAL_PROCS);
        pinfo[0].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[0].value.data.darray, nmsize, PMIX_PROC);
        procs_tmp = (pmix_proc_t *) pinfo[0].value.data.darray->array;
        n = 0;
        PRTE_LIST_FOREACH(nm, &local_procs, prte_namelist_t)
        {
            PMIX_LOAD_PROCID(&procs_tmp[n], nm->name.nspace, nm->name.rank);
            ++n;
        }
    }

    PRTE_LIST_DESTRUCT(&local_procs);

    /* now load the rest of the job info */
    if (0 < nmsize) {
        n = 1;
    } else {
        n = 0;
    }
    PRTE_LIST_FOREACH(kv, info, prte_info_item_t)
    {
        PMIX_INFO_XFER(&pinfo[n], &kv->info);
        ++n;
    }
    PRTE_LIST_RELEASE(info);

    /* now load the node info */
    PRTE_LIST_FOREACH(iarray, &nodeinfo, prte_info_array_item_t)
    {
        nmsize = prte_list_get_size(&iarray->infolist);
        PMIX_LOAD_KEY(pinfo[n].key, PMIX_NODE_INFO_ARRAY);
        pinfo[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[n].value.data.darray, nmsize, PMIX_INFO);
        iptr = (pmix_info_t *) pinfo[n].value.data.darray->array;
        k = 0;
        PRTE_LIST_FOREACH(kv, &iarray->infolist, prte_info_item_t)
        {
            PMIX_INFO_XFER(&iptr[k], &kv->info);
            ++k;
        }
        ++n;
    }
    PRTE_LIST_DESTRUCT(&nodeinfo);

    /* now load the app info */
    PRTE_LIST_FOREACH(iarray, &appinfo, prte_info_array_item_t)
    {
        nmsize = prte_list_get_size(&iarray->infolist);
        PMIX_LOAD_KEY(pinfo[n].key, PMIX_APP_INFO_ARRAY);
        pinfo[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(pinfo[n].value.data.darray, nmsize, PMIX_INFO);
        iptr = (pmix_info_t *) pinfo[n].value.data.darray->array;
        k = 0;
        PRTE_LIST_FOREACH(kv, &iarray->infolist, prte_info_item_t)
        {
            PMIX_INFO_XFER(&iptr[k], &kv->info);
            ++k;
        }
        ++n;
    }
    PRTE_LIST_DESTRUCT(&appinfo);

    /* register it */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(pproc.nspace, jdata->num_local_procs, pinfo, ninfo, opcbfunc,
                                      &lock);
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
        ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            return rc;
        }
        ret = PMIx_Data_pack(NULL, &pbkt, pinfo, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            PMIX_INFO_FREE(pinfo, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }
        PMIX_INFO_FREE(pinfo, ninfo);
        ret = PMIx_Data_unload(&pbkt, &pbo);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            rc = prte_pmix_convert_status(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }

        ninfo = 4;
        PMIX_INFO_CREATE(pinfo, ninfo);

        /* first pass the packed values with a key of the nspace */
        n = 0;
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
        if (PMIX_SUCCESS
            != (ret = pmix_server_publish_fn(&prte_process_info.myproc, pinfo, ninfo, opcbfunc,
                                             &lock))) {
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
    prte_pmix_lock_t *lock = (prte_pmix_lock_t *) cbdata;

    lock->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

/* add any info that the tool couldn't self-assign */
int prte_pmix_server_register_tool(pmix_nspace_t nspace)
{
    void *ilist;
    pmix_status_t ret;
    hwloc_obj_t machine;
    char *tmp;
    pmix_data_array_t darray;
    pmix_info_t *iptr;
    size_t ninfo;
    prte_pmix_lock_t lock;
    int rc;
    prte_pmix_tool_t *tl;

    PMIX_INFO_LIST_START(ilist);

#if HWLOC_API_VERSION < 0x20000
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_HWLOC_XML_V1,
                       prte_topo_signature, PMIX_STRING);
#else
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_HWLOC_XML_V2,
                       prte_topo_signature, PMIX_STRING);
#endif

    /* total available physical memory */
    machine = hwloc_get_next_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_MACHINE, NULL);
    if (NULL != machine) {
#if HWLOC_API_VERSION < 0x20000
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_AVAIL_PHYS_MEMORY,
                           &machine->memory.total_memory, PMIX_UINT64);
#else
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_AVAIL_PHYS_MEMORY,
                           &machine->total_memory, PMIX_UINT64);
#endif
    }

    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_TMPDIR,
                       prte_process_info.jobfam_session_dir, PMIX_STRING);

    /* create and pass a job-level session directory */
    if (0 > prte_asprintf(&tmp, "%s/%u", prte_process_info.jobfam_session_dir,
                          PRTE_LOCAL_JOBID(nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    rc = prte_os_dirpath_create(tmp, S_IRWXU);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        free(tmp);
        return rc;
    }
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_NSDIR, tmp, PMIX_STRING);

    /* record this tool */
    tl = PRTE_NEW(prte_pmix_tool_t);
    PMIX_LOAD_PROCID(&tl->name, nspace, 0);
    tl->nsdir = tmp;
    prte_list_append(&prte_pmix_server_globals.tools, &tl->super);

    /* pass it down */
    PMIX_INFO_LIST_CONVERT(ret, ilist, &darray);
    if (PMIX_ERR_EMPTY == ret) {
        iptr = NULL;
        ninfo = 0;
    } else if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        return rc;
    } else {
        iptr = (pmix_info_t *) darray.array;
        ninfo = darray.size;
    }
    PMIX_INFO_LIST_RELEASE(ilist);

    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    ret = PMIx_server_register_nspace(nspace, 1, iptr, ninfo,
                                      opcbfunc, &lock);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        PMIX_INFO_FREE(iptr, ninfo);
        PRTE_PMIX_DESTRUCT_LOCK(&lock);
        return rc;
    }
    PRTE_PMIX_WAIT_THREAD(&lock);
    rc = lock.status;
    PRTE_PMIX_DESTRUCT_LOCK(&lock);
    PMIX_INFO_FREE(iptr, ninfo);
    return rc;
}
