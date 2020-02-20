/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
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

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/dss/dss.h"
#include "src/class/prrte_list.h"
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
static int xcast(prrte_vpid_t *vpids,
                 size_t nprocs,
                 prrte_buffer_t *buf);
static int allgather(prrte_grpcomm_coll_t *coll,
                     prrte_buffer_t *buf, int mode);

/* Module def */
prrte_grpcomm_base_module_t prrte_grpcomm_direct_module = {
    init,
    finalize,
    xcast,
    allgather
};

/* internal functions */
static void xcast_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata);
static void allgather_recv(int status, prrte_process_name_t* sender,
                           prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                           void* cbdata);
static void barrier_release(int status, prrte_process_name_t* sender,
                            prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                            void* cbdata);

/* internal variables */
static prrte_list_t tracker;

/**
 * Initialize the module
 */
static int init(void)
{
    PRRTE_CONSTRUCT(&tracker, prrte_list_t);

    /* post the receives */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_XCAST,
                            PRRTE_RML_PERSISTENT,
                            xcast_recv, NULL);
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_ALLGATHER_DIRECT,
                            PRRTE_RML_PERSISTENT,
                            allgather_recv, NULL);
    /* setup recv for barrier release */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_COLL_RELEASE,
                            PRRTE_RML_PERSISTENT,
                            barrier_release, NULL);

    return PRRTE_SUCCESS;
}

/**
 * Finalize the module
 */
static void finalize(void)
{
    PRRTE_LIST_DESTRUCT(&tracker);
    return;
}

static int xcast(prrte_vpid_t *vpids,
                 size_t nprocs,
                 prrte_buffer_t *buf)
{
    int rc;

    /* send it to the HNP (could be myself) for relay */
    PRRTE_RETAIN(buf);  // we'll let the RML release it
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf, PRRTE_RML_TAG_XCAST,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return rc;
    }
    return PRRTE_SUCCESS;
}

static int allgather(prrte_grpcomm_coll_t *coll,
                     prrte_buffer_t *buf, int mode)
{
    int rc;
    prrte_buffer_t *relay;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: allgather",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* the base functions pushed us into the event library
     * before calling us, so we can safely access global data
     * at this point */

    relay = PRRTE_NEW(prrte_buffer_t);
    /* pack the signature */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(relay, &coll->sig, 1, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(relay);
        return rc;
    }

    /* pack the mode */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(relay, &mode, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(relay);
        return rc;
    }

    /* pass along the payload */
    prrte_dss.copy_payload(relay, buf);

    /* send this to ourselves for processing */
    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:allgather sending to ourself",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* send the info to ourselves for tracking */
    rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_NAME, relay,
                                 PRRTE_RML_TAG_ALLGATHER_DIRECT,
                                 prrte_rml_send_callback, NULL);
    return rc;
}

static void allgather_recv(int status, prrte_process_name_t* sender,
                           prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                           void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prrte_grpcomm_signature_t *sig;
    prrte_buffer_t *reply;
    prrte_grpcomm_coll_t *coll;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recvd from %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(sender)));

    /* unpack the signature */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &sig, &cnt, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = prrte_grpcomm_base_get_tracker(sig, true))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        PRRTE_RELEASE(sig);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &mode, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }
    /* increment nprocs reported for collective */
    coll->nreported++;
    /* capture any provided content */
    prrte_dss.copy_payload(&coll->bucket, buffer);

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct allgather recv nexpected %d nrep %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)coll->nexpected, (int)coll->nreported));

    /* see if everyone has reported */
    if (coll->nreported == coll->nexpected) {
        if (PRRTE_PROC_IS_MASTER) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather HNP reports complete",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            /* the allgather is complete - send the xcast */
            reply = PRRTE_NEW(prrte_buffer_t);
            /* pack the signature */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &sig, 1, PRRTE_SIGNATURE))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(reply);
                PRRTE_RELEASE(sig);
                return;
            }
            /* pack the status - success since the allgather completed. This
             * would be an error if we timeout instead */
            ret = PRRTE_SUCCESS;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &ret, 1, PRRTE_INT))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(reply);
                PRRTE_RELEASE(sig);
                return;
            }
            /* pack the mode */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &mode, 1, PRRTE_INT))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(reply);
                PRRTE_RELEASE(sig);
                return;
            }
            /* if we were asked to provide a context id, do so */
            if (1 == mode) {
                size_t sz;
                sz = prrte_grpcomm_base.context_id;
                ++prrte_grpcomm_base.context_id;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &sz, 1, PRRTE_SIZE))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(reply);
                    PRRTE_RELEASE(sig);
                    return;
                }
            }
            /* transfer the collected bucket */
            prrte_dss.copy_payload(reply, &coll->bucket);
            /* send the release via xcast */
            (void)prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_COLL_RELEASE, reply);
            PRRTE_RELEASE(reply);
        } else {
            PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather rollup complete - sending to %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_PARENT)));
            /* relay the bucket upward */
            reply = PRRTE_NEW(prrte_buffer_t);
            /* pack the signature */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &sig, 1, PRRTE_SIGNATURE))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(reply);
                PRRTE_RELEASE(sig);
                return;
            }
            /* pack the mode */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &mode, 1, PRRTE_INT))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(reply);
                PRRTE_RELEASE(sig);
                return;
            }
            /* transfer the collected bucket */
            prrte_dss.copy_payload(reply, &coll->bucket);
            /* send the info to our parent */
            rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_PARENT, reply,
                                         PRRTE_RML_TAG_ALLGATHER_DIRECT,
                                         prrte_rml_send_callback, NULL);
        }
    }
    PRRTE_RELEASE(sig);
}

static void xcast_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tg,
                       void* cbdata)
{
    prrte_list_item_t *item;
    prrte_namelist_t *nm;
    int ret, cnt;
    prrte_buffer_t *relay=NULL, *rly;
    prrte_daemon_cmd_flag_t command = PRRTE_DAEMON_NULL_CMD;
    prrte_buffer_t datbuf, *data;
    int8_t flag;
    prrte_job_t *jdata;
    prrte_proc_t *rec;
    prrte_list_t coll;
    prrte_grpcomm_signature_t *sig;
    prrte_rml_tag_t tag;
    size_t inlen, cmplen;
    uint8_t *packed_data, *cmpdata;

    PRRTE_OUTPUT_VERBOSE((1, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:xcast:recv: with %d bytes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)buffer->bytes_used));

    /* we need a passthru buffer to send to our children - we leave it
     * as compressed data */
    rly = PRRTE_NEW(prrte_buffer_t);
    prrte_dss.copy_payload(rly, buffer);
    PRRTE_CONSTRUCT(&datbuf, prrte_buffer_t);
    /* setup the relay list */
    PRRTE_CONSTRUCT(&coll, prrte_list_t);

    /* unpack the flag to see if this payload is compressed */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &flag, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_FORCED_TERMINATE(ret);
        PRRTE_DESTRUCT(&datbuf);
        PRRTE_DESTRUCT(&coll);
        PRRTE_RELEASE(rly);
        return;
    }
    if (flag) {
        /* unpack the data size */
        cnt=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &inlen, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_FORCED_TERMINATE(ret);
            PRRTE_DESTRUCT(&datbuf);
            PRRTE_DESTRUCT(&coll);
            PRRTE_RELEASE(rly);
            return;
        }
        /* unpack the unpacked data size */
        cnt=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(buffer, &cmplen, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_FORCED_TERMINATE(ret);
            PRRTE_DESTRUCT(&datbuf);
            PRRTE_DESTRUCT(&coll);
            PRRTE_RELEASE(rly);
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
            PRRTE_DESTRUCT(&datbuf);
            PRRTE_DESTRUCT(&coll);
            PRRTE_RELEASE(rly);
            return;
        }
        /* decompress the data */
        if (prrte_compress.decompress_block(&cmpdata, cmplen,
                                       packed_data, inlen)) {
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

    /* get the signature that we do not need */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(data, &sig, &cnt, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_DESTRUCT(&datbuf);
        PRRTE_DESTRUCT(&coll);
        PRRTE_RELEASE(rly);
        PRRTE_FORCED_TERMINATE(ret);
        return;
    }
    PRRTE_RELEASE(sig);

    /* get the target tag */
    cnt=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss.unpack(data, &tag, &cnt, PRRTE_RML_TAG))) {
        PRRTE_ERROR_LOG(ret);
        PRRTE_DESTRUCT(&datbuf);
        PRRTE_DESTRUCT(&coll);
        PRRTE_RELEASE(rly);
        PRRTE_FORCED_TERMINATE(ret);
        return;
    }

    /* copy the msg for relay to ourselves */
    relay = PRRTE_NEW(prrte_buffer_t);
    prrte_dss.copy_payload(relay, data);

    if (!prrte_do_not_launch) {
        /* get the list of next recipients from the routed module */
        prrte_routed.get_routing_list(&coll);

        /* if list is empty, no relay is required */
        if (prrte_list_is_empty(&coll)) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay - recipient list is empty!",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
            goto CLEANUP;
        }

        /* send the message to each recipient on list, deconstructing it as we go */
        while (NULL != (item = prrte_list_remove_first(&coll))) {
            nm = (prrte_namelist_t*)item;

            PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:send_relay sending relay msg of %d bytes to %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)rly->bytes_used,
                                 PRRTE_NAME_PRINT(&nm->name)));
            PRRTE_RETAIN(rly);
            /* check the state of the recipient - no point
             * sending to someone not alive
             */
            jdata = prrte_get_job_data_object(nm->name.jobid);
            if (NULL == (rec = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, nm->name.vpid))) {
                if (!prrte_abnormal_term_ordered && !prrte_prteds_term_ordered) {
                    prrte_output(0, "%s grpcomm:direct:send_relay proc %s not found - cannot relay",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(&nm->name));
                }
                PRRTE_RELEASE(rly);
                PRRTE_RELEASE(item);
                PRRTE_FORCED_TERMINATE(PRRTE_ERR_UNREACH);
                continue;
            }
            if ((PRRTE_PROC_STATE_RUNNING < rec->state &&
                PRRTE_PROC_STATE_CALLED_ABORT != rec->state) ||
                !PRRTE_FLAG_TEST(rec, PRRTE_PROC_FLAG_ALIVE)) {
                if (!prrte_abnormal_term_ordered && !prrte_prteds_term_ordered) {
                    prrte_output(0, "%s grpcomm:direct:send_relay proc %s not running - cannot relay: %s ",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_NAME_PRINT(&nm->name),
                                PRRTE_FLAG_TEST(rec, PRRTE_PROC_FLAG_ALIVE) ? prrte_proc_state_to_str(rec->state) : "NOT ALIVE");
                }
                PRRTE_RELEASE(rly);
                PRRTE_RELEASE(item);
                PRRTE_FORCED_TERMINATE(PRRTE_ERR_UNREACH);
                continue;
            }
            if (PRRTE_SUCCESS != (ret = prrte_rml.send_buffer_nb(&nm->name, rly, PRRTE_RML_TAG_XCAST,
                                                               prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(ret);
                PRRTE_RELEASE(rly);
                PRRTE_RELEASE(item);
                PRRTE_FORCED_TERMINATE(PRRTE_ERR_UNREACH);
                continue;
            }
            PRRTE_RELEASE(item);
        }
    }

 CLEANUP:
    /* cleanup */
    PRRTE_LIST_DESTRUCT(&coll);
    PRRTE_RELEASE(rly);  // retain accounting

    /* now pass the relay buffer to myself for processing - don't
     * inject it into the RML system via send as that will compete
     * with the relay messages down in the OOB. Instead, pass it
     * directly to the RML message processor */
    if (PRRTE_DAEMON_DVM_NIDMAP_CMD != command) {
        PRRTE_RML_POST_MESSAGE(PRRTE_PROC_MY_NAME, tag, 1,
                              relay->base_ptr, relay->bytes_used);
        relay->base_ptr = NULL;
        relay->bytes_used = 0;
    }
    if (NULL != relay) {
        PRRTE_RELEASE(relay);
    }
    PRRTE_DESTRUCT(&datbuf);
}

static void barrier_release(int status, prrte_process_name_t* sender,
                            prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                            void* cbdata)
{
    int32_t cnt;
    int rc, ret, mode;
    prrte_grpcomm_signature_t *sig;
    prrte_grpcomm_coll_t *coll;

    PRRTE_OUTPUT_VERBOSE((5, prrte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: barrier release called with %d bytes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)buffer->bytes_used));

    /* unpack the signature */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &sig, &cnt, PRRTE_SIGNATURE))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &ret, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the mode */
    cnt = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &mode, &cnt, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker - it is not an error if not
     * found as that just means we wre not involved
     * in the collective */
    if (NULL == (coll = prrte_grpcomm_base_get_tracker(sig, false))) {
        PRRTE_RELEASE(sig);
        return;
    }

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, buffer, coll->cbdata);
    }
    prrte_list_remove_item(&prrte_grpcomm_base.ongoing, &coll->super);
    PRRTE_RELEASE(coll);
    PRRTE_RELEASE(sig);
}
