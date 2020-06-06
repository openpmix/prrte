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

#include "src/dss/dss.h"
#include "src/class/prte_list.h"
#include "src/pmix/pmix-internal.h"
#include "src/mca/prtecompress/prtecompress.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"

#include "src/mca/grpcomm/base/base.h"
#include "grpcomm_direct.h"


/* Static API's */
static int init(void);
static void finalize(void);
static int xcast(prte_vpid_t *vpids,
                 size_t nprocs,
                 prte_buffer_t *buf);
static int allgather(prte_grpcomm_coll_t *coll,
                     prte_buffer_t *buf, int mode);

/* Module def */
prte_grpcomm_base_module_t prte_grpcomm_direct_module = {
    init,
    finalize,
    xcast,
    allgather,
    NULL,
    NULL,
    NULL
};

/* internal functions */
static void xcast_recv(int status, prte_process_name_t* sender,
                       prte_buffer_t* buffer, prte_rml_tag_t tag,
                       void* cbdata);
static void allgather_recv(int status, prte_process_name_t* sender,
                           prte_buffer_t* buffer, prte_rml_tag_t tag,
                           void* cbdata);
static void barrier_release(int status, prte_process_name_t* sender,
                            prte_buffer_t* buffer, prte_rml_tag_t tag,
                            void* cbdata);

/* internal variables */
static prte_list_t tracker;

/**
 * Initialize the module
 */
static int init(void)
{
    PRTE_CONSTRUCT(&tracker, prte_list_t);

    /* post the receives */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_XCAST,
                            PRTE_RML_PERSISTENT,
                            xcast_recv, NULL);
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_ALLGATHER_DIRECT,
                            PRTE_RML_PERSISTENT,
                            allgather_recv, NULL);
    /* setup recv for barrier release */
    prte_rml.recv_buffer_nb(PRTE_NAME_WILDCARD,
                            PRTE_RML_TAG_COLL_RELEASE,
                            PRTE_RML_PERSISTENT,
                            barrier_release, NULL);

    return PRTE_SUCCESS;
}

/**
 * Finalize the module
 */
static void finalize(void)
{
    PRTE_LIST_DESTRUCT(&tracker);
    return;
}

static int xcast(prte_vpid_t *vpids,
                 size_t nprocs,
                 prte_buffer_t *buf)
{
    int rc;

    /* send it to the HNP (could be myself) for relay */
    PRTE_RETAIN(buf);  // we'll let the RML release it
    if (0 > (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_XCAST,
                                          prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return rc;
    }
    return PRTE_SUCCESS;
}

static int allgather(prte_grpcomm_coll_t *coll,
                     prte_buffer_t *buf, int mode)
{
    int rc;
    prte_buffer_t *relay;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: allgather",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* the base functions pushed us into the event library
     * before calling us, so we can safely access global data
     * at this point */

    relay = PRTE_NEW(prte_buffer_t);
    /* pack the signature */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(relay, &coll->sig, 1, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(relay);
        return rc;
    }

    /* pack the mode */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(relay, &mode, 1, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(relay);
        return rc;
    }

    /* pass along the payload */
    prte_dss.copy_payload(relay, buf);

    /* send this to ourselves for processing */
    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:allgather sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* send the info to ourselves for tracking */
    rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_NAME, relay,
                                 PRTE_RML_TAG_ALLGATHER_DIRECT,
                                 prte_rml_send_callback, NULL);
    return rc;
}

static void allgather_recv(int status, prte_process_name_t* sender,
                           prte_buffer_t* buffer, prte_rml_tag_t tag,
                           void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prte_grpcomm_signature_t *sig;
    prte_buffer_t *reply;
    prte_grpcomm_coll_t *coll;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* unpack the signature */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &sig, &cnt, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = prte_grpcomm_base_get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PRTE_RELEASE(sig);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &mode, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        return;
    }
    /* increment nprocs reported for collective */
    coll->nreported++;
    /* capture any provided content */
    prte_dss.copy_payload(&coll->bucket, buffer);

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recv nexpected %d nrep %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int)coll->nexpected, (int)coll->nreported));

    /* see if everyone has reported */
    if (coll->nreported == coll->nexpected) {
        if (PRTE_PROC_IS_MASTER) {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather HNP reports complete",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            /* the allgather is complete - send the xcast */
            reply = PRTE_NEW(prte_buffer_t);
            /* pack the signature */
            if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &sig, 1, PRTE_SIGNATURE))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(reply);
                PRTE_RELEASE(sig);
                return;
            }
            /* pack the status - success since the allgather completed. This
             * would be an error if we timeout instead */
            ret = PRTE_SUCCESS;
            if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &ret, 1, PRTE_INT))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(reply);
                PRTE_RELEASE(sig);
                return;
            }
            /* pack the mode */
            if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &mode, 1, PRTE_INT))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(reply);
                PRTE_RELEASE(sig);
                return;
            }
            /* if we were asked to provide a context id, do so */
            if (1 == mode) {
                size_t sz;
                sz = prte_grpcomm_base.context_id;
                ++prte_grpcomm_base.context_id;
                if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &sz, 1, PRTE_SIZE))) {
                    PRTE_ERROR_LOG(rc);
                    PRTE_RELEASE(reply);
                    PRTE_RELEASE(sig);
                    return;
                }
            }
            /* transfer the collected bucket */
            prte_dss.copy_payload(reply, &coll->bucket);
            /* send the release via xcast */
            (void)prte_grpcomm.xcast(sig, PRTE_RML_TAG_COLL_RELEASE, reply);
            PRTE_RELEASE(reply);
        } else {
            PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather rollup complete - sending to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));
            /* relay the bucket upward */
            reply = PRTE_NEW(prte_buffer_t);
            /* pack the signature */
            if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &sig, 1, PRTE_SIGNATURE))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(reply);
                PRTE_RELEASE(sig);
                return;
            }
            /* pack the mode */
            if (PRTE_SUCCESS != (rc = prte_dss.pack(reply, &mode, 1, PRTE_INT))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(reply);
                PRTE_RELEASE(sig);
                return;
            }
            /* transfer the collected bucket */
            prte_dss.copy_payload(reply, &coll->bucket);
            /* send the info to our parent */
            rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_PARENT, reply,
                                         PRTE_RML_TAG_ALLGATHER_DIRECT,
                                         prte_rml_send_callback, NULL);
        }
    }
    PRTE_RELEASE(sig);
}

static void xcast_recv(int status, prte_process_name_t* sender,
                       prte_buffer_t* buffer, prte_rml_tag_t tg,
                       void* cbdata)
{
    prte_list_item_t *item;
    prte_namelist_t *nm;
    int ret, cnt;
    prte_buffer_t *relay=NULL, *rly;
    prte_daemon_cmd_flag_t command = PRTE_DAEMON_NULL_CMD;
    prte_buffer_t datbuf, *data;
    int8_t flag;
    prte_job_t *jdata;
    prte_proc_t *rec;
    prte_list_t coll;
    prte_grpcomm_signature_t *sig;
    prte_rml_tag_t tag;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;

    PRTE_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast:recv: with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));

    /* we need a passthru buffer to send to our children - we leave it
     * as compressed data */
    rly = PRTE_NEW(prte_buffer_t);
    prte_dss.copy_payload(rly, buffer);
    PRTE_CONSTRUCT(&datbuf, prte_buffer_t);
    /* setup the relay list */
    PRTE_CONSTRUCT(&coll, prte_list_t);

    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &flag, &cnt, PRTE_INT8))) {
        PRTE_ERROR_LOG(ret);
        PRTE_FORCED_TERMINATE(ret);
        PRTE_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PRTE_RELEASE(rly);
        return;
    }
    if (flag) {
        /* unpack the data size */
        cnt=1;
        if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &inlen, &cnt, PRTE_SIZE))) {
            PRTE_ERROR_LOG(ret);
            PRTE_FORCED_TERMINATE(ret);
            PRTE_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PRTE_RELEASE(rly);
            return;
        }
        /* unpack the unpacked data size */
        cnt=1;
        if (PRTE_SUCCESS != (ret = prte_dss.unpack(buffer, &cmplen, &cnt, PRTE_SIZE))) {
            PRTE_ERROR_LOG(ret);
            PRTE_FORCED_TERMINATE(ret);
            PRTE_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PRTE_RELEASE(rly);
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
            PRTE_DESTRUCT(&datbuf);
            PRTE_DESTRUCT(&coll);
            PRTE_RELEASE(rly);
            return;
        }
        /* decompress the data */
        if (prte_compress.decompress_block(&cmpdata, cmplen,
                                       packed_data, inlen)) {
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

    /* get the signature that we do not need */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(data, &sig, &cnt, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(ret);
        PRTE_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PRTE_RELEASE(rly);
        PRTE_FORCED_TERMINATE(ret);
        return;
    }
    PRTE_RELEASE(sig);

    /* get the target tag */
    cnt=1;
    if (PRTE_SUCCESS != (ret = prte_dss.unpack(data, &tag, &cnt, PRTE_RML_TAG))) {
        PRTE_ERROR_LOG(ret);
        PRTE_DESTRUCT(&datbuf);
        PRTE_DESTRUCT(&coll);
        PRTE_RELEASE(rly);
        PRTE_FORCED_TERMINATE(ret);
        return;
    }

    /* copy the msg for relay to ourselves */
    relay = PRTE_NEW(prte_buffer_t);
    prte_dss.copy_payload(relay, data);

    if (!prte_do_not_launch) {
        /* get the list of next recipients from the routed module */
        prte_routed.get_routing_list(&coll);

        /* if list is empty, no relay is required */
        if (prte_list_is_empty(&coll)) {
            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay - recipient list is empty!",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            goto CLEANUP;
        }

        /* send the message to each recipient on list, deconstructing it as we go */
        while (NULL != (item = prte_list_remove_first(&coll))) {
            nm = (prte_namelist_t*)item;

            PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay sending relay msg of %d bytes to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int)rly->bytes_used,
                                 PRTE_NAME_PRINT(&nm->name)));
            PRTE_RETAIN(rly);
            /* check the state of the recipient - no point
             * sending to someone not alive
             */
            jdata = prte_get_job_data_object(nm->name.jobid);
            if (NULL == (rec = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, nm->name.vpid))) {
                if (!prte_abnormal_term_ordered && !prte_prteds_term_ordered) {
                    prte_output(0, "%s grpcomm:direct:send_relay proc %s not found - cannot relay",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name));
                }
                PRTE_RELEASE(rly);
                PRTE_RELEASE(item);
                PRTE_FORCED_TERMINATE(PRTE_ERR_UNREACH);
                continue;
            }
            if ((PRTE_PROC_STATE_RUNNING < rec->state &&
                PRTE_PROC_STATE_CALLED_ABORT != rec->state) ||
                !PRTE_FLAG_TEST(rec, PRTE_PROC_FLAG_ALIVE)) {
                if (!prte_abnormal_term_ordered && !prte_prteds_term_ordered) {
                    prte_output(0, "%s grpcomm:direct:send_relay proc %s not running - cannot relay: %s ",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name),
                                PRTE_FLAG_TEST(rec, PRTE_PROC_FLAG_ALIVE) ? prte_proc_state_to_str(rec->state) : "NOT ALIVE");
                }
                PRTE_RELEASE(rly);
                PRTE_RELEASE(item);
                PRTE_FORCED_TERMINATE(PRTE_ERR_UNREACH);
                continue;
            }
            if (PRTE_SUCCESS != (ret = prte_rml.send_buffer_nb(&nm->name, rly, PRTE_RML_TAG_XCAST,
                                                               prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(ret);
                PRTE_RELEASE(rly);
                PRTE_RELEASE(item);
                PRTE_FORCED_TERMINATE(PRTE_ERR_UNREACH);
                continue;
            }
            PRTE_RELEASE(item);
        }
    }

 CLEANUP:
    /* cleanup */
    PRTE_LIST_DESTRUCT(&coll);
    PRTE_RELEASE(rly);  // retain accounting

    /* now pass the relay buffer to myself for processing - don't
     * inject it into the RML system via send as that will compete
     * with the relay messages down in the OOB. Instead, pass it
     * directly to the RML message processor */
    if (PRTE_DAEMON_DVM_NIDMAP_CMD != command) {
        PRTE_RML_POST_MESSAGE(PRTE_PROC_MY_NAME, tag, 1,
                              relay->base_ptr, relay->bytes_used);
        relay->base_ptr = NULL;
        relay->bytes_used = 0;
    }
    if (NULL != relay) {
        PRTE_RELEASE(relay);
    }
    PRTE_DESTRUCT(&datbuf);
}

static void barrier_release(int status, prte_process_name_t* sender,
                            prte_buffer_t* buffer, prte_rml_tag_t tag,
                            void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prte_grpcomm_signature_t *sig;
    prte_grpcomm_coll_t *coll;

    PRTE_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: barrier release called with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int)buffer->bytes_used));

    /* unpack the signature */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &sig, &cnt, PRTE_SIGNATURE))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &ret, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &mode, &cnt, PRTE_INT))) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker - it is not an error if not
     * found as that just means we wre not involved
     * in the collective */
    if (NULL == (coll = prte_grpcomm_base_get_tracker(sig, false))) {
        PRTE_RELEASE(sig);
        return;
    }

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, buffer, coll->cbdata);
    }
    prte_list_remove_item(&prte_grpcomm_base.ongoing, &coll->super);
    PRTE_RELEASE(coll);
    PRTE_RELEASE(sig);
}
