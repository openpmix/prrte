/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * In windows, many of the socket functions return an EWOULDBLOCK
 * instead of things like EAGAIN, EINPROGRESS, etc. It has been
 * verified that this will not conflict with other error codes that
 * are returned by these functions under UNIX/Linux environments
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#include <ctype.h>

#include "src/class/pmix_list.h"
#include "src/include/prte_socket_errno.h"
#include "src/util/pmix_argv.h"
#include "src/util/error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_parse_options.h"

#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_listener.h"
#include "src/rml/oob/oob_tcp_peer.h"

static void *listen_thread(pmix_object_t *obj);
static int create_listeners(int family);
static void unadvertise(char ***conns, char ***masks);
static void connection_handler(int sd, short flags, void *cbdata);
static void connection_event_handler(int sd, short flags, void *cbdata);

/*
 * Open the listening sockets on the interfaces prte_oob_open selected, and
 * start harvesting connections from them: on a thread in the DVM master, on
 * the event base everywhere else.  A family that cannot be listened on is
 * dropped from our contact URI.
 */
int prte_oob_tcp_start_listening(void)
{
    int rc = PRTE_SUCCESS;
    prte_oob_tcp_listener_t *listener;

    /* if we don't have any TCP interfaces, we shouldn't be here */
    if (NULL == prte_oob_base.ipv4conns
#if PRTE_ENABLE_IPV6
        && NULL == prte_oob_base.ipv6conns
#endif
    ) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* create listen socket(s) for incoming connection attempts */
    rc = create_listeners(AF_INET);
    if (PRTE_SUCCESS != rc) {
        unadvertise(&prte_oob_base.ipv4conns, &prte_oob_base.ipv4masks);
    }
#if PRTE_ENABLE_IPV6
    rc = create_listeners(AF_INET6);
    if (PRTE_SUCCESS != rc) {
        unadvertise(&prte_oob_base.ipv6conns, &prte_oob_base.ipv6masks);
    }
#endif
    if (0 == pmix_list_get_size(&prte_oob_base.listeners)) {
        /* we were unable to open any listening sockets */
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "no-listeners", true);
        return PRTE_ERR_SOCKET_NOT_AVAILABLE;
    }

    /* if I am the HNP, start a listening thread so we can
     * harvest connection requests as rapidly as possible
     */
    if (PRTE_PROC_IS_MASTER) {
        if (0 > pipe(prte_oob_base.stop_thread)) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* Make sure the pipe FDs are set to close-on-exec so that
           they don't leak into children */
        if (pmix_fd_set_cloexec(prte_oob_base.stop_thread[0]) != PRTE_SUCCESS
            || pmix_fd_set_cloexec(prte_oob_base.stop_thread[1]) != PRTE_SUCCESS) {
            close(prte_oob_base.stop_thread[0]);
            close(prte_oob_base.stop_thread[1]);
            PRTE_ERROR_LOG(PRTE_ERR_IN_ERRNO);
            return PRTE_ERR_IN_ERRNO;
        }

        prte_oob_base.listen_thread_active = true;
        prte_oob_base.listen_thread.t_run = listen_thread;
        prte_oob_base.listen_thread.t_arg = NULL;
        if (PRTE_SUCCESS != (rc = pmix_thread_start(&prte_oob_base.listen_thread))) {
            PRTE_ERROR_LOG(rc);
            pmix_output(0, "%s Unable to start listen thread", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        }
        return rc;
    }

    /* otherwise, setup to listen via the event lib */

    PMIX_LIST_FOREACH(listener, &prte_oob_base.listeners, prte_oob_tcp_listener_t)
    {
        listener->ev_active = true;
        prte_event_set(prte_event_base, &listener->event, listener->sd,
                       PRTE_EV_READ | PRTE_EV_PERSIST, connection_event_handler, listener);
        PMIX_POST_OBJECT(listener);
        prte_event_add(&listener->event, 0);
    }

    return PRTE_SUCCESS;
}

/* The ports a family's listeners may use, in the order to try them.  A static
 * port is the one every daemon is known to listen on, so only the first entry
 * of a static list means anything; a dynamic range is tried in order; with
 * neither, "0" lets the kernel choose. */
static char **candidate_ports(int family, bool *isstatic)
{
    char **static_ports = prte_oob_base.tcp_static_ports;
    char **dyn_ports = prte_oob_base.tcp_dyn_ports;
    char **ports = NULL;

    if (AF_INET6 == family) {
        static_ports = prte_oob_base.tcp6_static_ports;
        dyn_ports = prte_oob_base.tcp6_dyn_ports;
    }

    *isstatic = false;
    if (NULL != static_ports) {
        PMIx_Argv_append_nosize(&ports, static_ports[0]);
        *isstatic = true;
    } else if (NULL != dyn_ports) {
        ports = PMIx_Argv_copy(dyn_ports);
    } else {
        PMIx_Argv_append_nosize(&ports, "0");
    }
    return ports;
}

static uint16_t get_port(struct sockaddr_storage *addr)
{
    if (AF_INET6 == addr->ss_family) {
        return ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
    }
    return ntohs(((struct sockaddr_in *) addr)->sin_port);
}

static void set_port(struct sockaddr_storage *addr, uint16_t port)
{
    if (AF_INET6 == addr->ss_family) {
        ((struct sockaddr_in6 *) addr)->sin6_port = htons(port);
    } else {
        ((struct sockaddr_in *) addr)->sin_port = htons(port);
    }
}

static bool same_address(struct sockaddr_storage *a, struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (AF_INET6 == a->ss_family) {
        return 0 == memcmp(&((struct sockaddr_in6 *) a)->sin6_addr,
                           &((struct sockaddr_in6 *) b)->sin6_addr, sizeof(struct in6_addr));
    }
    return ((struct sockaddr_in *) a)->sin_addr.s_addr == ((struct sockaddr_in *) b)->sin_addr.s_addr;
}

/*
 * Open one non-blocking, close-on-exec listening socket on @c addr, at the
 * port @c addr already carries (0 lets the kernel choose).  On success the
 * address actually bound - including the chosen port - is written back into
 * @c addr.
 *
 * Returns PRTE_SUCCESS; PRTE_ERR_TAKE_NEXT_OPTION when that address:port is
 * taken or cannot be assigned, so the caller may try another port; or an error
 * for anything else.
 */
static int open_listener(struct sockaddr_storage *addr, bool isstatic, int *sdout)
{
    int flags, sd, err;
    prte_socklen_t addrlen;

    addrlen = (AF_INET6 == addr->ss_family) ? sizeof(struct sockaddr_in6)
                                            : sizeof(struct sockaddr_in);

    sd = socket(addr->ss_family, SOCK_STREAM, 0);
    if (0 > sd) {
        if (EAFNOSUPPORT != prte_socket_errno) {
            pmix_output(0, "%s oob:tcp:listen: socket() failed: %s (%d)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        strerror(prte_socket_errno), prte_socket_errno);
        }
        return PRTE_ERR_IN_ERRNO;
    }

    /* Enable/disable reusing ports */
    flags = isstatic ? 1 : 0;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char *) &flags, sizeof(flags)) < 0) {
        pmix_output(0, "%s oob:tcp:listen: unable to set the SO_REUSEADDR option (%s:%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* Set the socket to close-on-exec so that no children inherit this FD */
    if (PRTE_SUCCESS != pmix_fd_set_cloexec(sd)) {
        pmix_output(0, "%s oob:tcp:listen: unable to set the listening socket to CLOEXEC (%s:%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

#ifdef IPV6_V6ONLY
    if (AF_INET6 == addr->ss_family) {
        /* accept only V6 connections here: an IPv4-mapped connection belongs
         * to the IPv4 listeners (cf. mca_btl_tcp_create_listen) */
        flags = 1;
        if (setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &flags, sizeof(flags)) < 0) {
            pmix_output(0, "%s oob:tcp:listen: unable to set the IPV6_V6ONLY option (%s:%d)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        strerror(prte_socket_errno), prte_socket_errno);
        }
    }
#endif

    if (bind(sd, (struct sockaddr *) addr, addrlen) < 0) {
        err = prte_socket_errno;
        CLOSE_THE_SOCKET(sd);
        if (EADDRINUSE == err || EADDRNOTAVAIL == err) {
            return PRTE_ERR_TAKE_NEXT_OPTION;
        }
        pmix_output(0, "%s bind() failed for %s port %d: %s (%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    pmix_net_get_hostname((struct sockaddr *) addr), (int) get_port(addr),
                    strerror(err), err);
        return PRTE_ERROR;
    }

    /* resolve assigned port */
    if (getsockname(sd, (struct sockaddr *) addr, &addrlen) < 0) {
        pmix_output(0, "%s oob:tcp:listen: getsockname(): %s (%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        pmix_output(0, "%s oob:tcp:listen: listen(): %s (%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0 ||
        fcntl(sd, F_SETFL, flags | O_NONBLOCK) < 0) {
        pmix_output(0, "%s oob:tcp:listen: fcntl() failed: %s (%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    *sdout = sd;
    return PRTE_SUCCESS;
}

/*
 * Listen on every selected address of @c family, all at the same port.
 *
 * One port, because a contact URI carries one port for all of its addresses
 * (set_addr pairs each address with the first port listed), and because a
 * static port is by definition the same everywhere.  With @c port 0 the
 * kernel chooses it for the first address and the rest are bound to match.
 *
 * All or nothing: if any address cannot take the port, every socket this call
 * opened is closed again and the caller may try another port.
 */
static int listen_on_port(int family, uint16_t port, bool isstatic, uint16_t *bound)
{
    pmix_list_t opened;
    pmix_pif_t *intf;
    prte_oob_tcp_listener_t *listener, *prior;
    struct sockaddr_storage addr;
    bool dup;
    int rc, sd;

    PMIX_CONSTRUCT(&opened, pmix_list_t);
    PMIX_LIST_FOREACH(intf, &prte_oob_base.local_ifs, pmix_pif_t)
    {
        if (family != intf->af_family) {
            continue;
        }
        memcpy(&addr, &intf->if_addr, sizeof(addr));
        set_port(&addr, port);

        /* an address can appear on the interface list more than once, and
         * binding it twice would fail on the port we just took */
        dup = false;
        PMIX_LIST_FOREACH(prior, &opened, prte_oob_tcp_listener_t)
        {
            if (same_address(&prior->addr, &addr)) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        rc = open_listener(&addr, isstatic, &sd);
        if (PRTE_SUCCESS != rc) {
            /* releasing each listener closes its socket */
            PMIX_LIST_DESTRUCT(&opened);
            return rc;
        }
        listener = PMIX_NEW(prte_oob_tcp_listener_t);
        listener->tcp6 = (AF_INET6 == family);
        listener->sd = sd;
        listener->port = get_port(&addr);
        memcpy(&listener->addr, &addr, sizeof(addr));
        pmix_list_append(&opened, &listener->item);
        /* the remaining addresses take whatever port the first one got */
        port = listener->port;

        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s oob:tcp:listen: listening on %s port %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            pmix_net_get_hostname((struct sockaddr *) &addr), (int) port);
    }

    while (NULL != (listener = (prte_oob_tcp_listener_t *) pmix_list_remove_first(&opened))) {
        pmix_list_append(&prte_oob_base.listeners, &listener->item);
    }
    PMIX_DESTRUCT(&opened);
    *bound = port;
    return PRTE_SUCCESS;
}

/* How many kernel-chosen ports to try before giving up, when the port the
 * first address was given turns out to be taken on another one */
#define PRTE_OOB_TCP_EPHEMERAL_ATTEMPTS 8

/*
 * Create the listening sockets for one address family: one per address
 * prte_oob_open selected, never a wildcard.  Binding the wildcard would accept
 * connections on interfaces the user excluded - and, for a DVM confined to
 * loopback, expose a port on every external interface it was meant to keep
 * off.  Nothing loses by it: a peer reaches us only at the addresses our
 * contact URI names, and those are exactly these.
 *
 * Returns PRTE_ERR_NOT_FOUND, quietly, when the family has no selected address.
 */
static int create_listeners(int family)
{
    char **ports, ***portlist, *tconn;
    pmix_pif_t *intf;
    bool isstatic, found = false;
    uint16_t port = 0, bound = 0;
    int i, n, attempts, rc = PRTE_ERR_SOCKET_NOT_AVAILABLE;

    PMIX_LIST_FOREACH(intf, &prte_oob_base.local_ifs, pmix_pif_t)
    {
        if (family == intf->af_family) {
            found = true;
            break;
        }
    }
    if (!found) {
        return PRTE_ERR_NOT_FOUND;
    }

    ports = candidate_ports(family, &isstatic);
    for (i = 0; NULL != ports && NULL != ports[i]; i++) {
        port = (uint16_t) strtol(ports[i], NULL, 10);
        attempts = (0 == port) ? PRTE_OOB_TCP_EPHEMERAL_ATTEMPTS : 1;
        for (n = 0; n < attempts; n++) {
            pmix_output_verbose(5, prte_oob_base.output,
                                "%s attempting to bind to %s port %d",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                (AF_INET6 == family) ? "IPv6" : "IPv4", (int) port);
            rc = listen_on_port(family, port, isstatic, &bound);
            if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
                break;
            }
        }
        if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            break;
        }
    }
    PMIx_Argv_free(ports);
    if (PRTE_ERR_TAKE_NEXT_OPTION == rc) {
        rc = PRTE_ERR_SOCKET_NOT_AVAILABLE;
    }
    if (PRTE_SUCCESS != rc) {
        pmix_output_verbose(5, prte_oob_base.output,
                            "%s oob:tcp:listen: no %s listener could be opened - "
                            "not advertising that family",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (AF_INET6 == family) ? "IPv6" : "IPv4");
        return rc;
    }

    portlist = (AF_INET6 == family) ? &prte_oob_base.ipv6ports : &prte_oob_base.ipv4ports;
    pmix_asprintf(&tconn, "%d", (int) bound);
    PMIx_Argv_append_nosize(portlist, tconn);
    free(tconn);
    if (0 == prte_process_info.my_port) {
        prte_process_info.my_port = bound;
    }
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s assigned %s port %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (AF_INET6 == family) ? "IPv6" : "IPv4", (int) bound);
    return PRTE_SUCCESS;
}

/* A family we could not listen on must not be advertised: a peer handed its
 * addresses would dial a port nobody holds. */
static void unadvertise(char ***conns, char ***masks)
{
    if (NULL != *conns) {
        PMIx_Argv_free(*conns);
        *conns = NULL;
    }
    if (NULL != *masks) {
        PMIx_Argv_free(*masks);
        *masks = NULL;
    }
}

/*
 * Is this accept() failure about the one connection being accepted, rather
 * than about the listening socket?  A client that resets before we accept it
 * yields ECONNABORTED, and Linux also hands back the pending network error of
 * the new connection (see accept(2)).  None of those is a reason to stop
 * listening, or even to say anything: the next connection is unaffected.
 */
static bool accept_error_is_transient(int err)
{
    switch (err) {
    case EINTR:
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ECONNABORTED:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    case ENETDOWN:
    case ENETUNREACH:
    case EOPNOTSUPP:
#ifdef ENONET
    case ENONET:
#endif
        return true;
    default:
        return false;
    }
}

/*
 * The listen thread created when listen_mode is threaded.  Accepts
 * incoming connections and places them in a queue for further
 * processing
 *
 * Runs until prte_oob_tcp_compnent.shutdown is set to true.
 */
static void *listen_thread(pmix_object_t *obj)
{
    int rc, max, accepted_connections, sd;
    prte_socklen_t addrlen;
    prte_oob_tcp_pending_connection_t *pending_connection;
    struct timeval timeout;
    fd_set readfds;
    prte_oob_tcp_listener_t *listener;
    PRTE_HIDE_UNUSED_PARAMS(obj);

    /* only execute during the initial VM startup stage - once
     * all the initial daemons have reported in, we will revert
     * to the event method for handling any further connections
     * so as to minimize overhead
     */
    while (prte_oob_base.listen_thread_active) {
        FD_ZERO(&readfds);
        max = -1;
        PMIX_LIST_FOREACH(listener, &prte_oob_base.listeners, prte_oob_tcp_listener_t)
        {
            FD_SET(listener->sd, &readfds);
            max = (listener->sd > max) ? listener->sd : max;
        }
        /* add the stop_thread fd */
        FD_SET(prte_oob_base.stop_thread[0], &readfds);
        max = (prte_oob_base.stop_thread[0] > max) ? prte_oob_base.stop_thread[0] : max;

        /* set timeout interval */
        timeout.tv_sec = prte_oob_base.listen_thread_tv.tv_sec;
        timeout.tv_usec = prte_oob_base.listen_thread_tv.tv_usec;

        /* Block in a select to avoid hammering the cpu.  If a connection
         * comes in, we'll get woken up right away.
         */
        rc = select(max + 1, &readfds, NULL, NULL, &timeout);
        if (!prte_oob_base.listen_thread_active) {
            /* we've been asked to terminate */
            return NULL;
        }
        if (rc < 0) {
            if (EAGAIN != prte_socket_errno && EINTR != prte_socket_errno) {
                perror("select");
            }
            continue;
        }

        /* Spin accepting connections until all active listen sockets
         * do not have any incoming connections, pushing each connection
         * onto the event queue for processing
         */
        do {
            accepted_connections = 0;
            PMIX_LIST_FOREACH(listener, &prte_oob_base.listeners, prte_oob_tcp_listener_t)
            {
                sd = listener->sd;

                /* according to the man pages, select replaces the given descriptor
                 * set with a subset consisting of those descriptors that are ready
                 * for the specified operation - in this case, a read. So we need to
                 * first check to see if this file descriptor is included in the
                 * returned subset
                 */
                if (0 == FD_ISSET(sd, &readfds)) {
                    /* this descriptor is not included */
                    continue;
                }

                /* this descriptor is ready to be read, which means a connection
                 * request has been received - so harvest it. All we want to do
                 * here is accept the connection and push the info onto the event
                 * library for subsequent processing - we don't want to actually
                 * process the connection here as it takes too long, and so the
                 * OS might start rejecting connections due to timeout.
                 */
                pending_connection = PMIX_NEW(prte_oob_tcp_pending_connection_t);
                prte_event_set(prte_event_base, &pending_connection->ev, -1, PRTE_EV_WRITE,
                               connection_handler, pending_connection);
                /* accept() overwrites this with the length of the address it
                 * returned, which is shorter for IPv4 than for IPv6 */
                addrlen = sizeof(pending_connection->addr);
                pending_connection->fd = accept(sd, (struct sockaddr *) &(pending_connection->addr),
                                                &addrlen);

                /* check for < 0 as indicating an error upon accept */
                if (pending_connection->fd < 0) {
                    PMIX_RELEASE(pending_connection);

                    /* Non-fatal errors */
                    if (accept_error_is_transient(prte_socket_errno)) {
                        continue;
                    }

                    /* If we run out of file descriptors, log an extra
                       warning (so that the user can know to fix this
                       problem) and abandon all hope. */
                    else if (EMFILE == prte_socket_errno) {
                        CLOSE_THE_SOCKET(sd);
                        listener->sd = -1;
                        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_SOCKETS);
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "accept failed", true,
                                       prte_process_info.nodename, prte_socket_errno,
                                       strerror(prte_socket_errno), "Out of file descriptors");
                        goto done;
                    }

                    /* For all other cases, print a
                       warning but try to continue */
                    else {
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "accept failed", true,
                                       prte_process_info.nodename, prte_socket_errno,
                                       strerror(prte_socket_errno),
                                       "Unknown cause; job will try to continue");
                        continue;
                    }
                }

                pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                    "%s prte_oob_tcp_listen_thread: incoming connection: "
                                    "(%d, %d) %s:%d\n",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), pending_connection->fd,
                                    prte_socket_errno,
                                    pmix_net_get_hostname(
                                        (struct sockaddr *) &pending_connection->addr),
                                    pmix_net_get_port(
                                        (struct sockaddr *) &pending_connection->addr));

                /* if we are on a privileged port, we only accept connections
                 * from other privileged sockets. A privileged port is one
                 * whose port is less than 1024 on Linux, so we'll check for that. */
                if (1024 >= listener->port) {
                    uint16_t inport;
                    inport = pmix_net_get_port((struct sockaddr *) &pending_connection->addr);
                    if (1024 < inport) {
                        /* someone tried to cross-connect privileges,
                         * say something */
                        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "privilege failure", true,
                                       prte_process_info.nodename, listener->port,
                                       pmix_net_get_hostname(
                                           (struct sockaddr *) &pending_connection->addr),
                                       inport);
                        CLOSE_THE_SOCKET(pending_connection->fd);
                        PMIX_RELEASE(pending_connection);
                        continue;
                    }
                }

                /* activate the event */
                PMIX_POST_OBJECT(pending_connection);
                prte_event_active(&pending_connection->ev, PRTE_EV_WRITE, 1);
                accepted_connections++;
            }
        } while (accepted_connections > 0);
    }

done:
    return NULL;
}

/*
 * Handler for accepting connections from the listen thread
 */
static void connection_handler(int sd, short flags, void *cbdata)
{
    prte_oob_tcp_pending_connection_t *new_connection;
    PRTE_HIDE_UNUSED_PARAMS(sd, flags);

    new_connection = (prte_oob_tcp_pending_connection_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(new_connection);

    pmix_output_verbose(4, prte_oob_base.output,
                        "%s connection_handler: working connection "
                        "(%d, %d) %s:%d\n",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), new_connection->fd, prte_socket_errno,
                        pmix_net_get_hostname((struct sockaddr *) &new_connection->addr),
                        pmix_net_get_port((struct sockaddr *) &new_connection->addr));

    /* process the connection */
    prte_oob_accept_connection(new_connection->fd, (struct sockaddr *) &(new_connection->addr));

    /* cleanup */
    PMIX_RELEASE(new_connection);
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_event_handler(int incoming_sd, short flags, void *cbdata)
{
    prte_oob_tcp_listener_t *listener = (prte_oob_tcp_listener_t *) cbdata;
    /* big enough for either family: accept() truncates to what it is given */
    struct sockaddr_storage addr;
    prte_socklen_t addrlen = sizeof(addr);
    int sd, err;
    PRTE_HIDE_UNUSED_PARAMS(flags);

    sd = accept(incoming_sd, (struct sockaddr *) &addr, &addrlen);
    if (sd < 0) {
        /* Non-fatal errors */
        if (accept_error_is_transient(prte_socket_errno)) {
            return;
        }

        /* Give up on this listener.  Its event is persistent, so it has to
         * come off the base before the socket is closed - left armed on a
         * dead descriptor it would fire, or watch whatever next reuses the
         * number - and the destructor must not close it a second time. */
        err = prte_socket_errno;
        prte_event_del(&listener->event);
        listener->ev_active = false;
        CLOSE_THE_SOCKET(incoming_sd);
        listener->sd = -1;

        /* If we run out of file descriptors, log an extra warning (so
           that the user can know to fix this problem) and abandon all
           hope. */
        if (EMFILE == err) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_SOCKETS);
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "accept failed", true,
                           prte_process_info.nodename, err, strerror(err),
                           "Out of file descriptors");
        } else {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "accept failed", true,
                           prte_process_info.nodename, err, strerror(err),
                           "Unknown cause; job will try to continue");
        }
        return;
    }
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s connection_event_handler: working connection "
                        "(%d) %s:%d\n",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sd,
                        pmix_net_get_hostname((struct sockaddr *) &addr),
                        pmix_net_get_port((struct sockaddr *) &addr));

    /* process the connection */
    prte_oob_accept_connection(sd, (struct sockaddr *) &addr);
}

static void tcp_ev_cons(prte_oob_tcp_listener_t *event)
{
    event->ev_active = false;
    event->tcp6 = false;
    event->sd = -1;
    event->port = 0;
}
static void tcp_ev_des(prte_oob_tcp_listener_t *event)
{
    if (event->ev_active) {
        prte_event_del(&event->event);
    }
    event->ev_active = false;
    if (0 <= event->sd) {
        CLOSE_THE_SOCKET(event->sd);
        event->sd = -1;
    }
}

PMIX_CLASS_INSTANCE(prte_oob_tcp_listener_t, pmix_list_item_t, tcp_ev_cons, tcp_ev_des);

PMIX_CLASS_INSTANCE(prte_oob_tcp_pending_connection_t, pmix_object_t, NULL, NULL);
