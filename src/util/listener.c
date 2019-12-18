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
 * Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
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

#include "prrte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include <ctype.h>

#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/prrte_socket_errno.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/fd.h"
#include "src/class/prrte_list.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"

#include "src/util/listener.h"

static void* listen_thread_fn(prrte_object_t *obj);
static prrte_list_t mylisteners;
static bool initialized = false;
static prrte_thread_t listen_thread;
static volatile bool listen_thread_active = false;
static struct timeval listen_thread_tv;
static int stop_thread[2];

#define CLOSE_THE_SOCKET(socket)    \
    do {                            \
        shutdown(socket, 2);        \
        close(socket);              \
        socket = -1;                \
    } while(0)


int prrte_register_listener(struct sockaddr* address, prrte_socklen_t addrlen,
                           prrte_event_base_t *evbase,
                           prrte_listener_callback_fn_t handler)
{
    prrte_listener_t *conn;
    int flags;
    int sd = -1;

    if (!initialized) {
        PRRTE_CONSTRUCT(&mylisteners, prrte_list_t);
        PRRTE_CONSTRUCT(&listen_thread, prrte_thread_t);
        if (0 > pipe(stop_thread)) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* Make sure the pipe FDs are set to close-on-exec so that
           they don't leak into children */
        if (prrte_fd_set_cloexec(stop_thread[0]) != PRRTE_SUCCESS ||
            prrte_fd_set_cloexec(stop_thread[1]) != PRRTE_SUCCESS) {
            close(stop_thread[0]);
            close(stop_thread[1]);
            PRRTE_ERROR_LOG(PRRTE_ERR_IN_ERRNO);
            return PRRTE_ERR_IN_ERRNO;
        }
        listen_thread_tv.tv_sec = 3600;
        listen_thread_tv.tv_usec = 0;
        initialized = true;
    }

    /* create a listen socket for incoming connection attempts */
    sd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        if (EAFNOSUPPORT != prrte_socket_errno) {
            prrte_output(0,"pmix_server_start_listening: socket() failed: %s (%d)",
                        strerror(prrte_socket_errno), prrte_socket_errno);
        }
        return PRRTE_ERR_IN_ERRNO;
    }
    /* Set the socket to close-on-exec so that no children inherit
       this FD */
    if (prrte_fd_set_cloexec(sd) != PRRTE_SUCCESS) {
        prrte_output(0, "pmix_server: unable to set the "
                    "listening socket to CLOEXEC (%s:%d)\n",
                    strerror(prrte_socket_errno), prrte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRRTE_ERROR;
    }


    if (bind(sd, (struct sockaddr*)address, addrlen) < 0) {
        prrte_output(0, "%s bind() failed on error %s (%d)",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    strerror(prrte_socket_errno),
                    prrte_socket_errno );
        CLOSE_THE_SOCKET(sd);
        return PRRTE_ERROR;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        prrte_output(0, "prrte_listener: listen() failed: %s (%d)",
                    strerror(prrte_socket_errno), prrte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRRTE_ERROR;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        prrte_output(0, "prrte_listener: fcntl(F_GETFL) failed: %s (%d)",
                    strerror(prrte_socket_errno), prrte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRRTE_ERROR;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sd, F_SETFL, flags) < 0) {
        prrte_output(0, "prrte_listener: fcntl(F_SETFL) failed: %s (%d)",
                    strerror(prrte_socket_errno), prrte_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return PRRTE_ERROR;
    }

    /* add this port to our connections */
    conn = PRRTE_NEW(prrte_listener_t);
    conn->sd = sd;
    conn->evbase = evbase;
    conn->handler = handler;
    prrte_list_append(&mylisteners, &conn->item);

    return PRRTE_SUCCESS;
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
int prrte_start_listening(void)
{
    int rc;

    /* if we aren't initialized, or have nothing
     * registered, or are already listening, then return SUCCESS */
    if (!initialized || 0 == prrte_list_get_size(&mylisteners) ||
        listen_thread_active) {
        return PRRTE_SUCCESS;
    }

    /* start our listener thread */
    listen_thread_active = true;
    listen_thread.t_run = listen_thread_fn;
    listen_thread.t_arg = NULL;
    if (PRRTE_SUCCESS != (rc = prrte_thread_start(&listen_thread))) {
        PRRTE_ERROR_LOG(rc);
        prrte_output(0, "%s Unable to start listen thread", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    }
    return rc;
}

void prrte_stop_listening(void)
{
    int i=0;

    if (!listen_thread_active) {
        return;
    }

    listen_thread_active = false;
    /* tell the thread to exit */
    write(stop_thread[1], &i, sizeof(int));
    prrte_thread_join(&listen_thread, NULL);
    PRRTE_DESTRUCT(&listen_thread);
    PRRTE_LIST_DESTRUCT(&mylisteners);
}

/*
 * The listen thread accepts incoming connections and places them
 * in a queue for further processing
 *
 * Runs until prrte_listener_shutdown is set to true.
 */
static void* listen_thread_fn(prrte_object_t *obj)
{
    int rc, max, accepted_connections, sd;
    prrte_socklen_t addrlen = sizeof(struct sockaddr_storage);
    prrte_pending_connection_t *pending_connection;
    struct timeval timeout;
    fd_set readfds;
    prrte_listener_t *listener;

    while (listen_thread_active) {
        FD_ZERO(&readfds);
        max = -1;
        PRRTE_LIST_FOREACH(listener, &mylisteners, prrte_listener_t) {
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
            if (EAGAIN != prrte_socket_errno && EINTR != prrte_socket_errno) {
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
            PRRTE_LIST_FOREACH(listener, &mylisteners, prrte_listener_t) {
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
                pending_connection = PRRTE_NEW(prrte_pending_connection_t);
                prrte_event_set(listener->evbase, &pending_connection->ev, -1,
                               PRRTE_EV_WRITE, listener->handler, pending_connection);
                prrte_event_set_priority(&pending_connection->ev, PRRTE_MSG_PRI);
                pending_connection->fd = accept(sd,
                                                (struct sockaddr*)&(pending_connection->addr),
                                                &addrlen);
                if (pending_connection->fd < 0) {
                    PRRTE_RELEASE(pending_connection);

                    /* Non-fatal errors */
                    if (EAGAIN == prrte_socket_errno ||
                        EWOULDBLOCK == prrte_socket_errno) {
                        continue;
                    }

                    /* If we run out of file descriptors, log an extra
                       warning (so that the user can know to fix this
                       problem) and abandon all hope. */
                    else if (EMFILE == prrte_socket_errno) {
                        CLOSE_THE_SOCKET(sd);
                        PRRTE_ERROR_LOG(PRRTE_ERR_SYS_LIMITS_SOCKETS);
                        prrte_show_help("help-oob-tcp.txt",
                                       "accept failed",
                                       true,
                                       prrte_process_info.nodename,
                                       prrte_socket_errno,
                                       strerror(prrte_socket_errno),
                                       "Out of file descriptors");
                        goto done;
                    }

                    /* For all other cases, close the socket, print a
                       warning but try to continue */
                    else {
                        CLOSE_THE_SOCKET(sd);
                        prrte_show_help("help-oob-tcp.txt",
                                       "accept failed",
                                       true,
                                       prrte_process_info.nodename,
                                       prrte_socket_errno,
                                       strerror(prrte_socket_errno),
                                       "Unknown cause; job will try to continue");
                        continue;
                    }
                }

                /* activate the event */
                prrte_event_active(&pending_connection->ev, PRRTE_EV_WRITE, 1);
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
static void lcons(prrte_listener_t *p)
{
    p->sd = -1;
    p->evbase = NULL;
    p->handler = NULL;
}
static void ldes(prrte_listener_t *p)
{
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
}
PRRTE_CLASS_INSTANCE(prrte_listener_t,
                   prrte_list_item_t,
                   lcons, ldes);

PRRTE_CLASS_INSTANCE(prrte_pending_connection_t,
                   prrte_object_t,
                   NULL,
                   NULL);
