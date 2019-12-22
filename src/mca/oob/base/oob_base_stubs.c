/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2014 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/util/output.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/state/state.h"
#include "src/mca/rml/rml.h"
#include "src/threads/threads.h"
#include "src/mca/oob/base/base.h"

static void process_uri(char *uri);

void prrte_oob_base_send_nb(int fd, short args, void *cbdata)
{
    prrte_oob_send_t *cd = (prrte_oob_send_t*)cbdata;
    prrte_rml_send_t *msg;
    prrte_mca_base_component_list_item_t *cli;
    prrte_oob_base_peer_t *pr;
    int rc;
    uint64_t ui64;
    bool msg_sent;
    prrte_oob_base_component_t *component;
    bool reachable;
    char *uri;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* done with this. release it now */
    msg = cd->msg;
    PRRTE_RELEASE(cd);

    prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                        "%s oob:base:send to target %s - attempt %u",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&msg->dst), msg->retries);

    /* don't try forever - if we have exceeded the number of retries,
     * then report this message as undeliverable even if someone continues
     * to think they could reach it */
    if (prrte_rml_base.max_retries <= msg->retries) {
        msg->status = PRRTE_ERR_NO_PATH_TO_TARGET;
        PRRTE_RML_SEND_COMPLETE(msg);
        return;
    }

    /* check if we have this peer in our hash table */
    memcpy(&ui64, (char*)&msg->dst, sizeof(uint64_t));
    if (PRRTE_SUCCESS != prrte_hash_table_get_value_uint64(&prrte_oob_base.peers,
                                                         ui64, (void**)&pr) ||
        NULL == pr) {
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "%s oob:base:send unknown peer %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&msg->dst));
        /* for direct launched procs, the URI might be in the database,
         * so check there next - if it is, the peer object will be added
         * to our hash table. However, we don't want to chase up to the
         * server after it, so indicate it is optional
         */
        PRRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_PROC_URI, &msg->dst,
                                      (char**)&uri, PRRTE_STRING);
        if (PRRTE_SUCCESS == rc ) {
            if (NULL != uri) {
                process_uri(uri);
                if (PRRTE_SUCCESS != prrte_hash_table_get_value_uint64(&prrte_oob_base.peers,
                                                                     ui64, (void**)&pr) ||
                    NULL == pr) {
                    /* that is just plain wrong */
                    PRRTE_ERROR_LOG(PRRTE_ERR_ADDRESSEE_UNKNOWN);
                    msg->status = PRRTE_ERR_ADDRESSEE_UNKNOWN;
                    PRRTE_RML_SEND_COMPLETE(msg);
                    return;
                }
            } else {
                PRRTE_ERROR_LOG(PRRTE_ERR_ADDRESSEE_UNKNOWN);
                msg->status = PRRTE_ERR_ADDRESSEE_UNKNOWN;
                PRRTE_RML_SEND_COMPLETE(msg);
                return;
            }
        } else {
            /* even though we don't know about this peer yet, we still might
             * be able to get to it via routing, so ask each component if
             * it can reach it
             */
            reachable = false;
            pr = NULL;
            PRRTE_LIST_FOREACH(cli, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
                component = (prrte_oob_base_component_t*)cli->cli_component;
                if (NULL != component->is_reachable) {
                    if (component->is_reachable(&msg->dst)) {
                        /* there is a way to reach this peer - record it
                         * so we don't waste this time again
                         */
                        if (NULL == pr) {
                            pr = PRRTE_NEW(prrte_oob_base_peer_t);
                            if (PRRTE_SUCCESS != (rc = prrte_hash_table_set_value_uint64(&prrte_oob_base.peers, ui64, (void*)pr))) {
                                PRRTE_ERROR_LOG(rc);
                                msg->status = PRRTE_ERR_ADDRESSEE_UNKNOWN;
                                PRRTE_RML_SEND_COMPLETE(msg);
                                return;
                            }
                        }
                        /* mark that this component can reach the peer */
                        prrte_bitmap_set_bit(&pr->addressable, component->idx);
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
                if (PRRTE_PROC_IS_DAEMON || PRRTE_PROC_IS_MASTER) {
                    ++msg->retries;
                    if (msg->retries < prrte_rml_base.max_retries) {
                        PRRTE_OOB_SEND(msg);
                        return;
                    }
                }
                msg->status = PRRTE_ERR_ADDRESSEE_UNKNOWN;
                PRRTE_RML_SEND_COMPLETE(msg);
                return;
            }
        }
    }


    /* if we already have a connection to this peer, use it */
    if (NULL != pr->component) {
        /* post this msg for send by this transport - the component
         * runs on our event base, so we can just call their function
         */
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "%s oob:base:send known transport for peer %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&msg->dst));
        if (PRRTE_SUCCESS == (rc = pr->component->send_nb(msg))) {
            return;
        }
    }

    /* if we haven't identified a transport to this peer,
     * loop across all available components in priority order until
     * one replies that it has a module that can reach this peer.
     * Let it try to make the connection
     */
    msg_sent = false;
    PRRTE_LIST_FOREACH(cli, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
        component = (prrte_oob_base_component_t*)cli->cli_component;
        /* is this peer reachable via this component? */
        if (!component->is_reachable(&msg->dst)) {
            continue;
        }
        /* it is addressable, so attempt to send via that transport */
        if (PRRTE_SUCCESS == (rc = component->send_nb(msg))) {
            /* the msg status will be set upon send completion/failure */
            msg_sent = true;
            /* point to this transport for any future messages */
            pr->component = component;
            break;
        } else if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            /* components return "next option" if they can't connect
             * to this peer. anything else is a true error.
             */
            PRRTE_ERROR_LOG(rc);
            msg->status = rc;
            PRRTE_RML_SEND_COMPLETE(msg);
            return;
        }
    }

    /* if no component can reach this peer, that's an error - post
     * it back to the RML for handling
     */
    if (!msg_sent) {
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "%s oob:base:send no path to target %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&msg->dst));
        msg->status = PRRTE_ERR_NO_PATH_TO_TARGET;
        PRRTE_RML_SEND_COMPLETE(msg);
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
void prrte_oob_base_get_addr(char **uri)
{
    char *turi, *final=NULL, *tmp;
    size_t len = 0;
    bool one_added = false;
    prrte_mca_base_component_list_item_t *cli;
    prrte_oob_base_component_t *component;
    pmix_value_t val;
    pmix_proc_t proc;
    pmix_status_t rc;

    /* start with our process name */
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_process_name_to_string(&final, PRRTE_PROC_MY_NAME))) {
        PRRTE_ERROR_LOG(rc);
        *uri = NULL;
        return;
    }
    len = strlen(final);

    /* loop across all available modules to get their input
     * up to the max length
     */
    PRRTE_LIST_FOREACH(cli, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
        component = (prrte_oob_base_component_t*)cli->cli_component;
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
            if (0 < prrte_oob_base.max_uri_length &&
                prrte_oob_base.max_uri_length < (int)(len + strlen(turi))) {
                /* cannot accept the payload */
                continue;
            }
            /* add new value to final one */
            prrte_asprintf(&tmp, "%s;%s", final, turi);
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
    (void)prrte_snprintf_jobid(proc.nspace, PMIX_MAX_NSLEN, PRRTE_PROC_MY_NAME->jobid);
    proc.rank = PRRTE_PROC_MY_NAME->vpid;
    PMIX_VALUE_LOAD(&val, final, PMIX_STRING);
    if (PMIX_SUCCESS != (rc = PMIx_Store_internal(&proc, PMIX_PROC_URI, &val))) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_VALUE_DESTRUCT(&val);
}

static void process_uri(char *uri)
{
    prrte_process_name_t peer;
    char *cptr;
    prrte_mca_base_component_list_item_t *cli;
    prrte_oob_base_component_t *component;
    char **uris=NULL;
    int rc;
    uint64_t ui64;
    prrte_oob_base_peer_t *pr;

    /* find the first semi-colon in the string */
    cptr = strchr(uri, ';');
    if (NULL == cptr) {
        /* got a problem - there must be at least two fields,
         * the first containing the process name of our peer
         * and all others containing the OOB contact info
         */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return;
    }
    *cptr = '\0';
    cptr++;

    /* the first field is the process name, so convert it */
    prrte_util_convert_string_to_process_name(&peer, uri);

    /* if the peer is us, no need to go further as we already
     * know our own contact info
     */
    if (peer.jobid == PRRTE_PROC_MY_NAME->jobid &&
        peer.vpid == PRRTE_PROC_MY_NAME->vpid) {
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "%s:set_addr peer %s is me",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&peer));
        return;
    }

    /* split the rest of the uri into component parts */
    uris = prrte_argv_split(cptr, ';');

    /* get the peer object for this process */
    memcpy(&ui64, (char*)&peer, sizeof(uint64_t));
    if (PRRTE_SUCCESS != prrte_hash_table_get_value_uint64(&prrte_oob_base.peers,
                                                         ui64, (void**)&pr) ||
        NULL == pr) {
        pr = PRRTE_NEW(prrte_oob_base_peer_t);
        if (PRRTE_SUCCESS != (rc = prrte_hash_table_set_value_uint64(&prrte_oob_base.peers, ui64, (void*)pr))) {
            PRRTE_ERROR_LOG(rc);
            prrte_argv_free(uris);
            return;
        }
    }

    /* loop across all available components and let them extract
     * whatever piece(s) of the uri they find relevant - they
     * are all operating on our event base, so we can just
     * directly call their functions
     */
    rc = PRRTE_ERR_UNREACH;
    PRRTE_LIST_FOREACH(cli, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
        component = (prrte_oob_base_component_t*)cli->cli_component;
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "%s:set_addr checking if peer %s is reachable via component %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&peer), component->oob_base.mca_component_name);
        if (NULL != component->set_addr) {
            if (PRRTE_SUCCESS == component->set_addr(&peer, uris)) {
                /* this component found reachable addresses
                 * in the uris
                 */
                prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                    "%s: peer %s is reachable via component %s",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                    PRRTE_NAME_PRINT(&peer), component->oob_base.mca_component_name);
                prrte_bitmap_set_bit(&pr->addressable, component->idx);
            } else {
                prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                    "%s: peer %s is NOT reachable via component %s",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                    PRRTE_NAME_PRINT(&peer), component->oob_base.mca_component_name);
            }
        }
    }
    prrte_argv_free(uris);
}
