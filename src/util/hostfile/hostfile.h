/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Resource Discovery (Hostfile)
 */
#ifndef PRTE_UTIL_HOSTFILE_H
#define PRTE_UTIL_HOSTFILE_H

#include "prte_config.h"

#include <sys/types.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* Both read the hostfile, and report what is wrong with it, for whoever
 * named it: req describes the requester (prte_requester_info_job() for a
 * job's owner, prte_requester_info_proc() for an allocation's requester),
 * whose user and group the file is opened as and to whom the messages go.
 * NULL, 0 is the DVM's own configuration - read as our own user, and the
 * messages go to everyone. See prte_textfile_open and prte_show_help_info. */
PRTE_EXPORT int prte_util_add_hostfile_nodes(pmix_list_t *nodes, char *hostfile,
                                             const pmix_info_t *req, size_t nreq);

PRTE_EXPORT int prte_util_filter_hostfile_nodes(pmix_list_t *nodes, char *hostfile, bool remove,
                                                const pmix_info_t *req, size_t nreq);

END_C_DECLS

#endif
