/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <stdio.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "constants.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/few.h"

int prte_few(char *argv[], int *status)
{
#if defined(HAVE_FORK) && defined(HAVE_EXECVE) && defined(HAVE_WAITPID)
    pid_t pid, ret;

    if ((pid = fork()) < 0) {
        return PRTE_ERR_IN_ERRNO;
    }

    /* Child execs.  If it fails to exec, exit. */

    else if (0 == pid) {
        execvp(argv[0], argv);
        exit(errno);
    }

    /* Parent loops waiting for the child to die. */

    else {
        do {
            /* If the child exited, return */

            if (pid == (ret = waitpid(pid, status, 0))) {
                break;
            }

            /* If waitpid was interrupted, loop around again */

            else if (ret < 0) {
                if (EINTR == errno) {
                    continue;
                }

                /* Otherwise, some bad juju happened -- need to quit */

                return PRTE_ERR_IN_ERRNO;
            }
        } while (true);
    }

    /* Return the status to the caller */

    return PRTE_SUCCESS;
#else
    return PRTE_ERR_NOT_SUPPORTED;
#endif
}
