/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Data server for PRRTE
 */
#ifndef PRRTE_DATA_SERVER_H
#define PRRTE_DATA_SERVER_H

#include "prrte_config.h"
#include "types.h"

#include "src/dss/dss_types.h"
#include "src/mca/rml/rml_types.h"

BEGIN_C_DECLS

#define PRRTE_PMIX_PUBLISH_CMD           0x01
#define PRRTE_PMIX_LOOKUP_CMD            0x02
#define PRRTE_PMIX_UNPUBLISH_CMD         0x03
#define PRRTE_PMIX_PURGE_PROC_CMD        0x04

/* provide hooks to startup and finalize the data server */
PRRTE_EXPORT int prrte_data_server_init(void);
PRRTE_EXPORT void prrte_data_server_finalize(void);

/* provide hook for the non-blocking receive */
PRRTE_EXPORT void prrte_data_server(int status, prrte_process_name_t* sender,
                                    prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                                    void* cbdata);

END_C_DECLS

#endif /* PRRTE_DATA_SERVER_H */
