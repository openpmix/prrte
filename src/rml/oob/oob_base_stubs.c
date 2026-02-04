/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2014 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/threads/pmix_threads.h"

#include "src/rml/oob/oob.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_peer.h"

static prte_oob_tcp_peer_t* process_uri(char *uri);

void prte_oob_base_send_nb(int fd, short args, void *cbdata)
{
    prte_oob_send_t *cd = (prte_oob_send_t *) cbdata;
    prte_rml_send_t *msg;
    prte_oob_tcp_peer_t *peer;
    pmix_proc_t hop;
    int rc;
    char *uri = NULL;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* done with this. release it now */
    msg = cd->msg;
    PMIX_RELEASE(cd);

    pmix_output_verbose(5, prte_oob_base.output,
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

    /* do we have a route to this peer (could be direct)? */
    PMIX_LOAD_NSPACE(hop.nspace, PRTE_PROC_MY_NAME->nspace);
    hop.rank = prte_rml_get_route(msg->dst.rank);
    /* do we know this hop? */
    if (NULL == (peer = prte_oob_tcp_peer_lookup(&hop))) {
        /* if this message is going to the HNP, send it direct */
        if (PRTE_PROC_MY_HNP->rank == msg->dst.rank) {
            hop.rank = PRTE_PROC_MY_HNP->rank;
            peer = prte_oob_tcp_peer_lookup(&hop);
            if (NULL != peer) {
                goto send;
            }
        }
        // see if we know the contact info for it
        PRTE_MODEX_RECV_VALUE_OPTIONAL(rc, PMIX_PROC_URI, &hop, (char **) &uri, PMIX_STRING);
        if (PRTE_SUCCESS == rc && NULL != uri) {
            peer = process_uri(uri);
            if (NULL == peer) {
                /* that is just plain wrong */
                pmix_output_verbose(5, prte_oob_base.output,
                                    "%s oob:base:send addressee unknown %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    PRTE_NAME_PRINT(&msg->dst));

                if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
                    /* just ignore the problem */
                    PMIX_RELEASE(msg);
                    return;
                }
                PRTE_ACTIVATE_PROC_STATE(&hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
                PMIX_RELEASE(msg);
                return;
            }
        } else {
            // unable to send it
             if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
                /* just ignore the problem */
                PMIX_RELEASE(msg);
                return;
            }
            PRTE_ACTIVATE_PROC_STATE(&hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
            PMIX_RELEASE(msg);
            return;
       }
   }

send:
    pmix_output_verbose(2, prte_oob_base.output,
                        "%s:[%s:%d] processing send to peer %s:%d seq_num = %d via %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__,
                        PRTE_NAME_PRINT(&msg->dst), msg->tag, msg->seq_num,
                        PRTE_NAME_PRINT(&peer->name));

    /* add the msg to the hop's send queue */
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s tcp:send_nb: already connected to %s - queueing for send",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));
        MCA_OOB_TCP_QUEUE_SEND(msg, peer);
        return;
    }

    /* add the message to the queue for sending after the
     * connection is formed
     */
    MCA_OOB_TCP_QUEUE_PENDING(msg, peer);

    if (MCA_OOB_TCP_CONNECTING != peer->state && MCA_OOB_TCP_CONNECT_ACK != peer->state) {
        /* we have to initiate the connection - again, we do not
         * want to block while the connection is created.
         * So throw us into an event that will create
         * the connection via a mini-state-machine :-)
         */
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s tcp:send_nb: initiating connection to %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));
        peer->state = MCA_OOB_TCP_CONNECTING;
        PRTE_ACTIVATE_TCP_CONN_STATE(peer, prte_oob_tcp_peer_try_connect);
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
    char *final = NULL, *tmp;
    char *cptr = NULL, *tp, *tm;
    size_t len = 0;
    pmix_status_t rc;

    /* start with our process name */
    rc = prte_util_convert_process_name_to_string(&final, PRTE_PROC_MY_NAME);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        *uri = NULL;
        return;
    }
    len = strlen(final);

    if (!prte_oob_base.disable_ipv4_family &&
        NULL != prte_oob_base.ipv4conns) {
        tmp = PMIx_Argv_join(prte_oob_base.ipv4conns, ',');
        tp = PMIx_Argv_join(prte_oob_base.ipv4ports, ',');
        tm = PMIx_Argv_join(prte_oob_base.if_masks, ',');
        pmix_asprintf(&cptr, "tcp://%s:%s:%s", tmp, tp, tm);
        free(tmp);
        free(tp);
        free(tm);
    }
#if PRTE_ENABLE_IPV6
    if (!prte_oob_base.disable_ipv6_family &&
        NULL != prte_oob_base.ipv6conns) {
        char *tmp2;

        /* Fixes #2498
         * RFC 3986, section 3.2.2
         * The notation in that case is to encode the IPv6 IP number in square brackets:
         * "http://[2001:db8:1f70::999:de8:7648:6e8]:100/"
         * A host identified by an Internet Protocol literal address, version 6 [RFC3513]
         * or later, is distinguished by enclosing the IP literal within square brackets.
         * This is the only place where square bracket characters are allowed in the URI
         * syntax. In anticipation of future, as-yet-undefined IP literal address formats,
         * an implementation may use an optional version flag to indicate such a format
         * explicitly rather than rely on heuristic determination.
         */
        tmp = PMIx_Argv_join(prte_oob_base.ipv6conns, ',');
        tp = PMIx_Argv_join(prte_oob_base.ipv6ports, ',');
        tm = PMIx_Argv_join(prte_oob_base.if_masks, ',');
        if (NULL == cptr) {
            /* no ipv4 stuff */
            pmix_asprintf(&cptr, "tcp6://[%s]:%s:%s", tmp, tp, tm);
        } else {
            pmix_asprintf(&tmp2, "%s;tcp6://[%s]:%s:%s", cptr, tmp, tp, tm);
            free(cptr);
            cptr = tmp2;
        }
        free(tmp);
        free(tp);
        free(tm);
    }
#endif // PRTE_ENABLE_IPV6

    if (NULL == cptr) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        *uri = NULL;
        return;
    }

    /* check overall length for limits */
    if (0 < prte_oob_base.max_uri_length
        && prte_oob_base.max_uri_length < (int) (len + strlen(cptr))) {
        /* cannot accept the payload */
        free(final);
        free(cptr);
        *uri = NULL;
        return;
    }
    /* add new value to final one */
    pmix_asprintf(&tmp, "%s;%s", final, cptr);
    free(cptr);
    free(final);
    final = tmp;

    *uri = final;
}

/* the host in this case is always in "dot" notation, and
 * thus we do not need to do a DNS lookup to convert it */
static int parse_uri(const uint16_t af_family, const char *host, const char *port,
                     struct sockaddr_storage *inaddr)
{
    struct sockaddr_in *in;

    if (AF_INET == af_family) {
        memset(inaddr, 0, sizeof(struct sockaddr_in));
        in = (struct sockaddr_in *) inaddr;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = inet_addr(host);
        if (in->sin_addr.s_addr == INADDR_NONE) {
            return PRTE_ERR_BAD_PARAM;
        }
        ((struct sockaddr_in *) inaddr)->sin_port = htons(atoi(port));
    }
#if PRTE_ENABLE_IPV6
    else if (AF_INET6 == af_family) {
        struct sockaddr_in6 *in6;
        memset(inaddr, 0, sizeof(struct sockaddr_in6));
        in6 = (struct sockaddr_in6 *) inaddr;

        if (0 == inet_pton(AF_INET6, host, (void *) &in6->sin6_addr)) {
            pmix_output(0, "oob_tcp_parse_uri: Could not convert %s\n", host);
            return PRTE_ERR_BAD_PARAM;
        }
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(atoi(port));
    }
#endif
    else {
        return PRTE_ERR_NOT_SUPPORTED;
    }
    return PRTE_SUCCESS;
}

static void set_addr(pmix_proc_t *peer, char **uris)
{
    char **addrs, **masks, *hptr;
    char *tcpuri = NULL, *host, *ports, *masks_string;
    int i, j, rc;
    uint16_t af_family = AF_UNSPEC;
    uint64_t ui64;
    prte_oob_tcp_peer_t *pr;
    prte_oob_tcp_addr_t *maddr;

    memcpy(&ui64, (char *) peer, sizeof(uint64_t));

    for (i = 0; NULL != uris[i]; i++) {
        tcpuri = strdup(uris[i]);
        if (NULL == tcpuri) {
            pmix_output_verbose(2, prte_oob_base.output,
                                "%s oob:tcp: out of memory", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            continue;
        }
        if (0 == strncmp(uris[i], "tcp:", 4)) {
            af_family = AF_INET;
            host = tcpuri + strlen("tcp://");
        } else if (0 == strncmp(uris[i], "tcp6:", 5)) {
#if PRTE_ENABLE_IPV6
            af_family = AF_INET6;
            host = tcpuri + strlen("tcp6://");
#else  // PRTE_ENABLE_IPV6
            /* we don't support this connection type */
            pmix_output_verbose(2, prte_oob_base.output,
                                "%s oob:tcp: address %s not supported",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uris[i]);
            free(tcpuri);
            continue;
#endif // PRTE_ENABLE_IPV6
        } else {
            /* not one of ours */
            pmix_output_verbose(2, prte_oob_base.output,
                                "%s oob:tcp: ignoring address %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uris[i]);
            free(tcpuri);
            continue;
        }

        /* this one is ours - record the peer */
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s oob:tcp: working peer %s address %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer), uris[i]);

        /* separate the mask from the network addrs */
        masks_string = strrchr(tcpuri, ':');
        if (NULL == masks_string) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            free(tcpuri);
            continue;
        }
        *masks_string = '\0';
        masks_string++;
        masks = PMIx_Argv_split(masks_string, ',');

        /* separate the ports from the network addrs */
        ports = strrchr(tcpuri, ':');
        if (NULL == ports) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            free(tcpuri);
            continue;
        }
        *ports = '\0';
        ports++;

        /* split the addrs */
        /* if this is a tcp6 connection, the first one will have a '['
         * at the beginning of it, and the last will have a ']' at the
         * end - we need to remove those extra characters
         */
        hptr = host;
#if PRTE_ENABLE_IPV6
        if (AF_INET6 == af_family) {
            if ('[' == host[0]) {
                hptr = &host[1];
            }
            if (']' == host[strlen(host) - 1]) {
                host[strlen(host) - 1] = '\0';
            }
        }
#endif // PRTE_ENABLE_IPV6
        addrs = PMIx_Argv_split(hptr, ',');

        /* cycle across the provided addrs */
        for (j = 0; NULL != addrs[j]; j++) {
            if (NULL == masks[j]) {
                /* Missing mask information */
                pmix_output_verbose(2, prte_oob_base.output,
                                    "%s oob:tcp: uri missing mask information.",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                return;
            }
            /* if they gave us "localhost", then just take the first conn on our list */
            if (0 == strcasecmp(addrs[j], "localhost")) {
#if PRTE_ENABLE_IPV6
                if (AF_INET6 == af_family) {
                    if (NULL == prte_oob_base.ipv6conns
                        || NULL == prte_oob_base.ipv6conns[0]) {
                        continue;
                    }
                    host = prte_oob_base.ipv6conns[0];
                } else {
#endif // PRTE_ENABLE_IPV6
                    if (NULL == prte_oob_base.ipv4conns
                        || NULL == prte_oob_base.ipv4conns[0]) {
                        continue;
                    }
                    host = prte_oob_base.ipv4conns[0];
#if PRTE_ENABLE_IPV6
                }
#endif
            } else {
                host = addrs[j];
            }

            if (NULL == (pr = prte_oob_tcp_peer_lookup(peer))) {
                pr = PMIX_NEW(prte_oob_tcp_peer_t);
                PMIX_XFER_PROCID(&pr->name, peer);
                pmix_output_verbose(20, prte_oob_base.output,
                                    "%s SET_PEER ADDING PEER %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer));
                pmix_list_append(&prte_oob_base.peers, &pr->super);
            }

            maddr = PMIX_NEW(prte_oob_tcp_addr_t);
            ((struct sockaddr_storage *) &(maddr->addr))->ss_family = af_family;
            if (PRTE_SUCCESS
                != (rc = parse_uri(af_family, host, ports,
                                   (struct sockaddr_storage *) &(maddr->addr)))) {
                PRTE_ERROR_LOG(rc);
                PMIX_RELEASE(maddr);
                pmix_list_remove_item(&prte_oob_base.peers, &pr->super);
                PMIX_RELEASE(pr);
                return;
            }
            maddr->if_mask = atoi(masks[j]);

            pmix_output_verbose(20, prte_oob_base.output,
                                "%s set_peer: peer %s is listening on net %s port %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer),
                                (NULL == host) ? "NULL" : host, (NULL == ports) ? "NULL" : ports);
            pmix_list_append(&pr->addrs, &maddr->super);
        }
        PMIx_Argv_free(addrs);
        free(tcpuri);
    }
}

static prte_oob_tcp_peer_t *get_peer(const pmix_proc_t *pr);

static prte_oob_tcp_peer_t* process_uri(char *uri)
{
    pmix_proc_t peer;
    char *cptr;
    char **uris = NULL;
    prte_oob_tcp_peer_t *pr;

    pmix_output_verbose(5, prte_oob_base.output,
                        "%s:set_addr processing uri %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uri);

    /* find the first semi-colon in the string */
    cptr = strchr(uri, ';');
    if (NULL == cptr) {
        /* got a problem - there must be at least two fields,
         * the first containing the process name of our peer
         * and all others containing the OOB contact info
         */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return NULL;
    }
    *cptr = '\0';
    cptr++;
    /* the first field is the process name, so convert it */
    prte_util_convert_string_to_process_name(&peer, uri);

    /* if the peer is us, no need to go further as we already
     * know our own contact info
     */
    if (PMIX_CHECK_PROCID(&peer, PRTE_PROC_MY_NAME)) {
        pmix_output_verbose(5, prte_oob_base.output,
                            "%s:set_addr peer %s is me",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_NAME_PRINT(&peer));
        return NULL;
    }

    /* split the rest of the uri into component parts */
    uris = PMIx_Argv_split(cptr, ';');

    /* get the peer object for this process */
    pr = get_peer(&peer);
    if (NULL == pr) {
        pr = PMIX_NEW(prte_oob_tcp_peer_t);
        PMIX_XFER_PROCID(&pr->name, &peer);
        pmix_list_append(&prte_oob_base.peers, &pr->super);
    }

    set_addr(&pr->name, uris);
    PMIx_Argv_free(uris);
    return pr;
}

static prte_oob_tcp_peer_t *get_peer(const pmix_proc_t *pr)
{
    prte_oob_tcp_peer_t *peer;

    PMIX_LIST_FOREACH(peer, &prte_oob_base.peers, prte_oob_tcp_peer_t)
    {
        if (PMIX_CHECK_PROCID(pr, &peer->name)) {
            return peer;
        }
    }
    return NULL;
}
