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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/util/prte_config_file.h"

/* Is gid one of ours? The kernel holds our groups, so this needs no
 * lookup. */
static bool our_group(gid_t gid)
{
    gid_t *groups;
    int n, i;
    bool found = false;

    if (gid == getegid()) {
        return true;
    }
    n = getgroups(0, NULL);
    if (0 >= n || NULL == (groups = (gid_t *) malloc((size_t) n * sizeof(gid_t)))) {
        return false;
    }
    n = getgroups(n, groups);
    for (i = 0; i < n; i++) {
        if (groups[i] == gid) {
            found = true;
            break;
        }
    }
    free(groups);
    return found;
}

/* May its group write what st describes? Only a group we are in, and not
 * at all when we run as root: the members of a group root belongs to can
 * be ordinary users. The rule PMIx applies to a parameter file. */
static bool group_write_sound(const struct stat *st)
{
    return 0 == (st->st_mode & S_IWGRP) || (0 != geteuid() && our_group(st->st_gid));
}

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
    } else if (0 == (st.st_mode & S_ISVTX) &&
               (0 != (st.st_mode & S_IWOTH) || !group_write_sound(&st))) {
        why = (0 != (st.st_mode & S_IWOTH)) ? "its directory can be written by anyone"
              : (0 == geteuid()) ? "its directory can be written by its group, and we run as root"
                                 : "its directory can be written by a group we are not in";
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
    } else if (0 != (st.st_mode & S_IWOTH)) {
        *why = "it can be written by anyone";
    } else if (!group_write_sound(&st)) {
        *why = (0 == geteuid()) ? "it can be written by its group, and we run as root"
                                : "it can be written by a group we are not in";
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
