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
 * Copyright (c) 2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "constants.h"
#include "src/mca/backtrace/backtrace.h"

int
prrte_backtrace_print(FILE *file, char *prefix, int strip)
{
    int i, len;
    int trace_size;
    void * trace[32];
    char buf[6];
    int fd = prrte_stacktrace_output_fileno;

    if( NULL != file ) {
        fd = fileno(file);
    }

    if (-1 == fd) {
        return PRRTE_ERR_BAD_PARAM;
    }

    trace_size = backtrace (trace, 32);

    for (i = strip; i < trace_size; i++) {
        if (NULL != prefix) {
            write (fd, prefix, strlen (prefix));
        }
        len = snprintf (buf, sizeof(buf), "[%2d] ", i - strip);
        write (fd, buf, len);
        backtrace_symbols_fd (&trace[i], 1, fd);
    }

    return PRRTE_SUCCESS;
}


int
prrte_backtrace_buffer(char ***message_out, int *len_out)
{
    int trace_size;
    void * trace[32];
    char ** funcs = (char **)NULL;

    trace_size = backtrace (trace, 32);
    funcs = backtrace_symbols (trace, trace_size);

    *message_out = funcs;
    *len_out = trace_size;

    return PRRTE_SUCCESS;
}
