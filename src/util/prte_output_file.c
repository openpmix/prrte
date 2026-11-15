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
 * is another user, in a child that has become it (prte_pmix_server_open_as,
 * prte_pmix_server_work_as):
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
    fd = prte_pmix_server_open_as(prte_process_info.euid, prte_process_info.egid,
                                  output_file_open_fn, &of);
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

typedef struct {
    output_file_t of;
    int content; /* the master's temporary file, inherited by the child */
} output_file_copy_t;

/* The open, its checks, and the whole write, made as the user the file is
 * written for, in a child (prte_pmix_server_work_as): the content is read
 * from the temporary file by position, so nothing depends on the
 * descriptor's shared offset */
static int output_file_copy_fn(void *arg)
{
    output_file_copy_t *oc = (output_file_copy_t *) arg;
    char buf[16384];
    off_t in = 0;
    ssize_t nr, nw, done;
    int fd;

    fd = output_file_open_fn(&oc->of);
    if (0 > fd) {
        return -1;
    }
    for (;;) {
        do {
            nr = pread(oc->content, buf, sizeof(buf), in);
        } while (0 > nr && EINTR == errno);
        if (0 > nr) {
            close(fd);
            return -1;
        }
        if (0 == nr) {
            break;
        }
        for (done = 0; done < nr; done += nw) {
            do {
                nw = write(fd, buf + done, (size_t) (nr - done));
            } while (0 > nw && EINTR == errno);
            if (0 >= nw) {
                close(fd);
                return -1;
            }
        }
        in += nr;
    }
    if (0 != close(fd)) {
        return -1;
    }
    return 0;
}

int prte_output_file_write_as(const char *path, mode_t mode, uid_t uid, gid_t gid, FILE *content)
{
    output_file_copy_t oc;

    if (NULL == path || '\0' == path[0] || NULL == content) {
        errno = EINVAL;
        return -1;
    }
    if (0 != fflush(content)) {
        return -1;
    }
    oc.of.path = path;
    oc.of.mode = mode;
    oc.content = fileno(content);
    return prte_pmix_server_work_as(uid, gid, output_file_copy_fn, &oc);
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
