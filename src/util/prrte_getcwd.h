/*
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Per https://svn.open-mpi.org/trac/ompi/ticket/933, use a
 * combination of $PWD and getcwd() to find the current working
 * directory.
 */

#ifndef PRRTE_GETCWD_H
#define PRRTE_GETCWD_H

#include "prrte_config.h"

BEGIN_C_DECLS

/**
 * Per https://svn.open-mpi.org/trac/ompi/ticket/933, use a
 * combination of $PWD and getcwd() to find the current working
 * directory.
 *
 * Use $PWD instead of getcwd() a) if $PWD exists and b) is a valid
 * synonym for the results from getcwd(). If both of these conditions
 * are not met, just fall back and use the results of getcwd().
 *
 * @param buf Caller-allocated buffer to put the result
 * @param size Length of the buf array
 *
 * @retval PRRTE_ERR_OUT_OF_RESOURCE If internal malloc() fails.
 * @retval PRRTE_ERR_TEMP_OUT_OF_RESOURCE If the supplied buf buffer
 * was not long enough to handle the result.
 * @retval PRRTE_ERR_BAD_PARAM If buf is NULL or size>INT_MAX
 * @retval PRRTE_ERR_IN_ERRNO If an other error occurred
 * @retval PRRTE_SUCCESS If all went well and a valid value was placed
 * in the buf buffer.
 */
PRRTE_EXPORT int prrte_getcwd(char *buf, size_t size);


END_C_DECLS

#endif /* PRRTE_GETCWD_H */
