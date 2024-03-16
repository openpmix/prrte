/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */

/*
 * includes
 */
#include "prte_config.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/error_strings.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"

static int pack_xcast(prte_grpcomm_signature_t *sig, pmix_data_buffer_t *buffer,
                      pmix_data_buffer_t *message, prte_rml_tag_t tag);

static int create_dmns(prte_grpcomm_signature_t *sig, pmix_rank_t **dmns, size_t *ndmns);

int prte_grpcomm_API_xcast(prte_grpcomm_signature_t *sig, prte_rml_tag_t tag,
                           pmix_data_buffer_t *msg)
{
    int rc = PRTE_ERROR;
    pmix_data_buffer_t *buf;
    prte_grpcomm_base_active_t *active;
    pmix_rank_t *dmns;
    size_t ndmns;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:xcast sending %u bytes to tag %ld",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == msg) ? 0 : (unsigned int) msg->bytes_used, (long) tag));

    /* this function does not access any framework-global data, and
     * so it does not require us to push it into the event library */

    /* prep the output buffer */
    PMIX_DATA_BUFFER_CREATE(buf);

    /* create the array of participating daemons */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &dmns, &ndmns))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* setup the payload */
    if (PRTE_SUCCESS != (rc = pack_xcast(sig, buf, msg, tag))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        if (NULL != dmns) {
            free(dmns);
        }
        return rc;
    }

    /* cycle thru the actives and see who can send it */
    PMIX_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t)
    {
        if (NULL != active->module->xcast) {
            if (PRTE_SUCCESS == (rc = active->module->xcast(dmns, ndmns, buf))) {
                break;
            }
        }
    }
    if (NULL != dmns) {
        free(dmns);
    }
    return rc;
}

static void grp_construct_stub(int fd, short args, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t *) cbdata;
    int ret = PRTE_SUCCESS;
    prte_grpcomm_base_active_t *active;
    prte_grpcomm_coll_t *coll;
    uint32_t *seq_number;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:grp_construct stub",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* retrieve an existing tracker, create it if not
     * already found. The grp_construct module is responsible
     * for releasing it upon completion of the collective */
    ret = pmix_hash_table_get_value_ptr(&prte_grpcomm_base.sig_table, (void *) cd->sig->groupID,
                                        strlen(cd->sig->groupID), (void **) &seq_number);
    if (PMIX_ERR_NOT_FOUND == ret) {
        seq_number = (uint32_t *) malloc(sizeof(uint32_t));
        *seq_number = 0;
    } else if (PMIX_SUCCESS == ret) {
        *seq_number = *seq_number + 1;
    } else {
        PMIX_OUTPUT((prte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:grp_construct cannot get signature from hash table",
                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(cd);
        return;
    }
    ret = pmix_hash_table_set_value_ptr(&prte_grpcomm_base.sig_table, (void *) cd->sig->groupID,
                                        strlen(cd->sig->groupID), (void *) seq_number);
    if (PMIX_SUCCESS != ret) {
        PMIX_OUTPUT((prte_grpcomm_base_framework.framework_output,
                     "%s rpcomm:base:grp_construct cannot add new signature to hash table",
                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(cd);
        return;
    }
    coll = prte_grpcomm_base_get_tracker(cd->sig, true);
    if (NULL == coll) {
        PMIX_RELEASE(cd->sig);
        PMIX_RELEASE(cd);
        return;
    }
    PMIX_RELEASE(cd->sig);
    cd->sig = NULL;
    coll->cbfunc = cd->grpcbfunc;
    coll->cbdata = cd;

    /* cycle thru the actives and see who can process it */
    PMIX_LIST_FOREACH(active, &prte_grpcomm_base.actives, prte_grpcomm_base_active_t)
    {
        if (NULL != active->module->grp_construct) {
            if (PRTE_SUCCESS == active->module->grp_construct(coll, cd)) {
                break;
            }
        }
    }
}

int prte_grpcomm_API_grp_construct(prte_pmix_grp_caddy_t *cd)
{
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:base:grp_construct",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, grp_construct_stub, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}


static int pack_xcast(prte_grpcomm_signature_t *sig, pmix_data_buffer_t *buffer,
                      pmix_data_buffer_t *message, prte_rml_tag_t tag)
{
    int rc;
    pmix_data_buffer_t data;
    bool compressed;
    pmix_byte_object_t bo;
    size_t sz;

    /* setup an intermediate buffer */
    PMIX_DATA_BUFFER_CONSTRUCT(&data);

    /* pass along the signature */
    rc = prte_grpcomm_sig_pack(&data, sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&data);
        return rc;
    }

    /* pass the final tag */
    rc = PMIx_Data_pack(NULL, &data, &tag, 1, PRTE_RML_TAG);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&data);
        return rc;
    }

    /* copy the payload into the new buffer - this is non-destructive, so our
     * caller is still responsible for releasing any memory in the buffer they
     * gave to us
     */
    rc = PMIx_Data_copy_payload(&data, message);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&data);
        return rc;
    }

    /* see if we want to compress this message */
    if (PMIx_Data_compress((uint8_t *) data.base_ptr, data.bytes_used, (uint8_t **) &bo.bytes,
                           &sz)) {
        /* the data was compressed - mark that we compressed it */
        compressed = true;
        bo.size = sz;
    } else {
        /* mark that it was not compressed */
        compressed = false;
        bo.bytes = data.base_ptr;
        bo.size = data.bytes_used;
        data.base_ptr = NULL;
        data.bytes_used = 0;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&data);
    rc = PMIx_Data_pack(NULL, buffer, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        return rc;
    }
    rc = PMIx_Data_pack(NULL, buffer, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        return rc;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);

    return PRTE_SUCCESS;
}

int prte_pack_ctrl_options(pmix_byte_object_t *ctrlsbo,
                           const pmix_info_t *info, size_t ninfo)
{
    pmix_data_buffer_t ctrlbuf;
    pmix_status_t rc;

    PMIx_Data_buffer_construct(&ctrlbuf);
    rc = PMIx_Data_pack(NULL, &ctrlbuf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Data_buffer_destruct(&ctrlbuf);
        return rc;
    }
    if (0 < ninfo) {
        rc = PMIx_Data_pack(NULL, &ctrlbuf, (void*)info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Data_buffer_destruct(&ctrlbuf);
            return rc;
        }
    }
    /* even if the control buffer is empty, we still have
     * to pack the byte object for it to ensure proper
     * unpacking on the remote end */
    rc = PMIx_Data_unload(&ctrlbuf, ctrlsbo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIx_Data_buffer_destruct(&ctrlbuf);
        return rc;
    }
    PMIx_Data_buffer_destruct(&ctrlbuf);
    return PRTE_SUCCESS;
}
