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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include "prte_config.h"

#include <string.h>

#include "constants.h"
#include "types.h"

#include "src/class/prte_list.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/rml/rml.h"

static void msg_match_recv(prte_rml_posted_recv_t *rcv, bool get_all);

void prte_rml_base_post_recv(int sd, short args, void *cbdata)
{
    prte_rml_recv_request_t *req = (prte_rml_recv_request_t *) cbdata;
    prte_rml_posted_recv_t *post, *recv;

    PRTE_ACQUIRE_OBJECT(req);

    prte_output_verbose(5, prte_rml_base_framework.framework_output, "%s posting recv",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (NULL == req) {
        /* this can only happen if something is really wrong, but
         * someone managed to get here in a bizarre test */
        prte_output(0, "%s CANNOT POST NULL RML RECV REQUEST", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        return;
    }
    post = req->post;

    /* if the request is to cancel a recv, then find the recv
     * and remove it from our list
     */
    if (req->cancel) {
        PRTE_LIST_FOREACH(recv, &prte_rml_base.posted_recvs, prte_rml_posted_recv_t)
        {
            if (PMIX_CHECK_PROCID(&post->peer, &recv->peer) && post->tag == recv->tag) {
                prte_output_verbose(5, prte_rml_base_framework.framework_output,
                                    "%s canceling recv %d for peer %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), post->tag,
                                    PRTE_NAME_PRINT(&recv->peer));
                /* got a match - remove it */
                prte_list_remove_item(&prte_rml_base.posted_recvs, &recv->super);
                PRTE_RELEASE(recv);
                break;
            }
        }
        PRTE_RELEASE(req);
        return;
    }

    /* bozo check - cannot have two receives for the same peer/tag combination */
    PRTE_LIST_FOREACH(recv, &prte_rml_base.posted_recvs, prte_rml_posted_recv_t)
    {
        if (PMIX_CHECK_PROCID(&post->peer, &recv->peer) && post->tag == recv->tag) {
            prte_output(0, "%s TWO RECEIVES WITH SAME PEER %s AND TAG %d - ABORTING",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&post->peer),
                        post->tag);
            abort();
        }
    }

    prte_output_verbose(5, prte_rml_base_framework.framework_output,
                        "%s posting %s recv on tag %d for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (post->persistent) ? "persistent" : "non-persistent", post->tag,
                        PRTE_NAME_PRINT(&post->peer));
    /* add it to the list of recvs */
    prte_list_append(&prte_rml_base.posted_recvs, &post->super);
    req->post = NULL;
    /* handle any messages that may have already arrived for this recv */
    msg_match_recv(post, post->persistent);

    /* cleanup */
    PRTE_RELEASE(req);
}

static void msg_match_recv(prte_rml_posted_recv_t *rcv, bool get_all)
{
    prte_list_item_t *item, *next;
    prte_rml_recv_t *msg;

    /* scan thru the list of unmatched recvd messages and
     * see if any matches this spec - if so, push the first
     * into the recvd msg queue and look no further
     */
    item = prte_list_get_first(&prte_rml_base.unmatched_msgs);
    while (item != prte_list_get_end(&prte_rml_base.unmatched_msgs)) {
        next = prte_list_get_next(item);
        msg = (prte_rml_recv_t *) item;
        prte_output_verbose(5, prte_rml_base_framework.framework_output,
                            "%s checking recv for %s against unmatched msg from %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&rcv->peer),
                            PRTE_NAME_PRINT(&msg->sender));

        /* since names could include wildcards, must use
         * the more generalized comparison function
         */
        if (PMIX_CHECK_PROCID(&msg->sender, &rcv->peer) && msg->tag == rcv->tag) {
            PRTE_RML_ACTIVATE_MESSAGE(msg);
            prte_list_remove_item(&prte_rml_base.unmatched_msgs, item);
            if (!get_all) {
                break;
            }
        }
        item = next;
    }
}

void prte_rml_base_process_msg(int fd, short flags, void *cbdata)
{
    prte_rml_recv_t *msg = (prte_rml_recv_t *) cbdata;
    prte_rml_posted_recv_t *post;

    PRTE_ACQUIRE_OBJECT(msg);

    PRTE_OUTPUT_VERBOSE(
        (5, prte_rml_base_framework.framework_output, "%s message received from %s for tag %d",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->sender), msg->tag));

    /* if this message is just to warmup the connection, then drop it */
    if (PRTE_RML_TAG_WARMUP_CONNECTION == msg->tag) {
        if (!prte_nidmap_communicated) {
            pmix_data_buffer_t buffer;
            int rc;

            PMIX_DATA_BUFFER_CONSTRUCT(&buffer);

            if (PRTE_SUCCESS != (rc = prte_util_nidmap_create(prte_node_pool, &buffer))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buffer);
                return;
            }

            if (PRTE_SUCCESS
                != (rc = prte_rml.send_buffer_nb(&msg->sender, &buffer,
                                                 PRTE_RML_TAG_NODE_REGEX_REPORT,
                                                 prte_rml_send_callback, NULL))) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&buffer);
                return;
            }
            PMIX_DATA_BUFFER_DESTRUCT(&buffer);
            PRTE_RELEASE(msg);
            return;
        }
    }

    /* see if we have a waiting recv for this message */
    PRTE_LIST_FOREACH(post, &prte_rml_base.posted_recvs, prte_rml_posted_recv_t)
    {
        /* since names could include wildcards, must use
         * the more generalized comparison function
         */
        if (PMIX_CHECK_PROCID(&msg->sender, &post->peer) && msg->tag == post->tag) {
            /* deliver the data to this location */
            post->cbfunc(PRTE_SUCCESS, &msg->sender, &msg->dbuf, msg->tag, post->cbdata);
            /* the user must have unloaded the buffer if they wanted
             * to retain ownership of it, so release whatever remains
             */
            PRTE_OUTPUT_VERBOSE((5, prte_rml_base_framework.framework_output,
                                 "%s message received %" PRIsize_t
                                 " bytes from %s for tag %d called callback",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), msg->dbuf.bytes_used,
                                 PRTE_NAME_PRINT(&msg->sender), msg->tag));
            /* release the message */
            PRTE_RELEASE(msg);
            PRTE_OUTPUT_VERBOSE((5, prte_rml_base_framework.framework_output,
                                 "%s message tag %d on released",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), post->tag));
            /* if the recv is non-persistent, remove it */
            if (!post->persistent) {
                prte_list_remove_item(&prte_rml_base.posted_recvs, &post->super);
                /*PRTE_OUTPUT_VERBOSE((5, prte_rml_base_framework.framework_output,
                                     "%s non persistent recv %p remove success releasing now",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     post));*/
                PRTE_RELEASE(post);
            }
            return;
        }
    }
    /* we get here if no matching recv was found - we then hold
     * the message until such a recv is issued
     */
    PRTE_OUTPUT_VERBOSE(
        (5, prte_rml_base_framework.framework_output,
         "%s message received bytes from %s for tag %d Not Matched adding to unmatched msgs",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->sender), msg->tag));
    prte_list_append(&prte_rml_base.unmatched_msgs, &msg->super);
}
