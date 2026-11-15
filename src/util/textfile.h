/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Reading a line-oriented configuration file.
 *
 * PRRTE's hostfile and rankfile are both "one record per line, fields
 * separated by whitespace, with comments" -- the same shape, read by two
 * copies of the same lexer.  This is that shape, in one place: it hands
 * back one logical line at a time, already stripped of comments and split
 * into fields, and the callers spend their code on what the fields mean.
 *
 * Comments are the three forms both formats accepted: "#" and "//" run to
 * the end of the line, and a block comment runs to its closing marker,
 * across as many lines as it takes.  A block comment that is never closed
 * swallows the rest of the file, which is what the lexer did.
 *
 * "=" is always a field of its own, so a caller sees the same three fields
 * whether the file says "slots=4" or "slots = 4".  Both spellings have
 * always parsed and both still do.
 *
 * A line is read whole, never in pieces: a rankfile line naming a long cpu
 * list is not an absurd thing to write, and a line read in pieces would
 * have its second half taken for a record of its own.  It is held to
 * PRTE_TEXTFILE_MAX_LINE, and the file to PRTE_TEXTFILE_MAX_BYTES (below).
 *
 * The file has to be text.  A NUL byte stops the read and sets "failed",
 * because the fields reach their callers as C strings and there is nothing
 * a line-oriented configuration file can mean by one -- and because the
 * alternative is worse than a refusal: read as a string, a NUL is
 * indistinguishable from the end of the line, so the rest of that line is
 * dropped and the next one read onto the end of it.  A hostfile saved as
 * UTF-16 is a NUL after every ASCII character, and that is not a file
 * nobody writes: this reader already treats CR as whitespace for the same
 * user.
 */

#ifndef PRTE_UTIL_TEXTFILE_H
#define PRTE_UTIL_TEXTFILE_H

#include "prte_config.h"

#include <stdio.h>
#include <sys/types.h>

BEGIN_C_DECLS

/*
 * How much a file may hold.  These files are named by whoever starts a job,
 * and they are read on the DVM master's progress thread, which serves every
 * user of the DVM while it reads: so what one file may cost is bounded.  A
 * file larger than PRTE_TEXTFILE_MAX_BYTES, or with a line longer than
 * PRTE_TEXTFILE_MAX_LINE, is refused - "failed" is set, and "limit" says
 * which bound it met (prte_textfile_report_limit).  The size is checked on
 * the descriptor when the file is opened and again as it is read, since a
 * file can grow after it is opened.  The largest real files are rankfiles,
 * at a line per rank: a million ranks is some 40 MB.
 *
 * And what a file may cost in time is bounded too.  The file is opened and
 * read whole in a child, which copies it into a temporary file the master
 * then reads (prte_pmix_server_work_as), and the master waits for that
 * child no longer than PMIx's pmix_runas_timeout (10 seconds by default) - for
 * a file on a filesystem that has stopped answering, at the open or part
 * way through, as for any other.  A read that took longer is refused the
 * same way, with "limit" PRTE_TEXTFILE_LIMIT_TIME.
 */
#define PRTE_TEXTFILE_MAX_BYTES (64 * 1024 * 1024)
#define PRTE_TEXTFILE_MAX_LINE  (1024 * 1024)

typedef enum {
    PRTE_TEXTFILE_LIMIT_NONE = 0,
    PRTE_TEXTFILE_LIMIT_SIZE, /* the file is larger than PRTE_TEXTFILE_MAX_BYTES */
    PRTE_TEXTFILE_LIMIT_LINE, /* line "lineno" is longer than PRTE_TEXTFILE_MAX_LINE */
    PRTE_TEXTFILE_LIMIT_TIME  /* the file was not read within PMIx's pmix_runas_timeout */
} prte_textfile_limit_t;

typedef struct {
    FILE *fp;
    char *raw;       /* one physical line, however long */
    char *code;      /* that line with the comments taken out */
    char **fields;   /* the split line; ours, and replaced on each call */
    int nfields;
    int lineno;      /* 1-based, of the line the fields came from */
    bool in_comment; /* a block comment is open across this line break */
    bool failed;     /* reading stopped short of the end of the file */
    size_t bytes;    /* read so far, held to PRTE_TEXTFILE_MAX_BYTES */
    prte_textfile_limit_t limit; /* which bound stopped the read, if one did */
} prte_textfile_t;

/*
 * Open a file for reading, as user uid and group gid: the owner of the job
 * that named it, the requester of an allocation that did, or our own user
 * (prte_process_info.euid/egid) for the DVM's own configuration.  The open
 * and the read are made as that user (prte_pmix_server_work_as), so a file
 * it could not read is not read for it.  Returns PRTE_SUCCESS, PRTE_ERR_NOT_FOUND if it
 * cannot be opened (by that user), or PRTE_ERR_BAD_PARAM if the path is not
 * a regular file -- which is worth telling apart, because fopen() opens a
 * directory quite happily and every read from the result then fails in a
 * way the caller has no reason to expect.  Saying nothing to the user is
 * deliberate: which of those two deserves which message differs between
 * the callers, so each says its own.
 */
PRTE_EXPORT int prte_textfile_open(prte_textfile_t *tf, const char *path, uid_t uid, gid_t gid);

/*
 * Return the next line that has anything on it, as a NULL-terminated array
 * of fields, or NULL at end of file.  Blank lines and lines holding only a
 * comment are skipped, so every array returned has at least one field.
 * The array belongs to the file and is replaced by the next call.
 *
 * NULL is also what a read error or a failed allocation returns, and a
 * caller must tell the two apart by checking "failed" once the loop ends.
 * Taking an early NULL for the end of the file accepts whatever was read
 * before the failure as the whole file: a hostfile on a network file system
 * that stops answering halfway through would be an allocation of half its
 * nodes, reported as a success.
 */
PRTE_EXPORT char **prte_textfile_next(prte_textfile_t *tf);

/*
 * Return the next physical line as it is, without its newline - comments
 * and all - in storage the caller frees, or NULL at the end of the file or
 * on a failure, which "failed" tells apart as for prte_textfile_next().  For
 * a reader with a syntax of its own; it is held to the same limits.
 */
PRTE_EXPORT char *prte_textfile_getline(prte_textfile_t *tf);

/*
 * If the read stopped at one of the limits above, say so to nspace for the
 * file at path and return true; otherwise say nothing and return false, for
 * the caller to report the failure as it reports any other.  Usable after
 * prte_textfile_close().
 */
PRTE_EXPORT bool prte_textfile_report_limit(prte_textfile_t *tf, const char *nspace,
                                            const char *path);

/* Close the file and release everything it is holding.  Safe to call on a
 * file that was never opened, and safe to call twice. */
PRTE_EXPORT void prte_textfile_close(prte_textfile_t *tf);

END_C_DECLS

#endif
