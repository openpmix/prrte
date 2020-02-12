/*
 *
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <math.h>
#include <string.h>

#include "src/dss/dss.h"
#include "src/class/prrte_list.h"
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
#include "src/mca/compress/compress.h"
#include "src/mca/grpcomm/base/base.h"
#include "grpcomm_bmg.h"

/* Static API's */
static int bmg_init(void);
static void bmg_finalize(void);

static int rbcast(prrte_buffer_t *buf);

static int register_cb_type(prrte_grpcomm_rbcast_cb_t callback);

static int unregister_cb_type(int type);
/* Module def */
prrte_grpcomm_base_module_t prrte_grpcomm_bmg_module = {
    bmg_init,
    bmg_finalize,
    NULL,
    NULL,
    rbcast,
    register_cb_type,
    unregister_cb_type
};

/* Internal functions */
static void rbcast_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata);
/* internal variables */
static prrte_list_t tracker;

/*
 * registration of callbacks
 */
#define RBCAST_CB_TYPE_MAX 7
static prrte_grpcomm_rbcast_cb_t prrte_grpcomm_rbcast_cb[RBCAST_CB_TYPE_MAX+1];

int register_cb_type(prrte_grpcomm_rbcast_cb_t callback) {
    int i;

    for(i = 0; i < RBCAST_CB_TYPE_MAX; i++) {
        if( NULL == prrte_grpcomm_rbcast_cb[i] ) {
            prrte_grpcomm_rbcast_cb[i] = callback;
            return i;
        }
    }
    return PRRTE_ERR_OUT_OF_RESOURCE;
}

int unregister_cb_type(int type) {
    if( RBCAST_CB_TYPE_MAX < type || 0 > type ) {
        return PRRTE_ERR_BAD_PARAM;
    }
    prrte_grpcomm_rbcast_cb[type] = NULL;
    return PRRTE_SUCCESS;
}

/*
 *  Initialize the module
 */
static int bmg_init(void)
{
    PRRTE_CONSTRUCT(&tracker, prrte_list_t);

    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_RBCAST,
                            PRRTE_RML_PERSISTENT,
                            rbcast_recv, NULL);
   return PRRTE_SUCCESS;
}

/*
 * Finalize the module
 */
static void bmg_finalize(void)
{
    /* cancel the rbcast recv */
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_RBCAST);
    PRRTE_LIST_DESTRUCT(&tracker);
    return;
}

static int rbcast(prrte_buffer_t *buf)
{
    int rc = false;

    /* number of "daemons" equal 1hnp + num of daemons, so here pass ndmns -1 */
    int nprocs = prrte_process_info.num_procs;// -1;
    int vpid;
    int i, d;
    prrte_process_name_t daemon;
    vpid = prrte_process_info.my_name.vpid;

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
        daemon.jobid = prrte_process_info.my_name.jobid;
        daemon.vpid = idx;
        PRRTE_RETAIN(buf);

        PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                    "%s grpcomm:bmg: broadcast message in %d daemons to %s",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), nprocs,
                    PRRTE_NAME_PRINT(&daemon)));
        if(0 > (rc = prrte_rml.send_buffer_nb(&daemon, buf,
                        PRRTE_RML_TAG_RBCAST, prrte_rml_send_callback, NULL))) {
            PRRTE_ERROR_LOG(rc);
        }
    }

    return rc;
}

static void rbcast_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tg,
                       void* cbdata)
{
    int ret, cnt;
    prrte_buffer_t datbuf,*relay, *rly, *data;
    prrte_grpcomm_signature_t *sig;
    prrte_rml_tag_t tag;
    int cbtype;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;
    int8_t flag;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:bmg:rbcast:recv: with %d bytes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));
    /* we need a passthru buffer to forward and to the callback */
    rly = PRRTE_NEW(prrte_buffer_t);
    relay =  PRRTE_NEW(prrte_buffer_t);
    prrte_dss.copy_payload(rly, buffer);

    PRRTE_CONSTRUCT(&datbuf, prrte_buffer_t);
    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &flag, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_FORCED_TERMINATE(ret);
        return;
     }
    if (flag) {
         /* unpack the data size */
         cnt=1;
         if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &inlen, &cnt, PRRTE_SIZE))) {
             PRRTE_ERROR_LOG(ret);
             PRRTE_FORCED_TERMINATE(ret);
             return;
         }

         /* unpack the unpacked data size */
         cnt=1;
         if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &cmplen, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_FORCED_TERMINATE(ret);
            return;
        }
        /* allocate the space */
        packed_data = (uint8_t*)malloc(inlen);
        /* unpack the data blob */
        cnt = inlen;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, packed_data, &cnt, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(ret);
            free(packed_data);
            PRRTE_FORCED_TERMINATE(ret);
            return;
        }
        /* decompress the data */
        if (prrte_compress.decompress_block(&cmpdata, cmplen,packed_data, inlen)) {
            /* the data has been uncompressed */
            prrte_dss.load(&datbuf, cmpdata, cmplen);
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
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(data, &sig, &cnt, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_DESTRUCT(&datbuf);
        PRRTE_RELEASE(sig);
        PRRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    /* get the target tag */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(data, &tag, &cnt, PRRTE_RML_TAG))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    prrte_dss.copy_payload(relay, data);
    /* get the cbtype */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(data, &cbtype, &cnt,PRRTE_INT ))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    if( prrte_grpcomm_rbcast_cb[cbtype](relay) ) {
        /* forward the rbcast */
        if (PRRTE_SUCCESS == (ret = rbcast(rly))) {
        }
    }

CLEANUP:
    PRRTE_RELEASE(rly);
    PRRTE_RELEASE(relay);
}

