/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdlib.h>

#include "src/util/malloc.h"
#include "src/util/output.h"
#include "src/runtime/runtime.h"


/*
 * Undefine "malloc" and "free"
 */

#if defined(malloc)
#undef malloc
#endif
#if defined(calloc)
#undef calloc
#endif
#if defined(free)
#undef free
#endif
#if defined(realloc)
#undef realloc
#endif

/*
 * Public variables
 */
int prrte_malloc_debug_level = PRRTE_MALLOC_DEBUG_LEVEL;
int prrte_malloc_output = -1;


/*
 * Private variables
 */
#if PRRTE_ENABLE_DEBUG
static prrte_output_stream_t malloc_stream;
#endif

#if PRRTE_ENABLE_DEBUG
/*
 * Finalize the malloc debug interface
 */
void prrte_malloc_finalize(void)
{
    if (-1 != prrte_malloc_output) {
        prrte_output_close(prrte_malloc_output);
        prrte_malloc_output = -1;
        PRRTE_DESTRUCT(&malloc_stream);
    }
}

/*
 * Initialize the malloc debug interface
 */
void prrte_malloc_init(void)
{
    PRRTE_CONSTRUCT(&malloc_stream, prrte_output_stream_t);
    malloc_stream.lds_is_debugging = true;
    malloc_stream.lds_verbose_level = 5;
    malloc_stream.lds_prefix = "malloc debug: ";
    malloc_stream.lds_want_stderr = true;
    prrte_malloc_output = prrte_output_open(&malloc_stream);
}
#else
void prrte_malloc_init (void)
{
}
void prrte_malloc_finalize(void)
{
}
#endif  /* PRRTE_ENABLE_DEBUG */

/*
 * Debug version of malloc
 */
void *prrte_malloc(size_t size, const char *file, int line)
{
    void *addr;
#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 1) {
        if (size <= 0) {
            prrte_output(prrte_malloc_output, "Request for %ld bytes (%s, %d)",
                        (long) size, file, line);
        }
    }
#endif /* PRRTE_ENABLE_DEBUG */

    addr = malloc(size);

#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 0) {
        if (NULL == addr) {
            prrte_output(prrte_malloc_output,
                        "Request for %ld bytes failed (%s, %d)",
                        (long) size, file, line);
        }
    }
#endif  /* PRRTE_ENABLE_DEBUG */
    return addr;
}


/*
 * Debug version of calloc
 */
void *prrte_calloc(size_t nmembers, size_t size, const char *file, int line)
{
    void *addr;
#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 1) {
        if (size <= 0) {
            prrte_output(prrte_malloc_output,
                        "Request for %ld zeroed elements of size %ld (%s, %d)",
                        (long) nmembers, (long) size, file, line);
        }
    }
#endif  /* PRRTE_ENABLE_DEBUG */
    addr = calloc(nmembers, size);
#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 0) {
        if (NULL == addr) {
            prrte_output(prrte_malloc_output,
                        "Request for %ld zeroed elements of size %ld failed (%s, %d)",
                        (long) nmembers, (long) size, file, line);
        }
    }
#endif  /* PRRTE_ENABLE_DEBUG */
    return addr;
}


/*
 * Debug version of realloc
 */
void *prrte_realloc(void *ptr, size_t size, const char *file, int line)
{
    void *addr;
#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 1) {
        if (size <= 0) {
            if (NULL == ptr) {
                prrte_output(prrte_malloc_output,
                            "Realloc NULL for %ld bytes (%s, %d)",
                            (long) size, file, line);
            } else {
                prrte_output(prrte_malloc_output, "Realloc %p for %ld bytes (%s, %d)",
                            ptr, (long) size, file, line);
            }
        }
    }
#endif  /* PRRTE_ENABLE_DEBUG */
    addr = realloc(ptr, size);
#if PRRTE_ENABLE_DEBUG
    if (prrte_malloc_debug_level > 0) {
        if (NULL == addr) {
            prrte_output(prrte_malloc_output,
                        "Realloc %p for %ld bytes failed (%s, %d)",
                        ptr, (long) size, file, line);
        }
    }
#endif  /* PRRTE_ENABLE_DEBUG */
    return addr;
}


/*
 * Debug version of free
 */
void prrte_free(void *addr, const char *file, int line)
{
    free(addr);
}

void prrte_malloc_debug(int level)
{
#if PRRTE_ENABLE_DEBUG
    prrte_malloc_debug_level = level;
#endif  /* PRRTE_ENABLE_DEBUG */
}
