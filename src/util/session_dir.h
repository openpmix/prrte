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
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * Find and/or create PRTE session directory.
 *
 */

#ifndef PRTE_SESSION_DIR_H_HAS_BEEN_INCLUDED
#define PRTE_SESSION_DIR_H_HAS_BEEN_INCLUDED

#include "prte_config.h"
#include "types.h"
#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

/** @param proc    Pointer to a process name for which the session
 *                dir name is desired. Passing:
 *
 *                PRTE_NAME_INVALID - top-level session directory
 *                will be created.
 *
 *                PRTE_NAME_WILDCARD - job-level session directory
 *                will be created
 *
 *                Valid procID - proc-level session directory will
 *                be created
 *
 *@retval PRTE_SUCCESS The directory was found and/or created with
 *                the proper permissions.
 * @retval PRTE_ERROR The directory cannot be found or created
 */
PRTE_EXPORT int prte_session_dir(pmix_proc_t *proc);

/**
 * Create a directory PRRTE names for itself under a shared temporary
 * directory, applying the session-directory rule: one already there -
 * the name is predictable, so it need not be one we made - is used only
 * if it is ours and no one else can write to it.
 *
 * When something we cannot use already holds the name, a directory is
 * made beside it instead, named "<directory>.XXXXXX" by mkdtemp(), and
 * *directory is replaced with that name. Nothing a caller does depends
 * on the name itself - it is handed on to PMIx and to everything else by
 * value - so the only effect of the fallback is that startup succeeds.
 *
 * @param directory The directory; on success, the one actually used,
 *                  which the caller owns either way.
 * @param created   If not NULL, set true only if this call made it, so
 *                  the caller knows it is the one to remove it.
 * @retval PRTE_SUCCESS    It is ours to use.
 * @retval PRTE_ERR_SILENT Neither the name nor an alternative could be
 *                         had; the reason has already been shown.
 */
PRTE_EXPORT int prte_session_dir_create(char **directory, bool *created);

/** The session_dir_finalize functions perform a cleanup of the
 * relevant session directory tree.
 */

PRTE_EXPORT void prte_job_session_dir_finalize(prte_job_t *jdata);

END_C_DECLS

#endif /* PRTE_SESSION_DIR_H_HAS_BEEN_INCLUDED */
