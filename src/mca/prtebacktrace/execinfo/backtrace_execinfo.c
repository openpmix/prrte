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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_EXECINFO_H
#    include <execinfo.h>
#endif

#include "constants.h"
#include "src/util/pmix_fd.h"
#include "src/mca/prtebacktrace/prtebacktrace.h"

int prte_backtrace_print(FILE *file, char *prefix, int strip)
{
    int i, len;
    int trace_size;
    void *trace[32];
    char buf[6];
    int fd = prte_stacktrace_output_fileno;

    if (NULL != file) {
        fd = fileno(file);
    }

    if (-1 == fd) {
        return PRTE_ERR_BAD_PARAM;
    }

    trace_size = backtrace(trace, 32);

    for (i = strip; i < trace_size; i++) {
        if (NULL != prefix) {
            pmix_fd_write(fd, strlen(prefix), prefix);
        }
        len = snprintf(buf, sizeof(buf), "[%2d] ", i - strip);
        pmix_fd_write(fd, len, buf);
        backtrace_symbols_fd(&trace[i], 1, fd);
    }

    return PRTE_SUCCESS;
}

int prte_backtrace_buffer(char ***message_out, int *len_out)
{
    int trace_size;
    void *trace[32];
    char **funcs = (char **) NULL;

    trace_size = backtrace(trace, 32);
    funcs = backtrace_symbols(trace, trace_size);

    *message_out = funcs;
    *len_out = trace_size;

    return PRTE_SUCCESS;
}
