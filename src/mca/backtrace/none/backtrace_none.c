/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"

#include <stdio.h>

#include "constants.h"
#include "src/mca/backtrace/backtrace.h"

int
prrte_backtrace_print(FILE *file, char *prefix, int strip)
{
    return PRRTE_ERR_NOT_IMPLEMENTED;
}


int
prrte_backtrace_buffer(char ***message_out, int *len_out)
{
    *message_out = NULL;
    *len_out = 0;

    return PRRTE_ERR_NOT_IMPLEMENTED;
}
