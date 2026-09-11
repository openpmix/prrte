/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2013      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Parsing for the rankfile format.
 *
 * This is a reader for a file the user wrote, which is why it sits here
 * beside the hostfile reader rather than inside rmaps/rank_file: what the
 * mapping component does with the result is policy, but turning the text
 * into records is not, and a parser that can only be reached by selecting a
 * mapper cannot be tested on its own.
 */

#ifndef PRTE_UTIL_RANKFILE_H
#define PRTE_UTIL_RANKFILE_H

#include "prte_config.h"

#include "src/class/pmix_object.h"
#include "src/class/pmix_pointer_array.h"

BEGIN_C_DECLS

/*
 * One line of a rankfile: where the rank named by this record's position in
 * the map is to run, and the cpus it is to be bound to.  NULL slot_list
 * means the line named a node and stopped.
 *
 * The cpu list is allocated rather than a fixed 64-byte array.  It was the
 * array, copied into up to its length and no further with nothing said, so
 * a rank given a long explicit cpu list -- "0,1,2,...,25" is already over
 * the limit -- was bound to a prefix of what the user asked for.
 */
typedef struct {
    pmix_object_t super;
    char *node_name;
    char *slot_list;
} prte_rankfile_map_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_rankfile_map_t);

/*
 * Read a rankfile into "rankmap", which the caller supplies already
 * constructed and initialized: each line's record is filed at the index of
 * the rank it describes.  "num_ranks" is incremented once per rank the file
 * names, so a caller reusing a map across calls sees the running total.
 *
 * Returns PRTE_SUCCESS, or an error after saying what was wrong with the
 * file through prte_show_help().  On failure the map holds whatever was
 * read before the bad line; reclaiming it is the caller's, since the caller
 * is what owns it.
 */
PRTE_EXPORT int prte_util_parse_rankfile(const char *rankfile,
                                         pmix_pointer_array_t *rankmap,
                                         int *num_ranks);

END_C_DECLS

#endif
