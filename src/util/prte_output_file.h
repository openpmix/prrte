/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* A file a tool writes out at a path the user gave it - the pid of
 * --report-pid, the proctable - and removes again when it is done. */

#ifndef PRTE_OUTPUT_FILE_H
#define PRTE_OUTPUT_FILE_H

#include "prte_config.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

BEGIN_C_DECLS

/* Open path to write it afresh: created with mode if it is not there,
 * emptied if it is. The name itself is not followed if it is a symlink,
 * and what is there already is used only if it is a regular file of our
 * own with no other links. On success *st, if given, records which file
 * it was (for prte_output_file_remove). NULL with errno set otherwise. */
PRTE_EXPORT FILE *prte_output_file_open(const char *path, mode_t mode, struct stat *st);

/* Write what content holds to path afresh, as prte_output_file_open()
 * would open it, but done as user uid, group gid - the owner of the job
 * that named path - so that it is created, checked, emptied and written as
 * that user: "of our own" above means of that user's, and the kernel
 * resolves the path with that user's permissions. All of it happens in a
 * child (prte_pmix_server_work_as) that the master waits for no longer
 * than PMIx's pmix_runas_timeout, since the path may be on a filesystem
 * that does not answer; content is a temporary file of ours (tmpfile())
 * that the child inherits and copies from the start. For a file the DVM
 * master writes for a job, the proctable. 0 on success, -1 with errno set
 * otherwise - ETIMEDOUT when the child did not finish in time. */
PRTE_EXPORT int prte_output_file_write_as(const char *path, mode_t mode, uid_t uid, gid_t gid,
                                          FILE *content);

/* Remove path - but only if it is still the file st recorded. */
PRTE_EXPORT void prte_output_file_remove(const char *path, const struct stat *st);

END_C_DECLS

#endif /* PRTE_OUTPUT_FILE_H */
