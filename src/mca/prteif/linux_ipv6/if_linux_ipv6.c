/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#    include <sys/sockio.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_IFADDRS_H
#    include <ifaddrs.h>
#endif

#include "constants.h"
#include "src/mca/prteif/base/base.h"
#include "src/mca/prteif/prteif.h"
#include "src/util/if.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/util/string_copy.h"

#define LOG_PREFIX "mca: prteif: linux_ipv6: "

static int if_linux_ipv6_open(void);

/* Discovers Linux IPv6 interfaces */
prte_if_base_component_t prte_prteif_linux_ipv6_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_IF_BASE_VERSION_2_0_0,

     /* Component name and version */
     "linux_ipv6", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,

     /* Component open and close functions */
     if_linux_ipv6_open, NULL},
    {/* This component is checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},
};

#if PRTE_ENABLE_IPV6
static bool hex2int(char hex, int *dst)
{
    if ('0' <= hex && hex <= '9') {
        *dst = hex - '0';
    } else if ('A' <= hex && hex <= 'F') {
        *dst = hex - 'A' + 10;
    } else if ('a' <= hex && hex <= 'f') {
        *dst = hex - 'a' + 10;
    } else {
        return false;
    }
    return true;
}

static bool hexdecode(const char *src, uint8_t *dst, size_t dstsize)
{
    int hi, lo;
    for (size_t i = 0; i < dstsize; i++) {
        if (hex2int(src[i * 2], &hi) && hex2int(src[i * 2 + 1], &lo)) {
            dst[i] = 16 * hi + lo;
        } else {
            return false;
        }
    }
    return true;
}
#endif

static int if_linux_ipv6_open(void)
{
#if PRTE_ENABLE_IPV6
    FILE *f;
    if ((f = fopen("/proc/net/if_inet6", "r"))) {
        char ifname[PRTE_IF_NAMESIZE];
        unsigned int idx, pfxlen, scope, dadstat;
        struct in6_addr a6;
        uint32_t flag;
        char addrhex[sizeof a6.s6_addr * 2 + 1];
        char addrstr[INET6_ADDRSTRLEN];

        while (fscanf(f, "%s %x %x %x %x %s\n", addrhex, &idx, &pfxlen, &scope, &dadstat, ifname)
               != EOF) {
            prte_if_t *intf;

            if (!hexdecode(addrhex, a6.s6_addr, sizeof a6.s6_addr)) {
                prte_show_help("help-prte-if-linux-ipv6.txt", "fail to parse if_inet6", true,
                               prte_process_info.nodename, ifname, addrhex);
                continue;
            };
            inet_ntop(AF_INET6, a6.s6_addr, addrstr, sizeof addrstr);

            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                LOG_PREFIX "found interface %s inet6 %s scope %x\n", ifname,
                                addrstr, scope);

            /* Only interested in global (0x00) scope */
            if (scope != 0x00) {
                prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                    LOG_PREFIX "skipped interface %s inet6 %s scope %x\n", ifname,
                                    addrstr, scope);
                continue;
            }

            intf = PRTE_NEW(prte_if_t);
            if (NULL == intf) {
                prte_output(0, LOG_PREFIX "unable to allocate %lu bytes\n",
                            (unsigned long) sizeof(prte_if_t));
                fclose(f);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            intf->af_family = AF_INET6;

            /* now construct the prte_if_t */
            prte_string_copy(intf->if_name, ifname, PRTE_IF_NAMESIZE);
            intf->if_index = prte_list_get_size(&prte_if_list) + 1;
            intf->if_kernel_index = (uint16_t) idx;
            ((struct sockaddr_in6 *) &intf->if_addr)->sin6_addr = a6;
            ((struct sockaddr_in6 *) &intf->if_addr)->sin6_family = AF_INET6;
            ((struct sockaddr_in6 *) &intf->if_addr)->sin6_scope_id = scope;
            intf->if_mask = pfxlen;
            if (PRTE_SUCCESS == prte_ifindextoflags(prte_ifnametoindex(ifname), &flag)) {
                intf->if_flags = flag;
            } else {
                intf->if_flags = IFF_UP;
            }

            /* copy new interface information to heap and append
               to list */
            prte_list_append(&prte_if_list, &(intf->super));
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                LOG_PREFIX "added interface %s inet6 %s scope %x\n", ifname,
                                addrstr, scope);
        } /* of while */
        fclose(f);
    }
#endif /* PRTE_ENABLE_IPV6 */

    return PRTE_SUCCESS;
}
