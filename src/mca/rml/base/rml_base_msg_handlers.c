/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */

/*
 * includes
 */
#include "prrte_config.h"

#include <string.h>

#include "constants.h"
#include "types.h"

#include "src/dss/dss.h"
#include "src/util/output.h"
#include "src/class/prrte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/threads/threads.h"

#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"


static void msg_match_recv(prrte_rml_posted_recv_t *rcv, bool get_all);


void prrte_rml_base_post_recv(int sd, short args, void *cbdata)
{
    prrte_rml_recv_request_t *req = (prrte_rml_recv_request_t*)cbdata;
    prrte_rml_posted_recv_t *post, *recv;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL | PRRTE_NS_CMP_WILD;

    PRRTE_ACQUIRE_OBJECT(req);

    prrte_output_verbose(5, prrte_rml_base_framework.framework_output,
                        "%s posting recv",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    if (NULL == req) {
        /* this can only happen if something is really wrong, but
         * someone managed to get here in a bizarre test */
        prrte_output(0, "%s CANNOT POST NULL RML RECV REQUEST",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        return;
    }
    post = req->post;

    /* if the request is to cancel a recv, then find the recv
     * and remove it from our list
     */
    if (req->cancel) {
        PRRTE_LIST_FOREACH(recv, &prrte_rml_base.posted_recvs, prrte_rml_posted_recv_t) {
            if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &post->peer, &recv->peer) &&
                post->tag == recv->tag) {
                prrte_output_verbose(5, prrte_rml_base_framework.framework_output,
                                    "%s canceling recv %d for peer %s",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                    post->tag, PRRTE_NAME_PRINT(&recv->peer));
                /* got a match - remove it */
                prrte_list_remove_item(&prrte_rml_base.posted_recvs, &recv->super);
                PRRTE_RELEASE(recv);
                break;
            }
        }
        PRRTE_RELEASE(req);
        return;
    }

    /* bozo check - cannot have two receives for the same peer/tag combination */
    PRRTE_LIST_FOREACH(recv, &prrte_rml_base.posted_recvs, prrte_rml_posted_recv_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &post->peer, &recv->peer) &&
            post->tag == recv->tag) {
            prrte_output(0, "%s TWO RECEIVES WITH SAME PEER %s AND TAG %d - ABORTING",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&post->peer), post->tag);
            abort();
        }
    }

    prrte_output_verbose(5, prrte_rml_base_framework.framework_output,
                        "%s posting %s recv on tag %d for peer %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (post->persistent) ? "persistent" : "non-persistent",
                        post->tag, PRRTE_NAME_PRINT(&post->peer));
    /* add it to the list of recvs */
    prrte_list_append(&prrte_rml_base.posted_recvs, &post->super);
    req->post = NULL;
    /* handle any messages that may have already arrived for this recv */
    msg_match_recv(post, post->persistent);

    /* cleanup */
    PRRTE_RELEASE(req);
}

static void msg_match_recv(prrte_rml_posted_recv_t *rcv, bool get_all)
{
    prrte_list_item_t *item, *next;
    prrte_rml_recv_t *msg;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL | PRRTE_NS_CMP_WILD;

    /* scan thru the list of unmatched recvd messages and
     * see if any matches this spec - if so, push the first
     * into the recvd msg queue and look no further
     */
    item = prrte_list_get_first(&prrte_rml_base.unmatched_msgs);
    while (item != prrte_list_get_end(&prrte_rml_base.unmatched_msgs)) {
        next = prrte_list_get_next(item);
        msg = (prrte_rml_recv_t*)item;
        prrte_output_verbose(5, prrte_rml_base_framework.framework_output,
                            "%s checking recv for %s against unmatched msg from %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&rcv->peer),
                            PRRTE_NAME_PRINT(&msg->sender));

        /* since names could include wildcards, must use
         * the more generalized comparison function
         */
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &msg->sender, &rcv->peer) &&
            msg->tag == rcv->tag) {
            PRRTE_RML_ACTIVATE_MESSAGE(msg);
            prrte_list_remove_item(&prrte_rml_base.unmatched_msgs, item);
            if (!get_all) {
                break;
            }
        }
        item = next;
    }
}

void prrte_rml_base_process_msg(int fd, short flags, void *cbdata)
{
    prrte_rml_recv_t *msg = (prrte_rml_recv_t*)cbdata;
    prrte_rml_posted_recv_t *post;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL | PRRTE_NS_CMP_WILD;
    prrte_buffer_t buf;

    PRRTE_ACQUIRE_OBJECT(msg);

    PRRTE_OUTPUT_VERBOSE((5, prrte_rml_base_framework.framework_output,
                         "%s message received from %s for tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(&msg->sender),
                         msg->tag));

    /* if this message is just to warmup the connection, then drop it */
    if (PRRTE_RML_TAG_WARMUP_CONNECTION == msg->tag) {
        if (!prrte_nidmap_communicated) {
            prrte_buffer_t * buffer = PRRTE_NEW(prrte_buffer_t);
            int rc;
            if (NULL == buffer) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                return;
            }

            if (PRRTE_SUCCESS != (rc = prrte_util_nidmap_create(prrte_node_pool, buffer))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buffer);
                return;
            }

            if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(&msg->sender, buffer,
                                                              PRRTE_RML_TAG_NODE_REGEX_REPORT,
                                                              prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buffer);
                return;
            }
            PRRTE_RELEASE(msg);
            return;
        }
    }

    /* see if we have a waiting recv for this message */
    PRRTE_LIST_FOREACH(post, &prrte_rml_base.posted_recvs, prrte_rml_posted_recv_t) {
        /* since names could include wildcards, must use
         * the more generalized comparison function
         */
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &msg->sender, &post->peer) &&
            msg->tag == post->tag) {
            /* deliver the data to this location */
            if (post->buffer_data) {
                /* deliver it in a buffer */
                PRRTE_CONSTRUCT(&buf, prrte_buffer_t);
                prrte_dss.load(&buf, msg->iov.iov_base, msg->iov.iov_len);
                /* xfer ownership of the malloc'd data to the buffer */
                msg->iov.iov_base = NULL;
                post->cbfunc.buffer(PRRTE_SUCCESS, &msg->sender, &buf, msg->tag, post->cbdata);
                /* the user must have unloaded the buffer if they wanted
                 * to retain ownership of it, so release whatever remains
                 */
                PRRTE_OUTPUT_VERBOSE((5, prrte_rml_base_framework.framework_output,
                                     "%s message received  bytes from %s for tag %d called callback",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&msg->sender),
                                     msg->tag));
                PRRTE_DESTRUCT(&buf);
            } else {
                /* deliver as an iovec */
                post->cbfunc.iov(PRRTE_SUCCESS, &msg->sender, &msg->iov, 1, msg->tag, post->cbdata);
                /* the user should have shifted the data to
                 * a local variable and NULL'd the iov_base
                 * if they wanted ownership of the data
                 */
            }
            /* release the message */
            PRRTE_RELEASE(msg);
            PRRTE_OUTPUT_VERBOSE((5, prrte_rml_base_framework.framework_output,
                                 "%s message tag %d on released",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 post->tag));
            /* if the recv is non-persistent, remove it */
            if (!post->persistent) {
                prrte_list_remove_item(&prrte_rml_base.posted_recvs, &post->super);
                /*PRRTE_OUTPUT_VERBOSE((5, prrte_rml_base_framework.framework_output,
                                     "%s non persistent recv %p remove success releasing now",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     post));*/
                PRRTE_RELEASE(post);

            }
            return;
        }
    }
    /* we get here if no matching recv was found - we then hold
     * the message until such a recv is issued
     */
     PRRTE_OUTPUT_VERBOSE((5, prrte_rml_base_framework.framework_output,
                            "%s message received bytes from %s for tag %d Not Matched adding to unmatched msgs",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&msg->sender),
                            msg->tag));
     prrte_list_append(&prrte_rml_base.unmatched_msgs, &msg->super);
}
