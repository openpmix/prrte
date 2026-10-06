/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the filem framework.
 *
 * The substantive work of filem -- staging bytes across a live DVM
 * (raw_preposition_files / recv_files / write_handler) and symlinking
 * them into per-proc session directories (raw_link_local_files) -- is
 * asynchronous, progress-thread, multi-node I/O and cannot run without a
 * live DVM; it is covered by the integration harnesses.
 *
 * What *can* be exercised in isolation are the three pieces the base owns
 * with no I/O:
 *
 *   1. The three reference-counted request classes
 *      (prte_filem_base_process_set_t / _file_set_t / _request_t): their
 *      constructors must establish the documented defaults and their
 *      destructors must drain their lists and free their strings without
 *      crashing.
 *
 *   2. The default "none" module that filem_base_frame.c installs into
 *      the global prte_filem when no component is selected.  Every one of
 *      its entry points must be a safe no-op that returns PRTE_SUCCESS,
 *      preposition must still fire its completion callback (or the job
 *      would wedge forever at VM_READY), and -- the point of a recent
 *      fix -- the fault_handler slot must be non-NULL and callable, since
 *      routed_radix.c invokes prte_filem.fault_handler() unconditionally
 *      on every daemon-fault recovery even when filem is "none".
 *
 *   3. The naming rules that keep a preloaded file inside the directory it
 *      is meant to land in -- what gets stripped, what gets refused, and
 *      what a dot-file must survive -- plus the shell quoting the archive
 *      commands depend on.  These are the whole of filem's path-safety
 *      property and they are pure functions, so nothing about them needs a
 *      DVM to check.
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
#include "src/runtime/runtime.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_printf.h"
#include "src/util/proc_info.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* completion-callback bookkeeping for the preposition no-op test */
static bool cb_fired = false;
static int cb_status = -1;
static void completion_cb(int status, void *cbdata)
{
    cb_fired = true;
    cb_status = status;
    *((bool *) cbdata) = true;
}

/*
 * The request classes must construct to their documented defaults and
 * tear down cleanly.  A file_set owns its two path strings; the
 * destructor must free them (run under a leak checker to confirm).  A
 * request owns two lists; appending to them and releasing the request
 * must drain and release every member.
 */
static int test_classes(void)
{
    int failures = 0;
    prte_filem_base_process_set_t *pset;
    prte_filem_base_file_set_t *fset;
    prte_filem_base_request_t *req;

    /* process_set: source and sink both start INVALID */
    pset = PMIX_NEW(prte_filem_base_process_set_t);
    CHECK("process_set source rank invalid", PMIX_RANK_INVALID == pset->source.rank);
    CHECK("process_set sink rank invalid", PMIX_RANK_INVALID == pset->sink.rank);
    PMIX_RELEASE(pset);

    /* file_set: NULL targets, NONE hints, UNKNOWN type; destructor frees
     * the strings we hand it */
    fset = PMIX_NEW(prte_filem_base_file_set_t);
    CHECK("file_set local_target NULL", NULL == fset->local_target);
    CHECK("file_set remote_target NULL", NULL == fset->remote_target);
    CHECK("file_set local hint NONE", PRTE_FILEM_HINT_NONE == fset->local_hint);
    CHECK("file_set remote hint NONE", PRTE_FILEM_HINT_NONE == fset->remote_hint);
    CHECK("file_set type UNKNOWN", PRTE_FILEM_TYPE_UNKNOWN == fset->target_flag);
    fset->local_target = strdup("/some/local/path");
    fset->remote_target = strdup("some/remote/path");
    PMIX_RELEASE(fset);

    /* request: empty lists, UNKNOWN movement type; appending members and
     * releasing must drain both lists */
    req = PMIX_NEW(prte_filem_base_request_t);
    CHECK("request process_sets empty", 0 == pmix_list_get_size(&req->process_sets));
    CHECK("request file_sets empty", 0 == pmix_list_get_size(&req->file_sets));
    CHECK("request movement UNKNOWN", PRTE_FILEM_MOVE_TYPE_UNKNOWN == req->movement_type);

    pset = PMIX_NEW(prte_filem_base_process_set_t);
    pmix_list_append(&req->process_sets, &pset->super);
    fset = PMIX_NEW(prte_filem_base_file_set_t);
    fset->local_target = strdup("f");
    pmix_list_append(&req->file_sets, &fset->super);
    CHECK("request holds one process_set", 1 == pmix_list_get_size(&req->process_sets));
    CHECK("request holds one file_set", 1 == pmix_list_get_size(&req->file_sets));
    /* releasing the request must drain and release both members */
    PMIX_RELEASE(req);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_classes\n");
    }
    return failures;
}

/*
 * With no component selected, the global prte_filem is the "none" module
 * from filem_base_frame.c.  Every slot must be a safe no-op.  Most
 * importantly the fault_handler must be present and callable -- a NULL
 * there is a crash on daemon-fault recovery -- and preposition must fire
 * its completion callback so the state machine can advance.
 */
static int test_none_module(void)
{
    int failures = 0;
    bool done = false;

    /* every function pointer must be wired */
    CHECK("none init set", NULL != prte_filem.filem_init);
    CHECK("none finalize set", NULL != prte_filem.filem_finalize);
    CHECK("none fault_handler set", NULL != prte_filem.fault_handler);
    CHECK("none put set", NULL != prte_filem.put);
    CHECK("none get set", NULL != prte_filem.get);
    CHECK("none rm set", NULL != prte_filem.rm);
    CHECK("none wait set", NULL != prte_filem.wait);
    CHECK("none wait_all set", NULL != prte_filem.wait_all);
    CHECK("none preposition set", NULL != prte_filem.preposition_files);
    CHECK("none link_local set", NULL != prte_filem.link_local_files);

    /* the no-op transfer slots all return success */
    CHECK("none put succeeds", PRTE_SUCCESS == prte_filem.put(NULL));
    CHECK("none put_nb succeeds", PRTE_SUCCESS == prte_filem.put_nb(NULL));
    CHECK("none get succeeds", PRTE_SUCCESS == prte_filem.get(NULL));
    CHECK("none get_nb succeeds", PRTE_SUCCESS == prte_filem.get_nb(NULL));
    CHECK("none rm succeeds", PRTE_SUCCESS == prte_filem.rm(NULL));
    CHECK("none rm_nb succeeds", PRTE_SUCCESS == prte_filem.rm_nb(NULL));
    CHECK("none wait succeeds", PRTE_SUCCESS == prte_filem.wait(NULL));
    CHECK("none wait_all succeeds", PRTE_SUCCESS == prte_filem.wait_all(NULL));
    CHECK("none link_local succeeds", PRTE_SUCCESS == prte_filem.link_local_files(NULL, NULL));

    /* the fault handler must be safe to invoke (it is a no-op for none) */
    prte_filem.fault_handler(NULL);
    CHECK("none fault_handler returns", true);

    /* preposition must fire the completion callback with SUCCESS even
     * though it stages nothing -- otherwise the DVM hangs at VM_READY */
    cb_fired = false;
    cb_status = -1;
    CHECK("none preposition succeeds",
          PRTE_SUCCESS == prte_filem.preposition_files(NULL, completion_cb, &done));
    CHECK("none preposition fired callback", cb_fired);
    CHECK("none preposition callback status SUCCESS", PRTE_SUCCESS == cb_status);
    CHECK("none preposition passed cbdata", done);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_none_module\n");
    }
    return failures;
}

/*
 * The naming rules that keep a preloaded file inside the directory it is
 * meant to land in.
 */
static int test_path_rules(void)
{
    int failures = 0;
    char *q;

    /* leading dot directories are what a user types and what the app does
     * NOT open the file by -- they come off */
    CHECK("strip ./f", 0 == strcmp("f", prte_filem_base_strip_leading_dots("./f")));
    CHECK("strip ../f", 0 == strcmp("f", prte_filem_base_strip_leading_dots("../f")));
    CHECK("strip ../../a/b", 0 == strcmp("a/b", prte_filem_base_strip_leading_dots("../../a/b")));
    CHECK("strip /a/b", 0 == strcmp("a/b", prte_filem_base_strip_leading_dots("/a/b")));
    /* a name that merely begins with a dot is a dot-FILE and is left be */
    CHECK("keep .bashrc", 0 == strcmp(".bashrc", prte_filem_base_strip_leading_dots(".bashrc")));
    CHECK("keep ./.bashrc", 0 == strcmp(".bashrc", prte_filem_base_strip_leading_dots("./.bashrc")));
    /* an ordinary relative path is untouched -- that is the name the app
     * will open it by */
    CHECK("keep sub/f.dat", 0 == strcmp("sub/f.dat", prte_filem_base_strip_leading_dots("sub/f.dat")));
    CHECK("strip empty", 0 == strcmp("", prte_filem_base_strip_leading_dots("")));

    /* ".." anywhere else is the case stripping does not cover: these all
     * resolve outside the directory the file is placed in */
    CHECK("dotdot a/../../f", prte_filem_base_has_dotdot("a/../../f"));
    CHECK("dotdot leading", prte_filem_base_has_dotdot("../f"));
    CHECK("dotdot trailing", prte_filem_base_has_dotdot("a/.."));
    CHECK("dotdot alone", prte_filem_base_has_dotdot(".."));
    /* ... and these do not, however dot-heavy they look */
    CHECK("no dotdot in ..f", !prte_filem_base_has_dotdot("..f"));
    CHECK("no dotdot in a/..f/b", !prte_filem_base_has_dotdot("a/..f/b"));
    CHECK("no dotdot in a..b", !prte_filem_base_has_dotdot("a..b"));
    CHECK("no dotdot in plain", !prte_filem_base_has_dotdot("sub/dir/f.dat"));
    CHECK("no dotdot in dotfile", !prte_filem_base_has_dotdot(".bashrc"));
    CHECK("no dotdot in empty", !prte_filem_base_has_dotdot(""));
    CHECK("no dotdot in dot", !prte_filem_base_has_dotdot("."));

    /* archive paths go through a shell, so an ordinary name with a space
     * in it has to survive as one argument */
    q = prte_filem_base_shell_quote("my data.tar.gz");
    CHECK("quote plain", NULL != q && 0 == strcmp("'my data.tar.gz'", q));
    free(q);
    q = prte_filem_base_shell_quote("");
    CHECK("quote empty", NULL != q && 0 == strcmp("''", q));
    free(q);
    /* an embedded single quote has to close, escape, and reopen */
    q = prte_filem_base_shell_quote("it's");
    CHECK("quote apostrophe", NULL != q && 0 == strcmp("'it'\\''s'", q));
    free(q);
    /* nothing inside the quotes can start a new command */
    q = prte_filem_base_shell_quote("a;rm -rf /`x`$(y)");
    CHECK("quote metachars", NULL != q && 0 == strcmp("'a;rm -rf /`x`$(y)'", q));
    free(q);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_path_rules\n");
    }
    return failures;
}

/*
 * The directory a placed file goes into is reached one component at a time,
 * never through a symlink, and only through directories that are ours or
 * our group's.
 */
static int test_open_dir_under(void)
{
    int failures = 0, fd;
    char base[] = "/tmp/prte_filem_dir_XXXXXX";
    char *p = NULL;
    struct stat st;

    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "FAIL [mkdtemp]: %s\n", strerror(errno));
        return 1;
    }

    fd = prte_filem_base_open_dir_under(base, NULL, 0755);
    CHECK("no subdirectory opens the root", 0 <= fd);
    if (0 <= fd) {
        close(fd);
    }

    fd = prte_filem_base_open_dir_under(base, "a/b", 0755);
    CHECK("missing directories are created", 0 <= fd);
    if (0 <= fd) {
        close(fd);
    }
    CHECK("asprintf", 0 <= pmix_asprintf(&p, "%s/a/b", base));
    CHECK("...where the name says", 0 == lstat(p, &st) && S_ISDIR(st.st_mode));
    free(p);

    fd = prte_filem_base_open_dir_under(base, "./a/b", 0755);
    CHECK("an existing directory of ours is used", 0 <= fd);
    if (0 <= fd) {
        close(fd);
    }

    /* a symlink anywhere in the path is not followed */
    CHECK("asprintf", 0 <= pmix_asprintf(&p, "%s/link", base));
    CHECK("symlink", 0 == symlink("a", p));
    free(p);
    fd = prte_filem_base_open_dir_under(base, "link/c", 0755);
    CHECK("a symlink in the path is refused", 0 > fd && (ELOOP == errno || ENOTDIR == errno));
    CHECK("asprintf", 0 <= pmix_asprintf(&p, "%s/a/c", base));
    CHECK("...and nothing is made through it", 0 != lstat(p, &st));
    free(p);

    /* nor is a file */
    CHECK("asprintf", 0 <= pmix_asprintf(&p, "%s/file", base));
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK("create file", 0 <= fd);
    if (0 <= fd) {
        close(fd);
    }
    free(p);
    fd = prte_filem_base_open_dir_under(base, "file/c", 0755);
    CHECK("a file in the path is refused", 0 > fd && ENOTDIR == errno);

    fd = prte_filem_base_open_dir_under(base, "a/../a", 0755);
    CHECK("'..' is refused", 0 > fd && EINVAL == errno);

    /* a directory someone else owns: refused unless its group is ours.
     * Only root can make one. */
    if (0 == geteuid()) {
        CHECK("asprintf", 0 <= pmix_asprintf(&p, "%s/theirs", base));
        CHECK("mkdir theirs", 0 == mkdir(p, 0777));
        CHECK("chown theirs", 0 == chown(p, 65534, 65534));
        fd = prte_filem_base_open_dir_under(base, "theirs/c", 0755);
        CHECK("another user's directory in another group is refused", 0 > fd && EACCES == errno);
        CHECK("chgrp theirs", 0 == chown(p, 65534, getegid()));
        fd = prte_filem_base_open_dir_under(base, "theirs/c", 0755);
        CHECK("another user's directory in our group is used", 0 <= fd);
        if (0 <= fd) {
            close(fd);
        }
        free(p);
    }

    CHECK("asprintf", 0 <= pmix_asprintf(&p, "rm -rf '%s'", base));
    CHECK("cleanup", 0 == system(p));
    free(p);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_open_dir_under\n");
    }
    return failures;
}

/*
 * The temporary a placed file is written to before it is renamed into
 * place: always a new file, under a name of its own, with the mode it was
 * asked for whatever the umask, and not inherited by anything this process
 * starts.
 */
static int test_open_temp(void)
{
    int failures = 0, dfd, fd1, fd2, fl;
    char base[] = "/tmp/prte_filem_test_XXXXXX";
    char *t1 = NULL, *t2 = NULL;
    struct stat st;
    mode_t old;

    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "FAIL [mkdtemp]: %s\n", strerror(errno));
        return 1;
    }
    dfd = open(base, O_RDONLY | O_DIRECTORY);
    CHECK("open dir", 0 <= dfd);

    old = umask(077);
    fd1 = prte_filem_base_open_temp_at(dfd, "data.bin", 0755, &t1);
    fd2 = prte_filem_base_open_temp_at(dfd, "data.bin", 0644, &t2);
    umask(old);
    CHECK("a temporary is created", 0 <= fd1 && NULL != t1);
    CHECK("named for the destination",
          NULL != t1 && 0 == strncmp(t1, "data.bin.prte-tmp.", 18) && 24 == strlen(t1));
    CHECK("a second is created", 0 <= fd2 && NULL != t2);
    CHECK("under a different name", NULL != t1 && NULL != t2 && 0 != strcmp(t1, t2));
    CHECK("the mode asked for, whatever the umask",
          NULL != t1 && 0 == fstatat(dfd, t1, &st, AT_SYMLINK_NOFOLLOW)
              && 0755 == (st.st_mode & 07777) && 0 == st.st_size);
    fl = (0 <= fd1) ? fcntl(fd1, F_GETFD) : -1;
    CHECK("close-on-exec", 0 <= fl && 0 != (fl & FD_CLOEXEC));
    if (0 <= fd1) {
        close(fd1);
    }
    if (0 <= fd2) {
        close(fd2);
    }
    if (NULL != t1) {
        unlinkat(dfd, t1, 0);
    }
    if (NULL != t2) {
        unlinkat(dfd, t2, 0);
    }
    free(t1);
    free(t2);

    /* nowhere to put it: an error, and nothing to clean up */
    t1 = (char *) 1;
    fd1 = prte_filem_base_open_temp_at(-1, "data.bin", 0644, &t1);
    CHECK("no directory is an error", 0 > fd1 && NULL == t1);

    close(dfd);
    rmdir(base);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_open_temp\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* open the framework so its verbosity channel is valid and the
     * request classes are registered.  We deliberately do NOT run
     * prte_filem_base_select(), so the global prte_filem remains the
     * default "none" module that frame.c installs.
     */
    rc = pmix_mca_base_framework_open(&prte_filem_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "filem framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_classes();
    failures += test_none_module();
    failures += test_path_rules();
    failures += test_open_dir_under();
    failures += test_open_temp();

    (void) pmix_mca_base_framework_close(&prte_filem_base_framework);

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all filem unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d filem unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
