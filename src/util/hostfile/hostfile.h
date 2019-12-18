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
#ifndef PRRTE_UTIL_HOSTFILE_H
#define PRRTE_UTIL_HOSTFILE_H

#include "prrte_config.h"

#include "src/class/prrte_list.h"


BEGIN_C_DECLS

PRRTE_EXPORT int prrte_util_add_hostfile_nodes(prrte_list_t *nodes,
                                               char *hostfile);

PRRTE_EXPORT int prrte_util_filter_hostfile_nodes(prrte_list_t *nodes,
                                                  char *hostfile,
                                                  bool remove);

PRRTE_EXPORT int prrte_util_get_ordered_host_list(prrte_list_t *nodes,
                                                  char *hostfile);

END_C_DECLS

#endif
