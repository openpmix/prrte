/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "constants.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/util/pmix_runas.h"
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

/*
 * What the child found, in the first byte of the copy it makes. Everything
 * after that byte is the file.
 */
#define TF_REGULAR     'r' /* a regular file, copied whole */
#define TF_NOT_REGULAR 'n' /* not a regular file: nothing copied */
#define TF_TOO_LARGE   's' /* larger than PRTE_TEXTFILE_MAX_BYTES: nothing copied */
#define TF_READ_ERROR  'e' /* opened, and a read failed part way */

typedef struct {
    const char *path;
    int copy; /* the master's temporary file, inherited by the child */
} textfile_copy_t;

static int put_status(int fd, char status)
{
    ssize_t nb;

    do {
        nb = pwrite(fd, &status, 1, 0);
    } while (0 > nb && EINTR == errno);
    return (1 == nb) ? 0 : -1;
}

/*
 * The open and the whole read, made as the user the file is read for, in
 * a child (prte_pmix_server_work_as) - so it may only make the calls: no
 * stdio, no output, nothing that takes a lock. The copy goes into the
 * master's temporary file at offset 1, the status into offset 0 once it is
 * known, both by position, so nothing depends on the descriptor's shared
 * offset. At most one byte more than PRTE_TEXTFILE_MAX_BYTES is copied: a
 * file that grew past the limit after it was measured is then still seen
 * to be too large by read_line(), and costs the child no more than that.
 */
static int textfile_copy_fn(void *arg)
{
    textfile_copy_t *tc = (textfile_copy_t *) arg;
    char buf[16384];
    struct stat sbuf;
    off_t out = 1;
    size_t left = (size_t) PRTE_TEXTFILE_MAX_BYTES + 1, want;
    ssize_t nr, nw, done;
    int fd, flags;

    /* non-blocking, so that a FIFO does not hold the open; the test that
     * follows is made on the file actually opened */
    fd = open(tc->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (0 > fd) {
        return -1;
    }
    if (0 != fstat(fd, &sbuf) || !S_ISREG(sbuf.st_mode)) {
        close(fd);
        return put_status(tc->copy, TF_NOT_REGULAR);
    }
    if ((off_t) PRTE_TEXTFILE_MAX_BYTES < sbuf.st_size) {
        close(fd);
        return put_status(tc->copy, TF_TOO_LARGE);
    }
    flags = fcntl(fd, F_GETFL);
    if (0 > flags || 0 > fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) {
        close(fd);
        return -1;
    }
    while (0 < left) {
        want = (left < sizeof(buf)) ? left : sizeof(buf);
        do {
            nr = read(fd, buf, want);
        } while (0 > nr && EINTR == errno);
        if (0 > nr) {
            close(fd);
            return put_status(tc->copy, TF_READ_ERROR);
        }
        if (0 == nr) {
            break;
        }
        for (done = 0; done < nr; done += nw) {
            do {
                nw = pwrite(tc->copy, buf + done, (size_t) (nr - done), out + done);
            } while (0 > nw && EINTR == errno);
            if (0 >= nw) {
                close(fd);
                return -1;
            }
        }
        out += nr;
        left -= (size_t) nr;
    }
    close(fd);
    return put_status(tc->copy, TF_REGULAR);
}

int prte_textfile_open(prte_textfile_t *tf, const char *path, uid_t uid, gid_t gid)
{
    textfile_copy_t tc;
    FILE *copy;
    int status;

    memset(tf, 0, sizeof(*tf));

    /* These files - a hostfile, a rankfile, a sequence file named in
     * "--map-by" - are read on the DVM master's progress thread, which must
     * not block. So nothing about the file is waited on here: the open and every read of it are made in a
     * child (textfile_copy_fn, by prte_pmix_server_work_as), which the
     * master waits for no longer than PMIx's pmix_runas_timeout, and which copies
     * the file into a temporary one of ours that the master then reads. A
     * path on a filesystem that stops answering - at the open, or half way
     * through the read - then costs the master that long and no longer.
     *
     * The child is the user the file is read for, so what that user may not
     * read is not read for it.
     *
     * Anything that is not a regular file is refused. fopen() opens a
     * directory quite happily and every read from the result then fails
     * with EISDIR, which the lexer this replaces did not tell apart from
     * "no input yet" - so it spun, and naming a directory hung the tool
     * instead of reporting a typo. */
    copy = tmpfile();
    if (NULL == copy) {
        return PRTE_ERR_NOT_FOUND;
    }
    /* it holds what the user's file holds: a process we launch closes what
     * it inherits anyway, and nothing else should get it either */
    (void) fcntl(fileno(copy), F_SETFD, FD_CLOEXEC);
    tc.path = path;
    tc.copy = fileno(copy);
    if (0 != prte_pmix_server_work_as(uid, gid, textfile_copy_fn, &tc)) {
        if (ETIMEDOUT == errno) {
            /* opened or not, nothing usable: the first read reports it, which
             * is where every caller already looks for a failure */
            fclose(copy);
            tf->failed = true;
            tf->limit = PRTE_TEXTFILE_LIMIT_TIME;
            return PRTE_SUCCESS;
        }
        fclose(copy);
        return PRTE_ERR_NOT_FOUND;
    }
    rewind(copy);
    status = getc(copy);
    switch (status) {
    case TF_REGULAR:
        tf->fp = copy;
        return PRTE_SUCCESS;
    case TF_NOT_REGULAR:
        fclose(copy);
        return PRTE_ERR_BAD_PARAM;
    case TF_TOO_LARGE:
        fclose(copy);
        tf->failed = true;
        tf->limit = PRTE_TEXTFILE_LIMIT_SIZE;
        return PRTE_SUCCESS;
    case TF_READ_ERROR:
        fclose(copy);
        tf->failed = true;
        return PRTE_SUCCESS;
    default:
        fclose(copy);
        return PRTE_ERR_NOT_FOUND;
    }
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
    case PRTE_TEXTFILE_LIMIT_TIME:
        prte_show_help(nspace, "help-prte-util.txt", "file-not-answered", true, path,
                       pmix_util_runas_timeout);
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
