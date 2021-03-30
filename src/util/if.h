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
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* @file */

#ifndef PRTE_IF_UTIL_
#define PRTE_IF_UTIL_

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif

/*
 * We previously defined IF_NAMESIZE to 32 if not already defined.
 * Due to this macro being reused from net/if.h, we could encounter
 * a macro mismatch. In particular, in cases where src/util/if.h was
 * included, but net/if.h was not, IF_NAMESIZE would be 32. If net/if.h
 * was included on Linux systems, IF_NAMESIZE would be 16. To avoid this
 * issue, we define our own PRTE_IF_NAMESIZE macro instead.
 */
#define PRTE_IF_NAMESIZE 32

BEGIN_C_DECLS

#define PRTE_IF_FORMAT_ADDR(n)                                                         \
    (((n) >> 24) & 0x000000FF), (((n) >> 16) & 0x000000FF), (((n) >> 8) & 0x000000FF), \
        ((n) &0x000000FF)

#define PRTE_IF_ASSEMBLE_NETWORK(n1, n2, n3, n4)                                           \
    (((n1) << 24) & 0xFF000000) | (((n2) << 16) & 0x00FF0000) | (((n3) << 8) & 0x0000FF00) \
        | ((n4) &0x000000FF)

/**
 *  Lookup an interface by name and return its prte_list index.
 *
 *  @param if_name (IN)  Interface name
 *  @return              Interface prte_list index
 */
PRTE_EXPORT int prte_ifnametoindex(const char *if_name);

/**
 *  Lookup an interface by name and return its kernel index.
 *
 *  @param if_name (IN)  Interface name
 *  @return              Interface kernel index
 */
PRTE_EXPORT int prte_ifnametokindex(const char *if_name);

/**
 *  Returns the number of available interfaces.
 */
PRTE_EXPORT int prte_ifcount(void);

/**
 *  Returns the index of the first available interface.
 */
PRTE_EXPORT int prte_ifbegin(void);

/**
 *  Lookup the current position in the interface list by
 *  index and return the next available index (if it exists).
 *
 *  @param if_index   Returns the next available index from the
 *                    current position.
 */
PRTE_EXPORT int prte_ifnext(int if_index);

/**
 *  Lookup an interface by index and return its name.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_name (OUT)  Interface name buffer
 *  @param size (IN)      Interface name buffer size
 */
PRTE_EXPORT int prte_ifindextoname(int if_index, char *if_name, int);

/**
 *  Lookup an interface by index and return its primary address.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_name (OUT)  Interface address buffer
 *  @param size (IN)      Interface address buffer size
 */
PRTE_EXPORT int prte_ifindextoaddr(int if_index, struct sockaddr *, unsigned int);
PRTE_EXPORT int prte_ifkindextoaddr(int if_kindex, struct sockaddr *if_addr, unsigned int length);

/**
 *  Lookup an interface by index and return its flags.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_flags (OUT) Interface flags
 */
PRTE_EXPORT int prte_ifindextoflags(int if_index, uint32_t *);

/**
 * Determine if given hostname / IP address is a local address
 *
 * @param hostname (IN)    Hostname (or stringified IP address)
 * @return                 true if \c hostname is local, false otherwise
 */
PRTE_EXPORT bool prte_ifislocal(const char *hostname);

/**
 * Convert a dot-delimited network tuple to an IP address
 *
 * @param addr (IN) character string tuple
 * @param net (IN) Pointer to returned network address
 * @param mask (IN) Pointer to returned netmask
 * @return PRTE_SUCCESS if no problems encountered
 * @return PRTE_ERROR if data could not be released
 */
PRTE_EXPORT int prte_iftupletoaddr(const char *addr, uint32_t *net, uint32_t *mask);

/**
 * Determine if given interface is loopback
 *
 *  @param if_index (IN)  Interface index
 */
PRTE_EXPORT bool prte_ifisloopback(int if_index);

/*
 * Determine if a specified interface is included in a NULL-terminated argv array
 */
PRTE_EXPORT int prte_ifmatches(int kidx, char **nets);

/*
 * Provide a list of strings that contain all known aliases for this node
 */
PRTE_EXPORT void prte_ifgetaliases(char ***aliases);

END_C_DECLS

#endif
