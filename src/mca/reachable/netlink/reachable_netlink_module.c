/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015 Cisco Systems.  All rights reserved.
 * Copyright (c) 2017 Amazon.com, Inc. or its affiliates.
 *                    All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/include/constants.h"
#include "src/include/types.h"

#ifdef HAVE_MATH_H
#include <math.h>
#endif

#include "src/util/net.h"
#include "src/util/string_copy.h"
#include "src/mca/reachable/base/base.h"
#include "reachable_netlink.h"
#include "libnl_utils.h"

enum connection_quality {
    CQ_NO_CONNECTION = 0,
    CQ_DIFFERENT_NETWORK = 50,
    CQ_SAME_NETWORK = 100
};

/* Local variables */
static int init_counter = 0;

static int get_weights(prrte_if_t *local_if, prrte_if_t *remote_if);
static int calculate_weight(int bandwidth_local, int bandwidth_remote,
                            int connection_quality);

static int netlink_init(void)
{
    ++init_counter;

    return PRRTE_SUCCESS;
}

static int netlink_fini(void)
{
    --init_counter;

    return PRRTE_SUCCESS;
}

/*
 * Determines whether a connection is possible between
 * pairs of local and remote interfaces. To determine
 * reachability, the kernel's routing table is queried.
 * Higher weightings are given to connections on the same
 * network.
 */
static prrte_reachable_t* netlink_reachable(prrte_list_t *local_ifs,
                                            prrte_list_t *remote_ifs)
{
    prrte_reachable_t *reachable_results = NULL;
    int i, j;
    prrte_if_t *local_iter, *remote_iter;

    reachable_results = prrte_reachable_allocate(local_ifs->prrte_list_length,
                                                remote_ifs->prrte_list_length);
    if (NULL == reachable_results) {
        return NULL;
    }

    i = 0;
    PRRTE_LIST_FOREACH(local_iter, local_ifs, prrte_if_t) {
        j = 0;
        PRRTE_LIST_FOREACH(remote_iter, remote_ifs, prrte_if_t) {
            reachable_results->weights[i][j] = get_weights(local_iter, remote_iter);
            j++;
        }
        i++;
    }

    return reachable_results;
}


static int get_weights(prrte_if_t *local_if, prrte_if_t *remote_if)
{
    char str_local[128], str_remote[128], *conn_type;
    int outgoing_interface, ret, weight, has_gateway;

    /* prrte_net_get_hostname returns a static buffer.  Great for
       single address printfs, need to copy in this case */
    prrte_string_copy(str_local,
            prrte_net_get_hostname((struct sockaddr *)&local_if->if_addr),
            sizeof(str_local));
    str_local[sizeof(str_local) - 1] = '\0';
    prrte_string_copy(str_remote,
            prrte_net_get_hostname((struct sockaddr *)&remote_if->if_addr),
            sizeof(str_remote));
    str_remote[sizeof(str_remote) - 1] = '\0';

    /*  initially, assume no connection is possible */
    weight = calculate_weight(0, 0, CQ_NO_CONNECTION);

    if (AF_INET == local_if->af_family && AF_INET == remote_if->af_family) {
        uint32_t local_ip, remote_ip;

        local_ip = (uint32_t)((struct sockaddr_in *)&(local_if->if_addr))->sin_addr.s_addr;
        remote_ip = (uint32_t)((struct sockaddr_in *)&(remote_if->if_addr))->sin_addr.s_addr;
        outgoing_interface = local_if->if_kernel_index;

        ret = prrte_reachable_netlink_rt_lookup(local_ip,
                                               remote_ip,
                                               outgoing_interface,
                                               &has_gateway);
        if (0 == ret) {
            if (0 == has_gateway) {
                conn_type = "IPv4 SAME NETWORK";
                weight = calculate_weight(local_if->if_bandwidth,
                                          remote_if->if_bandwidth,
                                          CQ_SAME_NETWORK);
            } else {
                conn_type = "IPv4 DIFFERENT NETWORK";
                weight = calculate_weight(local_if->if_bandwidth,
                                          remote_if->if_bandwidth,
                                          CQ_DIFFERENT_NETWORK);
            }
        } else {
            conn_type = "IPv4 NO CONNECTION";
            weight = calculate_weight(0, 0, CQ_NO_CONNECTION);
        }

#if PRRTE_ENABLE_IPV6
    } else if (AF_INET6 == local_if->af_family && AF_INET6 == remote_if->af_family) {
        struct in6_addr *local_ip, *remote_ip;

        local_ip = &((struct sockaddr_in6 *)&(local_if->if_addr))->sin6_addr;
        remote_ip = &((struct sockaddr_in6 *)&(remote_if->if_addr))->sin6_addr;
        outgoing_interface = local_if->if_kernel_index;

        ret = prrte_reachable_netlink_rt_lookup6(local_ip,
                                                remote_ip,
                                                outgoing_interface,
                                                &has_gateway);

        if (0 == ret) {
            if (0 == has_gateway) {
                conn_type = "IPv6 SAME NETWORK";
                weight = calculate_weight(local_if->if_bandwidth,
                                          remote_if->if_bandwidth,
                                          CQ_SAME_NETWORK);
            } else {
                conn_type = "IPv6 DIFFERENT NETWORK";
                weight = calculate_weight(local_if->if_bandwidth,
                                          remote_if->if_bandwidth,
                                          CQ_DIFFERENT_NETWORK);
            }
        } else {
            conn_type = "IPv6 NO CONNECTION";
            weight = calculate_weight(0, 0, CQ_NO_CONNECTION);
        }
#endif /* #if PRRTE_ENABLE_IPV6 */

    } else {
        /* we don't have an address family match, so assume no
           connection */
        conn_type = "Address type mismatch";
        weight = calculate_weight(0, 0, CQ_NO_CONNECTION);
    }

    prrte_output_verbose(20, prrte_reachable_base_framework.framework_output,
                        "reachable:netlink: path from %s to %s: %s",
                        str_local, str_remote, conn_type);

    return weight;
}


const prrte_reachable_base_module_t prrte_reachable_netlink_module = {
    netlink_init,
    netlink_fini,
    netlink_reachable
};


/*
 * Weights determined by bandwidth between
 * interfaces (limited by lower bandwidth
 * interface).  A penalty is added to minimize
 * the discrepancy in bandwidth.  This helps
 * prevent pairing of fast and slow interfaces
 *
 * Formula: connection_quality * (min(a,b) + 1/(1 + |a-b|))
 *
 * Examples: a     b     f(a,b)
 *           0     0     1
 *           0     1     0.5
 *           1     1     2
 *           1     2     1.5
 *           1     3     1.33
 *           1     10    1.1
 *           10    10    11
 *           10    14    10.2
 *           11    14    11.25
 *           11    15    11.2
 *
 * NOTE: connection_quality of 1 is assumed for examples.
 * In reality, since we're using integers, we need
 * connection_quality to be large enough
 * to capture decimals
 */
static int calculate_weight(int bandwidth_local, int bandwidth_remote,
                            int connection_quality)
{
    int weight = connection_quality * (MIN(bandwidth_local, bandwidth_remote) +
                                       1.0/(1.0 + (double)abs(bandwidth_local - bandwidth_remote)));
    return weight;
}
