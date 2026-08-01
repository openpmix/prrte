/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the PRRTE tools -- src/tools.
 *
 * A tool is a main() around a library, so almost nothing in src/tools can
 * be called from a test: prted's main() joins a DVM, prun's and pterm's
 * connect to one, prte_info's walks every framework and exits.  Those
 * belong to the live smoke test and to contrib/dockerswarm.
 *
 * What *is* testable is the decision-making the tools do before they
 * touch a runtime -- and that is precisely where this review found
 * defects, all of them invisible because the code sat inside main():
 *
 *  - prte_parse_pid_option(), the "--pid <n> | file:<path>" interpreter
 *    that pterm and prun_common each had their own copy of.  Both read
 *    the file with fscanf(fp, "%lu", (unsigned long *) &pid), which
 *    writes eight bytes into a four-byte pid_t on every LP64 platform.
 *    pterm's copy also had no else-branch: a value that was neither an
 *    integer nor a "file:" spec was silently ignored, and pterm went on
 *    to terminate whatever DVM it happened to find.
 *
 *  - prte_parse_umask(), the PRTE_DAEMON_UMASK_VALUE reader from prted.
 *    It called strtol() without clearing errno, tested for EINVAL (which
 *    strtol never sets), and accepted an empty string as zero -- so
 *    PRTE_DAEMON_UMASK_VALUE= in the environment made the daemon call
 *    umask(0) and every file it created world-writable.
 *
 *  - prte_load_appfile(), the "--app <file>" expander from prun (prte.c
 *    has the same feature).  One app context per line, ':'-delimited as
 *    if the lines had been typed on the command line.
 *
 * These need no DVM, no frameworks, and no event base -- just
 * prte_init_minimum() for the install dirs the error paths reference.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/util/prte_cmd_line.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

static char tmpl[] = "/tmp/prte_test_tools_XXXXXX";
static char *scratch = NULL;

static char *scratch_file(const char *name, const char *contents)
{
    static char path[512];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", scratch, name);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return NULL;
    }
    if (NULL != contents) {
        fputs(contents, fp);
    }
    fclose(fp);
    return path;
}

/*
 * --pid <value>
 */
static int test_pid_option(void)
{
    int failures = 0;
    pid_t pid;
    const char *fname;
    char *path;
    int rc;

    /* the plain integer form */
    pid = 0;
    fname = (const char *) 0x1; /* must be cleared even on the integer path */
    rc = prte_parse_pid_option("12345", &pid, &fname);
    CHECK("int form accepted", PRTE_SUCCESS == rc);
    CHECK("int form value", 12345 == pid);
    CHECK("int form has no filename", NULL == fname);

    /* things that are not a PID at all.  The middle two are the case
     * pterm used to accept in silence and then act on */
    CHECK("empty rejected", PRTE_SUCCESS != prte_parse_pid_option("", &pid, NULL));
    CHECK("garbage rejected", PRTE_SUCCESS != prte_parse_pid_option("notapid", &pid, NULL));
    CHECK("trailing garbage rejected",
          PRTE_SUCCESS != prte_parse_pid_option("123abc", &pid, NULL));
    CHECK("negative rejected", PRTE_SUCCESS != prte_parse_pid_option("-1", &pid, NULL));
    CHECK("NULL value rejected", PRTE_SUCCESS != prte_parse_pid_option(NULL, &pid, NULL));

    /* an out-of-range value must not be truncated into a valid PID */
    CHECK("overlarge rejected",
          PRTE_SUCCESS != prte_parse_pid_option("99999999999999999999", &pid, NULL));
    CHECK("above INT_MAX rejected",
          PRTE_SUCCESS != prte_parse_pid_option("4294967295", &pid, NULL));

    /* the file: form */
    path = scratch_file("pidfile", "4242\n");
    CHECK("scratch pidfile created", NULL != path);
    if (NULL != path) {
        char spec[600];
        snprintf(spec, sizeof(spec), "file:%s", path);
        pid = 0;
        fname = NULL;
        rc = prte_parse_pid_option(spec, &pid, &fname);
        CHECK("file form accepted", PRTE_SUCCESS == rc);
        CHECK("file form value", 4242 == pid);
        CHECK("file form reports the path",
              NULL != fname && 0 == strcmp(fname, path));

        /* the reader must not run off the end of a pid_t.  If it writes
         * sizeof(long) bytes - which is what the code this replaces did -
         * the guard below is what a debug allocator would trip on; here
         * we can at least insist the value survived intact */
        CHECK("file form did not corrupt the value", 4242 == pid);
    }

    /* a file that holds no number */
    path = scratch_file("pidfile.bad", "not a number\n");
    if (NULL != path) {
        char spec[600];
        snprintf(spec, sizeof(spec), "file:%s", path);
        rc = prte_parse_pid_option(spec, &pid, &fname);
        CHECK("unreadable pid file reported distinctly", PRTE_ERR_FILE_READ_FAILURE == rc);
        CHECK("unreadable pid file names the file",
              NULL != fname && 0 == strcmp(fname, path));
    }

    /* an empty file is not a read failure the caller can ignore */
    path = scratch_file("pidfile.empty", "");
    if (NULL != path) {
        char spec[600];
        snprintf(spec, sizeof(spec), "file:%s", path);
        CHECK("empty pid file rejected",
              PRTE_ERR_FILE_READ_FAILURE == prte_parse_pid_option(spec, &pid, &fname));
    }

    /* a file that does not exist is a *different* failure - the tools
     * print a different message for each */
    rc = prte_parse_pid_option("file:/nonexistent/prte/pidfile", &pid, &fname);
    CHECK("missing pid file reported distinctly", PRTE_ERR_FILE_OPEN_FAILURE == rc);

    /* malformed file specs */
    CHECK("file with no colon rejected",
          PRTE_ERR_BAD_PARAM == prte_parse_pid_option("filename", &pid, NULL));
    CHECK("file with no path rejected",
          PRTE_ERR_BAD_PARAM == prte_parse_pid_option("file:", &pid, NULL));

    /* the prefix match is case-insensitive, as it always has been */
    path = scratch_file("pidfile2", "77");
    if (NULL != path) {
        char spec[600];
        snprintf(spec, sizeof(spec), "FILE:%s", path);
        pid = 0;
        CHECK("FILE: accepted", PRTE_SUCCESS == prte_parse_pid_option(spec, &pid, NULL));
        CHECK("FILE: value", 77 == pid);
    }

    return failures;
}

/*
 * PRTE_DAEMON_UMASK_VALUE
 */
static int test_umask(void)
{
    int failures = 0;
    mode_t mask;

    mask = 0;
    CHECK("octal accepted", prte_parse_umask("077", &mask));
    CHECK("octal value", 077 == mask);

    mask = 0;
    CHECK("bare octal accepted", prte_parse_umask("22", &mask));
    CHECK("bare octal is base 8", 022 == mask);

    mask = 0777;
    CHECK("explicit zero accepted", prte_parse_umask("0", &mask));
    CHECK("explicit zero value", 0 == mask);

    /* the whole point: an empty value must not be read as "umask(0)" */
    CHECK("empty rejected", !prte_parse_umask("", &mask));
    CHECK("NULL rejected", !prte_parse_umask(NULL, &mask));
    CHECK("garbage rejected", !prte_parse_umask("rwx", &mask));
    CHECK("trailing garbage rejected", !prte_parse_umask("022junk", &mask));
    CHECK("non-octal digit rejected", !prte_parse_umask("099", &mask));
    CHECK("out of range rejected", !prte_parse_umask("7777", &mask));
    CHECK("negative rejected", !prte_parse_umask("-1", &mask));

    /* a rejected value must leave the caller's mask untouched, since the
     * caller's next move is to decide whether to call umask() at all */
    mask = 0123;
    (void) prte_parse_umask("bogus", &mask);
    CHECK("rejection leaves mask alone", 0123 == mask);

    return failures;
}

/*
 * --app <file>
 */
/*
 * prte_cli_bool_value() - the optional value on a boolean directive.
 *
 * The bare spelling asserts the directive; the value form is what lets a
 * directive an MCA parameter turned on be turned back off, and what lets
 * one app segment of an MPMD line contradict another visibly.  A value that
 * is neither true nor false must be REFUSED rather than read: the truth
 * test underneath reports anything it does not recognize as false, so
 * accepting "tag=maybe" would silently mean "no tags".
 */
static int test_bool_value(void)
{
    int failures = 0;
    bool flag;
    char label[64];
    size_t i;
    /* Every spelling of a truth value a user may reasonably type.  Matching
     * is case-insensitive; the single letters "y"/"t"/"n"/"f" are accepted
     * whole, and the words are matched to their full length (so "true" and
     * "truest" both read as true, but "tru" does NOT - it is a prefix of the
     * word, not the word).  A number reads as C does: zero is false and
     * anything else is true. */
    const char *yes[] = {"1", "2", "42", "y", "Y", "t", "T", "yes", "YES",
                         "Yes", "true", "TRUE", "True",
                         "enable", "ENABLE", "enabled", NULL};
    const char *no[] = {"0", "00", "n", "N", "f", "F", "no", "NO", "No",
                        "false", "FALSE", "False",
                        "disable", "DISABLE", "disabled", NULL};
    /* ...and things that are neither, which must be REFUSED.  The truth
     * test underneath reports every one of these as "false", so accepting
     * them would silently turn a directive off.  Note "on"/"off" and the
     * truncated words: they LOOK like truth values and are not, which is
     * exactly the case a user needs told about rather than guessed at. */
    const char *neither[] = {"maybe", "/tmp/out", "on", "off", "-1",
                             "sure", "tru", "fals", NULL};

    /* the bare form, however it reaches us */
    flag = false;
    CHECK("bool:null-rc", PRTE_SUCCESS == prte_cli_bool_value(NULL, &flag));
    CHECK("bool:null-true", flag);
    flag = false;
    CHECK("bool:empty-rc", PRTE_SUCCESS == prte_cli_bool_value("", &flag));
    CHECK("bool:empty-true", flag);

    for (i = 0; NULL != yes[i]; i++) {
        flag = false;
        snprintf(label, sizeof(label), "bool:true[%s]", yes[i]);
        CHECK(label, PRTE_SUCCESS == prte_cli_bool_value(yes[i], &flag) && flag);
    }
    for (i = 0; NULL != no[i]; i++) {
        flag = true;
        snprintf(label, sizeof(label), "bool:false[%s]", no[i]);
        CHECK(label, PRTE_SUCCESS == prte_cli_bool_value(no[i], &flag) && !flag);
    }
    for (i = 0; NULL != neither[i]; i++) {
        snprintf(label, sizeof(label), "bool:refused[%s]", neither[i]);
        CHECK(label, PRTE_SUCCESS != prte_cli_bool_value(neither[i], &flag));
    }

    CHECK("bool:null-flag", PRTE_SUCCESS != prte_cli_bool_value("1", NULL));

    return failures;
}

static int test_appfile(void)
{
    int failures = 0;
    char **argv = NULL;
    char *path;
    int rc, n;

    path = scratch_file("appfile",
                        "-n 1 hostname\n"
                        "-n 2 echo hello\n");
    CHECK("scratch appfile created", NULL != path);
    if (NULL == path) {
        return failures;
    }

    /* the tools call this with the tool's own argv already in place */
    PMIx_Argv_append_nosize(&argv, "prun");
    rc = prte_load_appfile(path, &argv);
    CHECK("appfile loaded", PRTE_SUCCESS == rc);
    n = PMIx_Argv_count(argv);
    /* prun -n 1 hostname : -n 2 echo hello */
    CHECK("appfile token count", 9 == n);
    if (9 == n) {
        CHECK("tool argv preserved", 0 == strcmp(argv[0], "prun"));
        CHECK("first app", 0 == strcmp(argv[1], "-n") && 0 == strcmp(argv[2], "1")
                               && 0 == strcmp(argv[3], "hostname"));
        CHECK("apps are colon separated", 0 == strcmp(argv[4], ":"));
        CHECK("second app", 0 == strcmp(argv[5], "-n") && 0 == strcmp(argv[6], "2")
                                && 0 == strcmp(argv[7], "echo")
                                && 0 == strcmp(argv[8], "hello"));
    }
    PMIx_Argv_free(argv);
    argv = NULL;

    /* a single line produces no delimiter at all */
    path = scratch_file("appfile.one", "hostname\n");
    if (NULL != path) {
        rc = prte_load_appfile(path, &argv);
        CHECK("single-line appfile loaded", PRTE_SUCCESS == rc);
        CHECK("single-line appfile has no delimiter", 1 == PMIx_Argv_count(argv));
        PMIx_Argv_free(argv);
        argv = NULL;
    }

    /* blank lines separate nothing from nothing: they must not emit a
     * delimiter, which would hand the parser an empty app context */
    path = scratch_file("appfile.blank", "hostname\n\n\nuptime\n");
    if (NULL != path) {
        rc = prte_load_appfile(path, &argv);
        CHECK("appfile with blank lines loaded", PRTE_SUCCESS == rc);
        n = PMIx_Argv_count(argv);
        CHECK("blank lines produce no extra delimiters", 3 == n);
        if (3 == n) {
            CHECK("blank-line appfile order",
                  0 == strcmp(argv[0], "hostname") && 0 == strcmp(argv[1], ":")
                      && 0 == strcmp(argv[2], "uptime"));
        }
        PMIx_Argv_free(argv);
        argv = NULL;
    }

    /* an empty appfile is not an error, it just contributes nothing */
    path = scratch_file("appfile.empty", "");
    if (NULL != path) {
        rc = prte_load_appfile(path, &argv);
        CHECK("empty appfile loaded", PRTE_SUCCESS == rc);
        CHECK("empty appfile adds nothing", NULL == argv);
    }

    /* a missing appfile must be reported, not treated as empty - the
     * tools print a help message and exit on this */
    CHECK("missing appfile reported",
          PRTE_ERR_FILE_OPEN_FAILURE == prte_load_appfile("/nonexistent/prte/appfile", &argv));
    CHECK("NULL argv rejected", PRTE_SUCCESS != prte_load_appfile(path, NULL));

    return failures;
}

int main(int argc, char *argv[])
{
    int failures = 0;
    int rc;
    char cmd[600];
    PRTE_HIDE_UNUSED_PARAMS(argc, argv);

    rc = prte_init_minimum();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_minimum failed: %d\n", rc);
        return 1;
    }

    scratch = mkdtemp(tmpl);
    if (NULL == scratch) {
        fprintf(stderr, "could not create a scratch directory\n");
        return 1;
    }

    failures += test_pid_option();
    failures += test_umask();
    failures += test_appfile();
    failures += test_bool_value();

    snprintf(cmd, sizeof(cmd), "rm -rf %s", scratch);
    rc = system(cmd);
    if (0 != rc) {
        fprintf(stderr, "warning: could not remove %s\n", scratch);
    }

    if (0 == failures) {
        printf("test_tools: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_tools: %d failure(s)\n", failures);
    return 1;
}
