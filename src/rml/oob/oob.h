/*
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
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * the oob framework
 */

#ifndef _MCA_OOB_BASE_H_
#define _MCA_OOB_BASE_H_

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#    include <net/uio.h>
#endif

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/include/prte_stdatomic.h"
#include "src/util/pmix_printf.h"
#include "src/threads/pmix_threads.h"

#include "src/rml/rml_types.h"

BEGIN_C_DECLS

/*
 * Convenience Typedef
 */
typedef struct {
    int output;
    int max_retries;                 /**< max number of retries before declaring peer gone */
    int max_uri_length;
    pmix_list_t events;              /**< events for monitoring connections */
    int peer_limit;                  /**< max size of tcp peer cache */
    pmix_list_t peers;               // connection addresses for peers
    int max_msg_size;                // max size of an OOB msg (in MBytes)
    
    /* Port specifications */
    int tcp_sndbuf;   /**< socket send buffer size */
    int tcp_rcvbuf;   /**< socket recv buffer size */

    /* IPv4 support */
    bool disable_ipv4_family; /**< disable this AF */
    char **tcp_static_ports;  /**< Static ports - IPV4 */
    char **tcp_dyn_ports;     /**< Dynamic ports - IPV4 */
    char **ipv4conns;
    char **ipv4ports;

    /* IPv6 support */
    bool disable_ipv6_family; /**< disable this AF */
    char **tcp6_static_ports; /**< Static ports - IPV6 */
    char **tcp6_dyn_ports;    /**< Dynamic ports - IPV6 */
    char **ipv6conns;
    char **ipv6ports;

    /* connection support */
    pmix_list_t local_ifs; /**< prte list of local pmix_pif_t interfaces */
    char **if_masks;
    int num_hnp_ports;           /**< number of ports the HNP should listen on */
    pmix_list_t listeners;       /**< List of sockets being monitored by event or thread */
    pmix_thread_t listen_thread; /**< handle to the listening thread */
    prte_atomic_bool_t listen_thread_active;
    struct timeval listen_thread_tv; /**< Timeout when using listen thread */
    int stop_thread[2];              /**< pipe used to exit the listen thread */
    int keepalive_probes;   /**< number of keepalives that can be missed before declaring error */
    int keepalive_time;     /**< idle time in seconds before starting to send keepalives */
    int keepalive_intvl;    /**< time between keepalives, in seconds */
    int retry_delay;        /**< time to wait before retrying connection */
    int max_recon_attempts; /**< maximum number of times to attempt connect before giving up (-1 for
                               never) */
    int retry_max_delay;    /**< cap (sec) on the connection-retry delay; when > retry_delay the
                               delay backs off exponentially up to this value (0 => fixed delay) */
    int connect_max_time;   /**< max seconds to keep retrying a non-lifeline peer before giving up
                               and letting the routing tree heal to an ancestor (0 => forever) */
} prte_oob_base_t;
PRTE_EXPORT extern prte_oob_base_t prte_oob_base;

/* MCA framework */
PRTE_EXPORT int prte_oob_open(void);
PRTE_EXPORT void prte_oob_close(void);
PRTE_EXPORT int prte_oob_register(void);

/* Simulate this node's failure better than simply killing the process */
PRTE_EXPORT void prte_oob_simulate_node_failure(void);

/* Access the OOB internal functions via set of event-based macros
 * for inserting messages and other commands into the
 * OOB event base. This ensures that all OOB operations occur
 * asynchronously in a thread-safe environment.
 * Note that this doesn't mean that messages will be *sent*
 * in order as that depends on the specific transport being
 * used, when that module's event base indicates the transport
 * is available, etc.
 */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_rml_send_t *msg;
} prte_oob_send_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_oob_send_t);

/* All OOB sends are async: the RML prepares the message and hands it to
 * the OOB base via PRTE_OOB_SEND, which thread-shifts onto the event base
 * and calls prte_oob_base_send_nb. That routine resolves the next hop
 * toward the target (see prte_rml_get_route), looks up or creates the TCP
 * peer for that hop, and queues the message on it - opening the connection
 * first if one does not already exist.
 */
PRTE_EXPORT void prte_oob_base_send_nb(int fd, short args, void *cbdata);
#define PRTE_OOB_SEND(m)                                                                          \
    do {                                                                                          \
        prte_oob_send_t *prte_oob_send_cd;                                                        \
        pmix_output_verbose(1, prte_oob_base.output, "%s OOB_SEND: %s:%d",    \
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__);              \
        prte_oob_send_cd = PMIX_NEW(prte_oob_send_t);                                             \
        prte_oob_send_cd->msg = (m);                                                              \
        PRTE_PMIX_THREADSHIFT(prte_oob_send_cd, prte_event_base, prte_oob_base_send_nb);          \
    } while (0)

/* Build this process's contact URI: our name followed by the TCP
 * address(es) we are listening on, in a semicolon-separated string. During
 * initial wireup this can only be transferred on the daemon command line,
 * so the result is a compact string representation of our listening
 * endpoints.
 *
 * Note: since there is a limit to what an OS will allow on a cmd line, we
 * impose a limit on the length of the resulting uri via an MCA param. The
 * default value of -1 implies unlimited - however, users with large numbers
 * of interfaces on their nodes may wish to restrict the size.
 *
 * Our address info is fixed once the listeners start, so this needs no
 * event-base synchronization.
 */
PRTE_EXPORT void prte_oob_base_get_addr(char **uri);

END_C_DECLS
#endif
