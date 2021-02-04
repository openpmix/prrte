%option nounput
%option noinput
%option yylineno

%{ /* -*- C -*- */
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
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/keyval/keyval_lex.h"

/*
 * local functions
 */

BEGIN_C_DECLS

int prte_util_keyval_yywrap(void);

END_C_DECLS

/*
 * global variables
 */
int prte_util_keyval_yynewlines = 1;
bool prte_util_keyval_parse_done = false;
char *prte_util_keyval_string = NULL;

#ifndef YY_BREAK
#define YY_BREAK
#endif

%}

WHITE       [\f\t\v ]
CHAR        [A-Za-z0-9_\-\.]

%x VALUE
%x comment
%x MCA_VALUE

%%

{WHITE}*\n          { prte_util_keyval_yynewlines++; return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }
#.*\n               { prte_util_keyval_yynewlines++; return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }
"//".*\n            { prte_util_keyval_yynewlines++; return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }

"/*"                { BEGIN(comment);
                      return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }
<comment>[^*\n]*    { break; } /* Eat up non '*'s */
<comment>"*"+[^*/\n]*  { break; } /* Eat '*'s not followed by a '/' */
<comment>\n         { prte_util_keyval_yynewlines++;
                      return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }
<comment>"*"+"/"    { BEGIN(INITIAL); /* Done with Block Comment */
                      return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }

{WHITE}*"="{WHITE}* { BEGIN(VALUE); return PRTE_UTIL_KEYVAL_PARSE_EQUAL; }
{WHITE}+            ; /* whitespace */
{CHAR}+             { return PRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD; }

<VALUE>{WHITE}*\n   { BEGIN(INITIAL); return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }
<VALUE>[^\n]*[^\t \n]/[\t ]*      { return PRTE_UTIL_KEYVAL_PARSE_VALUE; }

<MCA_VALUE>[^\n \t]+      { BEGIN(INITIAL); return PRTE_UTIL_KEYVAL_PARSE_VALUE; }
<MCA_VALUE>"'"[^\n]*"'"{WHITE}+      { BEGIN(INITIAL); return PRTE_UTIL_KEYVAL_PARSE_VALUE; }
<MCA_VALUE>"\""[^\n]*"\""{WHITE}+      { BEGIN(INITIAL); return PRTE_UTIL_KEYVAL_PARSE_VALUE; }
<MCA_VALUE>{WHITE}*\n   { BEGIN(INITIAL); BEGIN(INITIAL); return PRTE_UTIL_KEYVAL_PARSE_NEWLINE; }

"-"?"-mca"{WHITE}+{CHAR}+{WHITE}+ {BEGIN(MCA_VALUE); return PRTE_UTIL_KEYVAL_PARSE_MCAVAR; }
"-"?"-x"{WHITE}+{CHAR}+{WHITE}*"="{WHITE}* {BEGIN(MCA_VALUE); return PRTE_UTIL_KEYVAL_PARSE_ENVEQL; }
"-"?"-x"{WHITE}+{CHAR}+{WHITE}*/[^=] { return PRTE_UTIL_KEYVAL_PARSE_ENVVAR; }

.	            { return PRTE_UTIL_KEYVAL_PARSE_ERROR; }

%%

/* Old flex (2.5.4a? and older) does not define a destroy function */
#if !defined(YY_FLEX_SUBMINOR_VERSION)
#define YY_FLEX_SUBMINOR_VERSION 0
#endif

#if (YY_FLEX_MAJOR_VERSION < 2) || (YY_FLEX_MAJOR_VERSION == 2 && (YY_FLEX_MINOR_VERSION < 5 || (YY_FLEX_MINOR_VERSION == 5 && YY_FLEX_SUBMINOR_VERSION < 5)))
int prte_util_keyval_yylex_destroy(void)
{
    if (NULL != YY_CURRENT_BUFFER) {
        yy_delete_buffer(YY_CURRENT_BUFFER);
#if defined(YY_CURRENT_BUFFER_LVALUE)
        YY_CURRENT_BUFFER_LVALUE = NULL;
#else
        YY_CURRENT_BUFFER = NULL;
#endif  /* YY_CURRENT_BUFFER_LVALUE */
    }
    return YY_NULL;
}
#endif

int prte_util_keyval_yywrap(void)
{
    prte_util_keyval_parse_done = true;
    return 1;
}

/*
 * Ensure that we have a valid yybuffer to use.  Specifically, if this
 * scanner is invoked a second time, finish_parsing() (above) will
 * have been executed, and the current buffer will have been freed.
 * Flex doesn't recognize this fact because as far as it's concerned,
 * its internal state was already initialized, so it thinks it should
 * have a valid buffer.  Hence, here we ensure to give it a valid
 * buffer.
 */
int prte_util_keyval_init_buffer(FILE *file)
{
    YY_BUFFER_STATE buf = yy_create_buffer(file, YY_BUF_SIZE);
    yy_switch_to_buffer(buf);

    return 0;
}
