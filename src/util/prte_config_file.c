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
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_DLFCN_H
#    include <dlfcn.h>
#endif

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

/*
 * Did uid have the means to change the code this process runs?
 *
 * Besides root and us, a configuration file may be owned by the owner of
 * the file holding the PRRTE library we have loaded - or of the
 * executable, when the library is linked into it - or by the owner of a
 * directory above it, since the owner of a directory can replace what is
 * in it. Each of them could have replaced that library before we started,
 * so a file one of them wrote changes nothing they could not already have
 * changed. That admits the files of an installation made by one user and
 * used by others, and a build tree run by root, without admitting anyone
 * new. The rule PMIx applies to a parameter file.
 *
 * Only a path that names the file actually mapped is used. On Linux that
 * is the kernel's own, from /proc/self/maps: dladdr() there reports
 * argv[0] for a symbol in the main program, and argv[0] is whatever our
 * caller chose. Elsewhere it is dladdr()'s, if absolute, and the path it
 * resolves to as well. When neither can be had, nobody is admitted. Owners
 * only: the group and mode of those directories add nobody. Asked only of
 * a file owned by neither root nor us, so the usual case costs nothing.
 */

/* an address inside this library: initialized read-only data, so it is
 * mapped from the library's file and not anonymous memory */
static const char code_owner_marker = 1;

/* does uid own path or a directory above it? */
static bool owns_path(uid_t uid, const char *path)
{
    char buf[PATH_MAX];
    struct stat st;
    char *slash;

    if ('/' != path[0] || sizeof(buf) <= strlen(path)) {
        return false;
    }
    strcpy(buf, path);
    while (1) {
        if (0 == lstat(buf, &st) && uid == st.st_uid) {
            return true;
        }
        slash = strrchr(buf, '/');
        if (NULL == slash) {
            return false;
        }
        if (slash == buf) {
            if ('\0' == buf[1]) {
                return false;
            }
            buf[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

#if defined(__linux__)
/* the file mapped at addr, as the kernel names it */
static bool mapped_file(const void *addr, char *path, size_t len)
{
    FILE *fp;
    char line[PATH_MAX + 128];
    unsigned long start, end, inode;
    int n;
    char *name, *nl;
    bool found = false;

    fp = fopen("/proc/self/maps", "re");
    if (NULL == fp) {
        return false;
    }
    while (NULL != fgets(line, sizeof(line), fp)) {
        n = 0;
        if (3 != sscanf(line, "%lx-%lx %*s %*s %*s %lu %n", &start, &end, &inode, &n) ||
            0 == n) {
            continue;
        }
        if ((uintptr_t) addr < start || (uintptr_t) addr >= end) {
            continue;
        }
        name = line + n;
        if (NULL != (nl = strchr(name, '\n'))) {
            *nl = '\0';
        }
        /* a file since removed or replaced names nothing we can check */
        if (0 != inode && '/' == name[0] && NULL == strstr(name, " (deleted)") &&
            len > strlen(name)) {
            strcpy(path, name);
            found = true;
        }
        break;
    }
    fclose(fp);
    return found;
}
#endif

static bool code_owner(uid_t uid)
{
#if defined(__linux__)
    char path[PATH_MAX];

    return mapped_file((const void *) &code_owner_marker, path, sizeof(path)) &&
           owns_path(uid, path);
#elif defined(HAVE_DLFCN_H) && defined(HAVE_DLADDR)
    Dl_info info;
    char real[PATH_MAX];

    if (0 == dladdr((const void *) &code_owner_marker, &info) || NULL == info.dli_fname ||
        '/' != info.dli_fname[0]) {
        return false;
    }
    if (owns_path(uid, info.dli_fname)) {
        return true;
    }
    return NULL != realpath(info.dli_fname, real) && owns_path(uid, real);
#else
    (void) uid;
    return false;
#endif
}

/* may a file, or a directory, owned by uid be taken? */
static bool owner_sound(uid_t uid)
{
    return 0 == uid || geteuid() == uid || code_owner(uid);
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
    } else if (!owner_sound(st.st_uid)) {
        why = "its directory is owned by a user who owns neither the library we run nor a "
              "directory above it";
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
    } else if (!owner_sound(st.st_uid)) {
        *why = "it is owned by a user who owns neither the library we run nor a directory "
               "above it";
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
