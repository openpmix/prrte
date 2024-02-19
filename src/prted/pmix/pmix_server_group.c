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
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}
static void group_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    int32_t cnt;
    int rc = PRTE_SUCCESS;
    pmix_status_t ret;
    bool assignedID = false;
    bool procsadded = false;
    size_t cid;
    pmix_proc_t *procs, *members;
    size_t n, num_members;
    pmix_data_array_t darray;
    pmix_info_t info;
    pmix_data_buffer_t dbuf;
    pmix_byte_object_t bo;
    int32_t byused;
    pmix_server_pset_t *pset;

    PMIX_ACQUIRE_OBJECT(cd);

   pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group request complete",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (PRTE_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* if this was a destruct operation, then there is nothing
     * further we need do */
    if (PMIX_GROUP_DESTRUCT == cd->op) {
        /* find this group ID on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, cd->grpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        rc = status;
        goto complete;
    }

    /* check for any directives */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    PMIX_DATA_BUFFER_LOAD(&dbuf, bo.bytes, bo.size);

    cd->ninfo = 2;
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(&info, PMIX_GROUP_CONTEXT_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info.value, cid, size_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->ninfo = 0;
                PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
                goto complete;
            }
            assignedID = true;
            cd->ninfo++;
        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_ADD_MEMBERS)) {
            members = (pmix_proc_t*)info.value.data.darray->array;
            num_members = info.value.data.darray->size;
            PMIX_PROC_CREATE(procs, cd->nprocs + num_members);
            for (n=0; n < cd->nprocs; n++) {
                PMIX_XFER_PROCID(&procs[n], &cd->procs[n]);
            }
            for (n=0; n < num_members; n++) {
                PMIX_XFER_PROCID(&procs[n+cd->nprocs], &members[n]);
            }
            PMIX_PROC_FREE(cd->procs, cd->nprocs);
            cd->procs = procs;
            cd->nprocs += num_members;
            procsadded = true;
        }
        /* cleanup */
        PMIX_INFO_DESTRUCT(&info);
        /* get the next object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&dbuf);

    /* the unpacking loop will have ended when the unpack either
     * went past the end of the buffer */
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    rc = PMIX_SUCCESS;

    if (PMIX_GROUP_CONSTRUCT == cd->op) {
       /* add it to our list of known groups */
        pset = PMIX_NEW(pmix_server_pset_t);
        pset->name = strdup(cd->grpid);
        pset->num_members = cd->nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
    }

    /* if anything is left in the buffer, then it is
     * modex data that needs to be stored */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    byused = buf->bytes_used - (buf->unpack_ptr - buf->base_ptr);
    if (0 < byused) {
        bo.bytes = buf->unpack_ptr;
        bo.size = byused;
    }
    if (NULL != bo.bytes && 0 < bo.size) {
        cd->ninfo++;
    }

    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    n = 0;
    // pass back the final group membership
    darray.type = PMIX_PROC;
    darray.array = cd->procs;
    darray.size = cd->nprocs;
    PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
    PMIX_PROC_FREE(cd->procs, cd->nprocs);
    ++n;
    if (assignedID) {
        PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
        ++n;
    }
    if (NULL != bo.bytes && 0 < bo.size) {
        PMIX_INFO_LOAD(&cd->info[n], PMIX_GROUP_ENDPT_DATA, &bo, PMIX_BYTE_OBJECT);
    }

complete:
    ret = prte_pmix_convert_rc(rc);
    /* return to the local procs in the collective */
    if (NULL != cd->infocbfunc) {
        cd->infocbfunc(ret, cd->info, cd->ninfo, cd->cbdata, relcb, cd);
    } else {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *grpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i;
    bool assignID = false;
    pmix_server_pset_t *pset;
    bool fence = false;
    bool force_local = false;
    pmix_proc_t *members = NULL;
    pmix_proc_t *mbrs, *p;
    size_t num_members = 0;
    size_t nmembers;
    bool copied = false;
    pmix_byte_object_t *bo = NULL;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s Group request recvd with %u directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ndirs);

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
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ENDPT_DATA)) {
            bo = (pmix_byte_object_t *) &directives[i].value.data.bo;
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_TIMEOUT)) {
            tv.tv_sec = directives[i].value.data.uint32;
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&directives[i]);
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ADD_MEMBERS)) {
            // there can be more than one entry here as this is the aggregate
            // of info keys from local procs that called group_construct
            if (NULL == members) {
                members = (pmix_proc_t*)directives[i].value.data.darray->array;
                num_members = directives[i].value.data.darray->size;
            } else {
                copied = true;
                // need to aggregate these
                mbrs = (pmix_proc_t*)directives[i].value.data.darray->array;
                nmembers = directives[i].value.data.darray->size;
                // create a new array
                PMIX_PROC_CREATE(p, nmembers * num_members);
                // xfer data across
                memcpy(p, members, num_members * sizeof(pmix_proc_t));
                memcpy(&p[num_members], mbrs, nmembers * sizeof(pmix_proc_t));
                // release the old array
                PMIX_PROC_FREE(members, num_members);
                // complete the xfer
                members = p;
                num_members = num_members + nmembers;
            }
        }
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned and they aren't adding members, or they
     * insist on forcing local completion of the operation, then
     * we are done */
    if ((!fence && !assignID && NULL == members) || force_local) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s group request - purely local",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        if (PMIX_GROUP_CONSTRUCT == op) {
            /* add it to our list of known groups */
            pset = PMIX_NEW(pmix_server_pset_t);
            pset->name = strdup(grpid);
            if (NULL != members) {}
            pset->num_members = nprocs;
            if (NULL != members) {
                pset->num_members += num_members;
            }
            PMIX_PROC_CREATE(pset->members, pset->num_members);
            memcpy(pset->members, procs, nprocs * sizeof(pmix_proc_t));
            if (NULL != members) {
                memcpy(&pset->members[nprocs], members, num_members * sizeof(pmix_proc_t));
            }
            pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
        } else if (PMIX_GROUP_DESTRUCT == op) {
            /* find this group ID on our list of groups */
            PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
            {
                if (0 == strcmp(pset->name, grpid)) {
                    pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                    PMIX_RELEASE(pset);
                    break;
                }
            }
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    cd = PMIX_NEW(prte_pmix_mdx_caddy_t);
    cd->op = op;
    cd->grpid = strdup(grpid);
    /* have to copy the procs in case we add members */
    PMIX_PROC_CREATE(cd->procs, nprocs);
    memcpy(cd->procs, procs, nprocs * sizeof(pmix_proc_t));
    cd->nprocs = nprocs;
    cd->grpcbfunc = group_release;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* compute the signature of this collective */
    cd->sig = PMIX_NEW(prte_grpcomm_signature_t);
    cd->sig->groupID = strdup(grpid);
    if (NULL != procs) {
        cd->sig->sz = nprocs;
        cd->sig->signature = (pmix_proc_t *) malloc(cd->sig->sz * sizeof(pmix_proc_t));
        memcpy(cd->sig->signature, procs, cd->sig->sz * sizeof(pmix_proc_t));
    }
    if (NULL != members) {
        cd->sig->nmembers = num_members;
        if (copied) {
            cd->sig->addmembers = members;
        } else {
            cd->sig->addmembers = (pmix_proc_t *) malloc(num_members * sizeof(pmix_proc_t));
            memcpy(cd->sig->addmembers, members, num_members * sizeof(pmix_proc_t));
        }
    }
    /* setup the ctrls blob - this will include any "add_members" directive */
    rc = prte_pack_ctrl_options(&cd->ctrls, directives, ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    PMIX_DATA_BUFFER_CREATE(cd->buf);
    /* if they provided us with a data blob, send it along */
    if (NULL != bo) {
        /* We don't own the byte_object and so we have to
         * copy it here */
        rc = PMIx_Data_embed(cd->buf, bo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* pass it to the global collective algorithm */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd))) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
