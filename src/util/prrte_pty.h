/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_UTIL_PTY_H
#define PRRTE_UTIL_PTY_H

#include "prrte_config.h"

#ifdef HAVE_UTIL_H
#include <util.h>
#endif
#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif
#ifdef HAVE_TERMIOS_H
# include <termios.h>
#else
# ifdef HAVE_TERMIO_H
#  include <termio.h>
# endif
#endif

BEGIN_C_DECLS

#if PRRTE_ENABLE_PTY_SUPPORT

PRRTE_EXPORT int prrte_openpty(int *amaster, int *aslave, char *name,
                               struct termios *termp, struct winsize *winp);

#else

PRRTE_EXPORT int prrte_openpty(int *amaster, int *aslave, char *name,
                               void *termp, void *winpp);

#endif

END_C_DECLS

#endif /* PRRTE_UTIL_PTY_H */
