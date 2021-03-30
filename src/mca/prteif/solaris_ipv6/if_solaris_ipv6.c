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

static int if_solaris_ipv6_open(void);

/* Discovers Solaris IPv6 interfaces */
prte_if_base_component_t prte_prteif_solaris_ipv6_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_IF_BASE_VERSION_2_0_0,

     /* Component name and version */
     "solaris_ipv6", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,

     /* Component open and close functions */
     if_solaris_ipv6_open, NULL},
    {/* This component is checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},
};

/* configure using getifaddrs(3) */
static int if_solaris_ipv6_open(void)
{
#if PRTE_ENABLE_IPV6
    int i;
    int sd;
    int error;
    uint16_t kindex;
    struct lifnum lifnum;
    struct lifconf lifconf;
    struct lifreq *lifreq, lifquery;

    sd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sd < 0) {
        prte_output(0, "prte_ifinit: unable to open IPv6 socket\n");
        return PRTE_ERROR;
    }

    /* we only ask for IPv6; IPv4 discovery has already been done */
    lifnum.lifn_family = AF_INET6;
    lifnum.lifn_flags = 0;
    lifnum.lifn_count = 0;

    /* get the number of interfaces in the system */
    error = ioctl(sd, SIOCGLIFNUM, &lifnum);
    if (error < 0) {
        prte_output(0, "prte_ifinit: ioctl SIOCGLIFNUM failed with errno=%d\n", errno);
        return PRTE_ERROR;
    }

    memset(&lifconf, 0, sizeof(struct lifconf));
    memset(&lifquery, 0, sizeof(struct lifreq));
    lifconf.lifc_family = AF_INET6;
    lifconf.lifc_flags = 0;
    lifconf.lifc_len = lifnum.lifn_count * sizeof(struct lifreq) * 2;
    lifconf.lifc_buf = malloc(lifconf.lifc_len);
    if (NULL == lifconf.lifc_buf) {
        prte_output(0, "prte_ifinit: IPv6 discovery: malloc() failed\n");
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    memset(lifconf.lifc_buf, 0, lifconf.lifc_len);

    error = ioctl(sd, SIOCGLIFCONF, &lifconf);
    if (error < 0) {
        prte_output(0, "prte_ifinit: IPv6 SIOCGLIFCONF failed with errno=%d\n", errno);
    }

    for (i = 0; i + sizeof(struct lifreq) <= lifconf.lifc_len; i += sizeof(*lifreq)) {

        lifreq = (struct lifreq *) ((caddr_t) lifconf.lifc_buf + i);
        prte_string_copy(lifquery.lifr_name, lifreq->lifr_name, sizeof(lifquery.lifr_name));

        /* lookup kernel index */
        error = ioctl(sd, SIOCGLIFINDEX, &lifquery);
        if (error < 0) {
            prte_output(0, "prte_ifinit: SIOCGLIFINDEX failed with errno=%d\n", errno);
            return PRTE_ERROR;
        }
        kindex = lifquery.lifr_index;

        /* lookup interface flags */
        error = ioctl(sd, SIOCGLIFFLAGS, &lifquery);
        if (error < 0) {
            prte_output(0, "prte_ifinit: SIOCGLIFFLAGS failed with errno=%d\n", errno);
            return PRTE_ERROR;
        }

        if (AF_INET6 == lifreq->lifr_addr.ss_family) {
            struct sockaddr_in6 *my_addr = (struct sockaddr_in6 *) &lifreq->lifr_addr;
            /* we surely want to check for sin6_scope_id, but Solaris
               does not set it correctly, so we have to look for
               global scope. For now, global is anything which is
               neither loopback nor link local.

               Bug, FIXME: site-local, multicast, ... missing
               Check for 2000::/3?
            */
            if ((!prte_if_retain_loopback && !IN6_IS_ADDR_LOOPBACK(&my_addr->sin6_addr))
                && (!IN6_IS_ADDR_LINKLOCAL(&my_addr->sin6_addr))) {
                /* create interface for newly found address */
                prte_if_t *intf;

                intf = PRTE_NEW(prte_if_t);
                if (NULL == intf) {
                    prte_output(0, "prte_ifinit: unable to allocate %d bytes\n", sizeof(prte_if_t));
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                intf->af_family = AF_INET6;

                prte_string_copy(intf->if_name, lifreq->lifr_name, PRTE_IF_NAMESIZE);
                intf->if_index = prte_list_get_size(&prte_if_list) + 1;
                memcpy(&intf->if_addr, my_addr, sizeof(*my_addr));
                intf->if_mask = 64;
                /* lifrq flags are uint64_t */
                intf->if_flags = (uint32_t)(0x00000000ffffffff) & lifquery.lifr_flags;

                /* append to list */
                prte_list_append(&prte_if_list, &(intf->super));
            }
        }
    } /* for */

    if (NULL != lifconf.lifc_buf) {
        free(lifconf.lifc_buf);
    }
#endif /* PRTE_ENABLE_IPV6 */

    return PRTE_SUCCESS;
}
