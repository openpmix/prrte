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

/* The same, done as user uid, group gid - the owner of the job that named
 * path - so that it is created, checked and emptied as that user: "of our
 * own" above means of that user's, and the kernel resolves the path with
 * that user's permissions (prte_pmix_server_open_as). */
PRTE_EXPORT FILE *prte_output_file_open_as(const char *path, mode_t mode, struct stat *st,
                                           uid_t uid, gid_t gid);

/* Remove path - but only if it is still the file st recorded. */
PRTE_EXPORT void prte_output_file_remove(const char *path, const struct stat *st);

END_C_DECLS

#endif /* PRTE_OUTPUT_FILE_H */
