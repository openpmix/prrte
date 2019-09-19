/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* @file */

#ifndef PRRTE_UTIL_NET_H
#define PRRTE_UTIL_NET_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

BEGIN_C_DECLS

/**
 * Intiailize the network helper subsystem
 *
 * Initialize the network helper subsystem.  Should be called exactly
 * once for any process that will use any function in the network
 * helper subsystem.
 *
 * @retval PRRTE_SUCCESS   Success
 * @retval PRRTE_ERR_TEMP_OUT_OF_RESOURCE Not enough memory for static
 *                        buffer creation
 */
PRRTE_EXPORT int prrte_net_init(void);
PRRTE_EXPORT void prrte_net_finalize (void);

/**
 * Calculate netmask in network byte order from CIDR notation
 *
 * @param prefixlen (IN)  CIDR prefixlen
 * @return                netmask in network byte order
 */
PRRTE_EXPORT uint32_t prrte_net_prefix2netmask(uint32_t prefixlen);


/**
 * Determine if given IP address is in the localhost range
 *
 * Determine if the given IP address is in the localhost range
 * (127.0.0.0/8), meaning that it can't be used to connect to machines
 * outside the current host.
 *
 * @param addr             struct sockaddr_in of IP address
 * @return                 true if \c addr is a localhost address,
 *                         false otherwise.
 */
PRRTE_EXPORT bool prrte_net_islocalhost(const struct sockaddr *addr);


/**
 * Are we on the same network?
 *
 * For IPv6, we only need to check for /64, there are no other
 * local netmasks.
 *
 * @param addr1             struct sockaddr of address
 * @param addr2             struct sockaddr of address
 * @param prefixlen         netmask (either CIDR or IPv6 prefixlen)
 * @return                  true if \c addr1 and \c addr2 are on the
 *                          same net, false otherwise.
 */
PRRTE_EXPORT bool prrte_net_samenetwork(const struct sockaddr *addr1,
                                        const struct sockaddr *addr2,
                                        uint32_t prefixlen);


/**
 * Is the given address a public IPv4 address?  Returns false for IPv6
 * address.
 *
 * @param addr      address as struct sockaddr
 * @return          true, if \c addr is IPv4 public, false otherwise
 */
PRRTE_EXPORT bool prrte_net_addr_isipv4public(const struct sockaddr *addr);

/**
 * Is the given address a link-local IPv6 address?  Returns false for IPv4
 * address.
 *
 * @param addr      address as struct sockaddr
 * @return          true, if \c addr is IPv6 link-local, false otherwise
 */
PRRTE_EXPORT bool prrte_net_addr_isipv6linklocal(const struct sockaddr *addr);

/**
 * Get string version of address
 *
 * Return the un-resolved address in a string format.  The string will
 * be returned in a per-thread static buffer and should not be freed
 * by the user.
 *
 * @param addr              struct sockaddr of address
 * @return                  literal representation of \c addr
 */
PRRTE_EXPORT char* prrte_net_get_hostname(const struct sockaddr *addr);


/**
 * Get port number from struct sockaddr
 *
 * Return the port number (as an integr) from either a struct
 * sockaddr_in or a struct sockaddr_in6.
 *
 * @param addr             struct sockaddr containing address
 * @return                 port number from \addr
 */
PRRTE_EXPORT int prrte_net_get_port(const struct sockaddr *addr);

/**
 * Test if a string is actually an IP address
 *
 * Returns true if the string is of IPv4 or IPv6 address form
 */
PRRTE_EXPORT bool prrte_net_isaddr(const char *name);

END_C_DECLS

#endif /* PRRTE_UTIL_NET_H */
