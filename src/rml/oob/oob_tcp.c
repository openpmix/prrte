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
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
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
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
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

#include "src/include/prte_socket_errno.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/error.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_show_help.h"

#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_listener.h"
#include "src/rml/oob/oob_tcp_peer.h"
#include "src/rml/oob/oob_tcp_sendrecv.h"

prte_oob_base_t prte_oob_base = {
    .output = -1,
    .addr_count = 0,
    .num_links = 0,
    .max_retries = 0,
    .max_uri_length = -1,
    .events = PMIX_LIST_STATIC_INIT,
    .peer_limit = 0,
    .peers = PMIX_LIST_STATIC_INIT,

    .tcp_sndbuf = 0,
    .tcp_rcvbuf = 0,

    .disable_ipv4_family = false,
    .tcp_static_ports = NULL,
    .tcp_dyn_ports = NULL,
    .ipv4conns = NULL,
    .ipv4ports = NULL,

    .disable_ipv6_family = true,
    .tcp6_static_ports = NULL,
    .tcp6_dyn_ports = NULL,
    .ipv6conns = NULL,
    .ipv6ports = NULL,

    .local_ifs = PMIX_LIST_STATIC_INIT,
    .if_masks = NULL,
    .num_hnp_ports = 1,
    .listeners = PMIX_LIST_STATIC_INIT,
    .listen_thread_active = false,
    .listen_thread_tv = {3600, 0},
    .stop_thread = {-1, -1},
    .keepalive_probes = 0,
    .keepalive_time = 0,
    .keepalive_intvl = 0,
    .retry_delay = 0,
    .max_recon_attempts = 0
};

static void split_and_resolve(char **orig_str, char *name,
                              char ***interfaces);

int prte_oob_open(void)
{
    pmix_pif_t *copied_interface, *selected_interface;
    struct sockaddr_storage my_ss;
    /* Larger than necessary, used for copying mask */
    char string[50], **interfaces = NULL;
    int kindex;
    int i, rc;
    bool keeploopback = false;
    bool including = false;

    pmix_output_verbose(5, prte_oob_base.output,
                        "oob:tcp: component_available called");

     PMIX_CONSTRUCT(&prte_oob_base.listeners, pmix_list_t);
    if (PRTE_PROC_IS_MASTER) {
        PMIX_CONSTRUCT(&prte_oob_base.listen_thread, pmix_thread_t);
        prte_oob_base.listen_thread_active = false;
        prte_oob_base.listen_thread_tv.tv_sec = 3600;
        prte_oob_base.listen_thread_tv.tv_usec = 0;
    }
    prte_oob_base.addr_count = 0;
    prte_oob_base.ipv4conns = NULL;
    prte_oob_base.ipv4ports = NULL;
    prte_oob_base.ipv6conns = NULL;
    prte_oob_base.ipv6ports = NULL;
    prte_oob_base.if_masks = NULL;

    PMIX_CONSTRUCT(&prte_oob_base.local_ifs, pmix_list_t);
        PMIX_CONSTRUCT(&prte_oob_base.peers, pmix_list_t);

   /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != prte_if_include) {
        split_and_resolve(&prte_if_include,
                          "include", &interfaces);
        including = true;
    } else if (NULL != prte_if_exclude) {
        split_and_resolve(&prte_if_exclude,
                          "exclude", &interfaces);
    }

    /* if we are the master, then check the interfaces for loopbacks
     * and keep loopbacks only if no non-loopback interface exists */
    if (PRTE_PROC_IS_MASTER) {
        keeploopback = true;
        PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t)
        {
            if (!(selected_interface->if_flags & IFF_LOOPBACK)) {
                keeploopback = false;
                break;
            }
        }
    }

    /* look at all available interfaces */
    PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t)
    {
        if ((selected_interface->if_flags & IFF_LOOPBACK) &&
            !keeploopback) {
            continue;
        }


        i = selected_interface->if_index;
        kindex = selected_interface->if_kernel_index;
        memcpy((struct sockaddr *) &my_ss, &selected_interface->if_addr,
               MIN(sizeof(struct sockaddr_storage), sizeof(selected_interface->if_addr)));

        /* ignore non-ip4/6 interfaces */
        if (AF_INET != my_ss.ss_family
#if PRTE_ENABLE_IPV6
            && AF_INET6 != my_ss.ss_family
#endif
            ) {
            continue;
        }

        /* ignore any virtual interfaces */
        if (0 == strncmp(selected_interface->if_name, "vir", 3)) {
            continue;
        }

        /* handle include/exclude directives */
        if (NULL != interfaces) {
            /* check for match */
            rc = pmix_ifmatches(kindex, interfaces);
            /* if one of the network specifications isn't parseable, then
             * error out as we can't do what was requested
             */
            if (PRTE_ERR_NETWORK_NOT_PARSEABLE == rc) {
                pmix_show_help("help-oob-tcp.txt", "not-parseable", true);
                PMIX_ARGV_FREE_COMPAT(interfaces);
                return PRTE_ERR_BAD_PARAM;
            }
            /* if we are including, then ignore this if not present */
            if (including) {
                if (PMIX_SUCCESS != rc) {
                    pmix_output_verbose(20, prte_oob_base.output,
                                        "%s oob:tcp:init rejecting interface %s (not in include list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    continue;
                }
            } else {
                /* we are excluding, so ignore if present */
                if (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(20, prte_oob_base.output,
                                        "%s oob:tcp:init rejecting interface %s (in exclude list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    continue;
                }
            }
        }

        /* Refs ticket #3019
         * it would probably be worthwhile to print out a warning if PRRTE detects multiple
         * IP interfaces that are "up" on the same subnet (because that's a Bad Idea). Note
         * that we should only check for this after applying the relevant include/exclude
         * list MCA params. If we detect redundant ports, we can also automatically ignore
         * them so that applications won't hang.
         */

        /* add this address to our connections */
        if (AF_INET == my_ss.ss_family) {
            pmix_output_verbose(10, prte_oob_base.output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_oob_base.ipv4conns,
                                           pmix_net_get_hostname((struct sockaddr *) &my_ss));
        } else if (AF_INET6 == my_ss.ss_family) {
#if PRTE_ENABLE_IPV6
            pmix_output_verbose(10, prte_oob_base.output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_oob_base.ipv6conns,
                                           pmix_net_get_hostname((struct sockaddr *) &my_ss));
#endif // PRTE_ENABLE_IPV6
        } else {
            pmix_output_verbose(10, prte_oob_base.output,
                                "%s oob:tcp:init ignoring %s from out list of connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss));
            continue;
        }
        copied_interface = PMIX_NEW(pmix_pif_t);
        if (NULL == copied_interface) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        pmix_string_copy(copied_interface->if_name, selected_interface->if_name, PMIX_IF_NAMESIZE);
        copied_interface->if_index = i;
        copied_interface->if_kernel_index = kindex;
        copied_interface->af_family = my_ss.ss_family;
        copied_interface->if_flags = selected_interface->if_flags;
        copied_interface->if_speed = selected_interface->if_speed;
        memcpy(&copied_interface->if_addr, &selected_interface->if_addr,
               sizeof(struct sockaddr_storage));
        copied_interface->if_mask = selected_interface->if_mask;
        /* If bandwidth is not found, set to arbitrary non zero value */
        copied_interface->if_bandwidth = selected_interface->if_bandwidth > 0
                                             ? selected_interface->if_bandwidth
                                             : 1;
        memcpy(&copied_interface->if_mac, &selected_interface->if_mac,
               sizeof(copied_interface->if_mac));
        copied_interface->ifmtu = selected_interface->ifmtu;
        /* Add the if_mask to the list */
        snprintf(string, 50, "%d", selected_interface->if_mask);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_oob_base.if_masks, string);
        pmix_list_append(&prte_oob_base.local_ifs, &(copied_interface->super));
    }
    if (NULL != interfaces) {
        PMIX_ARGV_FREE_COMPAT(interfaces);
    }

    if (0 == PMIX_ARGV_COUNT_COMPAT(prte_oob_base.ipv4conns)
#if PRTE_ENABLE_IPV6
        && 0 == PMIX_ARGV_COUNT_COMPAT(prte_oob_base.ipv6conns)
#endif
    ) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    // start the listeners
    if (PRTE_SUCCESS != (rc = prte_oob_tcp_start_listening())) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

void prte_oob_close(void)
{
    int i = 0, rc;

    if (PRTE_PROC_IS_MASTER && prte_oob_base.listen_thread_active) {
        prte_oob_base.listen_thread_active = false;
        /* tell the thread to exit */
        rc = write(prte_oob_base.stop_thread[1], &i, sizeof(int));
        if (0 < rc) {
            pmix_thread_join(&prte_oob_base.listen_thread, NULL);
        }

        close(prte_oob_base.stop_thread[0]);
        close(prte_oob_base.stop_thread[1]);

    }

    PMIX_LIST_DESTRUCT(&prte_oob_base.local_ifs);
    PMIX_LIST_DESTRUCT(&prte_oob_base.peers);

    if (NULL != prte_oob_base.ipv4conns) {
        PMIX_ARGV_FREE_COMPAT(prte_oob_base.ipv4conns);
    }
    if (NULL != prte_oob_base.ipv4ports) {
        PMIX_ARGV_FREE_COMPAT(prte_oob_base.ipv4ports);
    }

#if PRTE_ENABLE_IPV6
    if (NULL != prte_oob_base.ipv6conns) {
        PMIX_ARGV_FREE_COMPAT(prte_oob_base.ipv6conns);
    }
    if (NULL != prte_oob_base.ipv6ports) {
        PMIX_ARGV_FREE_COMPAT(prte_oob_base.ipv6ports);
    }
#endif
    if (NULL != prte_oob_base.if_masks) {
        PMIX_ARGV_FREE_COMPAT(prte_oob_base.if_masks);
    }

    if (0 <= prte_oob_base.output) {
        pmix_output_close(prte_oob_base.output);
    }
}

static char *static_port_string;
#if PRTE_ENABLE_IPV6
static char *static_port_string6;
#endif // PRTE_ENABLE_IPV6

static char *dyn_port_string;
#if PRTE_ENABLE_IPV6
static char *dyn_port_string6;
#endif

int prte_oob_register(void)
{
    prte_oob_base.peer_limit = -1;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "peer_limit",
                                        "Maximum number of peer connections to simultaneously maintain (-1 = infinite)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.peer_limit);

    prte_oob_base.max_retries = 2;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "peer_retries",
                                        "Number of times to try shutting down a connection before giving up",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.max_retries);

    prte_oob_base.tcp_sndbuf = 0;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "sndbuf",
                                        "TCP socket send buffering size (in bytes, 0 => leave system default)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.tcp_sndbuf);

    prte_oob_base.tcp_rcvbuf = 0;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "rcvbuf",
                                        "TCP socket receive buffering size (in bytes, 0 => leave system default)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.tcp_rcvbuf);


    static_port_string = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "static_ipv4_ports",
                                        "Static ports for daemons and procs (IPv4)",
                                        PMIX_MCA_BASE_VAR_TYPE_STRING,
                                        &static_port_string);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string) {
        pmix_util_parse_range_options(static_port_string, &prte_oob_base.tcp_static_ports);
        if (0 == strcmp(prte_oob_base.tcp_static_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_oob_base.tcp_static_ports);
            prte_oob_base.tcp_static_ports = NULL;
        }
    } else {
        prte_oob_base.tcp_static_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    static_port_string6 = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "static_ipv6_ports",
                                        "Static ports for daemons and procs (IPv6)",
                                        PMIX_MCA_BASE_VAR_TYPE_STRING,
                                        &static_port_string6);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string6) {
        pmix_util_parse_range_options(static_port_string6,
                                      &prte_oob_base.tcp6_static_ports);
        if (0 == strcmp(prte_oob_base.tcp6_static_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_oob_base.tcp6_static_ports);
            prte_oob_base.tcp6_static_ports = NULL;
        }
    } else {
        prte_oob_base.tcp6_static_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    if (NULL != prte_oob_base.tcp_static_ports
        || NULL != prte_oob_base.tcp6_static_ports) {
        prte_static_ports = true;
    }

    dyn_port_string = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "dynamic_ipv4_ports",
                                        "Range of ports to be dynamically used by daemons and procs (IPv4)",
                                        PMIX_MCA_BASE_VAR_TYPE_STRING,
                                        &dyn_port_string);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err = PMIX_ARGV_JOIN_COMPAT(prte_oob_base.tcp_static_ports, ',');
            pmix_show_help("help-oob-tcp.txt", "static-and-dynamic", true, err, dyn_port_string);
            free(err);
            return PRTE_ERROR;
        }
        pmix_util_parse_range_options(dyn_port_string, &prte_oob_base.tcp_dyn_ports);
        if (0 == strcmp(prte_oob_base.tcp_dyn_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_oob_base.tcp_dyn_ports);
            prte_oob_base.tcp_dyn_ports = NULL;
        }
    } else {
        prte_oob_base.tcp_dyn_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    dyn_port_string6 = NULL;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "dynamic_ipv6_ports",
                                        "Range of ports to be dynamically used by daemons and procs (IPv6)",
                                        PMIX_MCA_BASE_VAR_TYPE_STRING,
                                        &dyn_port_string6);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string6) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err4 = NULL, *err6 = NULL;
            if (NULL != prte_oob_base.tcp_static_ports) {
                err4 = PMIX_ARGV_JOIN_COMPAT(prte_oob_base.tcp_static_ports, ',');
            }
            if (NULL != prte_oob_base.tcp6_static_ports) {
                err6 = PMIX_ARGV_JOIN_COMPAT(prte_oob_base.tcp6_static_ports, ',');
            }
            pmix_show_help("help-oob-tcp.txt", "static-and-dynamic-ipv6", true,
                           (NULL == err4) ? "N/A" : err4, (NULL == err6) ? "N/A" : err6,
                           dyn_port_string6);
            if (NULL != err4) {
                free(err4);
            }
            if (NULL != err6) {
                free(err6);
            }
            return PRTE_ERROR;
        }
        pmix_util_parse_range_options(dyn_port_string6, &prte_oob_base.tcp6_dyn_ports);
        if (0 == strcmp(prte_oob_base.tcp6_dyn_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_oob_base.tcp6_dyn_ports);
            prte_oob_base.tcp6_dyn_ports = NULL;
        }
    } else {
        prte_oob_base.tcp6_dyn_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    prte_oob_base.disable_ipv4_family = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "disable_ipv4_family",
                                        "Disable the IPv4 interfaces",
                                        PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                        &prte_oob_base.disable_ipv4_family);

#if PRTE_ENABLE_IPV6
    prte_oob_base.disable_ipv6_family = false;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "disable_ipv6_family",
                                        "Disable the IPv6 interfaces",
                                        PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                        &prte_oob_base.disable_ipv6_family);
#endif // PRTE_ENABLE_IPV6

    // Wait for this amount of time before sending the first keepalive probe
    prte_oob_base.keepalive_time = 300;
    (void)pmix_mca_base_var_register("prte", "prte", NULL, "keepalive_time",
                                        "Idle time in seconds before starting to send keepalives (keepalive_time <= 0 disables "
                                        "keepalive functionality)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.keepalive_time);

    // Resend keepalive probe every INT seconds
    prte_oob_base.keepalive_intvl = 20;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "keepalive_intvl",
                                        "Time between successive keepalive pings when peer has not responded, in seconds (ignored "
                                        "if keepalive_time <= 0)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.keepalive_intvl);

    // After sending PR probes every INT seconds consider the connection dead
    prte_oob_base.keepalive_probes = 9;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "keepalive_probes",
                                        "Number of keepalives that can be missed before "
                                        "declaring error (ignored if keepalive_time <= 0)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.keepalive_probes);

    prte_oob_base.retry_delay = 0;
    (void) pmix_mca_base_var_register("prte","prte", NULL,  "retry_delay",
                                        "Time (in sec) to wait before trying to connect to peer again",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.retry_delay);

    prte_oob_base.max_recon_attempts = 10;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "max_recon_attempts",
                                        "Max number of times to attempt connection before giving up (-1 -> never give up)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.max_recon_attempts);
    prte_oob_base.max_msg_size = 100;
    (void) pmix_mca_base_var_register("prte", "prte", NULL, "max_msg_size",
                                        "Max size of an OOB message in Megabytes(default = 100)",
                                        PMIX_MCA_BASE_VAR_TYPE_INT,
                                        &prte_oob_base.max_recon_attempts);

    return PRTE_SUCCESS;
}

/*
 * Local utility functions
 */
static void recv_handler(int sd, short flags, void *user);

/* Called by prte_oob_tcp_accept() and connection_handler() on
 * a socket that has been accepted.  This call finishes processing the
 * socket, including setting socket options and registering for the
 * OOB-level connection handshake.  Used in both the threaded and
 * event listen modes.
 */
void prte_oob_accept_connection(const int accepted_fd, const struct sockaddr *addr)
{
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s accept_connection: %s:%d\n", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        pmix_net_get_hostname(addr), pmix_net_get_port(addr));

    /* setup socket options */
    prte_oob_tcp_set_socket_options(accepted_fd);

    /* use a one-time event to wait for receipt of peer's
     *  process ident message to complete this connection
     */
    PRTE_ACTIVATE_TCP_ACCEPT_STATE(accepted_fd, addr, recv_handler);
}

/* API functions */
void prte_oob_ping(const pmix_proc_t *proc)
{
    prte_oob_tcp_peer_t *peer;

    pmix_output_verbose(2, prte_oob_base.output,
                        "%s:[%s:%d] processing ping to peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        __FILE__, __LINE__, PRTE_NAME_PRINT(proc));

    /* do we know this peer? */
    if (NULL == (peer = prte_oob_tcp_peer_lookup(proc))) {
        /* push this back to the component so it can try
         * another module within this transport. If no
         * module can be found, the component can push back
         * to the framework so another component can try
         */
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s:[%s:%d] hop %s unknown", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            __FILE__, __LINE__, PRTE_NAME_PRINT(proc));
        PRTE_ACTIVATE_TCP_MSG_ERROR(NULL, NULL, proc, prte_mca_oob_tcp_component_hop_unknown);
        return;
    }

    /* if we are already connected, there is nothing to do */
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s:[%s:%d] already connected to peer %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__,
                            PRTE_NAME_PRINT(proc));
        return;
    }

    /* if we are already connecting, there is nothing to do */
    if (MCA_OOB_TCP_CONNECTING == peer->state || MCA_OOB_TCP_CONNECT_ACK == peer->state) {
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s:[%s:%d] already connecting to peer %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__,
                            PRTE_NAME_PRINT(proc));
        return;
    }

    /* attempt the connection */
    peer->state = MCA_OOB_TCP_CONNECTING;
    PRTE_ACTIVATE_TCP_CONN_STATE(peer, prte_oob_tcp_peer_try_connect);
}

/*
 * Event callback when there is data available on the registered
 * socket to recv.  This is called for the listen sockets to accept an
 * incoming connection, on new sockets trying to complete the software
 * connection process, and for probes.  Data on an established
 * connection is handled elsewhere.
 */
static void recv_handler(int sd, short flg, void *cbdata)
{
    prte_oob_tcp_conn_op_t *op = (prte_oob_tcp_conn_op_t *) cbdata;
    int flags;
    prte_oob_tcp_hdr_t hdr;
    prte_oob_tcp_peer_t *peer;
    PRTE_HIDE_UNUSED_PARAMS(flg);

    PMIX_ACQUIRE_OBJECT(op);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s:tcp:recv:handler called", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* get the handshake */
    if (PRTE_SUCCESS != prte_oob_tcp_peer_recv_connect_ack(NULL, sd, &hdr)) {
        goto cleanup;
    }

    /* finish processing ident */
    if (MCA_OOB_TCP_IDENT == hdr.type) {
        if (NULL == (peer = prte_oob_tcp_peer_lookup(&hdr.origin))) {
            /* should never happen */
            goto cleanup;
        }
        /* set socket up to be non-blocking */
        if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
            pmix_output(0, "%s prte_oob_tcp_recv_connect: fcntl(F_GETFL) failed: %s (%d)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerror(prte_socket_errno),
                        prte_socket_errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(sd, F_SETFL, flags) < 0) {
                pmix_output(0, "%s prte_oob_tcp_recv_connect: fcntl(F_SETFL) failed: %s (%d)",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerror(prte_socket_errno),
                            prte_socket_errno);
            }
        }
        /* is the peer instance willing to accept this connection */
        peer->sd = sd;
        if (prte_oob_tcp_peer_accept(peer) == false) {
            if (OOB_TCP_DEBUG_CONNECT
                <= pmix_output_get_verbosity(prte_oob_base.output)) {
                pmix_output(0,
                            "%s-%s prte_oob_tcp_recv_connect: "
                            "rejected connection from %s connection state %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            PRTE_NAME_PRINT(&(hdr.origin)), peer->state);
            }
            CLOSE_THE_SOCKET(sd);
        }
    }

cleanup:
    PMIX_RELEASE(op);
}

/*
 * Go through a list of argv; if there are any subnet specifications
 * (a.b.c.d/e), resolve them to an interface name (Currently only
 * supporting IPv4).  If unresolvable, warn and remove.
 */
static void split_and_resolve(char **orig_str, char *name,
                              char ***interfaces)
{
    pmix_pif_t *selected_interface;
    int i, n, ret, match_count;
    bool found;
    char **argv, *str, *tmp;
    char if_name[IF_NAMESIZE];
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;

    /* Sanity check */
    if (NULL == orig_str || NULL == *orig_str) {
        return;
    }

    argv = PMIX_ARGV_SPLIT_COMPAT(*orig_str, ',');
    if (NULL == argv) {
        return;
    }
    for (i = 0; NULL != argv[i]; ++i) {
        if (isalpha(argv[i][0])) {
            /* This is an interface name. If not already in the interfaces array, add it */
            found = false;
            if (NULL != interfaces) {
                for (n = 0; NULL != interfaces[n]; n++) {
                    if (0 == strcmp(argv[i], *interfaces[n])) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                pmix_output_verbose(20,
                                    prte_oob_base.output,
                                    "oob:tcp: Using interface: %s ", argv[i]);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(interfaces, argv[i]);
            }
            continue;
        }

        /* Found a subnet notation.  Convert it to an IP
           address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        tmp = strdup(argv[i]);
        str = strchr(argv[i], '/');
        if (NULL == str) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename,
                           tmp, "Invalid specification (missing \"/\")");
            free(argv[i]);
            free(tmp);
            continue;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr*) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i],
                        &((struct sockaddr_in*) &argv_inaddr)->sin_addr);
        free(argv[i]);

        if (1 != ret) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename, tmp,
                           "Invalid specification (inet_pton() failed)");
            free(tmp);
            continue;
        }
        pmix_output_verbose(20, prte_oob_base.output,
                            "%s oob:tcp: Searching for %s address+prefix: %s / %u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            name,
                            pmix_net_get_hostname((struct sockaddr*) &argv_inaddr),
                            argv_prefix);

        /* Go through all interfaces and see if we can find a match */
        match_count = 0;
        PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t) {
            ret = pmix_ifkindextoaddr(selected_interface->if_kernel_index,
                                     (struct sockaddr*) &if_inaddr,
                                     sizeof(if_inaddr));
            if (PMIX_SUCCESS == ret &&
                pmix_net_samenetwork((struct sockaddr_storage*) &argv_inaddr,
                                     (struct sockaddr_storage*) &if_inaddr,
                                     argv_prefix)) {
                /* We found a match. If it's not already in the interfaces array,
                   add it. If it's already in the array, treat it as a match */
                match_count = match_count + 1;
                pmix_ifkindextoname(selected_interface->if_kernel_index, if_name, sizeof(if_name));
                found = false;
                if (NULL != interfaces) {
                    for (n = 0; NULL != interfaces[n]; n++) {
                        if (0 == strcmp(if_name, *interfaces[n])) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    pmix_output_verbose(20,
                                        prte_oob_base.output,
                                        "oob:tcp: Found match: %s (%s)",
                                        pmix_net_get_hostname((struct sockaddr*) &if_inaddr),
                                        if_name);
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(interfaces, if_name);
                }
            }
        }
        /* If we didn't find a match, keep trying */
        if (0 == match_count) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename, tmp,
                           "Did not find interface matching this subnet");
            free(tmp);
            continue;
        }

        free(tmp);
    }

    // cleanup and construct output string
    free(argv);
    free(*orig_str);
    if (NULL != interfaces) {
        *orig_str = PMIX_ARGV_JOIN_COMPAT(*interfaces, ',');
    } else {
        *orig_str = NULL;
    }
    return;
}

PMIX_CLASS_INSTANCE(prte_oob_send_t,
                    pmix_object_t,
                    NULL, NULL);
