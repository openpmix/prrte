/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2018 Intel, Inc.  All rights reserved.
 *
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

#ifndef ORTE_QUIT_H
#define ORTE_QUIT_H

#include "orte_config.h"

#include "orte/runtime/orte_globals.h"

BEGIN_C_DECLS

ORTE_DECLSPEC void orte_quit(int fd, short args, void *cbdata);

ORTE_DECLSPEC char* orte_dump_aborted_procs(orte_job_t *jdata);

END_C_DECLS

#endif /* ORTE_CR_H */
