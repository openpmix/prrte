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
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void relcb(void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    PMIX_RELEASE(cd);
}

static void opcbfunc(int status, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PMIX_RELEASE(cd);
}

static void local_complete(int sd, short args, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    prte_pmix_grp_caddy_t *cd2;
    pmix_server_pset_t *pset;
    pmix_data_array_t members = PMIX_DATA_ARRAY_STATIC_INIT;
    pmix_proc_t *addmembers = NULL;
    size_t nmembers = 0, naddmembers = 0;
    pmix_proc_t *p;
    void *ilist;
    pmix_status_t rc;
    size_t n;
    pmix_data_array_t darray;
    pmix_data_buffer_t dbuf;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (PMIX_GROUP_CONSTRUCT == cd->op) {

        PMIX_INFO_LIST_START(ilist);

        for (n=0; n < cd->ndirs; n++) {
            // check if they gave us any grp or endpt info
            if (PMIX_CHECK_KEY(&cd->directives[n], PMIX_PROC_DATA) ||
                PMIX_CHECK_KEY(&cd->directives[n], PMIX_GROUP_INFO)) {
                rc = PMIx_Info_list_add_value(ilist, cd->directives[n].key, &cd->info[n].value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            // check for add members - server lib would have aggregated them
            } else if (PMIX_CHECK_KEY(&cd->directives[n], PMIX_GROUP_ADD_MEMBERS)) {
                naddmembers = cd->directives[n].value.data.darray->size;
                addmembers = (pmix_proc_t*)cd->directives[n].value.data.darray->array;
            }
        }

        // construct the final group membership
        nmembers = cd->nprocs + naddmembers;
        PMIX_DATA_ARRAY_CONSTRUCT(&members, nmembers, PMIX_PROC);
        p = (pmix_proc_t*)members.array;
        memcpy(p, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
        if (0 < naddmembers) {
            memcpy(&p[cd->nprocs], addmembers, naddmembers * sizeof(pmix_proc_t));
        }
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_MEMBERSHIP, &members, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ID, cd->grpid, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        /* add it to our list of known groups */
        pset = PMIX_NEW(pmix_server_pset_t);
        pset->name = strdup(cd->grpid);
        pset->num_members = nmembers;
        PMIX_PROC_CREATE(pset->members, nmembers);
        memcpy(pset->members, p, nmembers * sizeof(pmix_proc_t));
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);

        // convert the info list
        PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
        cd->info = (pmix_info_t*)darray.array;
        cd->ninfo = darray.size;
        PMIX_INFO_LIST_RELEASE(ilist);

        // generate events for any add members as they are waiting for notification
        if (NULL != addmembers) {

            cd2 = PMIX_NEW(prte_pmix_grp_caddy_t);
            cd2->ninfo = cd->ninfo + 3;
            PMIX_INFO_CREATE(cd2->info, cd2->ninfo);
            // carry over the info we created
            for (n=0; n < cd->ninfo; n++) {
                rc = PMIx_Info_xfer(&cd2->info[n], &cd->info[n]);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }

            // set the range to be only procs that were added
            darray.type = PMIX_PROC;
            darray.array = addmembers;
            darray.size = naddmembers;
            // load the array - note: this copies the array!
            PMIX_INFO_LOAD(&cd2->info[n], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
            ++n;

            // mark that this event stays local and does not go up to the host
            PMIX_INFO_LOAD(&cd2->info[n], PMIX_EVENT_STAYS_LOCAL, NULL, PMIX_BOOL);
            ++n;

            // add the job-level info
            PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
            rc = PMIx_server_collect_job_info(p, nmembers, &dbuf);
            if (PMIX_SUCCESS == rc) {
                PMIx_Data_buffer_unload(&dbuf, &bo.bytes, &bo.size);
                PMIX_INFO_LOAD(&cd2->info[n], PMIX_GROUP_JOB_INFO, &bo, PMIX_BYTE_OBJECT);
                PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            }
            PMIX_DATA_BUFFER_DESTRUCT(&dbuf);

            // notify local procs
            PMIx_Notify_event(PMIX_GROUP_INVITED, &prte_process_info.myproc,
                              PMIX_RANGE_CUSTOM,
                              cd2->info, cd2->ninfo, opcbfunc, cd2);
        }

        // return this to the PMIx server
        cd->cbfunc(PMIX_SUCCESS, cd->info, cd->ninfo, cd->cbdata, relcb, cd);

    } else {
        /* find this group ID on our list of groups and remove it */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, cd->grpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }

        // return their callback
        cd->cbfunc(PMIX_SUCCESS, NULL, 0, cd->cbdata, relcb, cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *grpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd;
    int rc;
    size_t i;
    bool assignID = false;
    bool fence = false;
    bool force_local = false;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s Group request recvd with %lu directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned long)ndirs);

    /* they are required to pass us an id */
    if (NULL == grpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            assignID = PMIX_INFO_TRUE(&directives[i]);

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_TIMEOUT)) {
            tv.tv_sec = directives[i].value.data.uint32;

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&directives[i]);
        }
    }
    if (0 < tv.tv_sec) {
         return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned and they aren't adding members, or they
     * insist on forcing local completion of the operation, then
     * we are done */
    if ((!fence && !assignID) || force_local) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s group request - purely local",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        if (force_local && assignID) {
            // we cannot do that
            return PMIX_ERR_BAD_PARAM;
        }
        cd = PMIX_NEW(prte_pmix_grp_caddy_t);
        cd->op = op;
        cd->grpid = strdup(grpid);
        cd->procs = procs;
        cd->nprocs = nprocs;
        cd->directives = directives;
        cd->ndirs = ndirs;
        cd->cbfunc = cbfunc;
        cd->cbdata = cbdata;
        PRTE_PMIX_THREADSHIFT(cd, prte_event_base, local_complete);
        return PMIX_SUCCESS;
    }

    rc = prte_grpcomm.group(op, grpid, procs, nprocs,
                            directives, ndirs, cbfunc, cbdata);
    if (PRTE_SUCCESS != rc) {
        rc = prte_pmix_convert_rc(rc);
    }
    return rc;
}
