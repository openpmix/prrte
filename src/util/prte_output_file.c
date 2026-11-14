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
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/proc_info.h"
#include "src/util/prte_output_file.h"

typedef struct {
    const char *path;
    mode_t mode;
} output_file_t;

/* The open, and the checks that decide whether what was opened may be
 * emptied - all made by whoever the file is written for, and so, when that
 * is another user, in a child that has become it (prte_pmix_server_open_as):
 * no output, no stdio, nothing but the calls. "Our own" file is then that
 * user's. */
static int output_file_open_fn(void *arg)
{
    output_file_t *of = (output_file_t *) arg;
    struct stat buf;
    int fd;

    /* opened without O_TRUNC: nothing is emptied until it has been
     * established that the file is the one we mean to replace */
    fd = pmix_os_dirpath_open_file(of->path, O_WRONLY | O_CREAT | O_CLOEXEC, of->mode);
    if (0 > fd) {
        return -1;
    }
    if (0 != fstat(fd, &buf) || !S_ISREG(buf.st_mode) || geteuid() != buf.st_uid ||
        1 != buf.st_nlink) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    if (0 != ftruncate(fd, 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

FILE *prte_output_file_open(const char *path, mode_t mode, struct stat *st)
{
    return prte_output_file_open_as(path, mode, st, prte_process_info.euid,
                                    prte_process_info.egid);
}

FILE *prte_output_file_open_as(const char *path, mode_t mode, struct stat *st, uid_t uid,
                               gid_t gid)
{
    output_file_t of;
    struct stat buf;
    FILE *fp;
    int fd;

    if (NULL == path || '\0' == path[0]) {
        errno = EINVAL;
        return NULL;
    }
    of.path = path;
    of.mode = mode;
    fd = prte_pmix_server_open_as(uid, gid, output_file_open_fn, &of);
    if (0 > fd) {
        return NULL;
    }
    if (0 != fstat(fd, &buf)) {
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
