/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_NIDMAP_H
#define PRRTE_NIDMAP_H

#include "prrte_config.h"

#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss_types.h"
#include "src/runtime/prrte_globals.h"

/* pass info about the nodes in an allocation */
PRRTE_EXPORT int prrte_util_nidmap_create(prrte_pointer_array_t *pool,
                                          prrte_buffer_t *buf);

PRRTE_EXPORT int prrte_util_decode_nidmap(prrte_buffer_t *buf);


/* pass topology and #slots info */
PRRTE_EXPORT int prrte_util_pass_node_info(prrte_buffer_t *buf);

PRRTE_EXPORT int prrte_util_parse_node_info(prrte_buffer_t *buf);


/* pass info about node assignments for a specific job */
PRRTE_EXPORT int prrte_util_generate_ppn(prrte_job_t *jdata,
                                         prrte_buffer_t *buf);

PRRTE_EXPORT int prrte_util_decode_ppn(prrte_job_t *jdata,
                                       prrte_buffer_t *buf);

#endif /* PRRTE_NIDMAP_H */
