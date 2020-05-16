/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/dss/dss.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/oob/base/base.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "rml_oob.h"

static void send_self_exe(int fd, short args, void* data)
{
    prte_self_send_xfer_t *xfer = (prte_self_send_xfer_t*)data;

    PRTE_ACQUIRE_OBJECT(xfer);

    PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                         "%s rml_send_to_self callback executing for tag %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), xfer->tag));

    /* execute the send callback function - note that
     * send-to-self always returns a SUCCESS status
     */
    if (NULL != xfer->iov) {
        if (NULL != xfer->cbfunc.iov) {
            /* non-blocking iovec send */
            xfer->cbfunc.iov(PRTE_SUCCESS, PRTE_PROC_MY_NAME, xfer->iov, xfer->count,
                             xfer->tag, xfer->cbdata);
        }
    } else if (NULL != xfer->buffer) {
        if (NULL != xfer->cbfunc.buffer) {
            /* non-blocking buffer send */
            xfer->cbfunc.buffer(PRTE_SUCCESS, PRTE_PROC_MY_NAME, xfer->buffer,
                                xfer->tag, xfer->cbdata);
        }
    } else {
        /* should never happen */
        abort();
    }

    /* cleanup the memory */
    PRTE_RELEASE(xfer);
}

int prte_rml_oob_send_nb(prte_process_name_t* peer,
                         struct iovec* iov,
                         int count,
                         prte_rml_tag_t tag,
                         prte_rml_callback_fn_t cbfunc,
                         void* cbdata)
{
    prte_rml_recv_t *rcv;
    prte_rml_send_t *snd;
    int bytes;
    prte_self_send_xfer_t *xfer;
    int i;
    char* ptr;

    PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                         "%s rml_send to peer %s at tag %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(peer), tag));

    if (PRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL == peer ||
        PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_NAME_INVALID, peer)) {
        /* cannot send to an invalid peer */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, peer, PRTE_PROC_MY_NAME)) {  /* local delivery */
        PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                             "%s rml_send_iovec_to_self at tag %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tag));
        /* send to self is a tad tricky - we really don't want
         * to track the send callback function throughout the recv
         * process and execute it upon receipt as this would provide
         * very different timing from a non-self message. Specifically,
         * if we just retain a pointer to the incoming data
         * and then execute the send callback prior to the receive,
         * then the caller will think we are done with the data and
         * can release it. So we have to copy the data in order to
         * execute the send callback prior to receiving the message.
         *
         * In truth, this really is a better mimic of the non-self
         * message behavior. If we actually pushed the message out
         * on the wire and had it loop back, then we would receive
         * a new block of data anyway.
         */

        /* setup the send callback */
        xfer = PRTE_NEW(prte_self_send_xfer_t);
        xfer->iov = iov;
        xfer->count = count;
        xfer->cbfunc.iov = cbfunc;
        xfer->tag = tag;
        xfer->cbdata = cbdata;
        /* setup the event for the send callback */
        PRTE_THREADSHIFT(xfer, prte_event_base, send_self_exe, PRTE_MSG_PRI);

        /* copy the message for the recv */
        rcv = PRTE_NEW(prte_rml_recv_t);
        rcv->sender = *peer;
        rcv->tag = tag;
        /* get the total number of bytes in the iovec array */
        bytes = 0;
        for (i = 0 ; i < count ; ++i) {
            bytes += iov[i].iov_len;
        }
        /* get the required memory allocation */
        if (0 < bytes) {
            rcv->iov.iov_base = (IOVBASE_TYPE*)malloc(bytes);
            rcv->iov.iov_len = bytes;
            /* transfer the bytes */
            ptr =  (char*)rcv->iov.iov_base;
            for (i = 0 ; i < count ; ++i) {
                memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
                ptr += iov[i].iov_len;
            }
        }
        /* post the message for receipt - since the send callback was posted
         * first and has the same priority, it will execute first
         */
        PRTE_RML_ACTIVATE_MESSAGE(rcv);
        return PRTE_SUCCESS;
    }

    snd = PRTE_NEW(prte_rml_send_t);
    snd->dst = *peer;
    snd->origin = *PRTE_PROC_MY_NAME;
    snd->tag = tag;
    snd->iov = iov;
    snd->count = count;
    snd->cbfunc.iov = cbfunc;
    snd->cbdata = cbdata;

    /* activate the OOB send state */
    PRTE_OOB_SEND(snd);

    return PRTE_SUCCESS;
}

int prte_rml_oob_send_buffer_nb(prte_process_name_t* peer,
                                prte_buffer_t* buffer,
                                prte_rml_tag_t tag,
                                prte_rml_buffer_callback_fn_t cbfunc,
                                void* cbdata)
{
    prte_rml_recv_t *rcv;
    prte_rml_send_t *snd;
    prte_self_send_xfer_t *xfer;

    PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                         "%s rml_send_buffer to peer %s at tag %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == peer) ? "NULL" : PRTE_NAME_PRINT(peer), tag));

    if (PRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL == peer ||
        PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, PRTE_NAME_INVALID, peer)) {
        /* cannot send to an invalid peer */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, peer, PRTE_PROC_MY_NAME)) {  /* local delivery */
        PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                             "%s rml_send_iovec_to_self at tag %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tag));
        /* send to self is a tad tricky - we really don't want
         * to track the send callback function throughout the recv
         * process and execute it upon receipt as this would provide
         * very different timing from a non-self message. Specifically,
         * if we just retain a pointer to the incoming data
         * and then execute the send callback prior to the receive,
         * then the caller will think we are done with the data and
         * can release it. So we have to copy the data in order to
         * execute the send callback prior to receiving the message.
         *
         * In truth, this really is a better mimic of the non-self
         * message behavior. If we actually pushed the message out
         * on the wire and had it loop back, then we would receive
         * a new block of data anyway.
         */

        /* setup the send callback */
        xfer = PRTE_NEW(prte_self_send_xfer_t);
        xfer->buffer = buffer;
        xfer->cbfunc.buffer = cbfunc;
        xfer->tag = tag;
        xfer->cbdata = cbdata;
        /* setup the event for the send callback */
        PRTE_THREADSHIFT(xfer, prte_event_base, send_self_exe, PRTE_MSG_PRI);

        /* copy the message for the recv */
        rcv = PRTE_NEW(prte_rml_recv_t);
        rcv->sender = *peer;
        rcv->tag = tag;
        rcv->iov.iov_base = (IOVBASE_TYPE*)malloc(buffer->bytes_used);
        memcpy(rcv->iov.iov_base, buffer->base_ptr, buffer->bytes_used);
        rcv->iov.iov_len = buffer->bytes_used;
        /* post the message for receipt - since the send callback was posted
         * first and has the same priority, it will execute first
         */
        PRTE_RML_ACTIVATE_MESSAGE(rcv);
        return PRTE_SUCCESS;
    }

    snd = PRTE_NEW(prte_rml_send_t);
    snd->dst = *peer;
    snd->origin = *PRTE_PROC_MY_NAME;
    snd->tag = tag;
    snd->buffer = buffer;
    snd->cbfunc.buffer = cbfunc;
    snd->cbdata = cbdata;

    /* activate the OOB send state */
    PRTE_OOB_SEND(snd);

    return PRTE_SUCCESS;
}
