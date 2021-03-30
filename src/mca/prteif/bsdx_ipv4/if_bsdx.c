/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/mca/prteif/prteif.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"

static int if_bsdx_open(void);

/* Supports specific flavors of BSD:
 * NetBSD
 * FreeBSD
 * OpenBSD
 * DragonFly
 */
prte_if_base_component_t prte_prteif_bsdx_ipv4_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_IF_BASE_VERSION_2_0_0,

     /* Component name and version */
     "bsdx_ipv4", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,

     /* Component open and close functions */
     if_bsdx_open, NULL},
    {/* This component is checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},
};

/* convert a netmask (in network byte order) to CIDR notation */
static int prefix(uint32_t netmask)
{
    uint32_t mask = ntohl(netmask);
    int plen = 0;

    if (0 == mask) {
        plen = 32;
    } else {
        while ((mask % 2) == 0) {
            plen += 1;
            mask /= 2;
        }
    }

    return (32 - plen);
}

/* configure using getifaddrs(3) */
static int if_bsdx_open(void)
{
    struct ifaddrs **ifadd_list;
    struct ifaddrs *cur_ifaddrs;
    struct sockaddr_in *sin_addr;

    /*
     * the manpage claims that getifaddrs() allocates the memory,
     * and freeifaddrs() is later used to release the allocated memory.
     * however, without this malloc the call to getifaddrs() segfaults
     */
    ifadd_list = (struct ifaddrs **) malloc(sizeof(struct ifaddrs *));

    /* create the linked list of ifaddrs structs */
    if (getifaddrs(ifadd_list) < 0) {
        prte_output(0, "prte_ifinit: getifaddrs() failed with error=%d\n", errno);
        return PRTE_ERROR;
    }

    for (cur_ifaddrs = *ifadd_list; NULL != cur_ifaddrs; cur_ifaddrs = cur_ifaddrs->ifa_next) {
        prte_if_t *intf;
        struct in_addr a4;

        /* skip non- af_inet interface addresses */
        if (AF_INET != cur_ifaddrs->ifa_addr->sa_family) {
            continue;
        }

        /* skip interface if it is down (IFF_UP not set) */
        if (0 == (cur_ifaddrs->ifa_flags & IFF_UP)) {
            continue;
        }

        /* skip interface if it is a loopback device (IFF_LOOPBACK set) */
        if (!prte_if_retain_loopback && 0 != (cur_ifaddrs->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }

        /* or if it is a point-to-point interface */
        /* TODO: do we really skip p2p? */
        if (0 != (cur_ifaddrs->ifa_flags & IFF_POINTOPOINT)) {
            continue;
        }

        sin_addr = (struct sockaddr_in *) cur_ifaddrs->ifa_addr;

        intf = PRTE_NEW(prte_if_t);
        if (NULL == intf) {
            prte_output(0, "prte_ifinit: unable to allocate %d bytes\n", (int) sizeof(prte_if_t));
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        intf->af_family = AF_INET;

        /* fill values into the prte_if_t */
        memcpy(&a4, &(sin_addr->sin_addr), sizeof(struct in_addr));

        prte_string_copy(intf->if_name, cur_ifaddrs->ifa_name, PRTE_IF_NAMESIZE);
        intf->if_index = prte_list_get_size(&prte_if_list) + 1;
        ((struct sockaddr_in *) &intf->if_addr)->sin_addr = a4;
        ((struct sockaddr_in *) &intf->if_addr)->sin_family = AF_INET;
        ((struct sockaddr_in *) &intf->if_addr)->sin_len = cur_ifaddrs->ifa_addr->sa_len;

        intf->if_mask = prefix(sin_addr->sin_addr.s_addr);
        intf->if_flags = cur_ifaddrs->ifa_flags;

        intf->if_kernel_index = (uint16_t) if_nametoindex(cur_ifaddrs->ifa_name);

        prte_list_append(&prte_if_list, &(intf->super));
    } /*  of for loop over ifaddrs list */

    return PRTE_SUCCESS;
}
