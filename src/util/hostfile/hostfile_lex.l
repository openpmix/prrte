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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
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

#include "src/util/hostfile/hostfile_lex.h"

/*
 * local functions
 */
BEGIN_C_DECLS

int prrte_util_hostfile_wrap(void);

END_C_DECLS

int prrte_util_hostfile_wrap(void)
{
    prrte_util_hostfile_done = true;
    return 1;
}


/*
 * global variables
 */
int prrte_util_hostfile_line=1;
prrte_hostfile_value_t prrte_util_hostfile_value = {0};
bool prrte_util_hostfile_done = false;

%}

WHITE       [\f\t\v ]

%x comment

%%

{WHITE}*\n         { prrte_util_hostfile_line++;
                     return PRRTE_HOSTFILE_NEWLINE; }
#.*\n              { prrte_util_hostfile_line++;
                     return PRRTE_HOSTFILE_NEWLINE; }
"//".*\n           { prrte_util_hostfile_line++;
                     return PRRTE_HOSTFILE_NEWLINE; }

"/*"               { BEGIN(comment);
                     return PRRTE_HOSTFILE_NEWLINE; }
<comment>[^*\n]*       ; /* Eat up non '*'s */
<comment>"*"+[^*/\n]*  ; /* Eat '*'s not followed by a '/' */
<comment>\n        { prrte_util_hostfile_line++;
                     return PRRTE_HOSTFILE_NEWLINE; }
<comment>"*"+"/"   { BEGIN(INITIAL); /* Done with Block Comment */
                     return PRRTE_HOSTFILE_NEWLINE; }

\"[^\"]*\"         { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_QUOTED_STRING; }

{WHITE}+           ;			/* whitespace */

"="                { return PRRTE_HOSTFILE_EQUAL; }

cpu                { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_CPU; }
count              { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_COUNT; }
slots              { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS; }
"slots-max"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
slots_max          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
"max-slots"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
max_slots          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
"cpu-max"          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
cpu_max            { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
"max-cpu"          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
max_cpu            { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
"count-max"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
count_max          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
"max-count"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }
max_count          { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOTS_MAX; }

rank                { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_RANK; }
slot               { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SLOT; }

username           { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_USERNAME; }
"user-name"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_USERNAME; }
"user_name"        { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_USERNAME; }

port               { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_PORT; }

boards             { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_BOARDS; }

sockets            { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SOCKETS_PER_BOARD; }

sockets_per_board  { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SOCKETS_PER_BOARD; }

"sockets-per-board" { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_SOCKETS_PER_BOARD; }

cores              { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_CORES_PER_SOCKET; }

cores_per_socket   { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_CORES_PER_SOCKET; }

"cores-per-socket" { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_CORES_PER_SOCKET; }

\+n[0-9]+	   { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_RELATIVE; }
\+[eE][\:][0-9]+   { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_RELATIVE; }
\+[eE]             { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_RELATIVE; }


[0-9]+             { prrte_util_hostfile_value.ival = atol(yytext);
                     return PRRTE_HOSTFILE_INT; }
%{ /* First detect hosts as standard Strings (but without ".")
    * then username@IPv4 or IPV4, then username@IPv6 or IPv6,
    * followed by username@hostname or hostname
    */
%}

[A-Za-z0-9_\-,:*@]*  { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_STRING; }

([A-Za-z0-9][A-Za-z0-9_\-]*"@")?([0-9]{1,3}"."){3}[0-9]{1,3} {
                      prrte_util_hostfile_value.sval = yytext;
                      return PRRTE_HOSTFILE_IPV4; }

([A-Za-z0-9][A-Za-z0-9_\-]*"@")?([A-Fa-f0-9]{0,4}":")+[":"]*([A-Fa-f0-9]{0,4}":")+[A-Fa-f0-9]{1,4} {
                      prrte_util_hostfile_value.sval = yytext;
                      return PRRTE_HOSTFILE_IPV6; }

(\^?[A-Za-z0-9][A-Za-z0-9_\-]*"@")?[A-Za-z0-9][A-Za-z0-9_\-\.]*  {
                     prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_HOSTNAME; }

.                  { prrte_util_hostfile_value.sval = yytext;
                     return PRRTE_HOSTFILE_ERROR; }

%%
