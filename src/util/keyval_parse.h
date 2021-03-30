/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file */

#ifndef PRTE_UTIL_KEYVAL_PARSE_H
#define PRTE_UTIL_KEYVAL_PARSE_H

#include "prte_config.h"

BEGIN_C_DECLS

extern int prte_util_keyval_parse_lineno;

/**
 * Callback triggered for each key = value pair
 *
 * Callback triggered from prte_util_keyval_parse for each key = value
 * pair.  Both key and value will be pointers into static buffers.
 * The buffers must not be free()ed and contents may be overwritten
 * immediately after the callback returns.
 */
typedef void (*prte_keyval_parse_fn_t)(const char *file, int lineno, const char *name,
                                       const char *value);

/**
 * Parse \c filename, made up of key = value pairs.
 *
 * Parse \c filename, made up of key = value pairs.  For each line
 * that appears to contain a key = value pair, \c callback will be
 * called exactly once.  In a multithreaded context, calls to
 * prte_util_keyval_parse() will serialize multiple calls.
 */
PRTE_EXPORT int prte_util_keyval_parse(const char *filename, prte_keyval_parse_fn_t callback);

PRTE_EXPORT int prte_util_keyval_parse_init(void);

PRTE_EXPORT void prte_util_keyval_parse_finalize(void);

END_C_DECLS

#endif
