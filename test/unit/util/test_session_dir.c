/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for claiming a top-level session directory.
 *
 * prte_session_dir_create() takes the name it is given when that name is
 * free or already holds a directory of ours that no one else can write
 * to, and otherwise makes a directory of its own beside it. Every case
 * runs inside a private scratch directory, never the real $TMPDIR.
 *
 * A directory owned by another user is the case that matters most and the
 * one that needs a second uid to set up; it is covered by the dockerswarm
 * suite, which can create one as root.
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "constants.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_printf.h"
#include "src/util/session_dir.h"

int test_session_dir(void);

#define CHECK(label, cond)                                                 \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL [session_dir:%s]: %s\n", label, #cond);  \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* is `path` a directory of ours, 0700? */
static bool private_dir(const char *path)
{
    struct stat st;

    return (0 == lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid()
            && 0700 == (st.st_mode & 07777));
}

/* did the claim move to "<wanted>.XXXXXX" - a sibling with the same prefix? */
static bool moved_beside(const char *wanted, const char *got)
{
    size_t len = strlen(wanted);

    return (0 == strncmp(got, wanted, len) && '.' == got[len] && 6 == strlen(got + len + 1));
}

int test_session_dir(void)
{
    int failures = 0, rc, fd;
    char base[] = "/tmp/prte_sessdir_test_XXXXXX";
    char *wanted = NULL, *dir = NULL;
    bool created;

    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "FAIL [session_dir:mkdtemp]: %s\n", strerror(errno));
        return 1;
    }
    if (0 > pmix_asprintf(&wanted, "%s/prte.4242", base) || NULL == wanted) {
        return 1;
    }

    /* a free name is taken as it is */
    dir = strdup(wanted);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("a free name is claimed", PRTE_SUCCESS == rc && 0 == strcmp(dir, wanted));
    CHECK("...and created, 0700", created && private_dir(dir));

    /* claiming it again finds our own directory and keeps it */
    rc = prte_session_dir_create(&dir, &created);
    CHECK("our own directory is reused", PRTE_SUCCESS == rc && 0 == strcmp(dir, wanted));
    CHECK("...and was not created this time", !created);
    free(dir);

    fprintf(stdout, "-- the next cases claim names that are taken --\n");

    /* a directory of ours that others can write to is not used */
    CHECK("chmod", 0 == chmod(wanted, 0777));
    dir = strdup(wanted);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("an open directory is passed over", PRTE_SUCCESS == rc && moved_beside(wanted, dir));
    CHECK("...for a private one beside it", created && private_dir(dir));
    (void) pmix_os_dirpath_destroy(dir, true, NULL);
    free(dir);
    CHECK("rmdir", 0 == rmdir(wanted));

    /* nor is a file */
    fd = open(wanted, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK("create file", 0 <= fd);
    if (0 <= fd) {
        close(fd);
    }
    dir = strdup(wanted);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("a file at the name is passed over", PRTE_SUCCESS == rc && moved_beside(wanted, dir));
    CHECK("...for a directory beside it", created && private_dir(dir));
    (void) pmix_os_dirpath_destroy(dir, true, NULL);
    free(dir);
    unlink(wanted);

    /* nor a symlink, even to a directory of ours */
    {
        char *target = NULL;
        CHECK("asprintf", 0 <= pmix_asprintf(&target, "%s/real", base));
        CHECK("mkdir target", 0 == mkdir(target, 0700));
        CHECK("symlink", 0 == symlink(target, wanted));
        dir = strdup(wanted);
        rc = prte_session_dir_create(&dir, &created);
        CHECK("a symlink at the name is passed over",
              PRTE_SUCCESS == rc && moved_beside(wanted, dir));
        CHECK("...for a real directory beside it", created && private_dir(dir));
        (void) pmix_os_dirpath_destroy(dir, true, NULL);
        free(dir);
        unlink(wanted);
        rmdir(target);
        free(target);
    }

    /* and when nothing is there, failing to create it is an error, not a
     * reason to look elsewhere - root can create it anyway, so there is
     * nothing to show then */
    if (0 != geteuid()) {
        char *ro = NULL, *sub = NULL;
        CHECK("asprintf", 0 <= pmix_asprintf(&ro, "%s/ro", base));
        CHECK("mkdir ro", 0 == mkdir(ro, 0500));
        CHECK("asprintf", 0 <= pmix_asprintf(&sub, "%s/prte.4242", ro));
        dir = strdup(sub);
        rc = prte_session_dir_create(&dir, &created);
        CHECK("an uncreatable name is an error", PRTE_SUCCESS != rc);
        CHECK("...with no directory made", !created && 0 == strcmp(dir, sub));
        free(dir);
        chmod(ro, 0700);
        rmdir(ro);
        free(ro);
        free(sub);
    }

    (void) pmix_os_dirpath_destroy(base, true, NULL);
    free(wanted);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_session_dir\n");
    }
    return failures;
}
