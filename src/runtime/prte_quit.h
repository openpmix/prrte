/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#ifndef PRTE_QUIT_H
#define PRTE_QUIT_H

#include "prte_config.h"

#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

PRTE_EXPORT void prte_quit(int fd, short args, void *cbdata);

PRTE_EXPORT char *prte_dump_aborted_procs(prte_job_t *jdata);

/* Render the diagnostic for a process that failed to launch, from plain
 * values rather than from the runtime objects - so a tool, which has none
 * of those, can produce the message itself and sign it with its own name.
 * Returns an allocated string (caller frees), or NULL if there is nothing
 * to say for this code. */
PRTE_EXPORT char *prte_render_launch_failure(int exit_code, const char *app, const char *cwd,
                                             const char *nodename, pmix_rank_t rank);

END_C_DECLS

#endif /* PRTE_CR_H */
