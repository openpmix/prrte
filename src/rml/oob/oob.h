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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
    uint32_t addr_count;             /**< total number of addresses */
    int num_links;                   /**< number of logical links per physical device */
    int max_retries;                 /**< max number of retries before declaring peer gone */
    int max_uri_length;
    pmix_list_t events;              /**< events for monitoring connections */
    int peer_limit;                  /**< max size of tcp peer cache */
    pmix_list_t peers;               // connection addresses for peers

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
} prte_oob_base_t;
PRTE_EXPORT extern prte_oob_base_t prte_oob_base;

/* MCA framework */
PRTE_EXPORT int prte_oob_open(void);
PRTE_EXPORT void prte_oob_close(void);
PRTE_EXPORT int prte_oob_register(void);

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

/* All OOB sends are based on iovec's and are async as the RML
 * acts as the initial interface to prepare all communications.
 * The send_nb function will enter the message into the OOB
 * base, which will then check to see if a transport for the
 * intended target has already been assigned. If so, the message
 * is immediately placed into that module's event base for
 * transmission. If not, the function will loop across all available
 * components until one identifies that it has a module capable
 * of reaching the target.
 */
typedef void (*mca_oob_send_callback_fn_t)(int status, struct iovec *iov, int count, void *cbdata);

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

/* During initial wireup, we can only transfer contact info on the daemon
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
 *
 * Since all components define their address info at component start,
 * it is unchanged and does not require acess via event
 */
PRTE_EXPORT void prte_oob_base_get_addr(char **uri);

END_C_DECLS
#endif
