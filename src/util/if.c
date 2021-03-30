/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
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
#include <ctype.h>

#include "constants.h"
#include "src/class/prte_list.h"
#include "src/util/argv.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/output.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/util/string_copy.h"

#include "src/mca/prteif/base/base.h"

#ifdef HAVE_STRUCT_SOCKADDR_IN

#    ifndef MIN
#        define MIN(a, b) ((a) < (b) ? (a) : (b))
#    endif

/*
 *  Look for interface by name and returns its
 *  corresponding prte_list index.
 */

int prte_ifnametoindex(const char *if_name)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (strcmp(intf->if_name, if_name) == 0) {
            return intf->if_index;
        }
    }
    return -1;
}

/*
 *  Look for interface by name and returns its
 *  corresponding kernel index.
 */

int prte_ifnametokindex(const char *if_name)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (strcmp(intf->if_name, if_name) == 0) {
            return intf->if_kernel_index;
        }
    }
    return -1;
}

/*
 *  Attempt to resolve the adddress (given as either IPv4/IPv6 string
 *  or hostname) and lookup corresponding interface.
 */

static int prte_ifaddrtoname(const char *if_addr, char *if_name, int length)
{
    prte_if_t *intf;
    int error;
    struct addrinfo hints, *res = NULL, *r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(if_addr, NULL, &hints, &res);

    if (error) {
        if (NULL != res) {
            freeaddrinfo(res);
        }
        return PRTE_ERR_NOT_FOUND;
    }

    for (r = res; r != NULL; r = r->ai_next) {
        PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
        {
            if (AF_INET == r->ai_family) {
                struct sockaddr_in ipv4;
                struct sockaddr_in *inaddr;

                inaddr = (struct sockaddr_in *) &intf->if_addr;
                memcpy(&ipv4, r->ai_addr, r->ai_addrlen);

                if (inaddr->sin_addr.s_addr == ipv4.sin_addr.s_addr) {
                    prte_string_copy(if_name, intf->if_name, length);
                    freeaddrinfo(res);
                    return PRTE_SUCCESS;
                }
            }
#    if PRTE_ENABLE_IPV6
            else {
                if (IN6_ARE_ADDR_EQUAL(&((struct sockaddr_in6 *) &intf->if_addr)->sin6_addr,
                                       &((struct sockaddr_in6 *) r->ai_addr)->sin6_addr)) {
                    prte_string_copy(if_name, intf->if_name, length);
                    freeaddrinfo(res);
                    return PRTE_SUCCESS;
                }
            }
#    endif
        }
    }
    if (NULL != res) {
        freeaddrinfo(res);
    }

    /* if we get here, it wasn't found */
    return PRTE_ERR_NOT_FOUND;
}

/*
 *  Return the number of discovered interface.
 */

int prte_ifcount(void)
{
    return prte_list_get_size(&prte_if_list);
}

/*
 *  Return the prte_list interface index for the first
 *  interface in our list.
 */

int prte_ifbegin(void)
{
    prte_if_t *intf;

    intf = (prte_if_t *) prte_list_get_first(&prte_if_list);
    if (NULL != intf)
        return intf->if_index;
    return (-1);
}

/*
 *  Located the current position in the list by if_index and
 *  return the interface index of the next element in our list
 *  (if it exists).
 */

int prte_ifnext(int if_index)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_index == if_index) {
            do {
                prte_if_t *if_next = (prte_if_t *) prte_list_get_next(intf);
                prte_if_t *if_end = (prte_if_t *) prte_list_get_end(&prte_if_list);
                if (if_next == if_end) {
                    return -1;
                }
                intf = if_next;
            } while (intf->if_index == if_index);
            return intf->if_index;
        }
    }
    return (-1);
}

/*
 *  Lookup the interface by prte_list index and return the
 *  primary address assigned to the interface.
 */

int prte_ifindextoaddr(int if_index, struct sockaddr *if_addr, unsigned int length)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_index == if_index) {
            memcpy(if_addr, &intf->if_addr, MIN(length, sizeof(intf->if_addr)));
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERROR;
}

/*
 *  Lookup the interface by prte_list kindex and return the
 *  primary address assigned to the interface.
 */
int prte_ifkindextoaddr(int if_kindex, struct sockaddr *if_addr, unsigned int length)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_kernel_index == if_kindex) {
            memcpy(if_addr, &intf->if_addr, MIN(length, sizeof(intf->if_addr)));
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERROR;
}

/*
 *  Lookup the interface by prte_list index and return
 *  the associated name.
 */

int prte_ifindextoname(int if_index, char *if_name, int length)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_index == if_index) {
            prte_string_copy(if_name, intf->if_name, length);
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERROR;
}

/*
 *  Lookup the interface by prte_list index and return the
 *  flags assigned to the interface.
 */

int prte_ifindextoflags(int if_index, uint32_t *if_flags)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_index == if_index) {
            memcpy(if_flags, &intf->if_flags, sizeof(uint32_t));
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERROR;
}

#    define ADDRLEN 100
bool prte_ifislocal(const char *hostname)
{
#    if PRTE_ENABLE_IPV6
    char addrname[NI_MAXHOST]; /* should be larger than ADDRLEN, but I think
                                  they really mean IFNAMESIZE */
#    else
    char addrname[ADDRLEN + 1];
#    endif
    int i;

    /* see if it matches any of our known aliases */
    if (NULL != prte_process_info.aliases) {
        for (i = 0; NULL != prte_process_info.aliases[i]; i++) {
            if (0 == strcmp(hostname, prte_process_info.aliases[i])) {
                return true;
            }
        }
    }

    /* okay, have to resolve the address - the prte_ifislocal
     * function will not attempt to resolve the address if
     * told not to do so */
    if (PRTE_SUCCESS == prte_ifaddrtoname(hostname, addrname, ADDRLEN)) {
        if (0 != strcmp(hostname, "localhost") && 0 != strcmp(hostname, "127.0.0.1")) {
            /* add this to our known aliases */
            prte_argv_append_nosize(&prte_process_info.aliases, hostname);
        }
        return true;
    }

    return false;
}

static int parse_ipv4_dots(const char *addr, uint32_t *net, int *dots)
{
    const char *start = addr, *end;
    uint32_t n[] = {0, 0, 0, 0};
    int i;

    /* now assemble the address */
    for (i = 0; i < 4; i++) {
        n[i] = strtoul(start, (char **) &end, 10);
        if (end == start) {
            /* this is not an error, but indicates that
             * we were given a partial address - e.g.,
             * 192.168 - usually indicating an IP range
             * in CIDR notation. So just return what we have
             */
            break;
        }
        /* did we read something sensible? */
        if (n[i] > 255) {
            return PRTE_ERR_NETWORK_NOT_PARSEABLE;
        }
        /* skip all the . */
        for (start = end; '\0' != *start; start++)
            if ('.' != *start)
                break;
    }
    *dots = i;
    *net = PRTE_IF_ASSEMBLE_NETWORK(n[0], n[1], n[2], n[3]);
    return PRTE_SUCCESS;
}

int prte_iftupletoaddr(const char *inaddr, uint32_t *net, uint32_t *mask)
{
    int pval, dots, rc = PRTE_SUCCESS;
    const char *ptr;

    /* if a mask was desired... */
    if (NULL != mask) {
        /* set default */
        *mask = 0xFFFFFFFF;

        /* if entry includes mask, split that off */
        if (NULL != (ptr = strchr(inaddr, '/'))) {
            ptr = ptr + 1; /* skip the / */
            /* is the mask a tuple? */
            if (NULL != strchr(ptr, '.')) {
                /* yes - extract mask from it */
                rc = parse_ipv4_dots(ptr, mask, &dots);
            } else {
                /* no - must be an int telling us how much of the addr to use: e.g., /16
                 * For more information please read http://en.wikipedia.org/wiki/Subnetwork.
                 */
                pval = strtol(ptr, NULL, 10);
                if ((pval > 31) || (pval < 1)) {
                    prte_output(0, "prte_iftupletoaddr: unknown mask");
                    return PRTE_ERR_NETWORK_NOT_PARSEABLE;
                }
                *mask = 0xFFFFFFFF << (32 - pval);
            }
        } else {
            /* use the number of dots to determine it */
            for (ptr = inaddr, pval = 0; '\0' != *ptr; ptr++) {
                if ('.' == *ptr) {
                    pval++;
                }
            }
            /* if we have three dots, then we have four
             * fields since it is a full address, so the
             * default netmask is fine
             */
            if (3 == pval) {
                *mask = 0xFFFFFFFF;
            } else if (2 == pval) { /* 2 dots */
                *mask = 0xFFFFFF00;
            } else if (1 == pval) { /* 1 dot */
                *mask = 0xFFFF0000;
            } else if (0 == pval) { /* no dots */
                *mask = 0xFF000000;
            } else {
                prte_output(0, "prte_iftupletoaddr: unknown mask");
                return PRTE_ERR_NETWORK_NOT_PARSEABLE;
            }
        }
    }

    /* if network addr is desired... */
    if (NULL != net) {
        /* now assemble the address */
        rc = parse_ipv4_dots(inaddr, net, &dots);
    }

    return rc;
}

/*
 *  Determine if the specified interface is loopback
 */

bool prte_ifisloopback(int if_index)
{
    prte_if_t *intf;

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        if (intf->if_index == if_index) {
            if ((intf->if_flags & IFF_LOOPBACK) != 0) {
                return true;
            }
        }
    }
    return false;
}

/* Determine if an interface matches any entry in the given list, taking
 * into account that the list entries could be given as named interfaces,
 * IP addrs, or subnet+mask
 */
int prte_ifmatches(int kidx, char **nets)
{
    bool named_if;
    int i, rc;
    size_t j;
    int kindex;
    struct sockaddr_in inaddr;
    uint32_t addr, netaddr, netmask;

    /* get the address info for the given network in case we need it */
    if (PRTE_SUCCESS
        != (rc = prte_ifkindextoaddr(kidx, (struct sockaddr *) &inaddr, sizeof(inaddr)))) {
        return rc;
    }
    addr = ntohl(inaddr.sin_addr.s_addr);

    for (i = 0; NULL != nets[i]; i++) {
        /* if the specified interface contains letters in it, then it
         * was given as an interface name and not an IP tuple
         */
        named_if = false;
        for (j = 0; j < strlen(nets[i]); j++) {
            if (isalpha(nets[i][j]) && '.' != nets[i][j]) {
                named_if = true;
                break;
            }
        }
        if (named_if) {
            if (0 > (kindex = prte_ifnametokindex(nets[i]))) {
                continue;
            }
            if (kindex == kidx) {
                return PRTE_SUCCESS;
            }
        } else {
            if (PRTE_SUCCESS != (rc = prte_iftupletoaddr(nets[i], &netaddr, &netmask))) {
                prte_show_help("help-prte-util.txt", "invalid-net-mask", true, nets[i]);
                return rc;
            }
            if (netaddr == (addr & netmask)) {
                return PRTE_SUCCESS;
            }
        }
    }
    /* get here if not found */
    return PRTE_ERR_NOT_FOUND;
}

void prte_ifgetaliases(char ***aliases)
{
    prte_if_t *intf;
    char ipv4[INET_ADDRSTRLEN];
    struct sockaddr_in *addr;
#    if PRTE_ENABLE_IPV6
    char ipv6[INET6_ADDRSTRLEN];
    struct sockaddr_in6 *addr6;
#    endif

    PRTE_LIST_FOREACH(intf, &prte_if_list, prte_if_t)
    {
        addr = (struct sockaddr_in *) &intf->if_addr;
        /* ignore purely loopback interfaces */
        if ((intf->if_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        /* ignore the obvious */
        if (addr->sin_family == AF_INET) {
            inet_ntop(AF_INET, &(addr->sin_addr.s_addr), ipv4, INET_ADDRSTRLEN);
            if (0 == strcmp(ipv4, "localhost") || 0 == strcmp(ipv4, "127.0.0.1")) {
                continue;
            }
            prte_argv_append_nosize(aliases, ipv4);
        }
#    if PRTE_ENABLE_IPV6
        else {
            addr6 = (struct sockaddr_in6 *) &intf->if_addr;
            inet_ntop(AF_INET6, &(addr6->sin6_addr), ipv6, INET6_ADDRSTRLEN);
            prte_argv_append_nosize(aliases, ipv6);
        }
#    endif
    }
}

#else /* HAVE_STRUCT_SOCKADDR_IN */

/* if we don't have struct sockaddr_in, we don't have traditional
   ethernet devices.  Just make everything a no-op error call */

int prte_ifnametoindex(const char *if_name)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifnametokindex(const char *if_name)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifcount(void)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifbegin(void)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifnext(int if_index)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifindextoname(int if_index, char *if_name, int length)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifindextoaddr(int if_index, struct sockaddr *if_addr, unsigned int length)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifkindextoaddr(int if_kindex, struct sockaddr *if_addr, unsigned int length)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

int prte_ifindextoflags(int if_index, uint32_t *if_flags)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

bool prte_ifislocal(const char *hostname)
{
    return false;
}

int prte_iftupletoaddr(const char *inaddr, uint32_t *net, uint32_t *mask)
{
    return 0;
}

bool prte_ifisloopback(int if_index)
{
    return false;
}

int prte_ifmatches(int idx, char **nets)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

void prte_ifgetaliases(char ***aliases)
{
    return;
}

#endif /* HAVE_STRUCT_SOCKADDR_IN */
