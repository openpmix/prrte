/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* A file whose contents decide how this process behaves - bootstrap's
 * prte.conf and the node list it names, the file naming a data server -
 * opened only when nobody but root or the user we run as could have put
 * it there. */

#ifndef PRTE_CONFIG_FILE_H
#define PRTE_CONFIG_FILE_H

#include "prte_config.h"

#include <stdio.h>

BEGIN_C_DECLS

/* Open path to read it, if it is the file at that name - not a symbolic
 * link to one, nor one of several names for it - a regular file owned by
 * root or the user we run as, that nobody else can write but the members
 * of a group we are in, in a directory held to the same rule (or one with
 * the sticky bit, as /tmp has). Run as root, no group may write either:
 * the members of a group root belongs to can be ordinary users. This is
 * PMIx's rule for a parameter file, plus the refusal of links. The file's
 * tests are made on the descriptor actually opened, and the open is
 * non-blocking, since a FIFO would block it.
 *
 * Returns the stream, or NULL with *why saying which test failed - NULL
 * when the file could not be opened at all, errno saying why. */
PRTE_EXPORT FILE *prte_open_config_file(const char *path, const char **why);

END_C_DECLS

#endif
