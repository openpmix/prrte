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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_UTIL_KEYVAL_LEX_H_
#define PRRTE_UTIL_KEYVAL_LEX_H_

#include "prrte_config.h"

#ifdef malloc
#undef malloc
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef free
#undef free
#endif

#include <stdio.h>

int prrte_util_keyval_yylex(void);
int prrte_util_keyval_init_buffer(FILE *file);
int prrte_util_keyval_yylex_destroy(void);

extern FILE *prrte_util_keyval_yyin;
extern bool prrte_util_keyval_parse_done;
extern char *prrte_util_keyval_yytext;
extern int prrte_util_keyval_yynewlines;
extern int prrte_util_keyval_yylineno;

/*
 * Make lex-generated files not issue compiler warnings
 */
#define YY_STACK_USED 0
#define YY_ALWAYS_INTERACTIVE 0
#define YY_NEVER_INTERACTIVE 0
#define YY_MAIN 0
#define YY_NO_UNPUT 1
#define YY_SKIP_YYWRAP 1

enum prrte_keyval_parse_state_t {
    PRRTE_UTIL_KEYVAL_PARSE_DONE,
    PRRTE_UTIL_KEYVAL_PARSE_ERROR,

    PRRTE_UTIL_KEYVAL_PARSE_NEWLINE,
    PRRTE_UTIL_KEYVAL_PARSE_EQUAL,
    PRRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD,
    PRRTE_UTIL_KEYVAL_PARSE_VALUE,
    PRRTE_UTIL_KEYVAL_PARSE_MCAVAR,
    PRRTE_UTIL_KEYVAL_PARSE_ENVVAR,
    PRRTE_UTIL_KEYVAL_PARSE_ENVEQL,

    PRRTE_UTIL_KEYVAL_PARSE_MAX
};
typedef enum prrte_keyval_parse_state_t prrte_keyval_parse_state_t;

#endif
