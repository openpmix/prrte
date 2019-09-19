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
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 */
#ifndef PRRTE_UTIL_DASH_HOST_H
#define PRRTE_UTIL_DASH_HOST_H

#include "prrte_config.h"

#include "src/class/prrte_list.h"

#include "src/runtime/prrte_globals.h"

BEGIN_C_DECLS

PRRTE_EXPORT int prrte_util_add_dash_host_nodes(prrte_list_t *nodes,
                                                char *hosts,
                                                bool allocating);

PRRTE_EXPORT int prrte_util_filter_dash_host_nodes(prrte_list_t *nodes,
                                                   char *hosts,
                                                   bool remove);

PRRTE_EXPORT int prrte_util_get_ordered_dash_host_list(prrte_list_t *nodes,
                                                       char *hosts);

PRRTE_EXPORT int prrte_util_dash_host_compute_slots(prrte_node_t *node, char *hosts);

END_C_DECLS

#endif
