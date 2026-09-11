/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "constants.h"
#include "src/util/textfile.h"

/* A carriage return is whitespace here, which it was not to the lexer this
 * replaces: its whitespace class was [\f\t\v ], so a file with CRLF line
 * endings met the catch-all error rule and was refused outright.  Writing
 * or editing a hostfile on Windows is an ordinary thing to do and should
 * not be a parse error. */
#define IS_SPACE(c) (' ' == (c) || '\t' == (c) || '\f' == (c) || '\v' == (c) || '\r' == (c))

static bool read_raw_line(prte_textfile_t *tf)
{
    size_t used = 0;
    size_t want;

    for (;;) {
        if (used + 2 > tf->rawsize) {
            want = (0 == tf->rawsize) ? 256 : tf->rawsize * 2;
            tf->raw = (char *) realloc(tf->raw, want);
            if (NULL == tf->raw) {
                tf->rawsize = 0;
                return false;
            }
            tf->rawsize = want;
        }
        if (NULL == fgets(tf->raw + used, (int) (tf->rawsize - used), tf->fp)) {
            /* end of file, or an error - either way, what we have is all
             * there is.  A final line with no newline is still a line. */
            return (0 < used);
        }
        used += strlen(tf->raw + used);
        if (0 < used && '\n' == tf->raw[used - 1]) {
            tf->raw[used - 1] = '\0';
            return true;
        }
        if (feof(tf->fp)) {
            return (0 < used);
        }
        /* the line is longer than the buffer: go round and read the rest
         * of it rather than handing back a fragment as a record */
    }
}

/*
 * Copy the line into tf->code with the comments taken out, carrying an
 * open block comment across the line break.
 *
 * A block comment is replaced by a space, not by nothing, because it
 * separates fields: the lexer this replaces ended a token at one, so a
 * name with a block comment written into the middle of it has always been
 * two names rather than one, and deleting the comment outright would
 * silently join them.
 */
static void strip_comments(prte_textfile_t *tf)
{
    const char *in = tf->raw;
    char *out = tf->code;

    while ('\0' != *in) {
        if (tf->in_comment) {
            if ('*' == in[0] && '/' == in[1]) {
                tf->in_comment = false;
                in += 2;
                *out++ = ' ';
            } else {
                in++;
            }
            continue;
        }
        if ('#' == in[0]) {
            break;
        }
        if ('/' == in[0] && '/' == in[1]) {
            break;
        }
        if ('/' == in[0] && '*' == in[1]) {
            tf->in_comment = true;
            in += 2;
            continue;
        }
        *out++ = *in++;
    }
    *out = '\0';
}

/*
 * Split tf->code into fields in place.  Fields are separated by
 * whitespace, and "=" is always a field of its own so that "slots=4" and
 * "slots = 4" reach the caller identically.
 */
static void split_fields(prte_textfile_t *tf)
{
    static char equals[] = "=";
    char *p = tf->code;
    int n = 0;

    while ('\0' != *p) {
        while (IS_SPACE(*p)) {
            p++;
        }
        if ('\0' == *p) {
            break;
        }
        if ('=' == *p) {
            tf->fields[n++] = equals;
            p++;
            continue;
        }
        tf->fields[n++] = p;
        while ('\0' != *p && !IS_SPACE(*p) && '=' != *p) {
            p++;
        }
        if ('=' == *p) {
            /* end the field here and give the "=" its own entry, so that
             * "slots=4" and "slots = 4" reach the caller alike */
            *p++ = '\0';
            tf->fields[n++] = equals;
        } else if ('\0' != *p) {
            *p++ = '\0';
        }
    }
    tf->fields[n] = NULL;
    tf->nfields = n;
}

int prte_textfile_open(prte_textfile_t *tf, const char *path)
{
    struct stat sbuf;

    memset(tf, 0, sizeof(*tf));

    /* Refuse anything that is not a regular file before opening it.
     * fopen() opens a directory quite happily and every read from the
     * result then fails with EISDIR, which the lexer this replaces did not
     * tell apart from "no input yet" -- so it spun, and naming a directory
     * hung the tool instead of reporting a typo. */
    if (0 == stat(path, &sbuf) && !S_ISREG(sbuf.st_mode)) {
        return PRTE_ERR_BAD_PARAM;
    }

    tf->fp = fopen(path, "r");
    if (NULL == tf->fp) {
        return PRTE_ERR_NOT_FOUND;
    }
    return PRTE_SUCCESS;
}

char **prte_textfile_next(prte_textfile_t *tf)
{
    size_t need;

    if (NULL == tf->fp) {
        return NULL;
    }

    while (read_raw_line(tf)) {
        tf->lineno++;

        /* the stripped line is never longer than the raw one, and it can
         * hold at most one field per character plus the NULL */
        need = strlen(tf->raw) + 1;
        tf->code = (char *) realloc(tf->code, need);
        tf->fields = (char **) realloc(tf->fields, (need + 1) * sizeof(char *));
        if (NULL == tf->code || NULL == tf->fields) {
            return NULL;
        }

        strip_comments(tf);
        split_fields(tf);

        if (0 < tf->nfields) {
            return tf->fields;
        }
        /* the line held nothing but whitespace and comments */
    }
    return NULL;
}

void prte_textfile_close(prte_textfile_t *tf)
{
    if (NULL != tf->fp) {
        fclose(tf->fp);
        tf->fp = NULL;
    }
    if (NULL != tf->raw) {
        free(tf->raw);
        tf->raw = NULL;
    }
    if (NULL != tf->code) {
        free(tf->code);
        tf->code = NULL;
    }
    if (NULL != tf->fields) {
        free(tf->fields);
        tf->fields = NULL;
    }
    tf->rawsize = 0;
    tf->nfields = 0;
    tf->in_comment = false;
}
