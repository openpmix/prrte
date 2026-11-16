/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/util/prte_config_file.h"

/* Whoever may write a file's directory may replace the file, between one
 * read and the next, so a file is only as sound as the directory it is
 * in. The sticky bit limits that to a file's own owner, which the file's
 * test already decides. NULL when the directory is sound. */
static const char *directory_unsound(const char *path)
{
    struct stat st;
    char *copy, *dir;
    const char *why = NULL;

    copy = strdup(path);
    if (NULL == copy) {
        return "its directory could not be checked";
    }
    dir = dirname(copy);
    if (0 != stat(dir, &st)) {
        why = "its directory could not be checked";
    } else if (geteuid() != st.st_uid && 0 != st.st_uid) {
        why = "its directory is owned by another user";
    } else if (0 != (st.st_mode & (S_IWGRP | S_IWOTH)) && 0 == (st.st_mode & S_ISVTX)) {
        why = "its directory's group or others can write it";
    }
    free(copy);
    return why;
}

FILE *prte_open_config_file(const char *path, const char **why)
{
    struct stat st;
    int fd, flags;
    FILE *fp;

    *why = NULL;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (0 > fd) {
        /* FreeBSD reports a link refused by O_NOFOLLOW as EMLINK */
        if (ELOOP == errno || EMLINK == errno) {
            *why = "it is a symbolic link";
        }
        return NULL;
    }
    if (0 != fstat(fd, &st)) {
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        *why = "it is not a regular file";
    } else if (1 != st.st_nlink) {
        *why = "it has more than one name";
    } else if (0 != (st.st_mode & (S_IWGRP | S_IWOTH))) {
        *why = "its group or others can write it";
    } else if (geteuid() != st.st_uid && 0 != st.st_uid) {
        *why = "it is owned by another user";
    } else {
        *why = directory_unsound(path);
    }
    if (NULL != *why) {
        close(fd);
        return NULL;
    }
    /* a regular file never blocks, and stdio does not expect EAGAIN */
    flags = fcntl(fd, F_GETFL);
    if (0 > flags || 0 > fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) ||
        NULL == (fp = fdopen(fd, "r"))) {
        close(fd);
        return NULL;
    }
    return fp;
}
