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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2024      Triad National Security, LLC. All rights
 *                         reserved.
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
#include "src/util/pmix_argv.h"
#include "src/util/error.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "types.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/pmix/pmix_server_internal.h"

/* caddy for carrying a registration through its asynchronous
 * completion chain */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_info_t *pinfo;
    size_t ninfo;
    pmix_op_cbfunc_t cbfunc;
    void *cbdata;
    pmix_status_t status;
    prte_job_t *jdata;
} prte_pmix_reg_caddy_t;
static void regcon(prte_pmix_reg_caddy_t *p)
{
    p->pinfo = NULL;
    p->ninfo = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
    p->status = PMIX_SUCCESS;
    p->jdata = NULL;
}
static void regdes(prte_pmix_reg_caddy_t *p)
{
    if (NULL != p->pinfo) {
        PMIX_INFO_FREE(p->pinfo, p->ninfo);
    }
    if (NULL != p->jdata) {
        PMIX_RELEASE(p->jdata);
    }
}
static PMIX_CLASS_INSTANCE(prte_pmix_reg_caddy_t, pmix_object_t, regcon, regdes);

/* the registration has completed and PMIx has handed it back on ITS
 * progress thread; regcbfunc shifted us here.  Invoke the caller's callback
 * on the PRRTE progress thread and clean up. */
static void _nspace_reg_done(int sd, short args, void *cbdata)
{
    prte_pmix_reg_caddy_t *cd = (prte_pmix_reg_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* Mark the job registered only now that PMIx actually holds it.  This
     * is what dmodex_req()'s wildcard arm reads to decide that a job-level
     * key it is being asked for is one we do not have, rather than one we
     * have not published yet - so it has to mean "PMIx has our answer",
     * not "we have finished assembling it". */
    if (PMIX_SUCCESS == cd->status && NULL != cd->jdata) {
        prte_set_attribute(&cd->jdata->attributes, PRTE_JOB_NSPACE_REGISTERED,
                           PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
    }

    if (NULL != cd->cbfunc) {
        cd->cbfunc(cd->status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_reg_caddy_t *cd = (prte_pmix_reg_caddy_t *) cbdata;

    /* this executes on the PMIx progress thread - shift to our
     * event base before continuing */
    cd->status = status;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, _nspace_reg_done, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
}

/* stuff proc attributes for sending back to a proc. The
 * registration completes asynchronously: a PRTE_SUCCESS return
 * means the provided callback will be invoked (on the PRRTE
 * progress thread) when the registration is done; an error
 * return means the callback will never fire */
int prte_pmix_server_register_nspace(prte_job_t *jdata,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    prte_proc_t *pptr;
    int i, k, n;
    void *info, *pmap, *iarray;
    prte_info_item_t *kv;
    prte_node_t *node;
    pmix_rank_t vpid;
    char **list, **procs, **micro, *tmp, *regex;
    prte_job_map_t *map;
    prte_app_context_t *app;
    uid_t uid;
    gid_t gid;
    pmix_list_t *cache;
    hwloc_obj_t machine;
    pmix_proc_t pproc, *parentproc = NULL, *procptr;
    pmix_status_t ret;
    pmix_info_t devinfo[2];
    prte_pmix_reg_caddy_t *cd;
    pmix_list_t local_procs, members;
    prte_namelist_t *nm;
    size_t nmsize;
    prte_pmix_server_pset_t *pset, *psptr;
    pmix_cpuset_t cpuset;
    uint32_t ui32, *ui32_ptr;
    pmix_data_array_t *devarray;
    uint32_t nodesize;
    pmix_device_distance_t *distances;
    size_t ndist;
    pmix_topology_t topo;
    prte_job_t *parent = NULL;
    pmix_data_array_t darray, lparray;
    bool flag, *fptr, newpset;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s register nspace for %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace));

    /* Everything below is assembled by walking the job's map, and a job
     * object outlives its map: the map is released when the job completes
     * and the object itself not until cleanup_job, a later event.  A caller
     * that reaches us in between would take the daemon down here.  The
     * caller that can hit that window - the wildcard direct-modex arm of
     * dmodex_req() - screens for it itself, because it owes its client an
     * answer rather than merely a survival; this is the backstop for the
     * rest. */
    if (NULL == jdata->map) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* setup the info list */
    PMIX_INFO_LIST_START(info);
    if (NULL == info) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    uid = geteuid();
    gid = getegid();
    topo.source = "hwloc";

    /* pass the session ID */
    ui32_ptr = &ui32;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_SESSION_ID, (void **) &ui32_ptr,
                           PMIX_UINT32)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_SESSION_ID, &ui32, PMIX_UINT32);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_LIST_RELEASE(info);
            rc = prte_pmix_convert_status(ret);
            return rc;
        }
    }

    /* jobid */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_JOBID, jdata->nspace, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(info);
        rc = prte_pmix_convert_status(ret);
        return rc;
    }

    /* offset */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_NPROC_OFFSET, &jdata->offset, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(info);
        rc = prte_pmix_convert_status(ret);
        return rc;
    }

    /* check for cached values to add to the job info */
    cache = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, (void **) &cache, PMIX_POINTER)
        && NULL != cache) {
        while (NULL != (kv = (prte_info_item_t *) pmix_list_remove_first(cache))) {
            /* the xfer copies the value, so we are done with the item
             * regardless of whether it succeeded */
            PMIX_INFO_LIST_XFER(ret, info, &kv->info);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_INFO_LIST_RELEASE(info);
                prte_remove_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE);
                PMIX_RELEASE(cache);
                rc = prte_pmix_convert_status(ret);
                return rc;
            }
        }
        prte_remove_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE);
        PMIX_RELEASE(cache);
    }

    /* assemble the node and proc map info */
    list = NULL;
    procs = NULL;
    map = jdata->map;
    PMIX_LOAD_NSPACE(pproc.nspace, jdata->nspace);
    PMIX_CONSTRUCT(&local_procs, pmix_list_t);
    for (i = 0; i < map->nodes->size; i++) {
        if (NULL != (node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, i))) {
            micro = NULL;
            tmp = NULL;
            vpid = PMIX_RANK_VALID;
            ui32 = 0;
            PMIx_Argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k = 0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, k))) {
                    if (PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                        PMIx_Argv_append_nosize(&micro, PRTE_VPID_PRINT(pptr->name.rank));
                        if (pptr->name.rank < vpid) {
                            vpid = pptr->name.rank;
                        }
                        ++ui32;
                    }
                    if (PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                        /* track all procs on our node */
                        nm = PMIX_NEW(prte_namelist_t);
                        PMIX_LOAD_PROCID(&nm->name, pptr->name.nspace, pptr->name.rank);
                        pmix_list_append(&local_procs, &nm->super);
                        if (PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                            /* Go ahead and register this client.  Handing it no
                             * callback asks for the blocking form: PMIx runs the
                             * registration on its own progress thread and returns
                             * here when it is done, so the status below is the
                             * registration's own and the rank is on record before
                             * the register_nspace at the bottom tells PMIx how
                             * many to expect. */
                            ret = PMIx_server_register_client(&pptr->name, uid, gid,
                                                              (void*)pptr, NULL, NULL);
                            /* A rank we already hold is not a failure - it is
                             * the state we wanted.  A daemon registers a given
                             * namespace once (dmodex_req()'s wildcard arm is
                             * what keeps that true), but "once" is not "once
                             * successfully": the failure exit below abandons
                             * this loop with the ranks ahead of it already
                             * registered and the job never marked registered,
                             * so a later wildcard get re-runs the whole
                             * registration and meets every one of them again.
                             * PMIx must refuse a second add of a rank - a
                             * duplicate entry in its rank list puts that list
                             * permanently past nlocalprocs, so all_registered
                             * is never set and every collective involving the
                             * namespace hangs - and reports it as a hard
                             * error, so treating it as fatal here would fail a
                             * registration that is in fact fine. */
                            if (PMIX_ERR_DUPLICATE_KEY == ret) {
                                ret = PMIX_SUCCESS;
                            }
                            if (PMIX_SUCCESS != ret && PMIX_OPERATION_SUCCEEDED != ret) {
                                /* Anything else means our own PMIx server will
                                 * refuse this proc's PMIx_Init, so forking it
                                 * would only move the failure to where nobody
                                 * can read it: the application sees an obscure
                                 * error some way downstream of a launch that
                                 * appeared to succeed.  Fail the registration
                                 * instead.  The launch path (job_reg_join, in
                                 * odls) turns that into NEVER_LAUNCHED, which is
                                 * what it is. */
                                PMIX_ERROR_LOG(ret);
                                PMIx_Argv_free(micro);
                                micro = NULL;
                                PMIX_INFO_LIST_RELEASE(info);
                                rc = prte_pmix_convert_status(ret);
                                goto errout;
                            }
                        }
                    }
                }
            }
            /* assemble the rank/node map */
            if (NULL != micro) {
                tmp = PMIx_Argv_join(micro, ',');
                PMIx_Argv_free(micro);
                PMIx_Argv_append_nosize(&procs, tmp);
            }
            /* construct the node info array */
            PMIX_INFO_LIST_START(iarray);
            /* start with the hostname */
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_HOSTNAME, node->name, PMIX_STRING);
            /* add any aliases */
            if (NULL != node->aliases) {
                regex = PMIx_Argv_join(node->aliases, ',');
                PMIX_INFO_LIST_ADD(ret, iarray, PMIX_HOSTNAME_ALIASES, regex, PMIX_STRING);
                free(regex);
            }
            /* pass the node ID */
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_NODEID, &node->index, PMIX_UINT32);
            /* Add node size. Widen it first: node->num_procs is a
             * prte_node_rank_t (uint16_t), so handing its address over as
             * PMIX_UINT32 makes PMIx read four bytes out of a two-byte
             * field. The extra two are the padding before node->procs -
             * PMIX_NEW does not zero its allocation - so the value the
             * client is told is num_procs with heap garbage in its upper
             * half. */
            nodesize = node->num_procs;
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_NODE_SIZE, &nodesize, PMIX_UINT32);
            /* add local size for this job */
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_LOCAL_SIZE, &ui32, PMIX_UINT32);
            /* pass the local ldr */
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_LOCALLDR, &vpid, PMIX_PROC_RANK);
            /* add the local peers */
            if (NULL != tmp) {
                PMIX_INFO_LIST_ADD(ret, iarray, PMIX_LOCAL_PEERS, tmp, PMIX_STRING);
                free(tmp);
            }
            /* if oversubscribed, mark it */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_OVERSUBSCRIBED)) {
                PMIX_INFO_LIST_ADD(ret, iarray, PMIX_NODE_OVERSUBSCRIBED, NULL, PMIX_BOOL);
            }
            /* add to the overall payload */
            PMIX_INFO_LIST_CONVERT(ret, iarray, &darray);
            PMIX_INFO_LIST_ADD(ret, info, PMIX_NODE_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_INFO_LIST_RELEASE(iarray);
        }
    }
    /* let the PMIx server generate the nodemap regex */
    if (NULL != list) {
        tmp = PMIx_Argv_join(list, ',');
        PMIx_Argv_free(list);
        list = NULL;
#if PRTE_PMIX_HAVE_REGEX2
        pmix_regex2_t nregex = PMIX_REGEX2_STATIC_INIT;
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex2(tmp, NULL, 0, &nregex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_LIST_RELEASE(info);
            rc = prte_pmix_convert_status(ret);
            goto errout;
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, info, PMIX_NODE_MAP, &nregex, PMIX_REGEX2);
        PMIx_Regex2_destruct(&nregex);
#else
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_LIST_RELEASE(info);
            rc = prte_pmix_convert_status(ret);
            goto errout;
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, info, PMIX_NODE_MAP, regex, PMIX_REGEX);
        free(regex);
#endif
    }

    /* let the PMIx server generate the procmap regex */
    if (NULL != procs) {
        tmp = PMIx_Argv_join(procs, ';');
        PMIx_Argv_free(procs);
        procs = NULL;
#if PRTE_PMIX_HAVE_REGEX2
        pmix_regex2_t pregex = PMIX_REGEX2_STATIC_INIT;
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex2(tmp, NULL, 0, &pregex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_LIST_RELEASE(info);
            rc = prte_pmix_convert_status(ret);
            goto errout;
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, info, PMIX_PROC_MAP, &pregex, PMIX_REGEX2);
        PMIx_Regex2_destruct(&pregex);
#else
        if (PMIX_SUCCESS != (ret = PMIx_generate_ppn(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_LIST_RELEASE(info);
            rc = prte_pmix_convert_status(ret);
            goto errout;
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, info, PMIX_PROC_MAP, regex, PMIX_REGEX);
        free(regex);
#endif
    }

    /* pass the number of nodes in the job */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_NUM_NODES, &map->num_nodes, PMIX_UINT32);

    /* univ size */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_UNIV_SIZE, &jdata->total_slots_alloc, PMIX_UINT32);

    /* job size */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_JOB_SIZE, &jdata->num_procs, PMIX_UINT32);

    /* number of apps in this job */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_JOB_NUM_APPS, &jdata->num_apps, PMIX_UINT32);

    /* max procs */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_MAX_PROCS, &jdata->total_slots_alloc, PMIX_UINT32);

    /* total available physical memory */
    machine = hwloc_get_next_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_MACHINE, NULL);
    if (NULL != machine) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_AVAIL_PHYS_MEMORY, &machine->total_memory, PMIX_UINT64);
    }

    /* pass the mapping policy used for this job */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_MAPBY, prte_rmaps_base_print_mapping(jdata->map->mapping), PMIX_STRING);

    /* pass the ranking policy used for this job */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_RANKBY, prte_rmaps_base_print_ranking(jdata->map->ranking), PMIX_STRING);

    /* pass the binding policy used for this job */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_BINDTO, prte_hwloc_base_print_binding(jdata->map->binding), PMIX_STRING);

    /* tell the user what we did with FQDN */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_HOSTNAME_KEEP_FQDN, &prte_keep_fqdn_hostnames, PMIX_BOOL);

    /* pass the top-level session directory */
    PMIX_INFO_LIST_ADD(ret, info, PMIX_TMPDIR, prte_process_info.top_session_dir, PMIX_STRING);

    /* create and pass a job-level session directory */
    pproc.rank = PMIX_RANK_INVALID;
    rc = prte_session_dir(&pproc);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_INFO_LIST_RELEASE(info);
        rc = prte_pmix_convert_status(rc);
        goto errout;
    }
    // job session dir will have been stored in the jdata object
    PMIX_INFO_LIST_ADD(ret, info, PMIX_NSDIR, jdata->session_dir, PMIX_STRING);

    /* check for output directives */
    fptr = &flag;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_TAG_OUTPUT, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT_DETAILED, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_TAG_DETAILED_OUTPUT, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT_FULLNAME, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_TAG_FULLNAME_OUTPUT, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_RANK_OUTPUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_RANK_OUTPUT, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_TIMESTAMP_OUTPUT, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_XML_OUTPUT, &flag, PMIX_BOOL);
    }
#ifdef PMIX_IOF_INHERIT
    /* Whether this job is to inherit the output forwarding of the job that
     * spawned it. PMIx asks this on the server the child's output ARRIVES
     * at, which need not be the one that processed the spawn, so the answer
     * has to travel with the job rather than with the spawn request - and
     * this is what carries it there. The mapper resolved it, subject to the
     * same inherit rule that governs mapping, ranking and binding.
     *
     * Sent only to say NO: absence means inherit, on both sides of the
     * interface, so a job with no opinion adds nothing to the wire. */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_NO_IOF_INHERIT, NULL, PMIX_BOOL)) {
        bool noinherit = false;
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_INHERIT, &noinherit, PMIX_BOOL);
    }
#endif
    tmp = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_FILE, (void **) &tmp, PMIX_STRING)
        && NULL != tmp) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_OUTPUT_TO_FILE, tmp, PMIX_STRING);
        free(tmp);
    }
    tmp = NULL;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_TO_DIRECTORY, (void **) &tmp, PMIX_STRING)
        && NULL != tmp) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_OUTPUT_TO_DIRECTORY, tmp, PMIX_STRING);
        free(tmp);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_NOCOPY, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_OUTPUT_NOCOPY, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_FILE_PATTERN, (void**)&fptr, PMIX_BOOL)) {
        /* PMIx expands the pattern when it opens the sink, so the flag has to
         * reach the nspace it will read the filename from */
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_FILE_PATTERN, &flag, PMIX_BOOL);
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_MERGE_STDERR_STDOUT, &flag, PMIX_BOOL);
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_RAW_OUTPUT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_IOF_OUTPUT_RAW, &flag, PMIX_BOOL);
    }

    // check for GPU directives
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_GPU_SUPPORT, (void**)&fptr, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, info, PMIX_GPU_SUPPORT, &flag, PMIX_BOOL);
    }

    /* for each app in the job, create an app-array */
    for (n = 0; n < jdata->apps->size; n++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n))) {
            continue;
        }
        PMIX_INFO_LIST_START(iarray);
        /* start with the app number */
        PMIX_INFO_LIST_ADD(ret, iarray, PMIX_APPNUM, &app->idx, PMIX_UINT32);
        /* add the app size */
        PMIX_INFO_LIST_ADD(ret, iarray, PMIX_APP_SIZE, &app->num_procs, PMIX_UINT32);
        /* add the app leader */
        PMIX_INFO_LIST_ADD(ret, iarray, PMIX_APPLDR, &app->first_rank, PMIX_PROC_RANK);
        /* add the wdir */
        PMIX_INFO_LIST_ADD(ret, iarray, PMIX_WDIR, app->cwd, PMIX_STRING);
        /* add the argv */
        tmp = PMIx_Argv_join(app->argv, ' ');
        PMIX_INFO_LIST_ADD(ret, iarray, PMIX_APP_ARGV, tmp, PMIX_STRING);
        free(tmp);
        /* add the pset name */
        tmp = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_PSET_NAME, (void **) &tmp, PMIX_STRING)
            && NULL != tmp) {
            PMIX_INFO_LIST_ADD(ret, iarray, PMIX_PSET_NAME, tmp, PMIX_STRING);
            /* Have we already recorded this one?  This function is not
             * called once per job: the wildcard arm of dmodex_req() calls it
             * again for a job-level key the local server does not hold, on
             * any daemon - including one hosting the job's procs - though
             * only until that arm marks the job registered.  The info arrays
             * below have to be rebuilt each time it does call us, but the
             * registry entry is a per-job fact, and appending it again both
             * duplicated the pset in every query answer and took a reference
             * on the job object that nothing would ever give back. */
            pset = NULL;
            PMIX_LIST_FOREACH(psptr, &prte_pmix_server_globals.psets,
                              prte_pmix_server_pset_t) {
                if (psptr->jdata == jdata && NULL != psptr->name &&
                    0 == strcmp(psptr->name, tmp)) {
                    pset = psptr;
                    break;
                }
            }
            if (NULL == pset) {
                pset = PMIX_NEW(prte_pmix_server_pset_t);
                pset->name = strdup(tmp);
                PMIX_RETAIN(jdata);
                pset->jdata = jdata;
                pmix_list_append(&prte_pmix_server_globals.psets, &pset->super);
                newpset = true;
            } else {
                newpset = false;
            }
            free(tmp);
            /* and its membership */
            PMIX_CONSTRUCT(&members, pmix_list_t);
            for (k = 0; k < jdata->procs->size; k++) {
                pptr = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, k);
                if (NULL == pptr) {
                    continue;
                }
                if (app->idx == pptr->app_idx) {
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, pptr->name.nspace, pptr->name.rank);
                    pmix_list_append(&members, &nm->super);
                }
            }
            nmsize = pmix_list_get_size(&members);
            if (0 < nmsize) {
                PMIX_DATA_ARRAY_CONSTRUCT(&darray, nmsize, PMIX_PROC);
                procptr = (pmix_proc_t*)darray.array;
                k = 0;
                PMIX_LIST_FOREACH(nm, &members, prte_namelist_t) {
                    PMIX_LOAD_PROCID(&procptr[k], nm->name.nspace, nm->name.rank);
                    ++k;
                }
                PMIX_INFO_LIST_ADD(ret, iarray, PMIX_PSET_MEMBERS, &darray, PMIX_DATA_ARRAY);
                if (newpset) {
                    pset->num_members = nmsize;
                    pset->members = (pmix_proc_t *) malloc(nmsize * sizeof(pmix_proc_t));
                    memcpy(pset->members, procptr, nmsize * sizeof(pmix_proc_t));
                    // let the PMIx server know
                    ret = PMIx_server_define_process_set(pset->members, pset->num_members,
                                                         pset->name);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                    }
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);
            }
            PMIX_LIST_DESTRUCT(&members);
        }
        /* add to the main payload */
        PMIX_INFO_LIST_CONVERT(ret, iarray, &darray);
        PMIX_INFO_LIST_ADD(ret, info, PMIX_APP_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        PMIX_INFO_LIST_RELEASE(iarray);
    }

    /* Get the parent job that spawned this one, if a *process* did.
     *
     * The proxy is not automatically that: a prterun-style launch records the
     * daemon itself, and a plain "prun ./app" records prun's own tool procID.
     * Neither is a parent process - a tool is not a member of a job and has no
     * procs to be spawned by - so PMIX_PARENT_ID must not be published for
     * either. An app reads that key to ask "was I spawned by another
     * application process?", and publishing prun's id answers yes to every
     * ordinary launch, which is how a program that branches on it (a spawned
     * child doing one thing, its parent another) ends up running the wrong
     * half of itself. The identical screen, and the reasoning, is in
     * prte_pmix_server_connection_spawned() (pmix_server_connect.c).
     *
     * strncmp, not PMIX_CHECK_NSPACE: that macro treats an empty nspace on
     * either side as a wildcard match, and this test must answer only for the
     * namespace it was actually given.
     */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void **) &parentproc, PMIX_PROC)) {
        if (0 != strncmp(PRTE_PROC_MY_NAME->nspace, parentproc->nspace, PMIX_MAX_NSLEN)) {
            parent = prte_get_job_data_object(parentproc->nspace);
        }
        if (NULL != parent && PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL)) {
            parent = NULL;
        }
        if (NULL == parent) {
            /* release it here: the tail of this function only releases it
             * when a parent was found, so every rejecting path must clean up
             * its own copy or the proc object leaks once per job launched
             */
            PMIX_PROC_RELEASE(parentproc);
            parentproc = NULL;
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
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, n))) {
            continue;
        }
        /* cycle across each proc on this node, passing all data that
         * varies by proc */
        for (i = 0; i < node->procs->size; i++) {
            if (NULL == (pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            /* only consider procs from this job */
            if (!PMIX_CHECK_NSPACE(pptr->name.nspace, jdata->nspace)) {
                continue;
            }
            /* setup the proc map object */
            PMIX_INFO_LIST_START(pmap);

            /* must start with rank */
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_RANK, &pptr->name.rank, PMIX_PROC_RANK);

            /* location, for local procs */
            if (NULL != pptr->cpuset) {
                /* provide the cpuset string for this proc */
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_CPUSET, pptr->cpuset, PMIX_STRING);
                /* let PMIx generate the locality string */
                PMIX_CPUSET_CONSTRUCT(&cpuset);
                cpuset.source = "hwloc";
                cpuset.bitmap = hwloc_bitmap_alloc();
                hwloc_bitmap_list_sscanf(cpuset.bitmap, pptr->cpuset);
                ret = PMIx_server_generate_locality_string(&cpuset, &tmp);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    hwloc_bitmap_free(cpuset.bitmap);
                    PMIX_INFO_LIST_RELEASE(info);
                    PMIX_INFO_LIST_RELEASE(pmap);
                    rc = prte_pmix_convert_status(ret);
                    goto errout;
                }
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_LOCALITY_STRING, tmp, PMIX_STRING);
                free(tmp);
                /* Device distances, but only for a proc we host.
                 *
                 * These are computed against the topology of the node the
                 * proc runs on, and a daemon does not have one for any node
                 * but its own: PRRTE collects topologies at the HNP, and
                 * prte_util_decode_nidmap() hands every node in a daemon's
                 * pool a retained reference to that daemon's OWN topology,
                 * "always default to homogeneous as that is the most common
                 * scenario".  So computing this for a proc we do not host
                 * measures our hardware and labels it as that proc's -
                 * correct by accident on a homogeneous cluster and silently
                 * wrong on any other.  (The HNP is the exception, holding
                 * the real topology of every node, but answering there and
                 * nowhere else is a worse contract than not answering.)
                 *
                 * A get for someone else's is refused with NOT_SUPPORTED by
                 * dmodex_req(), rather than being left to fail as though the
                 * data were merely missing. */
                if (0 != prte_pmix_server_globals.generate_dist &&
                    PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                    /* compute the device distances for this proc */
                    topo.topology = node->topology->topo;
                    devinfo[1].value.data.string = node->name;
                    ret = PMIx_Compute_distances(&topo, &cpuset,
                                                 devinfo, 2, &distances, &ndist);
                    devinfo[1].value.data.string = NULL;
                    if (PMIX_SUCCESS == ret) {
                        if (4 < pmix_output_get_verbosity(prte_pmix_server_globals.output)) {
                            size_t f;
                            for (f=0; f < ndist; f++) {
                                pmix_output(0, "UUID: %s OSNAME: %s TYPE: %s MIND: %u MAXD: %u",
                                            distances[f].uuid, distances[f].osname,
                                            PMIx_Device_type_string(distances[f].type),
                                            distances[f].mindist, distances[f].maxdist);
                            }
                        }
                        darray.type = PMIX_DEVICE_DIST;
                        darray.array = distances;
                        darray.size = ndist;
                        PMIX_INFO_LIST_ADD(ret, pmap, PMIX_DEVICE_DISTANCES, &darray, PMIX_DATA_ARRAY);
                        PMIX_DEVICE_DIST_FREE(distances, ndist);
                    }
                }
                hwloc_bitmap_free(cpuset.bitmap);
            } else if (PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                /* the proc is not bound, and we are the daemon that will
                 * fork it, so we know that rather than merely not having
                 * been told */
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_LOCALITY_STRING, NULL, PMIX_STRING);
            }
            /* Nothing published for a remote proc with no cpuset: the launch
             * message scatters the bindings, so what we hold for a proc some
             * other daemon forks is nothing at all - and a NULL locality here
             * is a positive claim that it is unbound, which the receiver
             * cannot tell from silence.  A get for it falls through to
             * dmodex_req, which declines it for the same reason and asks the
             * daemon that does know. */
            if (PRTE_PROC_MY_NAME->rank == node->daemon->name.rank) {
                /* create and pass a proc-level session directory */
                rc = prte_session_dir(&pptr->name);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    PMIX_INFO_LIST_RELEASE(info);
                    PMIX_INFO_LIST_RELEASE(pmap);
                    rc = prte_pmix_convert_status(rc);
                    goto errout;
                }
                pmix_asprintf(&tmp, "%s/%s", jdata->session_dir,
                                          PMIX_RANK_PRINT(pptr->name.rank));
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_PROCDIR, tmp, PMIX_STRING);
                free(tmp);
            }

            /* global/univ rank */
            vpid = pptr->name.rank + jdata->offset;
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_GLOBAL_RANK, &vpid, PMIX_PROC_RANK);

            /* parent ID, if we were spawned by a non-tool */
            if (NULL != parent) {
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_PARENT_ID, parentproc, PMIX_PROC);
            }

            /* appnum */
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_APPNUM, &pptr->app_idx, PMIX_UINT32);

            /* app rank */
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_APP_RANK, &pptr->app_rank, PMIX_PROC_RANK);

            /* local rank */
            if (PRTE_LOCAL_RANK_INVALID != pptr->local_rank) {
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_LOCAL_RANK, &pptr->local_rank, PMIX_UINT16);
            }

            /* node rank */
            if (PRTE_NODE_RANK_INVALID != pptr->node_rank) {
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_NODE_RANK, &pptr->node_rank, PMIX_UINT16);
            }

            /* node ID */
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_NODEID, &pptr->node->index, PMIX_UINT32);

            /* reincarnation number */
            ui32 = 0; // we are starting this proc for the first time
            PMIX_INFO_LIST_ADD(ret, pmap, PMIX_REINCARNATION, &ui32, PMIX_UINT32);

            if (map->num_nodes < prte_hostname_cutoff) {
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_HOSTNAME, pptr->node->name, PMIX_STRING);
            }

            /* the device this proc was mapped against, when it was mapped by
             * one. PRRTE cannot bind a process to a device the way it binds
             * to cpus - no such mechanism exists - so the assignment is only
             * worth making if the process can be told about it. The value is
             * the device's UUID rather than an index: a runtime's own device
             * numbering need not be the topology's (CUDA orders by speed
             * before bus by default), so an ordinal would name a different
             * device than the one we chose. The same UUID appears in the
             * PMIX_DEVICE_DISTANCES this process can query, which is what
             * lets it correlate the two. */
            devarray = NULL;
            if (prte_get_attribute(&pptr->attributes, PRTE_PROC_DEVICE_ID,
                                   (void **) &devarray, PMIX_DATA_ARRAY)) {
                PMIX_INFO_LIST_ADD(ret, pmap, PMIX_DEVICE_ID, devarray, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_FREE(devarray);
            }
            PMIX_INFO_LIST_CONVERT(ret, pmap, &darray);
            PMIX_INFO_LIST_ADD(ret, info, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_INFO_LIST_RELEASE(pmap);
        }
    }
    if (NULL != parent) {
        PMIX_PROC_RELEASE(parentproc);
        parentproc = NULL;
    }
    if (0 != prte_pmix_server_globals.generate_dist) {
        PMIX_INFO_DESTRUCT(&devinfo[0]);
        PMIX_INFO_DESTRUCT(&devinfo[1]);
    }

    /* add the local procs, if they are defined */
    if (0 < (nmsize = pmix_list_get_size(&local_procs))) {
        pmix_proc_t *procs_tmp;
        PMIX_DATA_ARRAY_CONSTRUCT(&lparray, nmsize, PMIX_PROC);
        procs_tmp = (pmix_proc_t *) lparray.array;
        n = 0;
        PMIX_LIST_FOREACH(nm, &local_procs, prte_namelist_t)
        {
            PMIX_LOAD_PROCID(&procs_tmp[n], nm->name.nspace, nm->name.rank);
            ++n;
        }
        PMIX_INFO_LIST_ADD(ret, info, PMIX_LOCAL_PROCS, &lparray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&lparray);
    }
    PMIX_LIST_DESTRUCT(&local_procs);

    /* register it.  A failure here has to be caught: PMIx_Info_list_convert()
     * initializes the caller's array before anything can fail, so what an
     * unchecked failure hands PMIx is a registration carrying no info at all
     * - a job whose every job-level key is silently missing, reported to
     * nobody. */
    PMIX_INFO_LIST_CONVERT(ret, info, &darray);
    PMIX_INFO_LIST_RELEASE(info);
    if (PMIX_SUCCESS != ret) {
        /* returns rather than joining the error tail below: by this point
         * both argv lists are freed, the local-proc list is destructed and
         * the proxy copy is released, so the tail would do each of them a
         * second time */
        PMIX_ERROR_LOG(ret);
        return prte_pmix_convert_status(ret);
    }

    /* do not block waiting for the registration - the callback
     * chain will thread-shift and then invoke the caller's callback
     * on our progress thread */
    cd = PMIX_NEW(prte_pmix_reg_caddy_t);
    cd->pinfo = (pmix_info_t *) darray.array;
    cd->ninfo = darray.size;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* held so the completion can mark it registered - the callers that do
     * not wait for us have no reason to keep the job alive for our sake */
    PMIX_RETAIN(jdata);
    cd->jdata = jdata;
    ret = PMIx_server_register_nspace(pproc.nspace, jdata->num_local_procs,
                                      cd->pinfo, cd->ninfo, regcbfunc, cd);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        /* the callback will never fire - releasing the caddy
         * also releases the info array */
        PMIX_RELEASE(cd);
        return rc;
    }
    return PRTE_SUCCESS;

errout:
    /* Every exit above is taken with the node and rank argv lists still
     * built, with the local-proc list holding one object per proc on this
     * node, and possibly with our own copy of the launch proxy in hand.
     * None of that is visible to the caller, so a failed registration
     * leaks all of it unless it is released here - once per job launched,
     * on a path that is rare but not unreachable. */
    if (NULL != list) {
        PMIx_Argv_free(list);
    }
    if (NULL != procs) {
        PMIx_Argv_free(procs);
    }
    if (NULL != parentproc) {
        PMIX_PROC_RELEASE(parentproc);
    }
    PMIX_LIST_DESTRUCT(&local_procs);
    return rc;
}

/* add any info that the tool couldn't self-assign.  Follows the same
 * asynchronous completion contract as prte_pmix_server_register_nspace
 * above, with one difference the caller has to tolerate: when the job is
 * already known the callback fires synchronously, before this returns. */
int prte_pmix_server_register_tool(prte_pmix_server_req_t *cd,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t ret;
    int rc;
    uint32_t u32;
    uint16_t u16;
    prte_job_t *jdata, *dmns;
    prte_app_context_t *app;
    prte_proc_t *proc, *dproc;
    prte_node_t *node;
    void *ilist, *joblist;
    pmix_data_array_t darray;
    prte_pmix_reg_caddy_t *rcd;

    // see if we already did this
    jdata = prte_get_job_data_object(cd->target.nspace);
    if (NULL != jdata) {
        // we did - the caller must tolerate the callback being
        // invoked prior to our return
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, cbdata);
        }
        return PRTE_SUCCESS;
    }

    /* The tool's rank is its own to choose, and it is not always zero - PMIx
     * lets a tool self-assign both halves of its identity, and _toolconn()
     * only defaults the rank when none was given.  The proc object built
     * below has to carry that rank and sit at that subscript, because
     * everything that later looks the tool up indexes jdata->procs by rank:
     * in particular prte_state_base_track_procs(), which is what retires the
     * tool's job when it departs.  Filed at zero, a tool with any other rank
     * was never found there, so its namespace never terminated - and with
     * it, the inheritance disposition of any allocation it had reserved.
     *
     * Screen the rank before building anything.  It arrived from the
     * connecting process, and the sentinel ranks are not subscripts: handing
     * PMIX_RANK_UNDEF to pmix_pointer_array_set_item() asks it to grow to
     * four billion entries. */
    if (PMIX_RANK_VALID <= cd->target.rank) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    // create a job tracker for it
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, cd->target.nspace);
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_TOOL);
    /* remember who ran this tool - a tool's namespace lasts only as long as
     * the command, so the user is the durable half of its identity and is
     * what lets a later command of theirs reach an allocation this one made */
    jdata->uid = cd->uid;
    jdata->gid = cd->gid;
    /* If this fails the job never entered the array, so nothing else will
     * ever release it and the nspace we would go on to register is one no
     * lookup can reach.  It fails for an nspace PRRTE will not accept -
     * an empty one, which is what a tool that sent PMIX_NSPACE carrying no
     * string leaves in cd->target - so this is reachable from the wire. */
    rc = prte_set_job_data_object(jdata);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(jdata);
        return rc;
    }
    app = PMIX_NEW(prte_app_context_t);
    /* The command line is optional and it comes from the connecting process:
     * PMIx sends PMIX_CMD_LINE only when it could read its own argv (it
     * cannot on a system where /proc is absent or the sysctl is refused),
     * and a tool speaking the protocol directly need not send it at all.
     * PMIx_Argv_split() of a NULL - or of a string that is all separators -
     * yields a NULL argv, so argv[0] was a dereference any tool could
     * provoke on the daemon it attached to, and through the TCONN relay on
     * the master.  Every consumer of app->app already tolerates NULL, so
     * saying nothing is the honest answer when we were told nothing. */
    if (NULL != cd->cmdline) {
        app->argv = PMIx_Argv_split(cd->cmdline, ' ');
    }
    if (NULL != app->argv) {
        app->app = strdup(app->argv[0]);
    }
    app->idx = 0;
    app->num_procs = 1;
    /* the app's leader is its lowest global rank, which for a one-proc tool
     * job is the tool's own rank - not necessarily zero */
    app->first_rank = cd->target.rank;
    pmix_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps++;
    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, cd->target.nspace, cd->target.rank);
    proc->pid = cd->pid;
    proc->state = PRTE_PROC_STATE_RUNNING;
    /* the sole proc of the sole app, alone on this node - say so on the job
     * object as well as in the registration below, or a query reads the
     * constructor's "undefined" back out of a proc we do know these about */
    proc->app_rank = 0;
    proc->local_rank = 0;
    proc->node_rank = 0;
    pmix_pointer_array_set_item(jdata->procs, cd->target.rank, proc);
    // find the node it is on - the tool is on our node, and our node is
    // whichever one carries our own daemon proc. Our vpid is not a subscript
    // into the node pool: the pool is indexed by node identity, and in a DVM
    // that has shrunk the two diverge (a departed daemon's vpid is retired,
    // its node's pool slot is not).
    node = NULL;
    dmns = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (NULL != dmns) {
        dproc = (prte_proc_t *) pmix_pointer_array_get_item(dmns->procs,
                                                            PRTE_PROC_MY_NAME->rank);
        if (NULL != dproc) {
            node = dproc->node;
        }
    }
    if (NULL == node) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(jdata);
        return PRTE_ERR_NOT_FOUND;
    }
    /* the node backpointer is borrowed, not retained */
    proc->node = node;
    jdata->num_procs = 1;

    // construct the data to register
    joblist = PMIx_Info_list_start();
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_JOBID, jdata->nspace, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    u32 = 1;
    /* pass the number of nodes in the job */
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_NUM_NODES, &u32, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }

    /* job size */
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_JOB_SIZE, &u32, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }

    /* number of apps in this job */
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_JOB_NUM_APPS, &u32, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }


    // create the node info array
    ilist = PMIx_Info_list_start();
    /* start with the hostname */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_HOSTNAME, prte_process_info.nodename, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add local size for this job */
    u32 = 1;
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_LOCAL_SIZE, &u32, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* pass the local ldr */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_LOCALLDR, &cd->target.rank, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add to the main payload */
    PMIX_INFO_LIST_CONVERT(ret, ilist, &darray);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_NODE_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_INFO_LIST_RELEASE(ilist);


    // create the app info array
    ilist = PMIx_Info_list_start();
    /* start with the app number */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APPNUM, &app->idx, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add the app size */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APP_SIZE, &app->num_procs, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add the app leader */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APPLDR, &app->first_rank, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add the cmd line */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APP_ARGV, cd->cmdline, PMIX_STRING);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add to the main payload */
    PMIX_INFO_LIST_CONVERT(ret, ilist, &darray);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_APP_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_INFO_LIST_RELEASE(ilist);


    // create the proc info array
    ilist = PMIx_Info_list_start();
    /* must start with rank */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_RANK, &proc->name.rank, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* appnum */
    u32 = 0;
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APPNUM, &u32, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* app rank - the rank WITHIN the app, which for the only proc of the only
     * app is zero however the tool numbered itself globally */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_APP_RANK, &proc->app_rank, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* local rank */
    u16 = 0;
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_LOCAL_RANK, &u16, PMIX_UINT16);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* node rank */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_NODE_RANK, &u16, PMIX_UINT16);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* Node ID.  This is the node's slot in the node pool, which is its
     * identity across the DVM - not our own vpid.  The two coincide on a DVM
     * that has never changed shape and diverge the moment one has (a departed
     * daemon's vpid is retired, its pool slot is not), which is the same
     * distinction the node lookup above is written for.  Publishing the vpid
     * told the tool about whichever node happened to sit in that slot. */
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_NODEID, &node->index, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    /* add to the main payload */
    PMIX_INFO_LIST_CONVERT(ret, ilist, &darray);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_INFO_LIST_ADD(ret, joblist, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(ilist);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_INFO_LIST_RELEASE(ilist);


    /* register it */
    PMIX_INFO_LIST_CONVERT(ret, joblist, &darray);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_LIST_RELEASE(joblist);
        rc = prte_pmix_convert_status(ret);
        goto errout;
    }
    PMIX_INFO_LIST_RELEASE(joblist);

    /* do not block waiting for the registration - the callback
     * will thread-shift and then invoke the caller's callback on
     * our progress thread */
    rcd = PMIX_NEW(prte_pmix_reg_caddy_t);
    rcd->pinfo = (pmix_info_t *) darray.array;
    rcd->ninfo = darray.size;
    rcd->cbfunc = cbfunc;
    rcd->cbdata = cbdata;
    /* marked registered by the completion, for the same reason as above:
     * the flag has to mean PMIx holds this namespace, not that we finished
     * describing it */
    PMIX_RETAIN(jdata);
    rcd->jdata = jdata;
    ret = PMIx_server_register_nspace(cd->target.nspace, 1,
                                      rcd->pinfo, rcd->ninfo,
                                      regcbfunc, rcd);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        rc = prte_pmix_convert_status(ret);
        /* the callback will never fire - releasing the caddy
         * also releases the info array */
        PMIX_RELEASE(rcd);
        goto errout;
    }
    return PRTE_SUCCESS;

errout:
    /* The job object went into the global array before any of this could
     * fail, and nothing else knows it is there: the tool is about to be
     * told its connection failed, so no client_finalized will ever retire
     * this namespace and the DVM would carry a phantom tool job - and hold
     * itself open for it - for the rest of the session.  Releasing it also
     * takes it back out of the array. */
    PMIX_RELEASE(jdata);
    return rc;
}
