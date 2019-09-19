%option nounput
%option noinput

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
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2013      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"
#include "src/util/output.h"

BEGIN_C_DECLS

int prrte_rmaps_rank_file_yywrap(void);

END_C_DECLS

/*
 * global variables
 */
int prrte_rmaps_rank_file_line=1;
prrte_rmaps_rank_file_value_t prrte_rmaps_rank_file_value = {0};
bool prrte_rmaps_rank_file_done = false;

%}

WHITE       [\f\t\v ]

%x comment

%%

{WHITE}*\n         { prrte_rmaps_rank_file_line++;
                     return PRRTE_RANKFILE_NEWLINE; }
#.*\n              { prrte_rmaps_rank_file_line++;
                     return PRRTE_RANKFILE_NEWLINE; }
"//".*\n           { prrte_rmaps_rank_file_line++;
                     return PRRTE_RANKFILE_NEWLINE; }

"/*"               { BEGIN(comment);
                     return PRRTE_RANKFILE_NEWLINE; }
<comment>[^*\n]*       ; /* Eat up non '*'s */
<comment>"*"+[^*/\n]*  ; /* Eat '*'s not followed by a '/' */
<comment>\n        { prrte_rmaps_rank_file_line++;
                     return PRRTE_RANKFILE_NEWLINE; }
<comment>"*"+"/"   { BEGIN(INITIAL); /* Done with Block Comment */
                     return PRRTE_RANKFILE_NEWLINE; }

\"[^\"]*\"         { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_QUOTED_STRING; }

{WHITE}+           ;

"="                { return PRRTE_RANKFILE_EQUAL; }

rank                { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_RANK; }
slot               { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_SLOT; }
slots              { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_SLOT; }

username           { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_USERNAME; }
"user-name"        { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_USERNAME; }
"user_name"        { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_USERNAME; }

[0-9]+             { prrte_rmaps_rank_file_value.ival = atol(yytext);
                     return PRRTE_RANKFILE_INT; }
%{ /* First detect hosts as standard Strings (but without ".")
    * then username@IPv4 or IPV4, then username@IPv6 or IPv6,
    * followed by username@hostname or hostname
    */
%}

[A-Za-z0-9_\-,\;:*@]*  { prrte_rmaps_rank_file_value.sval = yytext;
                         return PRRTE_RANKFILE_STRING; }

([A-Za-z0-9][A-Za-z0-9_\-]*"@")?([0-9]{1,3}"."){3}[0-9]{1,3} {
                      prrte_rmaps_rank_file_value.sval = yytext;
                      return PRRTE_RANKFILE_IPV4; }

([A-Za-z0-9][A-Za-z0-9_\-]*"@")?([A-Fa-f0-9]{0,4}":")+[":"]*([A-Fa-f0-9]{0,4}":")+[A-Fa-f0-9]{1,4} {
                      prrte_rmaps_rank_file_value.sval = yytext;
                      return PRRTE_RANKFILE_IPV6; }

([A-Za-z0-9][A-Za-z0-9_\-]*"@")?[A-Za-z][A-Za-z0-9_\-\.]*  {
                     prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_HOSTNAME; }

\+n[0-9]+           { prrte_rmaps_rank_file_value.sval = yytext;
					 return PRRTE_RANKFILE_RELATIVE; }

.                  { prrte_rmaps_rank_file_value.sval = yytext;
                     return PRRTE_RANKFILE_ERROR; }

%%

/* Old flex (2.5.4a? and older) does not define a destroy function */
#if !defined(YY_FLEX_SUBMINOR_VERSION)
#define YY_FLEX_SUBMINOR_VERSION 0
#endif

#if (YY_FLEX_MAJOR_VERSION < 2) || (YY_FLEX_MAJOR_VERSION == 2 && (YY_FLEX_MINOR_VERSION < 5 || (YY_FLEX_MINOR_VERSION == 5 && YY_FLEX_SUBMINOR_VERSION < 5)))
int prrte_rmaps_rank_file_lex_destroy (void)
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

int prrte_rmaps_rank_file_wrap(void)
{
    prrte_rmaps_rank_file_done = true;
    return 1;
}
