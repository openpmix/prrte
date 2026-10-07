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
#include "src/util/pmix_string_copy.h"
#include "src/util/textfile.h"

/* A carriage return is whitespace here, which it was not to the lexer this
 * replaces: its whitespace class was [\f\t\v ], so a file with CRLF line
 * endings met the catch-all error rule and was refused outright.  Writing
 * or editing a hostfile on Windows is an ordinary thing to do and should
 * not be a parse error. */
#define IS_SPACE(c) (' ' == (c) || '\t' == (c) || '\f' == (c) || '\v' == (c) || '\r' == (c))

/*
 * Copy the line into tf->code with the comments taken out, carrying an
 * open block comment across the line break.
 *
 * A block comment is replaced by a space, not by nothing, because it
 * separates fields: the lexer this replaces emitted a newline token at
 * each marker, so a name with a block comment written into the middle of
 * it has always been two names rather than one, and deleting the comment
 * outright would silently join them.
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
    char *code;
    char **fields;
    bool failed;

    if (NULL == tf->fp) {
        return NULL;
    }

    for (;;) {
        /* a NUL byte is refused along with a read error.  The fields reach
         * their callers as C strings, so the rest of its line would vanish;
         * and a hostfile saved as UTF-16 - which is what a Windows editor
         * will do if asked - is a NUL after every ASCII character, which
         * would otherwise be a node list built from fragments, reported as
         * a success */
        free(tf->raw);
        tf->raw = pmix_getline(tf->fp, &failed);
        if (NULL == tf->raw) {
            if (failed) {
                tf->failed = true;
            }
            return NULL;
        }
        tf->lineno++;

        /* the stripped line is never longer than the raw one, and it can
         * hold at most one field per character plus the NULL.  Assign each
         * buffer only once its realloc has worked, so a failure leaves the
         * old one where prte_textfile_close() will find and free it. */
        need = strlen(tf->raw) + 1;
        code = (char *) realloc(tf->code, need);
        if (NULL == code) {
            tf->failed = true;
            return NULL;
        }
        tf->code = code;
        fields = (char **) realloc(tf->fields, (need + 1) * sizeof(char *));
        if (NULL == fields) {
            tf->failed = true;
            return NULL;
        }
        tf->fields = fields;

        strip_comments(tf);
        split_fields(tf);

        if (0 < tf->nfields) {
            return tf->fields;
        }
        /* the line held nothing but whitespace and comments */
    }
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
    tf->nfields = 0;
    tf->in_comment = false;
    /* "failed" and "lineno" are left as they are: together they are the
     * answer to a question a caller may still be asking after closing the
     * file - whether the read reached the end, and where it stopped if not.
     * prte_textfile_open() zeroes the whole struct, so reusing one for a
     * second file does not inherit them. */
}
