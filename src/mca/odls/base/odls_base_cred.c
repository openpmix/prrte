/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Starting a child as the user its job belongs to.
 *
 * Who that is, and its groups, is settled on the progress thread before the
 * fork (spawn_caddy_credentials in odls_base_default_fns.c). What is left
 * for the child is the three calls that change its credentials, and a check
 * that they took. It runs in the window between fork() and execve(), so it
 * makes only system calls: no lookup, no allocation, no message - a failure
 * is a fixed record up the pipe, rendered by the parent. */

#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#ifdef HAVE_GRP_H
#    include <grp.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/odls/base/base.h"

void prte_odls_base_set_credentials(prte_odls_spawn_caddy_t *cd, int write_fd)
{
    if (!cd->cred_switch) {
        return;
    }

    /* Groups first, then the group, then the user: each needs the
     * privilege the next one gives up. setgroups() is not on POSIX's list
     * of async-signal-safe calls, but it is a single system call that takes
     * no lock - the forked child has one thread, and the list was built
     * before the fork. */
    if (0 != setgroups(cd->cred_ngids, cd->cred_gids)) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_CREDENTIALS, errno);
    }
    if (0 != setgid(cd->cred_gid)) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_CREDENTIALS, errno);
    }
    if (0 != setuid(cd->cred_uid)) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_CREDENTIALS, errno);
    }

    /* Make sure it took. Called as root, setuid() sets the real, effective
     * and saved user alike, and setgid() the same for the group - so all of
     * them must now name the owner, and an owner other than root must not
     * be able to take root back. */
    if (getuid() != cd->cred_uid || geteuid() != cd->cred_uid ||
        getgid() != cd->cred_gid || getegid() != cd->cred_gid) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_CREDENTIALS, EPERM);
    }
    if (0 != cd->cred_uid && 0 == setuid(0)) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_CREDENTIALS, EPERM);
    }
}
