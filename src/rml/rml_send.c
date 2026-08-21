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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/pmix/pmix-internal.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_name_fns.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"

#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"
#include "src/rml/relm/relm.h"

/* One send path for both the owned and the shared case.  When payload is
 * non-NULL it supplies the buffer and buffer must be NULL; the send takes a
 * reference to it rather than ownership of its bytes. */
static int send_buffer(pmix_rank_t rank,
                       pmix_data_buffer_t *buffer,
                       prte_rml_payload_t *payload,
                       prte_rml_tag_t tag,
                       bool direct,
                       prte_rml_buffer_callback_fn_t cbfunc,
                       void *cbdata)
{
    prte_rml_recv_t *rcv;
    prte_rml_send_t *snd;

    if (NULL != payload) {
        buffer = payload->dbuf;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_rml_base.rml_output,
         "%s rml_send_buffer%s to peer %s at tag %d",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), direct ? "_direct" : "",
         PMIX_RANK_PRINT(rank), tag));

    if (PRTE_RML_TAG_INVALID == tag) {
        /* cannot send to an invalid tag */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    if (PMIX_RANK_INVALID == rank) {
        /* cannot send to an invalid peer */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    if (!prte_rml_is_node_up(rank)) {
        /* cannot send to a down peer */
        PRTE_ERROR_LOG(PRTE_ERR_NODE_DOWN);
        return PRTE_ERR_NODE_DOWN;
    }

    /* if this is a message to myself, then just post the message
     * for receipt - no need to dive into the oob
     */
    if (PRTE_PROC_MY_NAME->rank == rank) { /* local delivery */
        PMIX_OUTPUT_VERBOSE((1, prte_rml_base.rml_output,
                             "%s rml_send_buffer_to_self at tag %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tag));
        if (NULL != payload) {
            /* the receive takes ownership of what it is handed, and a shared
             * payload has other destinations still to reach - so this one
             * destination gets a copy of its own.  Making it a copy here rather
             * than a rule the caller has to know keeps a self-send from being
             * a special case anywhere else. */
            pmix_data_buffer_t *copy;
            int rc;

            PMIX_DATA_BUFFER_CREATE(copy);
            rc = PMIx_Data_copy_payload(copy, buffer);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(copy);
                /* every other exit from this function reports a PRRTE code */
                return prte_pmix_convert_status(rc);
            }
            buffer = copy;
        }
        /* copy the message for the recv */
        rcv = PMIX_NEW(prte_rml_recv_t);
        PMIX_LOAD_PROCID(&rcv->sender, PRTE_PROC_MY_NAME->nspace, rank);
        rcv->tag = tag;
        rcv->dbuf = buffer;
        /* post the message for receipt - since the send callback was posted
         * first and has the same priority, it will execute first
         */
        PRTE_RML_ACTIVATE_MESSAGE(rcv);
        return PRTE_SUCCESS;
    }

    snd = PMIX_NEW(prte_rml_send_t);
    PMIX_LOAD_PROCID(&snd->dst, PRTE_PROC_MY_NAME->nspace, rank);
    snd->origin = *PRTE_PROC_MY_NAME;
    snd->tag = tag;
    snd->dbuf = buffer;
    if (NULL != payload) {
        /* our own reference, dropped when this send completes.  Taken here,
         * after every way out above has already returned, so a refused send
         * leaves the caller's reference count exactly as it found it. */
        PMIX_RETAIN(payload);
        snd->payload = payload;
    }
    snd->direct = direct;
    /* the constructor installs prte_rml_send_callback; only override it when
     * the caller actually asked to be told how the send ended */
    if (NULL != cbfunc) {
        snd->cbfunc = cbfunc;
        snd->cbdata = cbdata;
    }

    /* activate the OOB send state */
    PRTE_OOB_SEND(snd);

    return PRTE_SUCCESS;
}

int prte_rml_send_buffer_nb(pmix_rank_t rank,
                            pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag)
{
    return send_buffer(rank, buffer, NULL, tag, false, NULL, NULL);
}

int prte_rml_send_buffer_cb_nb(pmix_rank_t rank,
                               pmix_data_buffer_t *buffer,
                               prte_rml_tag_t tag,
                               prte_rml_buffer_callback_fn_t cbfunc,
                               void *cbdata)
{
    return send_buffer(rank, buffer, NULL, tag, false, cbfunc, cbdata);
}

int prte_rml_send_payload_cb_nb(pmix_rank_t rank,
                                prte_rml_payload_t *payload,
                                prte_rml_tag_t tag,
                                prte_rml_buffer_callback_fn_t cbfunc,
                                void *cbdata)
{
    if (NULL == payload || NULL == payload->dbuf) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    return send_buffer(rank, NULL, payload, tag, false, cbfunc, cbdata);
}

int prte_rml_send_buffer_direct_nb(pmix_rank_t rank,
                                   pmix_data_buffer_t *buffer,
                                   prte_rml_tag_t tag)
{
    return send_buffer(rank, buffer, NULL, tag, true, NULL, NULL);
}

int prte_rml_send_buffer_reliable_nb(pmix_rank_t rank,
                                     pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag)
{
    if(PRTE_PROC_MY_NAME->rank == rank){
        // Sends to self don't need reliability
        return prte_rml_send_buffer_nb(rank, buffer, tag);
    }

    return prte_relm_start_msg(rank, buffer, tag);
}
