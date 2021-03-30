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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include <ctype.h>

#include "src/class/prte_list.h"
#include "src/include/prte_socket_errno.h"
#include "src/util/error.h"
#include "src/util/fd.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "src/util/listener.h"

static void *listen_thread_fn(prte_object_t *obj);
static prte_list_t mylisteners;
static bool initialized = false;
static prte_thread_t listen_thread;
static volatile bool listen_thread_active = false;
static struct timeval listen_thread_tv;
static int stop_thread[2];

#define CLOSE_THE_SOCKET(socket) \
    do {                         \
        shutdown(socket, 2);     \
        close(socket);           \
        socket = -1;             \
    } while (0)

int prte_register_listener(struct sockaddr *address, prte_socklen_t addrlen,
                           prte_event_base_t *evbase, prte_listener_callback_fn_t handler)
{
    prte_listener_t *conn;
    int flags;
    int sd = -1;

    if (!initialized) {
        PRTE_CONSTRUCT(&mylisteners, prte_list_t);
        PRTE_CONSTRUCT(&listen_thread, prte_thread_t);
        if (0 > pipe(stop_thread)) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* Make sure the pipe FDs are set to close-on-exec so that
           they don't leak into children */
        if (prte_fd_set_cloexec(stop_thread[0]) != PRTE_SUCCESS
            || prte_fd_set_cloexec(stop_thread[1]) != PRTE_SUCCESS) {
            close(stop_thread[0]);
            close(stop_thread[1]);
            PRTE_ERROR_LOG(PRTE_ERR_IN_ERRNO);
            return PRTE_ERR_IN_ERRNO;
        }
        listen_thread_tv.tv_sec = 3600;
        listen_thread_tv.tv_usec = 0;
        initialized = true;
    }

    /* create a listen socket for incoming connection attempts */
    sd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        if (EAFNOSUPPORT != prte_socket_errno) {
            prte_output(0, "pmix_server_start_listening: socket() failed: %s (%d)",
                        strerror(prte_socket_errno), prte_socket_errno);
        }
        return PRTE_ERR_IN_ERRNO;
    }
    /* Set the socket to close-on-exec so that no children inherit
       this FD */
    if (prte_fd_set_cloexec(sd) != PRTE_SUCCESS) {
        prte_output(0,
                    "pmix_server: unable to set the "
                    "listening socket to CLOEXEC (%s:%d)\n",
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    if (bind(sd, (struct sockaddr *) address, addrlen) < 0) {
        prte_output(0, "%s bind() failed on error %s (%d)", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    strerror(prte_socket_errno), prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        prte_output(0, "prte_listener: listen() failed: %s (%d)", strerror(prte_socket_errno),
                    prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        prte_output(0, "prte_listener: fcntl(F_GETFL) failed: %s (%d)", strerror(prte_socket_errno),
                    prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sd, F_SETFL, flags) < 0) {
        prte_output(0, "prte_listener: fcntl(F_SETFL) failed: %s (%d)", strerror(prte_socket_errno),
                    prte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRTE_ERROR;
    }

    /* add this port to our connections */
    conn = PRTE_NEW(prte_listener_t);
    conn->sd = sd;
    conn->evbase = evbase;
    conn->handler = handler;
    prte_list_append(&mylisteners, &conn->item);

    return PRTE_SUCCESS;
}

/*
 * Component initialization - create a module for each available
 * TCP interface and initialize the static resources associated
 * with that module.
 *
 * Also initializes the list of devices that will be used/supported by
 * the module, using the if_include and if_exclude variables.  This is
 * the only place that this sorting should occur -- all other places
 * should use the tcp_avaiable_devices list.  This is a change from
 * previous versions of this component.
 */
int prte_start_listening(void)
{
    int rc;

    /* if we aren't initialized, or have nothing
     * registered, or are already listening, then return SUCCESS */
    if (!initialized || 0 == prte_list_get_size(&mylisteners) || listen_thread_active) {
        return PRTE_SUCCESS;
    }

    /* start our listener thread */
    listen_thread_active = true;
    listen_thread.t_run = listen_thread_fn;
    listen_thread.t_arg = NULL;
    if (PRTE_SUCCESS != (rc = prte_thread_start(&listen_thread))) {
        PRTE_ERROR_LOG(rc);
        prte_output(0, "%s Unable to start listen thread", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
    }
    return rc;
}

void prte_stop_listening(void)
{
    int i = 0;

    if (!listen_thread_active) {
        return;
    }

    listen_thread_active = false;
    /* tell the thread to exit */
    if (-1 == write(stop_thread[1], &i, sizeof(int))) {
        return;
    }
    prte_thread_join(&listen_thread, NULL);
    PRTE_DESTRUCT(&listen_thread);
    PRTE_LIST_DESTRUCT(&mylisteners);
}

/*
 * The listen thread accepts incoming connections and places them
 * in a queue for further processing
 *
 * Runs until prte_listener_shutdown is set to true.
 */
static void *listen_thread_fn(prte_object_t *obj)
{
    int rc, max, accepted_connections, sd;
    prte_socklen_t addrlen = sizeof(struct sockaddr_storage);
    prte_pending_connection_t *pending_connection;
    struct timeval timeout;
    fd_set readfds;
    prte_listener_t *listener;

    while (listen_thread_active) {
        FD_ZERO(&readfds);
        max = -1;
        PRTE_LIST_FOREACH(listener, &mylisteners, prte_listener_t)
        {
            FD_SET(listener->sd, &readfds);
            max = (listener->sd > max) ? listener->sd : max;
        }
        /* add the stop_thread fd */
        FD_SET(stop_thread[0], &readfds);
        max = (stop_thread[0] > max) ? stop_thread[0] : max;

        /* set timeout interval */
        timeout.tv_sec = listen_thread_tv.tv_sec;
        timeout.tv_usec = listen_thread_tv.tv_usec;

        /* Block in a select to avoid hammering the cpu.  If a connection
         * comes in, we'll get woken up right away.
         */
        rc = select(max + 1, &readfds, NULL, NULL, &timeout);
        if (!listen_thread_active) {
            /* we've been asked to terminate */
            goto done;
        }
        if (rc < 0) {
            if (EAGAIN != prte_socket_errno && EINTR != prte_socket_errno) {
                perror("select");
            }
            continue;
        }

        /* Spin accepting connections until all active listen sockets
         * do not have any incoming connections, pushing each connection
         * onto its respective event queue for processing
         */
        do {
            accepted_connections = 0;
            PRTE_LIST_FOREACH(listener, &mylisteners, prte_listener_t)
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
                pending_connection = PRTE_NEW(prte_pending_connection_t);
                prte_event_set(listener->evbase, &pending_connection->ev, -1, PRTE_EV_WRITE,
                               listener->handler, pending_connection);
                prte_event_set_priority(&pending_connection->ev, PRTE_MSG_PRI);
                pending_connection->fd = accept(sd, (struct sockaddr *) &(pending_connection->addr),
                                                &addrlen);
                if (pending_connection->fd < 0) {
                    PRTE_RELEASE(pending_connection);

                    /* Non-fatal errors */
                    if (EAGAIN == prte_socket_errno || EWOULDBLOCK == prte_socket_errno) {
                        continue;
                    }

                    /* If we run out of file descriptors, log an extra
                       warning (so that the user can know to fix this
                       problem) and abandon all hope. */
                    else if (EMFILE == prte_socket_errno) {
                        CLOSE_THE_SOCKET(sd);
                        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_SOCKETS);
                        prte_show_help("help-oob-tcp.txt", "accept failed", true,
                                       prte_process_info.nodename, prte_socket_errno,
                                       strerror(prte_socket_errno), "Out of file descriptors");
                        goto done;
                    }

                    /* For all other cases, close the socket, print a
                       warning but try to continue */
                    else {
                        CLOSE_THE_SOCKET(sd);
                        prte_show_help("help-oob-tcp.txt", "accept failed", true,
                                       prte_process_info.nodename, prte_socket_errno,
                                       strerror(prte_socket_errno),
                                       "Unknown cause; job will try to continue");
                        continue;
                    }
                }

                /* activate the event */
                prte_event_active(&pending_connection->ev, PRTE_EV_WRITE, 1);
                accepted_connections++;
            }
        } while (accepted_connections > 0);
    }

done:
    close(stop_thread[0]);
    close(stop_thread[1]);
    return NULL;
}

/* INSTANTIATE CLASSES */
static void lcons(prte_listener_t *p)
{
    p->sd = -1;
    p->evbase = NULL;
    p->handler = NULL;
}
static void ldes(prte_listener_t *p)
{
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
}
PRTE_CLASS_INSTANCE(prte_listener_t, prte_list_item_t, lcons, ldes);

PRTE_CLASS_INSTANCE(prte_pending_connection_t, prte_object_t, NULL, NULL);
