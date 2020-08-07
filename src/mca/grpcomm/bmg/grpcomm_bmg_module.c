/*
 *
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
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

#include "src/dss/dss.h"
#include "src/class/prte_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/mca/routed/routed.h"
#include "src/mca/errmgr/detector/errmgr_detector.h"
#include "src/mca/prtecompress/prtecompress.h"
#include "src/mca/grpcomm/base/base.h"
#include "grpcomm_bmg.h"

/* Static API's */
static int bmg_init(void);
static void bmg_finalize(void);
static int rbcast(prte_buffer_t *buf);
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
static void rbcast_recv(int status, prte_process_name_t* sender,
                       prte_buffer_t* buffer, prte_rml_tag_t tag,
                       void* cbdata);
/* internal variables */
static prte_list_t tracker;

/*
 * registration of callbacks
 */
#define RBCAST_CB_TYPE_MAX 7
static prte_grpcomm_rbcast_cb_t prte_grpcomm_rbcast_cb[RBCAST_CB_TYPE_MAX+1];

static int register_cb_type(prte_grpcomm_rbcast_cb_t callback) {
    int i;

    for(i = 0; i < RBCAST_CB_TYPE_MAX; i++) {
        if( NULL == prte_grpcomm_rbcast_cb[i] ) {
            prte_grpcomm_rbcast_cb[i] = callback;
            return i;
        }
    }
    return PRTE_ERR_OUT_OF_RESOURCE;
}

static int unregister_cb_type(int type) {
    if( RBCAST_CB_TYPE_MAX < type || 0 > type ) {
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
    PRTE_CONSTRUCT(&tracker, prte_list_t);

    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_RBCAST,
                            PRTE_RML_PERSISTENT,
                            rbcast_recv, NULL);
   return PRTE_SUCCESS;
}

/*
 * Finalize the module
 */
static void bmg_finalize(void)
{
    /* cancel the rbcast recv */
    prte_rml.recv_cancel(PRTE_NAME_WILDCARD, PRTE_RML_TAG_RBCAST);
    PRTE_LIST_DESTRUCT(&tracker);
    return;
}

static int rbcast(prte_buffer_t *buf)
{
    int rc = false;

    /* number of "daemons" equal 1hnp + num of daemons, so here pass ndmns -1 */
    int nprocs = prte_process_info.num_daemons;// -1;
    int vpid;
    int i, d;
    prte_process_name_t daemon;
    vpid = prte_process_info.my_name.vpid;

    int log2no = (int)(log(nprocs));
    int start_i, increase_val;

    if(vpid%2==0)
    {
        start_i = 1;
        increase_val = 1;
    }
    else
    {
        start_i = log2no;
        increase_val = -1;
    }
    for(i=start_i; i <= log2no+1 && i >0; i=i+increase_val) for(d=1; d >= -1; d-=2) {
    //for(i=1; i <= nprocs/2; i*=2) for(d=1; d >= -1; d-=2) {
        int idx = (nprocs+vpid+d*((int)pow(2,i)-1))%nprocs;

        /* daemon.vpid cannot be 0, because daemond id ranges 1-nprocs, thus if idx==0, change it to NO.nprocs */
        /*if (idx ==0 ){
            idx = nprocs;
        }*/
        daemon.jobid = prte_process_info.my_name.jobid;
        daemon.vpid = idx;
        PRTE_RETAIN(buf);

        PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                    "%s grpcomm:bmg: broadcast message in %d daemons to %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nprocs,
                    PRTE_NAME_PRINT(&daemon)));
        if(0 > (rc = prte_rml.send_buffer_nb(&daemon, buf,
                        PRTE_RML_TAG_RBCAST, prte_rml_send_callback, NULL))) {
            PRTE_ERROR_LOG(rc);
        }
    }

    return rc;
}

static void rbcast_recv(int status, prte_process_name_t* sender,
                       prte_buffer_t* buffer, prte_rml_tag_t tg,
                       void* cbdata)
{
    int ret, cnt;
    prte_buffer_t datbuf,*relay, *rly, *data;
    prte_grpcomm_signature_t *sig;
    prte_rml_tag_t tag;
    int cbtype;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;
    int8_t flag;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:bmg:rbcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));
    /* we need a passthru buffer to forward and to the callback */
    rly = PRTE_NEW(prte_buffer_t);
    relay =  PRTE_NEW(prte_buffer_t);
    prte_dss.copy_payload(rly, buffer);

    PRTE_CONSTRUCT(&datbuf, prte_buffer_t);
    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &flag, &cnt, PRTE_INT8))) {
        PRTE_ERROR_LOG(ret);
        PRTE_FORCED_TERMINATE(ret);
        return;
     }
    if (flag) {
         /* unpack the data size */
         cnt=1;
         if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &inlen, &cnt, PRTE_SIZE))) {
             PRTE_ERROR_LOG(ret);
             PRTE_FORCED_TERMINATE(ret);
             return;
         }

         /* unpack the unpacked data size */
         cnt=1;
         if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &cmplen, &cnt, PRTE_SIZE))) {
            PRTE_ERROR_LOG(ret);
            PRTE_FORCED_TERMINATE(ret);
            return;
        }
        /* allocate the space */
        packed_data = (uint8_t*)malloc(inlen);
        /* unpack the data blob */
        cnt = inlen;
        if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, packed_data, &cnt, PRTE_UINT8))) {
            PRTE_ERROR_LOG(ret);
            free(packed_data);
            PRTE_FORCED_TERMINATE(ret);
            return;
        }
        /* decompress the data */
        if (prte_compress.decompress_block(&cmpdata, cmplen,packed_data, inlen)) {
            /* the data has been uncompressed */
            prte_dss.load(&datbuf, cmpdata, cmplen);
            data = &datbuf;
        } else {
                    data = buffer;
               }
               free(packed_data);
               } else {
                            data = buffer;
               }
    /* get the signature that we need to create the dmns*/
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(data, &sig, &cnt, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(ret);
        PRTE_DESTRUCT(&datbuf);
        PRTE_RELEASE(sig);
        PRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    /* get the target tag */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(data, &tag, &cnt, PRTE_RML_TAG))) {
        PRTE_ERROR_LOG(ret);
        PRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    prte_dss.copy_payload(relay, data);
    /* get the cbtype */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(data, &cbtype, &cnt,PRTE_INT ))) {
        PRTE_ERROR_LOG(ret);
        PRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    if( prte_grpcomm_rbcast_cb[cbtype](relay) ) {
        /* forward the rbcast */
        if (PRTE_SUCCESS == (ret = rbcast(rly))) {
        }
    }

CLEANUP:
    PRTE_RELEASE(rly);
    PRTE_RELEASE(relay);
}

