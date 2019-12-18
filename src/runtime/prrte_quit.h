/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
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

#ifndef PRRTE_QUIT_H
#define PRRTE_QUIT_H

#include "prrte_config.h"

#include "src/runtime/prrte_globals.h"

BEGIN_C_DECLS

PRRTE_EXPORT void prrte_quit(int fd, short args, void *cbdata);

PRRTE_EXPORT char* prrte_dump_aborted_procs(prrte_job_t *jdata);

END_C_DECLS

#endif /* PRRTE_CR_H */
