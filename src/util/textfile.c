/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "constants.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/prte_show_help.h"
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

/* the open itself, made as the user the file is read for - in a child that
 * has become that user, so it may only make the call */
static int textfile_open_fn(void *arg)
{
    return open((const char *) arg, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

int prte_textfile_open(prte_textfile_t *tf, const char *path, uid_t uid, gid_t gid)
{
    struct stat sbuf;
    int fd, flags;

    memset(tf, 0, sizeof(*tf));

    /* Refuse anything that is not a regular file. fopen() opens a directory
     * quite happily and every read from the result then fails with EISDIR,
     * which the lexer this replaces did not tell apart from "no input yet"
     * -- so it spun, and naming a directory hung the tool instead of
     * reporting a typo.
     *
     * The test is made on the file actually opened, not on the path
     * beforehand, so it describes the file that is read. These files (a
     * hostfile, a rankfile, a sequence file named in "--map-by") are read
     * on the DVM master's progress thread, where a blocking open() of a
     * FIFO would stall it; opening non-blocking avoids that, and fstat() on
     * the descriptor answers for the file we hold. Blocking is turned back on before any read: a regular
     * file never blocks anyway, and stdio is not written to expect EAGAIN.
     *
     * The open is made as the user the file is read for
     * (prte_pmix_server_open_as), so what that user may not read is not
     * read for it; everything after it is done here, on the descriptor. */
    fd = prte_pmix_server_open_as(uid, gid, textfile_open_fn, (void *) path);
    if (0 > fd) {
        return PRTE_ERR_NOT_FOUND;
    }
    if (0 != fstat(fd, &sbuf) || !S_ISREG(sbuf.st_mode)) {
        close(fd);
        return PRTE_ERR_BAD_PARAM;
    }
    if ((off_t) PRTE_TEXTFILE_MAX_BYTES < sbuf.st_size) {
        /* opened, and nothing to read: the first read reports the limit,
         * which is where every caller already looks for a failure */
        close(fd);
        tf->failed = true;
        tf->limit = PRTE_TEXTFILE_LIMIT_SIZE;
        return PRTE_SUCCESS;
    }
    flags = fcntl(fd, F_GETFL);
    if (0 > flags || 0 > fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) {
        close(fd);
        return PRTE_ERR_NOT_FOUND;
    }
    tf->fp = fdopen(fd, "r");
    if (NULL == tf->fp) {
        close(fd);
        return PRTE_ERR_NOT_FOUND;
    }
    return PRTE_SUCCESS;
}

/*
 * One physical line, without its newline, in storage the caller frees -
 * pmix_getline(), with the limits: a NUL byte or a read error fails the
 * line, and so does a line longer than PRTE_TEXTFILE_MAX_LINE or a file
 * that has grown past PRTE_TEXTFILE_MAX_BYTES since it was opened. Each is
 * stopped where it is met, so neither is ever held in memory whole.
 */
static char *read_line(prte_textfile_t *tf)
{
    char *line = NULL, *grown;
    size_t len = 0, size = 0;
    bool started = false;
    int c;

    for (;;) {
        c = getc(tf->fp);
        if (EOF == c) {
            if (ferror(tf->fp)) {
                goto fail;
            }
            /* a final line with no newline is still a line */
            break;
        }
        started = true;
        if (PRTE_TEXTFILE_MAX_BYTES <= tf->bytes) {
            tf->limit = PRTE_TEXTFILE_LIMIT_SIZE;
            goto fail;
        }
        tf->bytes++;
        if ('\n' == c) {
            break;
        }
        if ('\0' == c) {
            goto fail;
        }
        if (PRTE_TEXTFILE_MAX_LINE <= len) {
            /* the line being read is the next one */
            tf->limit = PRTE_TEXTFILE_LIMIT_LINE;
            tf->lineno++;
            goto fail;
        }
        /* room for this character and the terminator after it */
        if (len + 2 > size) {
            size = (0 == size) ? 256 : 2 * size;
            grown = (char *) realloc(line, size);
            if (NULL == grown) {
                goto fail;
            }
            line = grown;
        }
        line[len++] = (char) c;
    }

    if (!started) {
        /* the end of the stream, with nothing at all on this line */
        return NULL;
    }
    if (NULL == line) {
        /* an empty line is still a line */
        line = strdup("");
        if (NULL == line) {
            goto fail;
        }
        return line;
    }
    line[len] = '\0';
    return line;

fail:
    free(line);
    tf->failed = true;
    return NULL;
}

char *prte_textfile_getline(prte_textfile_t *tf)
{
    char *line;

    if (NULL == tf->fp) {
        return NULL;
    }
    line = read_line(tf);
    if (NULL != line) {
        tf->lineno++;
    }
    return line;
}

bool prte_textfile_report_limit(prte_textfile_t *tf, const char *nspace, const char *path)
{
    switch (tf->limit) {
    case PRTE_TEXTFILE_LIMIT_SIZE:
        prte_show_help(nspace, "help-prte-util.txt", "file-too-large", true, path,
                       PRTE_TEXTFILE_MAX_BYTES / (1024 * 1024));
        return true;
    case PRTE_TEXTFILE_LIMIT_LINE:
        prte_show_help(nspace, "help-prte-util.txt", "file-line-too-long", true, path,
                       tf->lineno, PRTE_TEXTFILE_MAX_LINE / 1024);
        return true;
    default:
        return false;
    }
}

char **prte_textfile_next(prte_textfile_t *tf)
{
    size_t need;
    char *code;
    char **fields;

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
        tf->raw = read_line(tf);
        if (NULL == tf->raw) {
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
    /* "failed", "limit" and "lineno" are left as they are: together they are the
     * answer to a question a caller may still be asking after closing the
     * file - whether the read reached the end, and where it stopped if not.
     * prte_textfile_open() zeroes the whole struct, so reusing one for a
     * second file does not inherit them. */
}
