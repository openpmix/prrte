/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2017      FUJITSU LIMITED.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_UTIL_ERROR_H
#define PRRTE_UTIL_ERROR_H

#include "prrte_config.h"

#include "src/util/output.h"

BEGIN_C_DECLS

#define PRRTE_ERROR_LOG(r) \
    prrte_output(0, "PRRTE ERROR: %s in file %s at line %d", \
                prrte_strerror((r)), __FILE__, __LINE__);
/**
 * Return string for given error message
 *
 * Accepts an error number argument \c errnum and returns a pointer to
 * the corresponding message string.  The result is returned in a
 * static buffer that should not be released with free().
 *
 * If errnum is \c PRRTE_ERR_IN_ERRNO, the system strerror is called
 * with an argument of the current value of \c errno and the resulting
 * string is returned.
 *
 * If the errnum is not a known value, the returned value may be
 * overwritten by subsequent calls to prrte_strerror.
 */
PRRTE_EXPORT const char *prrte_strerror(int errnum);

END_C_DECLS

#endif /* PRRTE_UTIL_ERROR_H */
