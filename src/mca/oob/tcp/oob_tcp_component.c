/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      NVIDIA Corporation.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
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
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/socket.h>

#include "src/class/prte_list.h"
#include "src/event/event-internal.h"
#include "src/include/prte_socket_errno.h"
#include "src/mca/prteif/prteif.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/attr.h"
#include "src/util/name_fns.h"
#include "src/util/parse_options.h"
#include "src/util/show_help.h"

#include "oob_tcp_peer.h"
#include "src/mca/oob/tcp/oob_tcp.h"
#include "src/mca/oob/tcp/oob_tcp_common.h"
#include "src/mca/oob/tcp/oob_tcp_component.h"
#include "src/mca/oob/tcp/oob_tcp_connection.h"
#include "src/mca/oob/tcp/oob_tcp_listener.h"
#include "src/mca/oob/tcp/oob_tcp_peer.h"

/*
 * Local utility functions
 */

static int tcp_component_register(void);
static int tcp_component_open(void);
static int tcp_component_close(void);

static int component_available(void);
static int component_startup(void);
static void component_shutdown(void);
static int component_send(prte_rml_send_t *msg);
static char *component_get_addr(void);
static int component_set_addr(pmix_proc_t *peer, char **uris);
static bool component_is_reachable(pmix_proc_t *peer);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prte_oob_tcp_component_t prte_oob_tcp_component = {
    {
        .oob_base = {
            PRTE_OOB_BASE_VERSION_2_0_0,
            .mca_component_name = "tcp",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),
            .mca_open_component = tcp_component_open,
            .mca_close_component = tcp_component_close,
            .mca_register_component_params = tcp_component_register,
        },
        .oob_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
        .priority = 30, // default priority of this transport
        .available = component_available,
        .startup = component_startup,
        .shutdown = component_shutdown,
        .send_nb = component_send,
        .get_addr = component_get_addr,
        .set_addr = component_set_addr,
        .is_reachable = component_is_reachable,
    },
};

/*
 * Initialize global variables used w/in this module.
 */
static int tcp_component_open(void)
{
    PRTE_CONSTRUCT(&prte_oob_tcp_component.peers, prte_list_t);
    PRTE_CONSTRUCT(&prte_oob_tcp_component.listeners, prte_list_t);
    if (PRTE_PROC_IS_MASTER) {
        PRTE_CONSTRUCT(&prte_oob_tcp_component.listen_thread, prte_thread_t);
        prte_oob_tcp_component.listen_thread_active = false;
        prte_oob_tcp_component.listen_thread_tv.tv_sec = 3600;
        prte_oob_tcp_component.listen_thread_tv.tv_usec = 0;
    }
    prte_oob_tcp_component.addr_count = 0;
    prte_oob_tcp_component.ipv4conns = NULL;
    prte_oob_tcp_component.ipv4ports = NULL;
    prte_oob_tcp_component.ipv6conns = NULL;
    prte_oob_tcp_component.ipv6ports = NULL;
    prte_oob_tcp_component.if_masks = NULL;

    /* if_include and if_exclude need to be mutually exclusive */
    if (PRTE_SUCCESS
        != prte_mca_base_var_check_exclusive(
            "prte", prte_oob_tcp_component.super.oob_base.mca_type_name,
            prte_oob_tcp_component.super.oob_base.mca_component_name, "if_include",
            prte_oob_tcp_component.super.oob_base.mca_type_name,
            prte_oob_tcp_component.super.oob_base.mca_component_name, "if_exclude")) {
        /* Return ERR_NOT_AVAILABLE so that a warning message about
           "open" failing is not printed */
        return PRTE_ERR_NOT_AVAILABLE;
    }
    PRTE_CONSTRUCT(&prte_oob_tcp_component.local_ifs, prte_list_t);
    return PRTE_SUCCESS;
}

/*
 * Cleanup of global variables used by this module.
 */
static int tcp_component_close(void)
{
    PRTE_LIST_DESTRUCT(&prte_oob_tcp_component.local_ifs);
    PRTE_LIST_DESTRUCT(&prte_oob_tcp_component.peers);

    if (NULL != prte_oob_tcp_component.ipv4conns) {
        prte_argv_free(prte_oob_tcp_component.ipv4conns);
    }
    if (NULL != prte_oob_tcp_component.ipv4ports) {
        prte_argv_free(prte_oob_tcp_component.ipv4ports);
    }

#if PRTE_ENABLE_IPV6
    if (NULL != prte_oob_tcp_component.ipv6conns) {
        prte_argv_free(prte_oob_tcp_component.ipv6conns);
    }
    if (NULL != prte_oob_tcp_component.ipv6ports) {
        prte_argv_free(prte_oob_tcp_component.ipv6ports);
    }
#endif
    if (NULL != prte_oob_tcp_component.if_masks) {
        prte_argv_free(prte_oob_tcp_component.if_masks);
    }

    return PRTE_SUCCESS;
}
static char *static_port_string;
#if PRTE_ENABLE_IPV6
static char *static_port_string6;
#endif // PRTE_ENABLE_IPV6

static char *dyn_port_string;
#if PRTE_ENABLE_IPV6
static char *dyn_port_string6;
#endif

static int tcp_component_register(void)
{
    prte_mca_base_component_t *component = &prte_oob_tcp_component.super.oob_base;
    int var_id;

    /* register oob module parameters */
    prte_oob_tcp_component.peer_limit = -1;
    (void) prte_mca_base_component_var_register(
        component, "peer_limit",
        "Maximum number of peer connections to simultaneously maintain (-1 = infinite)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.peer_limit);

    prte_oob_tcp_component.max_retries = 2;
    (void) prte_mca_base_component_var_register(
        component, "peer_retries",
        "Number of times to try shutting down a connection before giving up",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.max_retries);

    prte_oob_tcp_component.tcp_sndbuf = 0;
    (void) prte_mca_base_component_var_register(
        component, "sndbuf", "TCP socket send buffering size (in bytes, 0 => leave system default)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.tcp_sndbuf);

    prte_oob_tcp_component.tcp_rcvbuf = 0;
    (void) prte_mca_base_component_var_register(
        component, "rcvbuf",
        "TCP socket receive buffering size (in bytes, 0 => leave system default)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.tcp_rcvbuf);

    prte_oob_tcp_component.if_include = NULL;
    var_id = prte_mca_base_component_var_register(
        component, "if_include",
        "Comma-delimited list of devices and/or CIDR notation of TCP networks to use for PRTE "
        "bootstrap communication (e.g., \"eth0,192.168.0.0/16\").  Mutually exclusive with "
        "oob_tcp_if_exclude.",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_2,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.if_include);
    (void) prte_mca_base_var_register_synonym(var_id, "prte", "oob", "tcp", "include",
                                              PRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED
                                                  | PRTE_MCA_BASE_VAR_SYN_FLAG_INTERNAL);

    prte_oob_tcp_component.if_exclude = NULL;
    var_id = prte_mca_base_component_var_register(
        component, "if_exclude",
        "Comma-delimited list of devices and/or CIDR notation of TCP networks to NOT use for PRTE "
        "bootstrap communication -- all devices not matching these specifications will be used "
        "(e.g., \"eth0,192.168.0.0/16\").  If set to a non-default value, it is mutually exclusive "
        "with oob_tcp_if_include.",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_2,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_oob_tcp_component.if_exclude);
    (void) prte_mca_base_var_register_synonym(var_id, "prte", "oob", "tcp", "exclude",
                                              PRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED
                                                  | PRTE_MCA_BASE_VAR_SYN_FLAG_INTERNAL);

    /* if_include and if_exclude need to be mutually exclusive */
    if (NULL != prte_oob_tcp_component.if_include && NULL != prte_oob_tcp_component.if_exclude) {
        /* Return ERR_NOT_AVAILABLE so that a warning message about
           "open" failing is not printed */
        prte_show_help("help-oob-tcp.txt", "include-exclude", true,
                       prte_oob_tcp_component.if_include, prte_oob_tcp_component.if_exclude);
        return PRTE_ERR_NOT_AVAILABLE;
    }

    static_port_string = NULL;
    (void) prte_mca_base_component_var_register(component, "static_ipv4_ports",
                                                "Static ports for daemons and procs (IPv4)",
                                                PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_2,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &static_port_string);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string) {
        prte_util_parse_range_options(static_port_string, &prte_oob_tcp_component.tcp_static_ports);
        if (0 == strcmp(prte_oob_tcp_component.tcp_static_ports[0], "-1")) {
            prte_argv_free(prte_oob_tcp_component.tcp_static_ports);
            prte_oob_tcp_component.tcp_static_ports = NULL;
        }
    } else {
        prte_oob_tcp_component.tcp_static_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    static_port_string6 = NULL;
    (void) prte_mca_base_component_var_register(component, "static_ipv6_ports",
                                                "Static ports for daemons and procs (IPv6)",
                                                PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_2,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &static_port_string6);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string6) {
        prte_util_parse_range_options(static_port_string6,
                                      &prte_oob_tcp_component.tcp6_static_ports);
        if (0 == strcmp(prte_oob_tcp_component.tcp6_static_ports[0], "-1")) {
            prte_argv_free(prte_oob_tcp_component.tcp6_static_ports);
            prte_oob_tcp_component.tcp6_static_ports = NULL;
        }
    } else {
        prte_oob_tcp_component.tcp6_static_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    if (NULL != prte_oob_tcp_component.tcp_static_ports
        || NULL != prte_oob_tcp_component.tcp6_static_ports) {
        prte_static_ports = true;
    }

    dyn_port_string = NULL;
    (void) prte_mca_base_component_var_register(
        component, "dynamic_ipv4_ports",
        "Range of ports to be dynamically used by daemons and procs (IPv4)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &dyn_port_string);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err = prte_argv_join(prte_oob_tcp_component.tcp_static_ports, ',');
            prte_show_help("help-oob-tcp.txt", "static-and-dynamic", true, err, dyn_port_string);
            free(err);
            return PRTE_ERROR;
        }
        prte_util_parse_range_options(dyn_port_string, &prte_oob_tcp_component.tcp_dyn_ports);
        if (0 == strcmp(prte_oob_tcp_component.tcp_dyn_ports[0], "-1")) {
            prte_argv_free(prte_oob_tcp_component.tcp_dyn_ports);
            prte_oob_tcp_component.tcp_dyn_ports = NULL;
        }
    } else {
        prte_oob_tcp_component.tcp_dyn_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    dyn_port_string6 = NULL;
    (void) prte_mca_base_component_var_register(
        component, "dynamic_ipv6_ports",
        "Range of ports to be dynamically used by daemons and procs (IPv6)",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &dyn_port_string6);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string6) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err4 = NULL, *err6 = NULL;
            if (NULL != prte_oob_tcp_component.tcp_static_ports) {
                err4 = prte_argv_join(prte_oob_tcp_component.tcp_static_ports, ',');
            }
            if (NULL != prte_oob_tcp_component.tcp6_static_ports) {
                err6 = prte_argv_join(prte_oob_tcp_component.tcp6_static_ports, ',');
            }
            prte_show_help("help-oob-tcp.txt", "static-and-dynamic-ipv6", true,
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
        prte_util_parse_range_options(dyn_port_string6, &prte_oob_tcp_component.tcp6_dyn_ports);
        if (0 == strcmp(prte_oob_tcp_component.tcp6_dyn_ports[0], "-1")) {
            prte_argv_free(prte_oob_tcp_component.tcp6_dyn_ports);
            prte_oob_tcp_component.tcp6_dyn_ports = NULL;
        }
    } else {
        prte_oob_tcp_component.tcp6_dyn_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    prte_oob_tcp_component.disable_ipv4_family = false;
    (void) prte_mca_base_component_var_register(component, "disable_ipv4_family",
                                                "Disable the IPv4 interfaces",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_oob_tcp_component.disable_ipv4_family);

#if PRTE_ENABLE_IPV6
    prte_oob_tcp_component.disable_ipv6_family = false;
    (void) prte_mca_base_component_var_register(component, "disable_ipv6_family",
                                                "Disable the IPv6 interfaces",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_oob_tcp_component.disable_ipv6_family);
#endif // PRTE_ENABLE_IPV6

    // Wait for this amount of time before sending the first keepalive probe
    prte_oob_tcp_component.keepalive_time = 300;
    (void) prte_mca_base_component_var_register(
        component, "keepalive_time",
        "Idle time in seconds before starting to send keepalives (keepalive_time <= 0 disables "
        "keepalive functionality)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_oob_tcp_component.keepalive_time);

    // Resend keepalive probe every INT seconds
    prte_oob_tcp_component.keepalive_intvl = 20;
    (void) prte_mca_base_component_var_register(
        component, "keepalive_intvl",
        "Time between successive keepalive pings when peer has not responded, in seconds (ignored "
        "if keepalive_time <= 0)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_oob_tcp_component.keepalive_intvl);

    // After sending PR probes every INT seconds consider the connection dead
    prte_oob_tcp_component.keepalive_probes = 9;
    (void) prte_mca_base_component_var_register(component, "keepalive_probes",
                                                "Number of keepalives that can be missed before "
                                                "declaring error (ignored if keepalive_time <= 0)",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_5,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_oob_tcp_component.keepalive_probes);

    prte_oob_tcp_component.retry_delay = 0;
    (void) prte_mca_base_component_var_register(
        component, "retry_delay", "Time (in sec) to wait before trying to connect to peer again",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_oob_tcp_component.retry_delay);

    prte_oob_tcp_component.max_recon_attempts = 10;
    (void) prte_mca_base_component_var_register(
        component, "max_recon_attempts",
        "Max number of times to attempt connection before giving up (-1 -> never give up)",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_oob_tcp_component.max_recon_attempts);

    return PRTE_SUCCESS;
}

static char **split_and_resolve(char **orig_str, char *name);

static int component_available(void)
{
    prte_if_t *copied_interface, *selected_interface;
    bool including = false, excluding = false;
    struct sockaddr_storage my_ss;
    char name[PRTE_IF_NAMESIZE];
    char **interfaces = NULL;
    /* Larger than necessary, used for copying mask */
    char string[50];
    int kindex;
    int i, rc;

    prte_output_verbose(5, prte_oob_base_framework.framework_output,
                        "oob:tcp: component_available called");

    /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != prte_oob_tcp_component.if_include) {
        interfaces = split_and_resolve(&prte_oob_tcp_component.if_include, "include");
        including = true;
        excluding = false;
    } else if (NULL != prte_oob_tcp_component.if_exclude) {
        interfaces = split_and_resolve(&prte_oob_tcp_component.if_exclude, "exclude");
        including = false;
        excluding = true;
    }

    /* look at all available interfaces */
    PRTE_LIST_FOREACH(selected_interface, &prte_if_list, prte_if_t)
    {
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
        prte_output_verbose(10, prte_oob_base_framework.framework_output,
                            "WORKING INTERFACE %d KERNEL INDEX %d FAMILY: %s", i, kindex,
                            (AF_INET == my_ss.ss_family) ? "V4" : "V6");

        /* ignore any virtual interfaces */
        if (0 == strncmp(selected_interface->if_name, "vir", 3)) {
            continue;
        }

        /* handle include/exclude directives */
        if (NULL != interfaces) {
            /* check for match */
            rc = prte_ifmatches(kindex, interfaces);
            /* if one of the network specifications isn't parseable, then
             * error out as we can't do what was requested
             */
            if (PRTE_ERR_NETWORK_NOT_PARSEABLE == rc) {
                prte_show_help("help-oob-tcp.txt", "not-parseable", true);
                prte_argv_free(interfaces);
                return PRTE_ERR_BAD_PARAM;
            }
            /* if we are including, then ignore this if not present */
            if (including) {
                if (PRTE_SUCCESS != rc) {
                    prte_output_verbose(
                        20, prte_oob_base_framework.framework_output,
                        "%s oob:tcp:init rejecting interface %s (not in include list)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    continue;
                }
            } else {
                /* we are excluding, so ignore if present */
                if (PRTE_SUCCESS == rc) {
                    prte_output_verbose(20, prte_oob_base_framework.framework_output,
                                        "%s oob:tcp:init rejecting interface %s (in exclude list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        selected_interface->if_name);
                    continue;
                }
            }
        } else {
            /* if no specific interfaces were provided, we ignore the loopback
             * interface unless nothing else is available
             */
            if (1 < prte_ifcount() && prte_ifisloopback(i)) {
                prte_output_verbose(20, prte_oob_base_framework.framework_output,
                                    "%s oob:tcp:init rejecting loopback interface %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    selected_interface->if_name);
                continue;
            }
        }

        /* Refs ticket #3019
         * it would probably be worthwhile to print out a warning if OMPI detects multiple
         * IP interfaces that are "up" on the same subnet (because that's a Bad Idea). Note
         * that we should only check for this after applying the relevant include/exclude
         * list MCA params. If we detect redundant ports, we can also automatically ignore
         * them so that applications won't hang.
         */

        /* add this address to our connections */
        if (AF_INET == my_ss.ss_family) {
            prte_output_verbose(10, prte_oob_base_framework.framework_output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                prte_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            prte_argv_append_nosize(&prte_oob_tcp_component.ipv4conns,
                                    prte_net_get_hostname((struct sockaddr *) &my_ss));
        } else if (AF_INET6 == my_ss.ss_family) {
#if PRTE_ENABLE_IPV6
            prte_output_verbose(10, prte_oob_base_framework.framework_output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                prte_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            prte_argv_append_nosize(&prte_oob_tcp_component.ipv6conns,
                                    prte_net_get_hostname((struct sockaddr *) &my_ss));
#endif // PRTE_ENABLE_IPV6
        } else {
            prte_output_verbose(10, prte_oob_base_framework.framework_output,
                                "%s oob:tcp:init ignoring %s from out list of connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                prte_net_get_hostname((struct sockaddr *) &my_ss));
        }
        copied_interface = PRTE_NEW(prte_if_t);
        if (NULL == copied_interface) {
            prte_argv_free(interfaces);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        prte_string_copy(copied_interface->if_name, selected_interface->if_name, sizeof(name));
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
        sprintf(string, "%d", selected_interface->if_mask);
        prte_argv_append_nosize(&prte_oob_tcp_component.if_masks, string);
        prte_list_append(&prte_oob_tcp_component.local_ifs, &(copied_interface->super));
    }

    /* cleanup */
    if (NULL != interfaces) {
        prte_argv_free(interfaces);
    }

    if (0 == prte_argv_count(prte_oob_tcp_component.ipv4conns)
#if PRTE_ENABLE_IPV6
        && 0 == prte_argv_count(prte_oob_tcp_component.ipv6conns)
#endif
    ) {
        if (including) {
            prte_show_help("help-oob-tcp.txt", "no-included-found", true,
                           prte_oob_tcp_component.if_include);
        } else if (excluding) {
            prte_show_help("help-oob-tcp.txt", "excluded-all", true,
                           prte_oob_tcp_component.if_exclude);
        }
        return PRTE_ERR_NOT_AVAILABLE;
    }

    return PRTE_SUCCESS;
}

/* Start all modules */
static int component_startup(void)
{
    int rc = PRTE_SUCCESS;

    prte_output_verbose(2, prte_oob_base_framework.framework_output, "%s TCP STARTUP",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if we are a daemon/HNP,
     * then it is possible that someone else may initiate a
     * connection to us. In these cases, we need to start the
     * listening thread/event. Otherwise, we will be the one
     * initiating communication, and there is no need for
     * a listener */
    if (PRTE_PROC_IS_MASTER || PRTE_PROC_IS_DAEMON) {
        if (PRTE_SUCCESS != (rc = prte_oob_tcp_start_listening())) {
            PRTE_ERROR_LOG(rc);
        }
    }

    return rc;
}

static void component_shutdown(void)
{
    int i = 0;

    prte_output_verbose(2, prte_oob_base_framework.framework_output, "%s TCP SHUTDOWN",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (PRTE_PROC_IS_MASTER && prte_oob_tcp_component.listen_thread_active) {
        prte_oob_tcp_component.listen_thread_active = false;
        /* tell the thread to exit */
        write(prte_oob_tcp_component.stop_thread[1], &i, sizeof(int));
        prte_thread_join(&prte_oob_tcp_component.listen_thread, NULL);

        close(prte_oob_tcp_component.stop_thread[0]);
        close(prte_oob_tcp_component.stop_thread[1]);

    } else {
        prte_output_verbose(2, prte_oob_base_framework.framework_output, "no hnp or not active");
    }

    /* cleanup listen event list */
    PRTE_LIST_DESTRUCT(&prte_oob_tcp_component.listeners);

    prte_output_verbose(2, prte_oob_base_framework.framework_output, "%s TCP SHUTDOWN done",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
}

static int component_send(prte_rml_send_t *msg)
{
    prte_output_verbose(5, prte_oob_base_framework.framework_output,
                        "%s oob:tcp:send_nb to peer %s:%d seq = %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&msg->dst), msg->tag,
                        msg->seq_num);

    /* The module will first see if it knows
     * of a way to send the data to the target, and then
     * attempt to send the data. It  will call the cbfunc
     * with the status upon completion - if it can't do it for
     * some reason, it will pass the error to our fn below so
     * it can do something about it
     */
    prte_oob_tcp_module.send_nb(msg);
    return PRTE_SUCCESS;
}

static char *component_get_addr(void)
{
    char *cptr = NULL, *tmp, *tp, *tm;

    if (!prte_oob_tcp_component.disable_ipv4_family && NULL != prte_oob_tcp_component.ipv4conns) {
        tmp = prte_argv_join(prte_oob_tcp_component.ipv4conns, ',');
        tp = prte_argv_join(prte_oob_tcp_component.ipv4ports, ',');
        tm = prte_argv_join(prte_oob_tcp_component.if_masks, ',');
        prte_asprintf(&cptr, "tcp://%s:%s:%s", tmp, tp, tm);
        free(tmp);
        free(tp);
        free(tm);
    }
#if PRTE_ENABLE_IPV6
    if (!prte_oob_tcp_component.disable_ipv6_family && NULL != prte_oob_tcp_component.ipv6conns) {
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
        tmp = prte_argv_join(prte_oob_tcp_component.ipv6conns, ',');
        tp = prte_argv_join(prte_oob_tcp_component.ipv6ports, ',');
        tm = prte_argv_join(prte_oob_tcp_component.if_masks, ',');
        if (NULL == cptr) {
            /* no ipv4 stuff */
            prte_asprintf(&cptr, "tcp6://[%s]:%s:%s", tmp, tp, tm);
        } else {
            prte_asprintf(&tmp2, "%s;tcp6://[%s]:%s:%s", cptr, tmp, tp, tm);
            free(cptr);
            cptr = tmp2;
        }
        free(tmp);
        free(tp);
        free(tm);
    }
#endif // PRTE_ENABLE_IPV6

    /* return our uri */
    return cptr;
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
            prte_output(0, "oob_tcp_parse_uri: Could not convert %s\n", host);
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

static int component_set_addr(pmix_proc_t *peer, char **uris)
{
    char **addrs, **masks, *hptr;
    char *tcpuri = NULL, *host, *ports, *masks_string;
    int i, j, rc;
    uint16_t af_family = AF_UNSPEC;
    uint64_t ui64;
    bool found;
    prte_oob_tcp_peer_t *pr;
    prte_oob_tcp_addr_t *maddr;

    memcpy(&ui64, (char *) peer, sizeof(uint64_t));
    /* cycle across component parts and see if one belongs to us */
    found = false;

    for (i = 0; NULL != uris[i]; i++) {
        tcpuri = strdup(uris[i]);
        if (NULL == tcpuri) {
            prte_output_verbose(2, prte_oob_base_framework.framework_output,
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
            prte_output_verbose(2, prte_oob_base_framework.framework_output,
                                "%s oob:tcp: address %s not supported",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uris[i]);
            free(tcpuri);
            continue;
#endif // PRTE_ENABLE_IPV6
        } else {
            /* not one of ours */
            prte_output_verbose(2, prte_oob_base_framework.framework_output,
                                "%s oob:tcp: ignoring address %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), uris[i]);
            free(tcpuri);
            continue;
        }

        /* this one is ours - record the peer */
        prte_output_verbose(2, prte_oob_base_framework.framework_output,
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
        masks = prte_argv_split(masks_string, ',');

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
        addrs = prte_argv_split(hptr, ',');

        /* cycle across the provided addrs */
        for (j = 0; NULL != addrs[j]; j++) {
            if (NULL == masks[j]) {
                /* Missing mask information */
                prte_output_verbose(2, prte_oob_base_framework.framework_output,
                                    "%s oob:tcp: uri missing mask information.",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                return PRTE_ERR_TAKE_NEXT_OPTION;
            }
            /* if they gave us "localhost", then just take the first conn on our list */
            if (0 == strcasecmp(addrs[j], "localhost")) {
#if PRTE_ENABLE_IPV6
                if (AF_INET6 == af_family) {
                    if (NULL == prte_oob_tcp_component.ipv6conns
                        || NULL == prte_oob_tcp_component.ipv6conns[0]) {
                        continue;
                    }
                    host = prte_oob_tcp_component.ipv6conns[0];
                } else {
#endif // PRTE_ENABLE_IPV6
                    if (NULL == prte_oob_tcp_component.ipv4conns
                        || NULL == prte_oob_tcp_component.ipv4conns[0]) {
                        continue;
                    }
                    host = prte_oob_tcp_component.ipv4conns[0];
#if PRTE_ENABLE_IPV6
                }
#endif
            } else {
                host = addrs[j];
            }

            if (NULL == (pr = prte_oob_tcp_peer_lookup(peer))) {
                pr = PRTE_NEW(prte_oob_tcp_peer_t);
                PMIX_XFER_PROCID(&pr->name, peer);
                prte_output_verbose(20, prte_oob_base_framework.framework_output,
                                    "%s SET_PEER ADDING PEER %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer));
                prte_list_append(&prte_oob_tcp_component.peers, &pr->super);
            }

            maddr = PRTE_NEW(prte_oob_tcp_addr_t);
            ((struct sockaddr_storage *) &(maddr->addr))->ss_family = af_family;
            if (PRTE_SUCCESS
                != (rc = parse_uri(af_family, host, ports,
                                   (struct sockaddr_storage *) &(maddr->addr)))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(maddr);
                prte_list_remove_item(&prte_oob_tcp_component.peers, &pr->super);
                PRTE_RELEASE(pr);
                return PRTE_ERR_TAKE_NEXT_OPTION;
            }
            maddr->if_mask = atoi(masks[j]);

            prte_output_verbose(20, prte_oob_base_framework.framework_output,
                                "%s set_peer: peer %s is listening on net %s port %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer),
                                (NULL == host) ? "NULL" : host, (NULL == ports) ? "NULL" : ports);
            prte_list_append(&pr->addrs, &maddr->super);

            found = true;
        }
        prte_argv_free(addrs);
        free(tcpuri);
    }
    if (found) {
        /* indicate that this peer is addressable by this component */
        return PRTE_SUCCESS;
    }

    /* otherwise indicate that it is not addressable by us */
    return PRTE_ERR_TAKE_NEXT_OPTION;
}

static bool component_is_reachable(pmix_proc_t *peer)
{
    pmix_proc_t hop;

    /* if we have a route to this peer, then we can reach it */
    hop = prte_routed.get_route(peer);
    if (PMIX_PROCID_INVALID(&hop)) {
        prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                            "%s is NOT reachable by TCP", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        return false;
    }
    /* assume we can reach the hop - the module will tell us if it can't
     * when we try to send the first time, and then we'll correct it */
    return true;
}

void prte_oob_tcp_component_set_module(int fd, short args, void *cbdata)
{
    prte_oob_tcp_peer_op_t *pop = (prte_oob_tcp_peer_op_t *) cbdata;
    prte_oob_base_peer_t *bpr;

    PRTE_ACQUIRE_OBJECT(pop);

    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:set_module called for peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&pop->peer));

    /* make sure the OOB knows that we can reach this peer - we
     * are in the same event base as the OOB base, so we can
     * directly access its storage
     */
    bpr = prte_oob_base_get_peer(&pop->peer);
    if (NULL == bpr) {
        bpr = PRTE_NEW(prte_oob_base_peer_t);
        PMIX_XFER_PROCID(&bpr->name, &pop->peer);
    }
    prte_bitmap_set_bit(&bpr->addressable, prte_oob_tcp_component.super.idx);
    bpr->component = &prte_oob_tcp_component.super;

    PRTE_RELEASE(pop);
}

void prte_oob_tcp_component_lost_connection(int fd, short args, void *cbdata)
{
    prte_oob_tcp_peer_op_t *pop = (prte_oob_tcp_peer_op_t *) cbdata;
    prte_oob_base_peer_t *bpr;

    PRTE_ACQUIRE_OBJECT(pop);

    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:lost connection called for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    /* Mark that we no longer support this peer */
    bpr = prte_oob_base_get_peer(&pop->peer);
    if (NULL != bpr) {
        prte_bitmap_clear_bit(&bpr->addressable, prte_oob_tcp_component.super.idx);
        prte_list_remove_item(&prte_oob_base.peers, &bpr->super);
        PRTE_RELEASE(bpr);
    }

    if (!prte_finalizing) {
        /* activate the proc state */
        if (PRTE_SUCCESS != prte_routed.route_lost(&pop->peer)) {
            PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_LIFELINE_LOST);
        } else {
            PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_COMM_FAILED);
        }
    }
    PRTE_RELEASE(pop);
}

void prte_oob_tcp_component_no_route(int fd, short args, void *cbdata)
{
    prte_oob_tcp_msg_error_t *mop = (prte_oob_tcp_msg_error_t *) cbdata;
    prte_oob_base_peer_t *bpr;

    PRTE_ACQUIRE_OBJECT(mop);

    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:no route called for peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&mop->hop));

    /* mark that we cannot reach this hop */
    bpr = prte_oob_base_get_peer(&mop->hop);
    if (NULL == bpr) {
        bpr = PRTE_NEW(prte_oob_base_peer_t);
        PMIX_XFER_PROCID(&bpr->name, &mop->hop);
    }
    prte_bitmap_clear_bit(&bpr->addressable, prte_oob_tcp_component.super.idx);

    /* report the error back to the OOB and let it try other components
     * or declare a problem
     */
    mop->rmsg->retries++;
    /* activate the OOB send state */
    PRTE_OOB_SEND(mop->rmsg);

    PRTE_RELEASE(mop);
}

void prte_oob_tcp_component_hop_unknown(int fd, short args, void *cbdata)
{
    prte_oob_tcp_msg_error_t *mop = (prte_oob_tcp_msg_error_t *) cbdata;
    prte_rml_send_t *snd;
    prte_oob_base_peer_t *bpr;
    pmix_status_t rc;
    pmix_byte_object_t bo;

    PRTE_ACQUIRE_OBJECT(mop);

    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:unknown hop called for peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&mop->hop));

    if (prte_finalizing || prte_abnormal_term_ordered) {
        /* just ignore the problem */
        PRTE_RELEASE(mop);
        return;
    }

    /* mark that this component cannot reach this hop */
    bpr = prte_oob_base_get_peer(&mop->hop);
    if (NULL == bpr) {
        /* the overall OOB has no knowledge of this hop. Only
         * way this could happen is if the peer contacted us
         * via this component, and it wasn't entered into the
         * OOB framework hash table. We have no way of knowing
         * what to do next, so just output an error message and
         * abort */
        prte_output(0,
                    "%s ERROR: message to %s requires routing and the OOB has no knowledge of the "
                    "reqd hop %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&mop->snd->hdr.dst),
                    PRTE_NAME_PRINT(&mop->hop));
        PRTE_ACTIVATE_PROC_STATE(&mop->hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        PRTE_RELEASE(mop);
        return;
    }
    prte_bitmap_clear_bit(&bpr->addressable, prte_oob_tcp_component.super.idx);

    /* mark that this component cannot reach this destination either */
    bpr = prte_oob_base_get_peer(&mop->snd->hdr.dst);
    if (NULL == bpr) {
        prte_output(
            0,
            "%s ERROR: message to %s requires routing and the OOB has no knowledge of this process",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&mop->snd->hdr.dst));
        PRTE_ACTIVATE_PROC_STATE(&mop->hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        PRTE_RELEASE(mop);
        return;
    }
    prte_bitmap_clear_bit(&bpr->addressable, prte_oob_tcp_component.super.idx);

    /* post the message to the OOB so it can see
     * if another component can transfer it
     */
    MCA_OOB_TCP_HDR_NTOH(&mop->snd->hdr);
    snd = PRTE_NEW(prte_rml_send_t);
    snd->retries = mop->rmsg->retries + 1;
    PMIX_XFER_PROCID(&snd->dst, &mop->snd->hdr.dst);
    PMIX_XFER_PROCID(&snd->origin, &mop->snd->hdr.origin);
    snd->tag = mop->snd->hdr.tag;
    snd->seq_num = mop->snd->hdr.seq_num;
    bo.bytes = mop->snd->data;
    bo.size = mop->snd->hdr.nbytes;
    rc = PMIx_Data_load(&snd->dbuf, &bo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    snd->cbfunc = NULL;
    snd->cbdata = NULL;
    /* activate the OOB send state */
    PRTE_OOB_SEND(snd);
    /* protect the data */
    mop->snd->data = NULL;

    PRTE_RELEASE(mop);
}

void prte_oob_tcp_component_failed_to_connect(int fd, short args, void *cbdata)
{
    prte_oob_tcp_peer_op_t *pop = (prte_oob_tcp_peer_op_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(pop);

    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:failed_to_connect called for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    /* if we are terminating, then don't attempt to reconnect */
    if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
        PRTE_RELEASE(pop);
        return;
    }

    /* activate the proc state */
    prte_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base_framework.framework_output,
                        "%s tcp:failed_to_connect unable to reach peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_FAILED_TO_CONNECT);
    PRTE_RELEASE(pop);
}

/*
 * Go through a list of argv; if there are any subnet specifications
 * (a.b.c.d/e), resolve them to an interface name (Currently only
 * supporting IPv4).  If unresolvable, warn and remove.
 */
static char **split_and_resolve(char **orig_str, char *name)
{
    int i, ret, save, if_index;
    char **argv, *str, *tmp;
    char if_name[PRTE_IF_NAMESIZE];
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;

    /* Sanity check */
    if (NULL == orig_str || NULL == *orig_str) {
        return NULL;
    }

    argv = prte_argv_split(*orig_str, ',');
    if (NULL == argv) {
        return NULL;
    }
    for (save = i = 0; NULL != argv[i]; ++i) {
        if (isalpha(argv[i][0])) {
            argv[save++] = argv[i];
            continue;
        }

        /* Found a subnet notation.  Convert it to an IP
           address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        tmp = strdup(argv[i]);
        str = strchr(argv[i], '/');
        if (NULL == str) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true, name,
                           prte_process_info.nodename, tmp,
                           "Invalid specification (missing \"/\")");
            free(argv[i]);
            free(tmp);
            continue;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr *) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i], &((struct sockaddr_in *) &argv_inaddr)->sin_addr);
        free(argv[i]);

        if (1 != ret) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true, name,
                           prte_process_info.nodename, tmp,
                           "Invalid specification (inet_pton() failed)");
            free(tmp);
            continue;
        }
        prte_output_verbose(20, prte_oob_base_framework.framework_output,
                            "%s oob:tcp: Searching for %s address+prefix: %s / %u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), name,
                            prte_net_get_hostname((struct sockaddr *) &argv_inaddr), argv_prefix);

        /* Go through all interfaces and see if we can find a match */
        for (if_index = prte_ifbegin(); if_index >= 0; if_index = prte_ifnext(if_index)) {
            prte_ifindextoaddr(if_index, (struct sockaddr *) &if_inaddr, sizeof(if_inaddr));
            if (prte_net_samenetwork((struct sockaddr *) &argv_inaddr,
                                     (struct sockaddr *) &if_inaddr, argv_prefix)) {
                break;
            }
        }
        /* If we didn't find a match, keep trying */
        if (if_index < 0) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true, name,
                           prte_process_info.nodename, tmp,
                           "Did not find interface matching this subnet");
            free(tmp);
            continue;
        }

        /* We found a match; get the name and replace it in the
           argv */
        prte_ifindextoname(if_index, if_name, sizeof(if_name));
        prte_output_verbose(20, prte_oob_base_framework.framework_output,
                            "%s oob:tcp: Found match: %s (%s)", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            prte_net_get_hostname((struct sockaddr *) &if_inaddr), if_name);
        argv[save++] = strdup(if_name);
        free(tmp);
    }

    /* The list may have been compressed if there were invalid
       entries, so ensure we end it with a NULL entry */
    argv[save] = NULL;
    free(*orig_str);
    *orig_str = prte_argv_join(argv, ',');
    return argv;
}

/* OOB TCP Class instances */

static void peer_cons(prte_oob_tcp_peer_t *peer)
{
    peer->auth_method = NULL;
    peer->sd = -1;
    PRTE_CONSTRUCT(&peer->addrs, prte_list_t);
    peer->active_addr = NULL;
    peer->state = MCA_OOB_TCP_UNCONNECTED;
    peer->num_retries = 0;
    PRTE_CONSTRUCT(&peer->send_queue, prte_list_t);
    peer->send_msg = NULL;
    peer->recv_msg = NULL;
    peer->send_ev_active = false;
    peer->recv_ev_active = false;
    peer->timer_ev_active = false;
}
static void peer_des(prte_oob_tcp_peer_t *peer)
{
    if (NULL != peer->auth_method) {
        free(peer->auth_method);
    }
    if (peer->send_ev_active) {
        prte_event_del(&peer->send_event);
    }
    if (peer->recv_ev_active) {
        prte_event_del(&peer->recv_event);
    }
    if (peer->timer_ev_active) {
        prte_event_del(&peer->timer_event);
    }
    if (0 <= peer->sd) {
        prte_output_verbose(2, prte_oob_base_framework.framework_output, "%s CLOSING SOCKET %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), peer->sd);
        CLOSE_THE_SOCKET(peer->sd);
    }
    PRTE_LIST_DESTRUCT(&peer->addrs);
    PRTE_LIST_DESTRUCT(&peer->send_queue);
}
PRTE_CLASS_INSTANCE(prte_oob_tcp_peer_t, prte_list_item_t, peer_cons, peer_des);

static void padd_cons(prte_oob_tcp_addr_t *ptr)
{
    memset(&ptr->addr, 0, sizeof(ptr->addr));
    ptr->retries = 0;
    ptr->state = MCA_OOB_TCP_UNCONNECTED;
}
PRTE_CLASS_INSTANCE(prte_oob_tcp_addr_t, prte_list_item_t, padd_cons, NULL);

static void pop_cons(prte_oob_tcp_peer_op_t *pop)
{
    pop->net = NULL;
    pop->port = NULL;
}
static void pop_des(prte_oob_tcp_peer_op_t *pop)
{
    if (NULL != pop->net) {
        free(pop->net);
    }
    if (NULL != pop->port) {
        free(pop->port);
    }
}
PRTE_CLASS_INSTANCE(prte_oob_tcp_peer_op_t, prte_object_t, pop_cons, pop_des);

PRTE_CLASS_INSTANCE(prte_oob_tcp_msg_op_t, prte_object_t, NULL, NULL);

PRTE_CLASS_INSTANCE(prte_oob_tcp_conn_op_t, prte_object_t, NULL, NULL);

static void nicaddr_cons(prte_oob_tcp_nicaddr_t *ptr)
{
    ptr->af_family = PF_UNSPEC;
    memset(&ptr->addr, 0, sizeof(ptr->addr));
}
PRTE_CLASS_INSTANCE(prte_oob_tcp_nicaddr_t, prte_list_item_t, nicaddr_cons, NULL);
