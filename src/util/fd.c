/*
 * Copyright (c) 2008-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2009 Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <ctype.h>

#include "src/util/fd.h"
#include "src/util/string_copy.h"
#include "constants.h"


/*
 * Simple loop over reading from a fd
 */
int prrte_fd_read(int fd, int len, void *buffer)
{
    int rc;
    char *b = buffer;

    while (len > 0) {
        rc = read(fd, b, len);
        if (rc < 0 && (EAGAIN == errno || EINTR == errno)) {
            continue;
        } else if (rc > 0) {
            len -= rc;
            b += rc;
        } else if (0 == rc) {
            return PRRTE_ERR_TIMEOUT;
        } else {
            return PRRTE_ERR_IN_ERRNO;
        }
    }
    return PRRTE_SUCCESS;
}


/*
 * Simple loop over writing to an fd
 */
int prrte_fd_write(int fd, int len, const void *buffer)
{
    int rc;
    const char *b = buffer;

    while (len > 0) {
        rc = write(fd, b, len);
        if (rc < 0 && (EAGAIN == errno || EINTR == errno)) {
            continue;
        } else if (rc > 0) {
            len -= rc;
            b += rc;
        } else {
            return PRRTE_ERR_IN_ERRNO;
        }
    }

    return PRRTE_SUCCESS;
}


int prrte_fd_set_cloexec(int fd)
{
#ifdef FD_CLOEXEC
    int flags;

    /* Stevens says that we should get the fd's flags before we set
       them.  So say we all. */
    flags = fcntl(fd, F_GETFD, 0);
    if (-1 == flags) {
        return PRRTE_ERR_IN_ERRNO;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC | flags) == -1) {
        return PRRTE_ERR_IN_ERRNO;
    }
#endif

    return PRRTE_SUCCESS;
}

bool prrte_fd_is_regular(int fd)
{
    struct stat buf;
    if (fstat(fd, &buf)) {
        return false;
    }
    return S_ISREG(buf.st_mode);
}

bool prrte_fd_is_chardev(int fd)
{
    struct stat buf;
    if (fstat(fd, &buf)) {
        return false;
    }
    return S_ISCHR(buf.st_mode);
}

bool prrte_fd_is_blkdev(int fd)
{
    struct stat buf;
    if (fstat(fd, &buf)) {
        return false;
    }
    return S_ISBLK(buf.st_mode);
}

const char *prrte_fd_get_peer_name(int fd)
{
    char *str;
    const char *ret = NULL;
    struct sockaddr sa;
    socklen_t slt = (socklen_t) sizeof(sa);

    int rc = getpeername(fd, &sa, &slt);
    if (0 != rc) {
        ret = strdup("Unknown");
        return ret;
    }

    size_t len = INET_ADDRSTRLEN;
#if PRRTE_ENABLE_IPV6
    len = INET6_ADDRSTRLEN;
#endif
    str = calloc(1, len);
    if (NULL == str) {
        return NULL;
    }

    if (sa.sa_family == AF_INET) {
        struct sockaddr_in *si;
        si = (struct sockaddr_in*) &sa;
        ret = inet_ntop(AF_INET, &(si->sin_addr), str, INET_ADDRSTRLEN);
        if (NULL == ret) {
            free(str);
        }
    }
#if PRRTE_ENABLE_IPV6
    else if (sa.sa_family == AF_INET6) {
        struct sockaddr_in6 *si6;
        si6 = (struct sockaddr_in6*) &sa;
        ret = inet_ntop(AF_INET6, &(si6->sin6_addr), str, INET6_ADDRSTRLEN);
        if (NULL == ret) {
            free(str);
        }
    }
#endif
    else {
        // This string is guaranteed to be <= INET_ADDRSTRLEN
        prrte_string_copy(str, "Unknown", len);
        ret = str;
    }

    return ret;
}

static int fdmax = -1;

/* close all open file descriptors w/ exception of stdin/stdout/stderr
   and the pipe up to the parent. */
void prrte_close_open_file_descriptors(int protected_fd)
{
    DIR *dir = opendir("/proc/self/fd");
    int fd;
    struct dirent *files;

    if (NULL == dir) {
        goto slow;
    }

    /* grab the fd of the opendir above so we don't close in the 
     * middle of the scan. */
    int dir_scan_fd = dirfd(dir);
    if(dir_scan_fd < 0 ) {
        goto slow;
    }

    
    while (NULL != (files = readdir(dir))) {
        if (!isdigit(files->d_name[0])) {
            continue;
        }
        int fd = strtol(files->d_name, NULL, 10);
        if (errno == EINVAL || errno == ERANGE) {
            closedir(dir);
            goto slow;
        }
        if (fd >=3 &&
            (-1 == protected_fd || fd != protected_fd) &&
            fd != dir_scan_fd) {
            close(fd);
        }
    }
    closedir(dir);
    return;

  slow:
    // close *all* file descriptors -- slow
    if (0 > fdmax) {
        fdmax = sysconf(_SC_OPEN_MAX);
    }
    for(fd=3; fd<fdmax; fd++) {
        if (fd != protected_fd) {
            close(fd);
        }
    }
}


