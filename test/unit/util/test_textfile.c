/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the line reader the hostfile and rankfile parsers share.
 *
 * Everything here is about the two jobs it does before its caller sees a
 * thing -- deciding where a line ends and where a comment does -- because
 * those are the decisions that used to belong to a flex scanner, and the
 * ones a caller cannot check for itself.  Each case renders the reader's
 * whole answer for a file as "<lineno>:<field>|<field>;" so that a line
 * skipped, a field split in the wrong place, or a line number that drifts
 * is a visible difference rather than a silent one.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/util/pmix_printf.h"
#include "src/util/textfile.h"

int test_textfile(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [textfile:%s]: %s\n", label, #cond);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

static char *read_all(const char *body, size_t bodylen)
{
    prte_textfile_t tf;
    char path[256];
    char *out = NULL, *tmp;
    char **f;
    FILE *fp;
    int i;

    snprintf(path, sizeof(path), "prte_tf_%lu_%p.txt", (unsigned long) getpid(), (void *) body);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return NULL;
    }
    fwrite(body, 1, bodylen, fp);
    fclose(fp);

    if (PRTE_SUCCESS != prte_textfile_open(&tf, path)) {
        unlink(path);
        return strdup("<open failed>");
    }

    out = strdup("");
    while (NULL != (f = prte_textfile_next(&tf))) {
        pmix_asprintf(&tmp, "%s%d:", out, tf.lineno);
        free(out);
        out = tmp;
        for (i = 0; NULL != f[i]; i++) {
            pmix_asprintf(&tmp, "%s%s%s", out, (0 == i) ? "" : "|", f[i]);
            free(out);
            out = tmp;
        }
        pmix_asprintf(&tmp, "%s;", out);
        free(out);
        out = tmp;
    }
    prte_textfile_close(&tf);
    unlink(path);
    return out;
}

/* As read_all(), but also reports whether the reader stopped short.  A
 * short read is not visible in the fields -- that is the whole point of the
 * "failed" flag -- so it needs its own way in. */
static char *read_all_status(const char *body, size_t bodylen, bool *failed)
{
    prte_textfile_t tf;
    char path[256];
    char *out = NULL, *tmp;
    char **f;
    FILE *fp;
    int i;

    *failed = false;
    snprintf(path, sizeof(path), "prte_tf_%lu_%p.txt", (unsigned long) getpid(), (void *) body);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return NULL;
    }
    fwrite(body, 1, bodylen, fp);
    fclose(fp);

    if (PRTE_SUCCESS != prte_textfile_open(&tf, path)) {
        unlink(path);
        return strdup("<open failed>");
    }

    out = strdup("");
    while (NULL != (f = prte_textfile_next(&tf))) {
        pmix_asprintf(&tmp, "%s%d:", out, tf.lineno);
        free(out);
        out = tmp;
        for (i = 0; NULL != f[i]; i++) {
            pmix_asprintf(&tmp, "%s%s%s", out, (0 == i) ? "" : "|", f[i]);
            free(out);
            out = tmp;
        }
        pmix_asprintf(&tmp, "%s;", out);
        free(out);
        out = tmp;
    }
    *failed = tf.failed;
    prte_textfile_close(&tf);
    unlink(path);
    return out;
}

static int expect(const char *label, const char *body, const char *want, int *failures)
{
    char *got = read_all(body, strlen(body));

    if (NULL == got) {
        fprintf(stderr, "FAIL [textfile:%s]: could not write a temp file\n", label);
        (*failures)++;
        return 1;
    }
    if (0 != strcmp(got, want)) {
        fprintf(stderr, "FAIL [textfile:%s]\n  expected: %s\n  actual:   %s\n", label, want, got);
        (*failures)++;
        free(got);
        return 1;
    }
    free(got);
    return 0;
}

int test_textfile(void)
{
    int failures = 0;
    prte_textfile_t tf;
    char *big, *want, *got;
    size_t i;

    /* --- fields and the equals sign ------------------------------- */
    expect("one field", "hostA\n", "1:hostA;", &failures);
    expect("two fields", "hostA hostB\n", "1:hostA|hostB;", &failures);
    expect("an equals binds tight", "slots=4\n", "1:slots|=|4;", &failures);
    expect("an equals with spaces", "slots = 4\n", "1:slots|=|4;", &failures);
    expect("an equals on the left only", "slots= 4\n", "1:slots|=|4;", &failures);
    expect("an equals on the right only", "slots =4\n", "1:slots|=|4;", &failures);
    expect("two equals in one field", "a=b=c\n", "1:a|=|b|=|c;", &failures);
    expect("a bare equals", "=\n", "1:=;", &failures);
    expect("tabs separate", "hostA\tslots=4\n", "1:hostA|slots|=|4;", &failures);
    expect("leading whitespace", "   hostA\n", "1:hostA;", &failures);
    expect("trailing whitespace", "hostA   \n", "1:hostA;", &failures);

    /* --- line endings ---------------------------------------------- */
    expect("no trailing newline", "hostA", "1:hostA;", &failures);
    expect("crlf", "hostA\r\nhostB\r\n", "1:hostA;2:hostB;", &failures);
    expect("a lone carriage return is whitespace", "hostA\rhostB\n", "1:hostA|hostB;", &failures);
    expect("blank lines are skipped but counted", "\n\nhostA\n", "3:hostA;", &failures);
    expect("a whitespace-only line is blank", "   \nhostA\n", "2:hostA;", &failures);
    expect("an empty file", "", "", &failures);

    /* --- comments --------------------------------------------------- */
    expect("hash to end of line", "hostA # and this\n", "1:hostA;", &failures);
    expect("a whole-line hash comment", "# all of it\nhostA\n", "2:hostA;", &failures);
    expect("slash-slash to end of line", "hostA // and this\n", "1:hostA;", &failures);
    expect("a block comment mid-line", "hostA /* x */ slots=4\n", "1:hostA|slots|=|4;", &failures);
    expect("a block comment before a field", "/* x */ hostA\n", "1:hostA;", &failures);
    expect("a block comment does not join fields", "host/* x */A\n", "1:host|A;", &failures);
    expect("a block comment across lines", "/* one\n   two */ hostA\n", "2:hostA;", &failures);
    expect("a block comment swallowing a line", "hostA\n/* x\n   y */\nhostB\n", "1:hostA;4:hostB;",
           &failures);
    expect("an unterminated block comment", "hostA\n/* never closed\nhostB\n", "1:hostA;",
           &failures);
    expect("a hash inside a field", "host#A\n", "1:host;", &failures);
    expect("a hash inside a block comment", "/* # */ hostA\n", "1:hostA;", &failures);
    expect("a block marker inside a line comment", "# /* x\nhostA\n", "2:hostA;", &failures);

    /* --- a line longer than any fixed buffer ------------------------
     * pmix_getline() would hand back the first 1023 bytes and read the
     * remainder as a record of its own.  A rankfile naming a long cpu list
     * is not an absurd thing to write, so the reader must not have a
     * maximum line length at all. */
    big = (char *) malloc(9000);
    want = (char *) malloc(9000);
    if (NULL != big && NULL != want) {
        strcpy(big, "hostA slot=");
        for (i = 0; i < 4000; i++) {
            strcat(big, "9");
        }
        strcpy(want, "1:hostA|slot|=|");
        strcat(want, big + strlen("hostA slot="));
        strcat(want, ";");
        strcat(big, "\n");
        got = read_all(big, strlen(big));
        CHECK("a 4000-character field survives", NULL != got && 0 == strcmp(got, want));
        if (NULL != got) {
            free(got);
        }
    }
    free(big);
    free(want);

    /* --- a byte a text file cannot mean -----------------------------
     * fgets() reports a line as a C string, so it cannot tell a NUL in the
     * file from the end of the line: read that way, everything after the
     * NUL is dropped and the following line is read onto the end of what
     * came before it.  A hostfile saved as UTF-16 is exactly that file --
     * a NUL after every ASCII character -- and it would have produced a
     * node list assembled out of fragments and called it a success. */
    {
        static const char nul_mid[] = "hostA\nhost\0B\nhostC\n";
        static const char utf16[] = "h\0o\0s\0t\0A\0\n\0";
        bool failed = false;

        got = read_all_status(nul_mid, sizeof(nul_mid) - 1, &failed);
        CHECK("a NUL byte stops the read", NULL != got && 0 == strcmp(got, "1:hostA;"));
        CHECK("a NUL byte is reported as a short read", failed);
        if (NULL != got) {
            free(got);
        }

        got = read_all_status(utf16, sizeof(utf16) - 1, &failed);
        CHECK("a UTF-16 file yields no nodes", NULL != got && 0 == strcmp(got, ""));
        CHECK("a UTF-16 file is reported as a short read", failed);
        if (NULL != got) {
            free(got);
        }
    }

    /* and an ordinary file is not reported as a short read */
    {
        bool failed = true;

        got = read_all_status("hostA\nhostB\n", 12, &failed);
        CHECK("a clean file reads clean", NULL != got && 0 == strcmp(got, "1:hostA;2:hostB;"));
        CHECK("a clean file is not a short read", !failed);
        if (NULL != got) {
            free(got);
        }
    }

    /* --- what open() refuses ---------------------------------------- */
    CHECK("a missing file is not found",
          PRTE_ERR_NOT_FOUND == prte_textfile_open(&tf, "prte-no-such-file-anywhere"));
    prte_textfile_close(&tf);
    CHECK("a directory is refused as a bad path",
          PRTE_ERR_BAD_PARAM == prte_textfile_open(&tf, "."));
    prte_textfile_close(&tf);
    /* ...and closing a file that was never opened, twice, is harmless */
    prte_textfile_close(&tf);

    if (0 == failures) {
        fprintf(stdout, "  PASS test_textfile\n");
    }
    return failures;
}
