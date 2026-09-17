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

/* Make room for `need` bytes in tf->raw, doubling rather than sizing to
 * fit so that reading a long line is not quadratic. */
static bool raw_room(prte_textfile_t *tf, size_t need)
{
    size_t want;
    char *grown;

    if (need <= tf->rawsize) {
        return true;
    }
    want = (0 == tf->rawsize) ? 256 : tf->rawsize;
    while (want < need) {
        want *= 2;
    }
    grown = (char *) realloc(tf->raw, want);
    if (NULL == grown) {
        /* tf->raw is still ours, and prte_textfile_close() frees it */
        return false;
    }
    tf->raw = grown;
    tf->rawsize = want;
    return true;
}

/*
 * Read one physical line into tf->raw, without its newline.
 *
 * This reads a character at a time rather than with fgets() because fgets()
 * cannot say how much it read: it reports the line as a C string, so a NUL
 * byte in the file is indistinguishable from the end of the line, and
 * everything after it on that line is silently dropped and the next line
 * read onto the end of it.  That is not a theoretical file.  A hostfile
 * saved as UTF-16 -- which is what a Windows editor will do if asked, and
 * this reader already accommodates that user by treating CR as whitespace
 * -- is a NUL after every ASCII character, and it would have produced a
 * node list built from fragments and reported it as a success.
 *
 * So a NUL is refused outright, the same way a read error is: the fields
 * reach their callers as C strings and there is nothing a text
 * configuration file can mean by one.
 */
static bool read_raw_line(prte_textfile_t *tf)
{
    size_t used = 0;
    bool started = false;
    int c;

    for (;;) {
        c = fgetc(tf->fp);
        if (EOF == c) {
            if (ferror(tf->fp)) {
                /* not the end of the file: whatever is left of it was never
                 * read, so neither this fragment nor the lines before it
                 * are the whole of what the file says */
                tf->failed = true;
                return false;
            }
            /* A final line with no newline is still a line. */
            break;
        }
        started = true;
        if ('\n' == c) {
            break;
        }
        if ('\0' == c) {
            tf->failed = true;
            return false;
        }
        /* room for this character and the terminator that follows it */
        if (!raw_room(tf, used + 2)) {
            tf->failed = true;
            return false;
        }
        tf->raw[used++] = (char) c;
    }

    if (!started) {
        /* end of the file, with nothing at all on this line */
        return false;
    }
    /* an empty line is still a line, and still needs a terminator */
    if (!raw_room(tf, used + 1)) {
        tf->failed = true;
        return false;
    }
    tf->raw[used] = '\0';
    return true;
}

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

    if (NULL == tf->fp) {
        return NULL;
    }

    while (read_raw_line(tf)) {
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
    /* "failed" and "lineno" are left as they are: together they are the
     * answer to a question a caller may still be asking after closing the
     * file - whether the read reached the end, and where it stopped if not.
     * prte_textfile_open() zeroes the whole struct, so reusing one for a
     * second file does not inherit them. */
}
