/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

/* internal functions */
static void fence(int sd, short args, void *cbdata);
static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_direct_fence_signature_t *sig, bool create);
static int create_dmns(prte_grpcomm_direct_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);
static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_direct_fence_signature_t *sig);
static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_direct_fence_signature_t **sig);

int prte_grpcomm_direct_fence(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo, char *data,
                              size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    // bozo check
    if (NULL == procs) {
        return PRTE_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(prte_pmix_fence_caddy_t);
    cd->procs = (pmix_proc_t*)procs;
    cd->nprocs = nprocs;
    cd->info = (pmix_info_t*)info;
    cd->ninfo = ninfo;
    cd->data = data;
    cd->ndata = ndata;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, fence, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void fence(int sd, short args, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd = (prte_pmix_fence_caddy_t *) cbdata;
    prte_grpcomm_direct_fence_signature_t sig;
    prte_grpcomm_fence_t *coll;
    int rc;
    pmix_data_buffer_t *relay, bkt;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* compute the signature of this collective */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_direct_fence_signature_t);
    sig.sz = cd->nprocs;
    sig.signature = (pmix_proc_t *) malloc(sig.sz * sizeof(pmix_proc_t));
    memcpy(sig.signature, cd->procs, sig.sz * sizeof(pmix_proc_t));

    /* retrieve an existing tracker, create it if not
     * already found. The fence module is responsible
     * for releasing it upon completion of the collective */
    coll = get_tracker(&sig, true);
    if (NULL == coll) {
        PMIX_DESTRUCT(&sig);
        PMIX_RELEASE(cd);
        return;
    }
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    // execute the fence operation
    PMIX_DATA_BUFFER_CREATE(relay);
    /* pack the signature */
    rc = fence_sig_pack(relay, coll->sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_RELEASE(cd);
        return;
    }

    // pack the info structs
    rc = PMIx_Data_pack(NULL, relay, &cd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_RELEASE(cd);
        return;
    }
    if (0 < cd->ninfo) {
        rc = PMIx_Data_pack(NULL, relay, cd->info, cd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_DATA_BUFFER_RELEASE(relay);
            PMIX_RELEASE(cd);
            return;
        }
    }

    /* pass along the payload */
    PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
    bo.bytes = cd->data;
    bo.size = cd->ndata;
    PMIx_Data_embed(&bkt, &bo);
    rc = PMIx_Data_copy_payload(relay, &bkt);
    PMIX_DATA_BUFFER_DESTRUCT(&bkt);
    if (PMIX_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_RELEASE(cd);
        return;
    }

    /* send this to ourselves for processing */
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, relay,
                  PRTE_RML_TAG_FENCE);
    PMIX_RELEASE(cd);
    return;
}

void prte_grpcomm_direct_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, timeout;
    size_t n, ninfo;
    pmix_status_t st;
    pmix_info_t *info = NULL;
    prte_grpcomm_direct_fence_signature_t *sig = NULL;
    pmix_data_buffer_t *reply;
    prte_grpcomm_fence_t *coll;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct fence recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }
    PMIX_RELEASE(sig);

    // unpack the info structs
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
    }

    /* cycle thru the info to look for keys we support */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, timeout, int);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (coll->timeout < timeout) {
                coll->timeout = timeout;
            }
            /* update the info with the collected value */
            info[n].value.type = PMIX_INT;
            info[n].value.data.integer = coll->timeout;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, st, pmix_status_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (PMIX_SUCCESS != st &&
                PMIX_SUCCESS == coll->status) {
                coll->status = st;
            }
            /* update the info with the collected value */
            info[n].value.type = PMIX_STATUS;
            info[n].value.data.status = coll->status;
        }
    }

    /* increment nprocs reported for collective */
    coll->nreported++;

    // transfer any data
    rc = PMIx_Data_copy_payload(&coll->bucket, buffer);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct fence recv nexpected %d nrep %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nexpected,
                         (int) coll->nreported));

    /* see if everyone has reported */
    if (coll->nreported == coll->nexpected) {
        if (PRTE_PROC_IS_MASTER) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct fence HNP reports complete",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* the allgather is complete - send the xcast */
            PMIX_DATA_BUFFER_CREATE(reply);

            /* pack the signature */
            rc = fence_sig_pack(reply, coll->sig);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            /* pack the status */
            rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }

            /* transfer the collected bucket */
            rc = PMIx_Data_copy_payload(reply, &coll->bucket);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }

            /* send the release via xcast */
            (void) prte_grpcomm.xcast(PRTE_RML_TAG_FENCE_RELEASE, reply);
        } else {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct fence rollup complete - sending to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));
            PMIX_DATA_BUFFER_CREATE(reply);
            /* pack the signature */
            rc = fence_sig_pack(reply, coll->sig);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }

            // pack the info structs
            rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (0 < ninfo) {
                rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_INFO_FREE(info, ninfo);
                    return;
                }
            }
            PMIX_INFO_FREE(info, ninfo);

            /* transfer the collected bucket */
            rc = PMIx_Data_copy_payload(reply, &coll->bucket);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            /* send the info to our parent */
            PRTE_RML_SEND(rc, PRTE_PROC_MY_PARENT->rank, reply,
                          PRTE_RML_TAG_FENCE);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
        }
    }
}

static void relcb(void *cbdata)
{
    uint8_t *data = (uint8_t *) cbdata;

    if (NULL != data) {
        free(data);
    }
}

void prte_grpcomm_direct_fence_release(int status, pmix_proc_t *sender,
                                       pmix_data_buffer_t *buffer,
                                       prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, ret;
    prte_grpcomm_direct_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence release called with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) buffer->bytes_used));

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }

    /* check for the tracker - it is not an error if not
     * found as that just means we are not involved
     * in the collective */
    if (NULL == (coll = get_tracker(sig, false))) {
        PMIX_RELEASE(sig);
        return;
    }

    /* unload the buffer */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    rc = PMIx_Data_unload(buffer, &bo);
    if (PMIX_SUCCESS != rc) {
        ret = rc;
    }

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, bo.bytes, bo.size, coll->cbdata, relcb, bo.bytes);
    }
    pmix_list_remove_item(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);
    PMIX_RELEASE(coll);
    PMIX_RELEASE(sig);
}

static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_direct_fence_signature_t *sig, bool create)
{
    prte_grpcomm_fence_t *coll;
    int rc;
    size_t n;

    /* search the existing tracker list to see if this already exists */
    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.fence_ops, prte_grpcomm_fence_t) {
        if (sig->sz == coll->sig->sz) {
            // must match proc signature
            if (0 == memcmp(sig->signature, coll->sig->signature, sig->sz * sizeof(pmix_proc_t))) {
                PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                     "%s grpcomm:base:returning existing collective",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                return coll;
            }
        }
    }
    /* if we get here, then this is a new collective - so create
     * the tracker for it */
    if (!create) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

        return NULL;
    }
    coll = PMIX_NEW(prte_grpcomm_fence_t);
    // we have to know the participating procs
    coll->sig = PMIX_NEW(prte_grpcomm_direct_fence_signature_t);
    coll->sig->sz = sig->sz;
    coll->sig->signature = (pmix_proc_t *) malloc(coll->sig->sz * sizeof(pmix_proc_t));
    memcpy(coll->sig->signature, sig->signature, coll->sig->sz * sizeof(pmix_proc_t));
    pmix_list_append(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        return NULL;
    }

    /* count the number of contributions we should get */
    coll->nexpected = prte_rml_get_num_contributors(coll->dmns, coll->ndmns);

    /* see if I am in the array of participants - note that I may
     * be in the rollup tree even though I'm not participating
     * in the collective itself */
    for (n = 0; n < coll->ndmns; n++) {
        if (coll->dmns[n] == PRTE_PROC_MY_NAME->rank) {
            coll->nexpected++;
            break;
        }
    }

    return coll;
}

static int create_dmns(prte_grpcomm_direct_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns)
{
    size_t n;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_node_t *node;
    prte_job_map_t *map;
    int i;
    pmix_list_t ds;
    prte_namelist_t *nm;
    pmix_rank_t vpid;
    bool found;
    size_t nds = 0;
    pmix_rank_t *dns = NULL;
    int rc = PRTE_SUCCESS;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence:create_dmns called with %s signature size %" PRIsize_t "",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == sig->signature) ? "NULL" : "NON-NULL", sig->sz));

    /* if the target jobid is our own,
     * then all daemons are participating */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, sig->signature[0].nspace)) {
        *ndmns = prte_process_info.num_daemons;
        *dmns = NULL;
        return PRTE_SUCCESS;
    }

    PMIX_CONSTRUCT(&ds, pmix_list_t);
    for (n = 0; n < sig->sz; n++) {
        if (NULL == (jdata = prte_get_job_data_object(sig->signature[n].nspace))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        map = (prte_job_map_t*)jdata->map;
        if (NULL == map || 0 == map->num_nodes) {
            /* we haven't generated a job map yet - if we are the HNP,
             * then we should only involve ourselves. Otherwise, we have
             * no choice but to abort to avoid hangs */
            if (PRTE_PROC_IS_MASTER) {
                rc = PRTE_SUCCESS;
                break;
            }
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        if (PMIX_RANK_WILDCARD == sig->signature[n].rank) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:fence::create_dmns called for all procs in job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(sig->signature[0].nspace)));
            /* all daemons hosting this jobid are participating */
            for (i = 0; i < map->nodes->size; i++) {
                if (NULL == (node = pmix_pointer_array_get_item(map->nodes, i))) {
                    continue;
                }
                if (NULL == node->daemon) {
                    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                    rc = PRTE_ERR_NOT_FOUND;
                    goto done;
                }
                found = false;
                PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
                {
                    if (nm->name.rank == node->daemon->name.rank) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                         "%s grpcomm:direct:fence::create_dmns adding daemon %s to list",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, node->daemon->name.rank);
                    pmix_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s sign: GETTING PROC OBJECT FOR %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&sig->signature[n])));
            proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs,
                                                               sig->signature[n].rank);
            if (NULL == proc) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            if (NULL == proc->node || NULL == proc->node->daemon) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            vpid = proc->node->daemon->name.rank;
            found = false;
            PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
            {
                if (nm->name.rank == vpid) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nm = PMIX_NEW(prte_namelist_t);
                PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, vpid);
                pmix_list_append(&ds, &nm->super);
            }
        }
    }

done:
    if (0 < pmix_list_get_size(&ds)) {
        dns = (pmix_rank_t *) malloc(pmix_list_get_size(&ds) * sizeof(pmix_rank_t));
        nds = 0;
        while (NULL != (nm = (prte_namelist_t *) pmix_list_remove_first(&ds))) {
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:fence::create_dmns adding daemon %s to array",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name)));
            dns[nds++] = nm->name.rank;
            PMIX_RELEASE(nm);
        }
    }
    PMIX_LIST_DESTRUCT(&ds);
    *dmns = dns;
    *ndmns = nds;
    return rc;
}

static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_direct_fence_signature_t *sig)
{
    pmix_status_t rc;

    // always send the participating procs
    rc = PMIx_Data_pack(NULL, bkt, &sig->sz, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->sz) {
        rc = PMIx_Data_pack(NULL, bkt, sig->signature, sig->sz, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    return PRTE_SUCCESS;
}

static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_direct_fence_signature_t **sig)
{
    pmix_status_t rc;
    int32_t cnt;
    prte_grpcomm_direct_fence_signature_t *s;

    s = PMIX_NEW(prte_grpcomm_direct_fence_signature_t);

    // unpack the participating procs
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->sz) {
        PMIX_PROC_CREATE(s->signature, s->sz);
        cnt = s->sz;
        rc = PMIx_Data_unpack(NULL, buffer, s->signature, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    *sig = s;
    return PRTE_SUCCESS;
}
