/*
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#ifndef PRTE_GETCWD_H
#define PRTE_GETCWD_H

#include "prte_config.h"

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
 * @retval PRTE_ERR_OUT_OF_RESOURCE If internal malloc() fails.
 * @retval PRTE_ERR_TEMP_OUT_OF_RESOURCE If the supplied buf buffer
 * was not long enough to handle the result.
 * @retval PRTE_ERR_BAD_PARAM If buf is NULL or size>INT_MAX
 * @retval PRTE_ERR_IN_ERRNO If an other error occurred
 * @retval PRTE_SUCCESS If all went well and a valid value was placed
 * in the buf buffer.
 */
PRTE_EXPORT int pmix_getcwd(char *buf, size_t size);

END_C_DECLS

#endif /* PRTE_GETCWD_H */
