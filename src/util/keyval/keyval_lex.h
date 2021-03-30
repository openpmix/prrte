/* -*- C -*-
 *
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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_UTIL_KEYVAL_LEX_H_
#define PRTE_UTIL_KEYVAL_LEX_H_

#include "prte_config.h"

#ifdef malloc
#    undef malloc
#endif
#ifdef realloc
#    undef realloc
#endif
#ifdef free
#    undef free
#endif

#include <stdio.h>

int prte_util_keyval_yylex(void);
int prte_util_keyval_init_buffer(FILE *file);
int prte_util_keyval_yylex_destroy(void);

extern FILE *prte_util_keyval_yyin;
extern bool prte_util_keyval_parse_done;
extern char *prte_util_keyval_yytext;
extern int prte_util_keyval_yynewlines;
extern int prte_util_keyval_yylineno;

/*
 * Make lex-generated files not issue compiler warnings
 */
#define YY_STACK_USED         0
#define YY_ALWAYS_INTERACTIVE 0
#define YY_NEVER_INTERACTIVE  0
#define YY_MAIN               0
#define YY_NO_UNPUT           1
#define YY_SKIP_YYWRAP        1

enum prte_keyval_parse_state_t {
    PRTE_UTIL_KEYVAL_PARSE_DONE,
    PRTE_UTIL_KEYVAL_PARSE_ERROR,

    PRTE_UTIL_KEYVAL_PARSE_NEWLINE,
    PRTE_UTIL_KEYVAL_PARSE_EQUAL,
    PRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD,
    PRTE_UTIL_KEYVAL_PARSE_VALUE,
    PRTE_UTIL_KEYVAL_PARSE_MCAVAR,
    PRTE_UTIL_KEYVAL_PARSE_ENVVAR,
    PRTE_UTIL_KEYVAL_PARSE_ENVEQL,

    PRTE_UTIL_KEYVAL_PARSE_MAX
};
typedef enum prte_keyval_parse_state_t prte_keyval_parse_state_t;

#endif
