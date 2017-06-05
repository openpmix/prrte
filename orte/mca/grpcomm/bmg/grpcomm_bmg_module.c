/*
 *
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"
#include "orte/runtime/orte_wait.h"

#include <math.h>
#include <string.h>

#include "opal/dss/dss.h"
#include "opal/class/opal_list.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/regx/regx.h"
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/base/rml_contact.h"
#include "orte/mca/routed/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/state/state.h"
#include "orte/util/compress.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/errmgr/detector/errmgr_detector.h"
#include "orte/mca/grpcomm/base/base.h"
#include "grpcomm_bmg.h"

/* Static API's */
static int bmg_init(void);
static void bmg_finalize(void);

static int rbcast(opal_buffer_t *buf);

static int register_cb_type(orte_grpcomm_rbcast_cb_t callback);

static int unregister_cb_type(int type);
/* Module def */
orte_grpcomm_base_module_t orte_grpcomm_bmg_module = {
    bmg_init,
    bmg_finalize,
    NULL,
    NULL,
    rbcast,
    register_cb_type,
    unregister_cb_type
};

/* Internal functions */
static void rbcast_recv(int status, orte_process_name_t* sender,
                       opal_buffer_t* buffer, orte_rml_tag_t tag,
                       void* cbdata);
/* internal variables */
static opal_list_t tracker;

/*
 * registration of callbacks
 */
#define RBCAST_CB_TYPE_MAX 7
static orte_grpcomm_rbcast_cb_t orte_grpcomm_rbcast_cb[RBCAST_CB_TYPE_MAX+1];

int register_cb_type(orte_grpcomm_rbcast_cb_t callback) {
    int i;

    for(i = 0; i < RBCAST_CB_TYPE_MAX; i++) {
        if( NULL == orte_grpcomm_rbcast_cb[i] ) {
            orte_grpcomm_rbcast_cb[i] = callback;
            return i;
        }
    }
    return ORTE_ERR_OUT_OF_RESOURCE;
}

int unregister_cb_type(int type) {
    if( RBCAST_CB_TYPE_MAX < type || 0 > type ) {
        return ORTE_ERR_BAD_PARAM;
    }
    orte_grpcomm_rbcast_cb[type] = NULL;
    return ORTE_SUCCESS;
}

/*
 *  Initialize the module
 */
static int bmg_init(void)
{
    OBJ_CONSTRUCT(&tracker, opal_list_t);

    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                            ORTE_RML_TAG_RBCAST,
                            ORTE_RML_PERSISTENT,
                            rbcast_recv, NULL);
   return OPAL_SUCCESS;
}

/*
 * Finalize the module
 */
static void bmg_finalize(void)
{
    /* cancel the rbcast recv */
    orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_RBCAST);
    OPAL_LIST_DESTRUCT(&tracker);
    return;
}

static int rbcast(opal_buffer_t *buf)
{
    int rc = false;

    /* number of "daemons" equal 1hnp + num of daemons, so here pass ndmns -1 */
    int nprocs = orte_process_info.num_procs;// -1;
    int vpid;
    int i, d;
    orte_process_name_t daemon;
    vpid = orte_process_info.my_name.vpid;

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
        daemon.jobid = orte_process_info.my_name.jobid;
        daemon.vpid = idx;
        OBJ_RETAIN(buf);

        OPAL_OUTPUT_VERBOSE((1, orte_grpcomm_base_framework.framework_output,
                    "%s grpcomm:bmg: broadcast message in %d daemons to %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), nprocs,
                    ORTE_NAME_PRINT(&daemon)));
        if(0 > (rc = orte_rml.send_buffer_nb(orte_coll_conduit, &daemon, buf,
                        ORTE_RML_TAG_RBCAST, orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }

    return rc;
}

static void rbcast_recv(int status, orte_process_name_t* sender,
                       opal_buffer_t* buffer, orte_rml_tag_t tg,
                       void* cbdata)
{
    int ret, cnt;
    opal_buffer_t datbuf,*relay, *rly, *data;
    orte_grpcomm_signature_t *sig;
    orte_rml_tag_t tag;
    int cbtype;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;
    int8_t flag;

    OPAL_OUTPUT_VERBOSE((1, orte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:bmg:rbcast:recv: with %d bytes",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));
    /* we need a passthru buffer to forward and to the callback */
    rly = OBJ_NEW(opal_buffer_t);
    relay =  OBJ_NEW(opal_buffer_t);
    opal_dss.copy_payload(rly, buffer);

    OBJ_CONSTRUCT(&datbuf, opal_buffer_t);
    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &flag, &cnt, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        ORTE_FORCED_TERMINATE(ret);
        return;
     }
    if (flag) {
         /* unpack the data size */
         cnt=1;
         if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &inlen, &cnt, OPAL_SIZE))) {
             ORTE_ERROR_LOG(ret);
             ORTE_FORCED_TERMINATE(ret);
             return;
         }

         /* unpack the unpacked data size */
         cnt=1;
         if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, &cmplen, &cnt, OPAL_SIZE))) {
            ORTE_ERROR_LOG(ret);
            ORTE_FORCED_TERMINATE(ret);
            return;
        }
        /* allocate the space */
        packed_data = (uint8_t*)malloc(inlen);
        /* unpack the data blob */
        cnt = inlen;
        if (ORTE_SUCCESS != (ret = opal_dss.unpack(buffer, packed_data, &cnt, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            free(packed_data);
            ORTE_FORCED_TERMINATE(ret);
            return;
        }
        /* decompress the data */
        if (orte_util_uncompress_block(&cmpdata, cmplen,packed_data, inlen)) {
            /* the data has been uncompressed */
            opal_dss.load(&datbuf, cmpdata, cmplen);
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
    if (ORTE_SUCCESS != (ret = opal_dss.unpack(data, &sig, &cnt, ORTE_SIGNATURE))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&datbuf);
        OBJ_RELEASE(sig);
        ORTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    /* get the target tag */
    cnt=1;
    if (ORTE_SUCCESS != (ret = opal_dss.unpack(data, &tag, &cnt, ORTE_RML_TAG))) {
        ORTE_ERROR_LOG(ret);
        ORTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    opal_dss.copy_payload(relay, data);
    /* get the cbtype */
    cnt=1;
    if (ORTE_SUCCESS != (ret = opal_dss.unpack(data, &cbtype, &cnt,OPAL_INT ))) {
        ORTE_ERROR_LOG(ret);
        ORTE_FORCED_TERMINATE(ret);
        goto CLEANUP;
    }
    if( orte_grpcomm_rbcast_cb[cbtype](relay) ) {
        /* forward the rbcast */
        if (ORTE_SUCCESS == (ret = rbcast(rly))) {
        }
    }

CLEANUP:
    OBJ_RELEASE(rly);
    OBJ_RELEASE(relay);
}

