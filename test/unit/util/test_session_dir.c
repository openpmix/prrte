/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for making a top-level session directory.
 *
 * prte_session_dir_create() makes "<stem>.XXXXXX" with mkdtemp(), mode
 * 0700, and puts an empty "node.<nodename>" file in it; every call makes
 * a directory of its own, whatever is already there. At finalize the
 * marker is kept while anything else is kept in the directory, and goes
 * with it otherwise. Every case runs inside a private scratch directory,
 * never the real $TMPDIR.
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
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"
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

/* is `got` "<stem>.XXXXXX" - the stem and a six-character suffix? */
static bool made_from(const char *stem, const char *got)
{
    size_t len = strlen(stem);

    return (0 == strncmp(got, stem, len) && '.' == got[len] && 6 == strlen(got + len + 1));
}

/* is there an empty regular file `name` in `dir`? */
static bool has_marker(const char *dir, const char *name)
{
    struct stat st;
    char *path = pmix_os_path(false, dir, name, NULL);
    bool ok;

    ok = (NULL != path && 0 == lstat(path, &st) && S_ISREG(st.st_mode) && 0 == st.st_size);
    free(path);
    return ok;
}

/* finalize the top-level directory `dir` as the DVM job does - which
 * frees the name it is given, so it is given a copy */
static void finalize_top(const char *dir)
{
    char *save_top = prte_process_info.top_session_dir;
    bool save_fin = prte_finalizing;
    bool save_rm = prte_process_info.rm_session_dirs;

    prte_process_info.top_session_dir = strdup(dir);
    prte_process_info.rm_session_dirs = false;
    prte_finalizing = true;
    prte_job_session_dir_finalize(NULL);
    prte_finalizing = save_fin;
    prte_process_info.rm_session_dirs = save_rm;
    prte_process_info.top_session_dir = save_top;
}

int test_session_dir(void)
{
    static char node7[] = "node7", racknode7[] = "rack/node7";
    int failures = 0, rc, fd;
    char base[] = "/tmp/prte_sessdir_test_XXXXXX";
    char *stem = NULL, *dir = NULL, *dir2 = NULL, *save_node, *path = NULL;
    bool created;

    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "FAIL [session_dir:mkdtemp]: %s\n", strerror(errno));
        return 1;
    }
    if (0 > pmix_asprintf(&stem, "%s/prte", base) || NULL == stem) {
        return 1;
    }
    save_node = prte_process_info.nodename;
    prte_process_info.nodename = node7;

    /* a directory of its own, private, marked with the node */
    dir = strdup(stem);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("a directory is made", PRTE_SUCCESS == rc && made_from(stem, dir));
    CHECK("...created, 0700", created && private_dir(dir));
    CHECK("...with the node's marker in it", has_marker(dir, "node.node7"));

    /* a second instance gets a second directory */
    dir2 = strdup(stem);
    rc = prte_session_dir_create(&dir2, &created);
    CHECK("a second instance is made its own directory",
          PRTE_SUCCESS == rc && made_from(stem, dir2) && 0 != strcmp(dir, dir2));
    CHECK("...created, 0700", created && private_dir(dir2));
    (void) pmix_os_dirpath_destroy(dir2, true, NULL);
    free(dir2);

    /* a directory of ours already at the stem is not taken over */
    CHECK("mkdir stem", 0 == mkdir(stem, 0700));
    dir2 = strdup(stem);
    rc = prte_session_dir_create(&dir2, &created);
    CHECK("a directory at the stem is not taken over",
          PRTE_SUCCESS == rc && made_from(stem, dir2) && created);
    (void) pmix_os_dirpath_destroy(dir2, true, NULL);
    free(dir2);
    CHECK("rmdir stem", 0 == rmdir(stem));

    /* finalized with nothing kept, the directory goes, marker and all */
    CHECK("asprintf", 0 <= pmix_asprintf(&path, "%s/1", dir));
    CHECK("mkdir job dir", 0 == mkdir(path, 0700));
    free(path);
    finalize_top(dir);
    CHECK("a directory with nothing kept is removed", 0 != access(dir, F_OK));
    free(dir);

    /* finalized with an output file kept, the marker stays with it */
    dir = strdup(stem);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("another directory is made", PRTE_SUCCESS == rc);
    CHECK("asprintf", 0 <= pmix_asprintf(&path, "%s/output-1", dir));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK("create output file", 0 <= fd && 1 == write(fd, "x", 1));
    if (0 <= fd) {
        close(fd);
    }
    free(path);
    finalize_top(dir);
    CHECK("a directory with an output file kept stays", private_dir(dir));
    CHECK("...and keeps its node's marker", has_marker(dir, "node.node7"));
    (void) pmix_os_dirpath_destroy(dir, true, NULL);
    free(dir);

    /* a '/' in the node's name does not leave the directory */
    prte_process_info.nodename = racknode7;
    dir = strdup(stem);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("a node name with a '/' is marked",
          PRTE_SUCCESS == rc && has_marker(dir, "node.rack_node7"));
    (void) pmix_os_dirpath_destroy(dir, true, NULL);
    free(dir);

    /* with no node name there is nothing to mark it with */
    prte_process_info.nodename = NULL;
    dir = strdup(stem);
    rc = prte_session_dir_create(&dir, &created);
    CHECK("no node name is an error", PRTE_SUCCESS != rc && !created && 0 == strcmp(dir, stem));
    free(dir);
    prte_process_info.nodename = node7;

    /* and a directory that cannot be made is an error, with nothing
     * made - root can make it anyway, so there is nothing to show then */
    if (0 != geteuid()) {
        char *ro = NULL, *sub = NULL;
        CHECK("asprintf", 0 <= pmix_asprintf(&ro, "%s/ro", base));
        CHECK("mkdir ro", 0 == mkdir(ro, 0500));
        CHECK("asprintf", 0 <= pmix_asprintf(&sub, "%s/prte", ro));
        dir = strdup(sub);
        rc = prte_session_dir_create(&dir, &created);
        CHECK("an uncreatable directory is an error", PRTE_SUCCESS != rc);
        CHECK("...with no directory made", !created && 0 == strcmp(dir, sub));
        free(dir);
        chmod(ro, 0700);
        rmdir(ro);
        free(ro);
        free(sub);
    }

    prte_process_info.nodename = save_node;
    (void) pmix_os_dirpath_destroy(base, true, NULL);
    free(stem);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_session_dir\n");
    }
    return failures;
}
