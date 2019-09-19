/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
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

#ifndef PRRTED_H
#define PRRTED_H

#include "prrte_config.h"
#include "types.h"

#include <time.h>

#include "src/dss/dss_types.h"
#include "src/class/prrte_pointer_array.h"
#include "src/mca/rml/rml_types.h"

BEGIN_C_DECLS

/* main orted routine */
PRRTE_EXPORT int prrte_daemon(int argc, char *argv[]);

/* orted communication functions */
PRRTE_EXPORT void prrte_daemon_recv(int status, prrte_process_name_t* sender,
                      prrte_buffer_t *buffer, prrte_rml_tag_t tag,
                      void* cbdata);

/* direct cmd processing entry points */
PRRTE_EXPORT void prrte_daemon_cmd_processor(int fd, short event, void *data);
PRRTE_EXPORT int prrte_daemon_process_commands(prrte_process_name_t* sender,
                                               prrte_buffer_t *buffer,
                                               prrte_rml_tag_t tag);

END_C_DECLS

/* Local function */
int send_to_local_applications(prrte_pointer_array_t *dead_names);

#endif /* PRRTED_H */
