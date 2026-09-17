/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/mca/common/slurm -- "is Slurm here, and what version
 * is it", asked once per process on behalf of ras/slurm, plm/slurm and
 * ess/slurm.
 *
 * The answer comes from popen()ing a Slurm client, so the test supplies
 * the client: a directory of stub `srun`/`scontrol`/`sbatch`/`sinfo`
 * scripts that print a chosen line, placed first on PATH.  That is the
 * only way to reach the parser, and it is the parser that has broken
 * before -- Debian and Ubuntu package Slurm as "slurm-wlm", so their
 * clients answer "slurm-wlm 23.11.4", and a parser anchored on "slurm "
 * reads the most widely deployed packaging there is as "no Slurm here".
 * That does not fail loudly: it leaves available == false, which is
 * exactly how a machine with no Slurm at all reads, so plm/slurm declines
 * to select and the DVM quietly falls back to ssh inside a live
 * allocation.
 *
 * The answer is cached for the life of the process -- deliberately, since
 * the point of the library is to ask once -- so each case runs in a forked
 * child with its own cache and its own PATH, and the child's exit status
 * is the verdict.
 */

#include "prte_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/mca/common/slurm/common_slurm.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* The four clients the library tries, in the order it tries them. */
static const char *const clients[] = {"srun", "scontrol", "sbatch", "sinfo", NULL};

typedef struct {
    const char *label;
    /* what the stub client prints on stdout; NULL means "print nothing",
     * which is what a client that cannot reach a configuration source
     * does (its complaint goes to stderr, and the probe discards it) */
    const char *output;
    int exit_code;
    /* what the library must conclude */
    bool available;
    int major;
    int minor;
    bool ancient;
    bool early;
    bool extended;
    const char *version;
} slurm_case_t;

static const slurm_case_t cases[] = {
    /* The stock upstream spelling. */
    {"upstream 24.11.6", "slurm 24.11.6", 0, true, 24, 11, false, false, true, "24.11.6"},

    /* Debian/Ubuntu build Slurm under its own package name.  This is the
     * regression: "slurm-wlm" is not "slurm ". */
    {"debian 23.11.4", "slurm-wlm 23.11.4", 0, true, 23, 11, false, false, false, "23.11.4"},

    /* ...including the packaging suffix Debian appends to the version. */
    {"debian 21.08.8-2", "slurm-wlm 21.08.8-2", 0, true, 21, 8, false, true, false,
     "21.08.8-2"},

    /* A client that cannot establish a configuration source exits non-zero
     * having printed nothing we can read.  That is "cannot tell", and it
     * must not masquerade as a version. */
    {"no config source", NULL, 1, false, 0, 0, false, false, false, "unknown"},

    /* A diagnostic is not an answer.  Slurm prefixes these with the tool
     * name, so the first word does not name the package. */
    {"diagnostic line", "srun: fatal: Could not establish a configuration source", 1,
     false, 0, 0, false, false, false, "unknown"},

    /* A first word that merely starts the same way but carries no version
     * is still not an answer. */
    {"no version at all", "slurmd", 0, false, 0, 0, false, false, false, "unknown"},

    /* The three thresholds, on their boundaries.  They are three separate
     * questions and each has its own edge. */
    {"ancient 17.02", "slurm 17.02.11", 0, true, 17, 2, true, true, false, "17.02.11"},
    {"oldest supported 17.11", "slurm 17.11.0", 0, true, 17, 11, false, true, false,
     "17.11.0"},
    {"early 23.02", "slurm 23.02.7", 0, true, 23, 2, false, true, false, "23.02.7"},
    {"not early 23.11", "slurm 23.11.0", 0, true, 23, 11, false, false, false, "23.11.0"},
    {"not extended 24.04", "slurm 24.04.9", 0, true, 24, 4, false, false, false, "24.04.9"},
    {"extended 24.05", "slurm 24.05.0", 0, true, 24, 5, false, false, true, "24.05.0"},

    /* A major with no minor at all must not walk off the end of the line. */
    {"major only", "slurm 25", 0, true, 25, 0, false, false, true, "25"},

    {NULL, NULL, 0, false, 0, 0, false, false, false, NULL}
};

/*
 * Write the four stub clients into dir, each printing this case's output
 * and exiting with its code.
 */
static int write_stubs(const char *dir, const slurm_case_t *c)
{
    char path[1024];
    FILE *fp;
    int i;

    for (i = 0; NULL != clients[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, clients[i]);
        fp = fopen(path, "w");
        if (NULL == fp) {
            return -1;
        }
        fprintf(fp, "#!/bin/sh\n");
        if (NULL != c->output) {
            /* a quoted heredoc so nothing in the line is interpreted */
            fprintf(fp, "cat <<'PRTE_EOF'\n%s\nPRTE_EOF\n", c->output);
        }
        fprintf(fp, "exit %d\n", c->exit_code);
        fclose(fp);
        if (0 != chmod(path, 0755)) {
            return -1;
        }
    }
    return 0;
}

static void remove_stubs(const char *dir)
{
    char path[1024];
    int i;

    for (i = 0; NULL != clients[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, clients[i]);
        unlink(path);
    }
    rmdir(dir);
}

/*
 * Run one case in this (forked) process and return the failure count.  The
 * cache is per-process, so this may only be called once per child.
 */
static int run_case(const slurm_case_t *c, const char *dir)
{
    const prte_common_slurm_version_t *v;
    char path[2048];
    int failures = 0;

    /* The stubs go first, but the real PATH stays behind them: /bin/sh has
     * to be found, and so does anything popen's shell needs. */
    snprintf(path, sizeof(path), "%s:/usr/bin:/bin", dir);
    setenv("PATH", path, 1);

    v = prte_common_slurm_version();
    CHECK(c->label, NULL != v);
    if (NULL == v) {
        return failures;
    }
    CHECK(c->label, c->available == v->available);
    CHECK(c->label, 0 == strcmp(c->version, v->version));
    if (!c->available) {
        /* nothing below available is meaningful; the contract only
         * promises the version string reads "unknown" */
        return failures;
    }
    CHECK(c->label, c->major == v->major);
    CHECK(c->label, c->minor == v->minor);
    CHECK(c->label, c->ancient == v->ancient);
    CHECK(c->label, c->early == v->early);
    CHECK(c->label, c->extended == v->extended);

    /* asked twice, answered the same: the cache must not re-probe, and it
     * must not lose what it found */
    CHECK(c->label, v == prte_common_slurm_version());

    return failures;
}

/*
 * prte_common_slurm_jobid() reads the current spelling first and falls back
 * to the historical one.  No subprocess, no cache, so it needs no fork.
 */
static int test_jobid(void)
{
    int failures = 0;
    const char *label = "jobid";
    char *j;

    unsetenv("SLURM_JOB_ID");
    unsetenv("SLURM_JOBID");
    CHECK(label, NULL == prte_common_slurm_jobid());

    setenv("SLURM_JOBID", "111", 1);
    j = prte_common_slurm_jobid();
    CHECK(label, NULL != j && 0 == strcmp("111", j));

    setenv("SLURM_JOB_ID", "222", 1);
    j = prte_common_slurm_jobid();
    CHECK(label, NULL != j && 0 == strcmp("222", j));

    unsetenv("SLURM_JOBID");
    j = prte_common_slurm_jobid();
    CHECK(label, NULL != j && 0 == strcmp("222", j));

    unsetenv("SLURM_JOB_ID");
    CHECK(label, NULL == prte_common_slurm_jobid());

    return failures;
}

int main(int argc, char *argv[])
{
    char template[] = "/tmp/prte_common_slurm_XXXXXX";
    char *dir;
    int i, failures = 0, status;
    pid_t pid;

    (void) argc;
    (void) argv;

    dir = mkdtemp(template);
    if (NULL == dir) {
        fprintf(stderr, "FAIL: could not create a stub directory\n");
        return 1;
    }

    for (i = 0; NULL != cases[i].label; i++) {
        if (0 != write_stubs(dir, &cases[i])) {
            fprintf(stderr, "FAIL [%s]: could not write the stub clients\n",
                    cases[i].label);
            failures++;
            continue;
        }
        pid = fork();
        if (0 > pid) {
            fprintf(stderr, "FAIL [%s]: fork\n", cases[i].label);
            failures++;
            continue;
        }
        if (0 == pid) {
            _exit(0 == run_case(&cases[i], dir) ? 0 : 1);
        }
        if (pid != waitpid(pid, &status, 0)) {
            fprintf(stderr, "FAIL [%s]: waitpid\n", cases[i].label);
            failures++;
            continue;
        }
        if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
            fprintf(stderr, "FAIL [%s]: case did not pass\n", cases[i].label);
            failures++;
        }
    }

    remove_stubs(dir);

    failures += test_jobid();

    if (0 == failures) {
        printf("test_common_slurm: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_common_slurm: %d failure(s)\n", failures);
    return 1;
}
