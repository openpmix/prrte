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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/prte_output_file.h"

FILE *prte_output_file_open(const char *path, mode_t mode, struct stat *st)
{
    struct stat buf;
    FILE *fp;
    int fd;

    if (NULL == path || '\0' == path[0]) {
        errno = EINVAL;
        return NULL;
    }
    /* opened without O_TRUNC: nothing is emptied until it has been
     * established that the file is the one we mean to replace */
    fd = pmix_os_dirpath_open_file(path, O_WRONLY | O_CREAT | O_CLOEXEC, mode);
    if (0 > fd) {
        return NULL;
    }
    if (0 != fstat(fd, &buf)) {
        close(fd);
        return NULL;
    }
    if (!S_ISREG(buf.st_mode) || geteuid() != buf.st_uid || 1 != buf.st_nlink) {
        close(fd);
        errno = EPERM;
        return NULL;
    }
    if (0 != ftruncate(fd, 0)) {
        close(fd);
        return NULL;
    }
    fp = fdopen(fd, "w");
    if (NULL == fp) {
        close(fd);
        return NULL;
    }
    if (NULL != st) {
        *st = buf;
    }
    return fp;
}

void prte_output_file_remove(const char *path, const struct stat *st)
{
    struct stat buf;

    if (NULL == path || NULL == st) {
        return;
    }
    if (0 == lstat(path, &buf) && S_ISREG(buf.st_mode) &&
        buf.st_dev == st->st_dev && buf.st_ino == st->st_ino) {
        (void) unlink(path);
    }
}
