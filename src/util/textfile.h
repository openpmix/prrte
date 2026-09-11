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
 * A line of any length is read.  That is worth saying because the obvious
 * alternative, pmix_getline(), reads into a 1024-byte buffer and hands
 * back whatever fits: a longer line would arrive split in two, and the
 * second half would be read as a record of its own.  A rankfile line
 * naming a long cpu list is not an absurd thing to write.
 */

#ifndef PRTE_UTIL_TEXTFILE_H
#define PRTE_UTIL_TEXTFILE_H

#include "prte_config.h"

#include <stdio.h>

BEGIN_C_DECLS

typedef struct {
    FILE *fp;
    char *raw;       /* one physical line, however long */
    size_t rawsize;
    char *code;      /* that line with the comments taken out */
    char **fields;   /* the split line; ours, and replaced on each call */
    int nfields;
    int lineno;      /* 1-based, of the line the fields came from */
    bool in_comment; /* a block comment is open across this line break */
} prte_textfile_t;

/*
 * Open a file for reading.  Returns PRTE_SUCCESS, PRTE_ERR_NOT_FOUND if it
 * cannot be opened, or PRTE_ERR_BAD_PARAM if the path is not a regular
 * file -- which is worth telling apart, because fopen() opens a directory
 * quite happily and every read from the result then fails in a way the
 * caller has no reason to expect.  Saying nothing to the user is
 * deliberate: which of those two deserves which message differs between
 * the callers, so each says its own.
 */
PRTE_EXPORT int prte_textfile_open(prte_textfile_t *tf, const char *path);

/*
 * Return the next line that has anything on it, as a NULL-terminated array
 * of fields, or NULL at end of file.  Blank lines and lines holding only a
 * comment are skipped, so every array returned has at least one field.
 * The array belongs to the file and is replaced by the next call.
 */
PRTE_EXPORT char **prte_textfile_next(prte_textfile_t *tf);

/* Close the file and release everything it is holding.  Safe to call on a
 * file that was never opened, and safe to call twice. */
PRTE_EXPORT void prte_textfile_close(prte_textfile_t *tf);

END_C_DECLS

#endif
