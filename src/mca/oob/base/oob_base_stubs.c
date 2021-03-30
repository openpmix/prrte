/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2014 Los Alamos National Security, LLC. All rights
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
#include "constants.h"

#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/threads/threads.h"

static void process_uri(char *uri);

void prte_oob_base_send_nb(int fd, short args, void *cbdata)
{
    prte_oob_send_t *cd = (prte_oob_send_t *) cbdata;
    prte_rml_send_t *msg;
    prte_mca_base_component_list_item_t *cli;
    prte_oob_base_peer_t *pr;
    int rc;
    bool msg_sent;
    prte_oob_base_component_t *component;
    bool reachable;
    char *uri;

    PRTE_ACQUIRE_OBJECT(cd);

    /* done with this. release it now */
    msg = cd->msg;
    PRTE_RELEASE(cd);

    prte_output_verbose(5, prte_oob_base_framework.framework_output,
                        "%s oob:base:send to target %s - attempt %u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->dst),
                        msg->retries);

    /* don't try forever - if we have exceeded the number of retries,
     * then report this message as undeliverable even if someone continues
     * to think they could reach it */
    if (prte_rml_base.max_retries <= msg->retries) {
        msg->status = PRTE_ERR_NO_PATH_TO_TARGET;
        PRTE_RML_SEND_COMPLETE(msg);
        return;
    }

    /* check if we have this peer in our list */
    pr = prte_oob_base_get_peer(&msg->dst);
    if (NULL == pr) {
        prte_output_verbose(5, prte_oob_base_framework.framework_output,
                            "%s oob:base:send unknown peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_NAME_PRINT(&msg->dst));
        /* for direct launched procs, the URI might be in the database,
         * so check there next - if it is, the peer object will be added
         * to our hash table. However, we don't want to chase up to the
         * server after it, so indicate it is optional
         */
        PRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_PROC_URI, &msg->dst, (char **) &uri, PMIX_STRING);
        if (PRTE_SUCCESS == rc) {
            if (NULL != uri) {
                process_uri(uri);
                pr = prte_oob_base_get_peer(&msg->dst);
                if (NULL == pr) {
                    /* that is just plain wrong */
                    prte_output_verbose(5, prte_oob_base_framework.framework_output,
                                        "%s oob:base:send addressee unknown %s",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&msg->dst));
                    PRTE_ERROR_LOG(PRTE_ERR_ADDRESSEE_UNKNOWN);
                    msg->status = PRTE_ERR_ADDRESSEE_UNKNOWN;
                    PRTE_RML_SEND_COMPLETE(msg);
                    return;
                }
            } else {
                PRTE_ERROR_LOG(PRTE_ERR_ADDRESSEE_UNKNOWN);
                msg->status = PRTE_ERR_ADDRESSEE_UNKNOWN;
                PRTE_RML_SEND_COMPLETE(msg);
                return;
            }
        } else {
            /* even though we don't know about this peer yet, we still might
             * be able to get to it via routing, so ask each component if
             * it can reach it
             */
            reachable = false;
            pr = NULL;
            PRTE_LIST_FOREACH(cli, &prte_oob_base.actives, prte_mca_base_component_list_item_t)
            {
                component = (prte_oob_base_component_t *) cli->cli_component;
                if (NULL != component->is_reachable) {
                    if (component->is_reachable(&msg->dst)) {
                        /* there is a way to reach this peer - record it
                         * so we don't waste this time again
                         */
                        if (NULL == pr) {
                            pr = PRTE_NEW(prte_oob_base_peer_t);
                            PMIX_XFER_PROCID(&pr->name, &msg->dst);
                        }
                        /* mark that this component can reach the peer */
                        prte_bitmap_set_bit(&pr->addressable, component->idx);
                        /* flag that at least one component can reach this peer */
                        reachable = true;
                    }
                }
            }
            /* if nobody could reach it, then that's an error */
            if (!reachable) {
                /* if we are a daemon or HNP, then it could be that
                 * this is a local proc we just haven't heard from
                 * yet due to a race condition. Check that situation */
                if (PRTE_PROC_IS_DAEMON || PRTE_PROC_IS_MASTER) {
                    ++msg->retries;
                    if (msg->retries < prte_rml_base.max_retries) {
                        PRTE_OOB_SEND(msg);
                        return;
                    }
                }
                msg->status = PRTE_ERR_ADDRESSEE_UNKNOWN;
                PRTE_RML_SEND_COMPLETE(msg);
                return;
            }
        }
    }

    /* if we already have a connection to this peer, use it */
    if (NULL != pr->component) {
        /* post this msg for send by this transport - the component
         * runs on our event base, so we can just call their function
         */
        prte_output_verbose(5, prte_oob_base_framework.framework_output,
                            "%s oob:base:send known transport for peer %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->dst));
        if (PRTE_SUCCESS == (rc = pr->component->send_nb(msg))) {
            return;
        }
    }

    /* if we haven't identified a transport to this peer,
     * loop across all available components in priority order until
     * one replies that it has a module that can reach this peer.
     * Let it try to make the connection
     */
    msg_sent = false;
    PRTE_LIST_FOREACH(cli, &prte_oob_base.actives, prte_mca_base_component_list_item_t)
    {
        component = (prte_oob_base_component_t *) cli->cli_component;
        /* is this peer reachable via this component? */
        if (!component->is_reachable(&msg->dst)) {
            continue;
        }
        /* it is addressable, so attempt to send via that transport */
        if (PRTE_SUCCESS == (rc = component->send_nb(msg))) {
            /* the msg status will be set upon send completion/failure */
            msg_sent = true;
            /* point to this transport for any future messages */
            pr->component = component;
            break;
        } else if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            /* components return "next option" if they can't connect
             * to this peer. anything else is a true error.
             */
            PRTE_ERROR_LOG(rc);
            msg->status = rc;
            PRTE_RML_SEND_COMPLETE(msg);
            return;
        }
    }

    /* if no component can reach this peer, that's an error - post
     * it back to the RML for handling
     */
    if (!msg_sent) {
        prte_output_verbose(5, prte_oob_base_framework.framework_output,
                            "%s oob:base:send no path to target %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->dst));
        msg->status = PRTE_ERR_NO_PATH_TO_TARGET;
        PRTE_RML_SEND_COMPLETE(msg);
    }
}

/**
 * Obtain a uri for initial connection purposes
 *
 * During initial wireup, we can only transfer contact info on the daemon
 * command line. This limits what we can send to a string representation of
 * the actual contact info, which gets sent in a uri-like form. Not every
 * oob module can support this transaction, so this function will loop
 * across all oob components/modules, letting each add to the uri string if
 * it supports bootstrap operations. An error will be returned in the cbfunc
 * if NO component can successfully provide a contact.
 *
 * Note: since there is a limit to what an OS will allow on a cmd line, we
 * impose a limit on the length of the resulting uri via an MCA param. The
 * default value of -1 implies unlimited - however, users with large numbers
 * of interfaces on their nodes may wish to restrict the size.
 */
void prte_oob_base_get_addr(char **uri)
{
    char *turi, *final = NULL, *tmp;
    size_t len = 0;
    bool one_added = false;
    prte_mca_base_component_list_item_t *cli;
    prte_oob_base_component_t *component;
    pmix_value_t val;
    pmix_status_t rc;

    /* start with our process name */
    if (PRTE_SUCCESS
        != (rc = prte_util_convert_process_name_to_string(&final, PRTE_PROC_MY_NAME))) {
        PRTE_ERROR_LOG(rc);
        *uri = NULL;
        return;
    }
    len = strlen(final);

    /* loop across all available modules to get their input
     * up to the max length
     */
    PRTE_LIST_FOREACH(cli, &prte_oob_base.actives, prte_mca_base_component_list_item_t)
    {
        component = (prte_oob_base_component_t *) cli->cli_component;
        /* ask the component for its input, obtained when it
         * opened its modules
         */
        if (NULL == component->get_addr) {
            /* doesn't support this ability */
            continue;
        }
        /* the components operate within our event base, so we
         * can directly call their get_uri function to get the
         * pointer to the uri - this is not a copy, so
         * do NOT free it!
         */
        turi = component->get_addr();
        if (NULL != turi) {
            /* check overall length for limits */
            if (0 < prte_oob_base.max_uri_length
                && prte_oob_base.max_uri_length < (int) (len + strlen(turi))) {
                /* cannot accept the payload */
                continue;
            }
            /* add new value to final one */
            prte_asprintf(&tmp, "%s;%s", final, turi);
            free(turi);
            free(final);
            final = tmp;
            len = strlen(final);
            /* flag that at least one contributed */
            one_added = true;
        }
    }

    if (!one_added) {
        /* nobody could contribute */
        if (NULL != final) {
            free(final);
            final = NULL;
        }
    }

    *uri = final;
    /* push this into our modex storage */
    PMIX_VALUE_LOAD(&val, final, PMIX_STRING);
    if (PMIX_SUCCESS
        != (rc = PMIx_Store_internal(&prte_process_info.myproc, PMIX_PROC_URI, &val))) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_VALUE_DESTRUCT(&val);
}

static void process_uri(char *uri)
{
    pmix_proc_t peer;
    char *cptr;
    prte_mca_base_component_list_item_t *cli;
    prte_oob_base_component_t *component;
    char **uris = NULL;
    prte_oob_base_peer_t *pr;

    prte_output_verbose(5, prte_oob_base_framework.framework_output,
                        "%s:set_addr processing uri %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uri);

    /* find the first semi-colon in the string */
    cptr = strchr(uri, ';');
    if (NULL == cptr) {
        /* got a problem - there must be at least two fields,
         * the first containing the process name of our peer
         * and all others containing the OOB contact info
         */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return;
    }
    *cptr = '\0';
    cptr++;

    /* the first field is the process name, so convert it */
    prte_util_convert_string_to_process_name(&peer, uri);

    /* if the peer is us, no need to go further as we already
     * know our own contact info
     */
    if (PMIX_CHECK_PROCID(&peer, PRTE_PROC_MY_NAME)) {
        prte_output_verbose(5, prte_oob_base_framework.framework_output,
                            "%s:set_addr peer %s is me", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_NAME_PRINT(&peer));
        return;
    }

    /* split the rest of the uri into component parts */
    uris = prte_argv_split(cptr, ';');

    /* get the peer object for this process */
    pr = prte_oob_base_get_peer(&peer);
    if (NULL == pr) {
        pr = PRTE_NEW(prte_oob_base_peer_t);
        PMIX_XFER_PROCID(&pr->name, &peer);
        prte_list_append(&prte_oob_base.peers, &pr->super);
    }

    /* loop across all available components and let them extract
     * whatever piece(s) of the uri they find relevant - they
     * are all operating on our event base, so we can just
     * directly call their functions
     */
    PRTE_LIST_FOREACH(cli, &prte_oob_base.actives, prte_mca_base_component_list_item_t)
    {
        component = (prte_oob_base_component_t *) cli->cli_component;
        prte_output_verbose(5, prte_oob_base_framework.framework_output,
                            "%s:set_addr checking if peer %s is reachable via component %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer),
                            component->oob_base.mca_component_name);
        if (NULL != component->set_addr) {
            if (PRTE_SUCCESS == component->set_addr(&peer, uris)) {
                /* this component found reachable addresses
                 * in the uris
                 */
                prte_output_verbose(5, prte_oob_base_framework.framework_output,
                                    "%s: peer %s is reachable via component %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer),
                                    component->oob_base.mca_component_name);
                prte_bitmap_set_bit(&pr->addressable, component->idx);
            } else {
                prte_output_verbose(5, prte_oob_base_framework.framework_output,
                                    "%s: peer %s is NOT reachable via component %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer),
                                    component->oob_base.mca_component_name);
            }
        }
    }
    prte_argv_free(uris);
}

prte_oob_base_peer_t *prte_oob_base_get_peer(const pmix_proc_t *pr)
{
    prte_oob_base_peer_t *peer;

    PRTE_LIST_FOREACH(peer, &prte_oob_base.peers, prte_oob_base_peer_t)
    {
        if (PMIX_CHECK_PROCID(pr, &peer->name)) {
            return peer;
        }
    }
    return NULL;
}
