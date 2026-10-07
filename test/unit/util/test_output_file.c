/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_output_file_open() / prte_output_file_remove(): a file written at a
 * path the user gave (--report-pid, the proctable) is written afresh only
 * if it is a regular file of our own with one link, never through a
 * symlink at the name, and removed again only if it is still that file.
 * Every case runs inside a private scratch directory.
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/util/pmix_printf.h"
#include "src/util/prte_output_file.h"

int test_output_file(void);

#define CHECK(label, cond)                                                 \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL [output_file:%s]: %s\n", label, #cond);  \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* the whole content of path, or NULL */
static char *slurp(const char *path)
{
    char buf[256];
    size_t n;
    FILE *fp = fopen(path, "r");

    if (NULL == fp) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strdup(buf);
}

static void put(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (NULL != fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

int test_output_file(void)
{
    int failures = 0;
    char base[] = "/tmp/prte_outfile_test_XXXXXX";
    char *path = NULL, *other = NULL, *lnk = NULL, *text;
    struct stat st;
    FILE *fp;

    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "FAIL [output_file:mkdtemp]: %s\n", strerror(errno));
        return 1;
    }
    if (0 > pmix_asprintf(&path, "%s/out", base) || 0 > pmix_asprintf(&other, "%s/other", base) ||
        0 > pmix_asprintf(&lnk, "%s/link", base)) {
        return 1;
    }

    /* a new file is created and written */
    fp = prte_output_file_open(path, 0644, &st);
    CHECK("create", NULL != fp);
    if (NULL != fp) {
        fputs("first\n", fp);
        fclose(fp);
    }
    text = slurp(path);
    CHECK("created-content", NULL != text && 0 == strcmp(text, "first\n"));
    free(text);

    /* a file of our own already there is replaced, not appended to */
    fp = prte_output_file_open(path, 0644, &st);
    CHECK("reopen", NULL != fp);
    if (NULL != fp) {
        fputs("2\n", fp);
        fclose(fp);
    }
    text = slurp(path);
    CHECK("rewritten", NULL != text && 0 == strcmp(text, "2\n"));
    free(text);

    /* removed only while it is still the file we wrote */
    put(other, "someone else\n");
    CHECK("replace", 0 == rename(other, path));
    prte_output_file_remove(path, &st);
    CHECK("other-file-kept", 0 == access(path, F_OK));
    CHECK("reopen-for-remove", NULL != (fp = prte_output_file_open(path, 0644, &st)));
    if (NULL != fp) {
        fclose(fp);
    }
    prte_output_file_remove(path, &st);
    CHECK("own-file-removed", 0 != access(path, F_OK));

    /* a symlink at the name is not followed, and its target is untouched */
    put(other, "target\n");
    CHECK("symlink", 0 == symlink(other, lnk));
    fp = prte_output_file_open(lnk, 0644, NULL);
    CHECK("symlink-refused", NULL == fp);
    if (NULL != fp) {
        fclose(fp);
    }
    text = slurp(other);
    CHECK("symlink-target-untouched", NULL != text && 0 == strcmp(text, "target\n"));
    free(text);
    unlink(lnk);

    /* nor is a file with a second link to it */
    CHECK("hardlink", 0 == link(other, path));
    fp = prte_output_file_open(path, 0644, NULL);
    CHECK("hardlink-refused", NULL == fp);
    if (NULL != fp) {
        fclose(fp);
    }
    text = slurp(other);
    CHECK("hardlink-target-untouched", NULL != text && 0 == strcmp(text, "target\n"));
    free(text);
    unlink(path);
    unlink(other);

    /* and a directory is not a file */
    CHECK("directory-refused", NULL == prte_output_file_open(base, 0644, NULL));

    rmdir(base);
    free(path);
    free(other);
    free(lnk);
    if (0 == failures) {
        fprintf(stdout, "PASSED test_output_file\n");
    }
    return failures;
}
