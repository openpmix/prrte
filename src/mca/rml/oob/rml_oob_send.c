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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/pmix/pmix-internal.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/oob/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "rml_oob.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"

static void send_self_exe(int fd, short args, void *data)
{
    prte_self_send_xfer_t *xfer = (prte_self_send_xfer_t *) data;

    PRTE_ACQUIRE_OBJECT(xfer);

    PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                         "%s rml_send_to_self callback executing for tag %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), xfer->tag));

    /* execute the send callback function - note that
     * send-to-self always returns a SUCCESS status
     */
    if (NULL != xfer->cbfunc) {
        /* non-blocking buffer send */
        xfer->cbfunc(PRTE_SUCCESS, PRTE_PROC_MY_NAME, &xfer->dbuf, xfer->tag, xfer->cbdata);
    }

    /* cleanup the memory */
    PRTE_RELEASE(xfer);
}

int prte_rml_oob_send_buffer_nb(pmix_proc_t *peer, pmix_data_buffer_t *buffer, prte_rml_tag_t tag,
                                prte_rml_buffer_callback_fn_t cbfunc, void *cbdata)
{
    prte_rml_recv_t *rcv;
    prte_rml_send_t *snd;
    prte_self_send_xfer_t *xfer;
    pmix_status_t rc;

    PRTE_OUTPUT_VERBOSE(
        (1, prte_rml_base_framework.framework_output, "%s rml_send_buffer to peer %s at tag %d",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (NULL == peer) ? "NULL" : PRTE_NAME_PRINT(peer), tag));

    if (PRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL == peer || PMIX_CHECK_PROCID(PRTE_NAME_INVALID, peer)) {
        /* cannot send to an invalid peer */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PMIX_CHECK_PROCID(peer, PRTE_PROC_MY_NAME)) { /* local delivery */
        PRTE_OUTPUT_VERBOSE((1, prte_rml_base_framework.framework_output,
                             "%s rml_send_buffer_to_self at tag %d",
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
        rc = PMIx_Data_copy_payload(&xfer->dbuf, buffer);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PRTE_RELEASE(xfer);
            return prte_pmix_convert_status(rc);
        }
        xfer->cbfunc = cbfunc;
        xfer->tag = tag;
        xfer->cbdata = cbdata;
        /* setup the event for the send callback */
        PRTE_THREADSHIFT(xfer, prte_event_base, send_self_exe, PRTE_MSG_PRI);

        /* copy the message for the recv */
        rcv = PRTE_NEW(prte_rml_recv_t);
        rcv->sender = *peer;
        rcv->tag = tag;
        rc = PMIx_Data_copy_payload(&rcv->dbuf, buffer);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PRTE_RELEASE(rcv);
            return prte_pmix_convert_status(rc);
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
    rc = PMIx_Data_copy_payload(&snd->dbuf, buffer);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PRTE_RELEASE(snd);
        return prte_pmix_convert_status(rc);
    }
    snd->cbfunc = cbfunc;
    snd->cbdata = cbdata;

    /* activate the OOB send state */
    PRTE_OOB_SEND(snd);

    return PRTE_SUCCESS;
}
