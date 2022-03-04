/*
 *
 * Copyright (c) 2016-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <math.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "grpcomm_bmg.h"
#include "src/mca/errmgr/detector/errmgr_detector.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"

/* Static API's */
static int bmg_init(void);
static void bmg_finalize(void);
static int rbcast(pmix_data_buffer_t *buf);
static int register_cb_type(prte_grpcomm_rbcast_cb_t callback);
static int unregister_cb_type(int type);

/* Module def */
prte_grpcomm_base_module_t prte_grpcomm_bmg_module = {
    .init = bmg_init,
    .finalize = bmg_finalize,
    .xcast = NULL,
    .allgather = NULL,
    .rbcast = rbcast,
    .register_cb = register_cb_type,
    .unregister_cb = unregister_cb_type
};

/* Internal functions */
static void rbcast_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                        prte_rml_tag_t tag, void *cbdata);
/* internal variables */
static pmix_list_t tracker;

/*
 * registration of callbacks
 */
#define RBCAST_CB_TYPE_MAX 7
static prte_grpcomm_rbcast_cb_t prte_grpcomm_rbcast_cb[RBCAST_CB_TYPE_MAX + 1];

static int register_cb_type(prte_grpcomm_rbcast_cb_t callback)
{
    int i;

    for (i = 0; i < RBCAST_CB_TYPE_MAX; i++) {
        if (NULL == prte_grpcomm_rbcast_cb[i]) {
            prte_grpcomm_rbcast_cb[i] = callback;
            return i;
        }
    }
    return PRTE_ERR_OUT_OF_RESOURCE;
}

static int unregister_cb_type(int type)
{
    if (RBCAST_CB_TYPE_MAX < type || 0 > type) {
        return PRTE_ERR_BAD_PARAM;
    }
    prte_grpcomm_rbcast_cb[type] = NULL;
    return PRTE_SUCCESS;
}

/*
 *  Initialize the module
 */
static int bmg_init(void)
{
    PMIX_CONSTRUCT(&tracker, pmix_list_t);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RBCAST,
                  PRTE_RML_PERSISTENT, rbcast_recv, NULL);
    return PRTE_SUCCESS;
}

/*
 * Finalize the module
 */
static void bmg_finalize(void)
{
    /* cancel the rbcast recv */
    PRTE_RML_CANCEL(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RBCAST);
    PMIX_LIST_DESTRUCT(&tracker);
    return;
}

static int rbcast(pmix_data_buffer_t *buf)
{
    int rc = false;

    /* number of "daemons" equal 1hnp + num of daemons, so here pass ndmns -1 */
    int nprocs = prte_process_info.num_daemons; // -1;
    int vpid;
    int i, d;
    pmix_proc_t daemon;
    vpid = prte_process_info.myproc.rank;

    int log2no = (int) (log(nprocs));
    int start_i, increase_val;

    if (vpid % 2 == 0) {
        start_i = 1;
        increase_val = 1;
    } else {
        start_i = log2no;
        increase_val = -1;
    }
    for (i = start_i; i <= log2no + 1 && i > 0; i = i + increase_val) {
        for (d = 1; d >= -1; d -= 2) {
            int idx = (nprocs + vpid + d * ((int) pow(2, i) - 1)) % nprocs;

            /* daemon.vpid cannot be 0, because daemond id ranges 1-nprocs, thus if idx==0, change
             * it to NO.nprocs */
            /*if (idx ==0 ){
                idx = nprocs;
            }*/
            PMIX_LOAD_PROCID(&daemon, prte_process_info.myproc.nspace, idx);

            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:bmg: broadcast message in %d daemons to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nprocs,
                                 PRTE_NAME_PRINT(&daemon)));

            pmix_data_buffer_t* sndbuf;
            PMIX_DATA_BUFFER_CREATE(sndbuf);
            rc = PMIx_Data_copy_payload(sndbuf, buf);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_BUFFER_RELEASE(sndbuf);
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            PRTE_RML_SEND(rc, daemon.rank, buf, PRTE_RML_TAG_RBCAST);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
            }
        }
    }

    return rc;
}

static void rbcast_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                        prte_rml_tag_t tg, void *cbdata)
{
    int ret, cnt, cbtype;
    pmix_data_buffer_t datbuf, *relay, *rly, *data;
    prte_grpcomm_signature_t sig;
    prte_rml_tag_t tag;
    bool flag;
    pmix_byte_object_t bo, pbo;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:bmg:rbcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) buffer->bytes_used));
    /* we need a passthru buffer to forward and to the callback */
    PMIX_DATA_BUFFER_CREATE(rly);
    PMIX_DATA_BUFFER_CREATE(relay);
    ret = PMIx_Data_copy_payload(rly, buffer);
    if (PMIX_SUCCESS != ret) {
        PMIX_DATA_BUFFER_RELEASE(rly);
        PMIX_DATA_BUFFER_RELEASE(relay);
        return;
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&datbuf);
    /* unpack the flag to see if this payload is compressed */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &flag, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(rly);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        return;
    }
    /* unpack the data blob */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &pbo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_DATA_BUFFER_RELEASE(rly);
        return;
    }
    if (flag) {
        /* decompress the data */
        if (PMIx_Data_decompress((uint8_t *) pbo.bytes, pbo.size,
                                 (uint8_t **) &bo.bytes, &bo.size)) {
            /* the data has been uncompressed */
            ret = PMIx_Data_load(&datbuf, &bo);
            if (PMIX_SUCCESS != ret) {
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
                PMIX_DATA_BUFFER_RELEASE(rly);
                return;
            }
        } else {
            PMIX_ERROR_LOG(PMIX_ERROR);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    } else {
        ret = PMIx_Data_load(&datbuf, &pbo);
        if (PMIX_SUCCESS != ret) {
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PMIX_DATA_BUFFER_RELEASE(rly);
            return;
        }
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
    data = &datbuf;

    /* get the signature that we need to create the dmns*/
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, data, &sig.sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        goto CLEANUP;
    }
    PMIX_PROC_CREATE(sig.signature, sig.sz);
    cnt = sig.sz;
    ret = PMIx_Data_unpack(NULL, data, sig.signature, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        PMIX_PROC_FREE(sig.signature, sig.sz);
        goto CLEANUP;
    }
    /* discard it */
    PMIX_PROC_FREE(sig.signature, sig.sz);

    /* get the target tag */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, data, &tag, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        goto CLEANUP;
    }

    /* copy the remaining payload */
    ret = PMIx_Data_copy_payload(relay, data);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        goto CLEANUP;
    }

    /* get the cbtype */
    cnt = 1;
    ret = PMIx_Data_unpack(NULL, data, &cbtype, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
        goto CLEANUP;
    }
    if (prte_grpcomm_rbcast_cb[cbtype](relay)) {
        /* forward the rbcast */
        ret = rbcast(rly);
        if (PRTE_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&datbuf);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
            goto CLEANUP;
        }
    }

CLEANUP:
    PMIX_DATA_BUFFER_RELEASE(rly);
    PMIX_DATA_BUFFER_RELEASE(relay);
}
