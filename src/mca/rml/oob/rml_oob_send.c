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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "types.h"

#include "src/dss/dss.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/oob/base/base.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "rml_oob.h"

static void send_self_exe(int fd, short args, void* data)
{
    prrte_self_send_xfer_t *xfer = (prrte_self_send_xfer_t*)data;

    PRRTE_ACQUIRE_OBJECT(xfer);

    PRRTE_OUTPUT_VERBOSE((1, prrte_rml_base_framework.framework_output,
                         "%s rml_send_to_self callback executing for tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), xfer->tag));

    /* execute the send callback function - note that
     * send-to-self always returns a SUCCESS status
     */
    if (NULL != xfer->iov) {
        if (NULL != xfer->cbfunc.iov) {
            /* non-blocking iovec send */
            xfer->cbfunc.iov(PRRTE_SUCCESS, PRRTE_PROC_MY_NAME, xfer->iov, xfer->count,
                             xfer->tag, xfer->cbdata);
        }
    } else if (NULL != xfer->buffer) {
        if (NULL != xfer->cbfunc.buffer) {
            /* non-blocking buffer send */
            xfer->cbfunc.buffer(PRRTE_SUCCESS, PRRTE_PROC_MY_NAME, xfer->buffer,
                                xfer->tag, xfer->cbdata);
        }
    } else {
        /* should never happen */
        abort();
    }

    /* cleanup the memory */
    PRRTE_RELEASE(xfer);
}

int prrte_rml_oob_send_nb(prrte_process_name_t* peer,
                         struct iovec* iov,
                         int count,
                         prrte_rml_tag_t tag,
                         prrte_rml_callback_fn_t cbfunc,
                         void* cbdata)
{
    prrte_rml_recv_t *rcv;
    prrte_rml_send_t *snd;
    int bytes;
    prrte_self_send_xfer_t *xfer;
    int i;
    char* ptr;

    PRRTE_OUTPUT_VERBOSE((1, prrte_rml_base_framework.framework_output,
                         "%s rml_send to peer %s at tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(peer), tag));

    if (PRRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL == peer ||
        PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_NAME_INVALID, peer)) {
        /* cannot send to an invalid peer */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, peer, PRRTE_PROC_MY_NAME)) {  /* local delivery */
        PRRTE_OUTPUT_VERBOSE((1, prrte_rml_base_framework.framework_output,
                             "%s rml_send_iovec_to_self at tag %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), tag));
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
        xfer = PRRTE_NEW(prrte_self_send_xfer_t);
        xfer->iov = iov;
        xfer->count = count;
        xfer->cbfunc.iov = cbfunc;
        xfer->tag = tag;
        xfer->cbdata = cbdata;
        /* setup the event for the send callback */
        PRRTE_THREADSHIFT(xfer, prrte_event_base, send_self_exe, PRRTE_MSG_PRI);

        /* copy the message for the recv */
        rcv = PRRTE_NEW(prrte_rml_recv_t);
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
        PRRTE_RML_ACTIVATE_MESSAGE(rcv);
        return PRRTE_SUCCESS;
    }

    snd = PRRTE_NEW(prrte_rml_send_t);
    snd->dst = *peer;
    snd->origin = *PRRTE_PROC_MY_NAME;
    snd->tag = tag;
    snd->iov = iov;
    snd->count = count;
    snd->cbfunc.iov = cbfunc;
    snd->cbdata = cbdata;

    /* activate the OOB send state */
    PRRTE_OOB_SEND(snd);

    return PRRTE_SUCCESS;
}

int prrte_rml_oob_send_buffer_nb(prrte_process_name_t* peer,
                                prrte_buffer_t* buffer,
                                prrte_rml_tag_t tag,
                                prrte_rml_buffer_callback_fn_t cbfunc,
                                void* cbdata)
{
    prrte_rml_recv_t *rcv;
    prrte_rml_send_t *snd;
    prrte_self_send_xfer_t *xfer;

    PRRTE_OUTPUT_VERBOSE((1, prrte_rml_base_framework.framework_output,
                         "%s rml_send_buffer to peer %s at tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(peer), tag));

    if (PRRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL == peer ||
        PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_NAME_INVALID, peer)) {
        /* cannot send to an invalid peer */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, peer, PRRTE_PROC_MY_NAME)) {  /* local delivery */
        PRRTE_OUTPUT_VERBOSE((1, prrte_rml_base_framework.framework_output,
                             "%s rml_send_iovec_to_self at tag %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), tag));
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
        xfer = PRRTE_NEW(prrte_self_send_xfer_t);
        xfer->buffer = buffer;
        xfer->cbfunc.buffer = cbfunc;
        xfer->tag = tag;
        xfer->cbdata = cbdata;
        /* setup the event for the send callback */
        PRRTE_THREADSHIFT(xfer, prrte_event_base, send_self_exe, PRRTE_MSG_PRI);

        /* copy the message for the recv */
        rcv = PRRTE_NEW(prrte_rml_recv_t);
        rcv->sender = *peer;
        rcv->tag = tag;
        rcv->iov.iov_base = (IOVBASE_TYPE*)malloc(buffer->bytes_used);
        memcpy(rcv->iov.iov_base, buffer->base_ptr, buffer->bytes_used);
        rcv->iov.iov_len = buffer->bytes_used;
        /* post the message for receipt - since the send callback was posted
         * first and has the same priority, it will execute first
         */
        PRRTE_RML_ACTIVATE_MESSAGE(rcv);
        return PRRTE_SUCCESS;
    }

    snd = PRRTE_NEW(prrte_rml_send_t);
    snd->dst = *peer;
    snd->origin = *PRRTE_PROC_MY_NAME;
    snd->tag = tag;
    snd->buffer = buffer;
    snd->cbfunc.buffer = cbfunc;
    snd->cbdata = cbdata;

    /* activate the OOB send state */
    PRRTE_OOB_SEND(snd);

    return PRRTE_SUCCESS;
}
