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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file */

#ifndef PRRTE_UTIL_KEYVAL_PARSE_H
#define PRRTE_UTIL_KEYVAL_PARSE_H

#include "prrte_config.h"

BEGIN_C_DECLS

extern int prrte_util_keyval_parse_lineno;

/**
 * Callback triggered for each key = value pair
 *
 * Callback triggered from prrte_util_keyval_parse for each key = value
 * pair.  Both key and value will be pointers into static buffers.
 * The buffers must not be free()ed and contents may be overwritten
 * immediately after the callback returns.
 */
typedef void (*prrte_keyval_parse_fn_t)(const char *key, const char *value);

/**
 * Parse \c filename, made up of key = value pairs.
 *
 * Parse \c filename, made up of key = value pairs.  For each line
 * that appears to contain a key = value pair, \c callback will be
 * called exactly once.  In a multithreaded context, calls to
 * prrte_util_keyval_parse() will serialize multiple calls.
 */
PRRTE_EXPORT int prrte_util_keyval_parse(const char *filename,
                                         prrte_keyval_parse_fn_t callback);

PRRTE_EXPORT int prrte_util_keyval_parse_init(void);

PRRTE_EXPORT void prrte_util_keyval_parse_finalize (void);

PRRTE_EXPORT int prrte_util_keyval_save_internal_envars(prrte_keyval_parse_fn_t callback);

END_C_DECLS

#endif
