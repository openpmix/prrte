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
 * Copyright (c) 2015-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/runtime/runtime.h"
#include "src/util/keyval_parse.h"
#include "src/util/keyval/keyval_lex.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"
#include "src/threads/mutex.h"
#include <string.h>
#include <ctype.h>

int prrte_util_keyval_parse_lineno = 0;

static const char *keyval_filename;
static prrte_keyval_parse_fn_t keyval_callback;
static char *key_buffer = NULL;
static size_t key_buffer_len = 0;
static prrte_mutex_t keyval_mutex;

static int parse_line(void);
static int parse_line_new(prrte_keyval_parse_state_t first_val);
static void parse_error(int num);

static char *env_str = NULL;
static int envsize = 1024;

void prrte_util_keyval_parse_finalize (void)
{
    free(key_buffer);
    key_buffer = NULL;
    key_buffer_len = 0;

    PRRTE_DESTRUCT(&keyval_mutex);
}

int prrte_util_keyval_parse_init(void)
{
    PRRTE_CONSTRUCT(&keyval_mutex, prrte_mutex_t);

    return PRRTE_SUCCESS;
}

int
prrte_util_keyval_parse(const char *filename,
                       prrte_keyval_parse_fn_t callback)
{
    int val;
    int ret = PRRTE_SUCCESS;;

    prrte_mutex_lock(&keyval_mutex);

    keyval_filename = filename;
    keyval_callback = callback;

    /* Open the prrte */
    prrte_util_keyval_yyin = fopen(keyval_filename, "r");
    if (NULL == prrte_util_keyval_yyin) {
        ret = PRRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    prrte_util_keyval_parse_done = false;
    prrte_util_keyval_yynewlines = 1;
    prrte_util_keyval_init_buffer(prrte_util_keyval_yyin);
    while (!prrte_util_keyval_parse_done) {
        val = prrte_util_keyval_yylex();
        switch (val) {
        case PRRTE_UTIL_KEYVAL_PARSE_DONE:
            /* This will also set prrte_util_keyval_parse_done to true, so just
               break here */
            break;

        case PRRTE_UTIL_KEYVAL_PARSE_NEWLINE:
            /* blank line!  ignore it */
            break;

        case PRRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD:
            parse_line();
            break;

        case PRRTE_UTIL_KEYVAL_PARSE_MCAVAR:
        case PRRTE_UTIL_KEYVAL_PARSE_ENVVAR:
        case PRRTE_UTIL_KEYVAL_PARSE_ENVEQL:
            parse_line_new(val);
            break;

        default:
            /* anything else is an error */
            parse_error(1);
            break;
        }
    }
    fclose(prrte_util_keyval_yyin);
    prrte_util_keyval_yylex_destroy ();

cleanup:
    prrte_mutex_unlock(&keyval_mutex);
    return ret;
}



static int parse_line(void)
{
    int val;

    prrte_util_keyval_parse_lineno = prrte_util_keyval_yylineno;

    /* Save the name name */
    if (key_buffer_len < strlen(prrte_util_keyval_yytext) + 1) {
        char *tmp;
        key_buffer_len = strlen(prrte_util_keyval_yytext) + 1;
        tmp = (char*)realloc(key_buffer, key_buffer_len);
        if (NULL == tmp) {
            free(key_buffer);
            key_buffer_len = 0;
            key_buffer = NULL;
            return PRRTE_ERR_TEMP_OUT_OF_RESOURCE;
        }
        key_buffer = tmp;
    }

    prrte_string_copy(key_buffer, prrte_util_keyval_yytext, key_buffer_len);

    /* The first thing we have to see is an "=" */

    val = prrte_util_keyval_yylex();
    if (prrte_util_keyval_parse_done || PRRTE_UTIL_KEYVAL_PARSE_EQUAL != val) {
        parse_error(2);
        return PRRTE_ERROR;
    }

    /* Next we get the value */

    val = prrte_util_keyval_yylex();
    if (PRRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD == val ||
        PRRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
        keyval_callback(key_buffer, prrte_util_keyval_yytext);

        /* Now we need to see the newline */

        val = prrte_util_keyval_yylex();
        if (PRRTE_UTIL_KEYVAL_PARSE_NEWLINE == val ||
            PRRTE_UTIL_KEYVAL_PARSE_DONE == val) {
            return PRRTE_SUCCESS;
        }
    }

    /* Did we get an EOL or EOF? */

    else if (PRRTE_UTIL_KEYVAL_PARSE_DONE == val ||
             PRRTE_UTIL_KEYVAL_PARSE_NEWLINE == val) {
        keyval_callback(key_buffer, NULL);
        return PRRTE_SUCCESS;
    }

    /* Nope -- we got something unexpected.  Bonk! */
    parse_error(3);
    return PRRTE_ERROR;
}


static void parse_error(int num)
{
    /* JMS need better error/warning message here */
    prrte_output(0, "keyval parser: error %d reading file %s at line %d:\n  %s\n",
                num, keyval_filename, prrte_util_keyval_yynewlines, prrte_util_keyval_yytext);
}

int prrte_util_keyval_save_internal_envars(prrte_keyval_parse_fn_t callback)
{
    if (NULL != env_str && 0 < strlen(env_str)) {
        callback("mca_base_env_list_internal", env_str);
        free(env_str);
        env_str = NULL;
    }
    return PRRTE_SUCCESS;
}

static void trim_name(char *buffer, const char* prefix, const char* suffix)
{
    char *pchr, *echr;
    size_t buffer_len;

    if (NULL == buffer) {
        return;
    }

    buffer_len = strlen (buffer);

    pchr = buffer;
    if (NULL != prefix) {
        size_t prefix_len = strlen (prefix);

        if (0 == strncmp (buffer, prefix, prefix_len)) {
            pchr += prefix_len;
        }
    }

    /* trim spaces at the beginning */
    while (isspace (*pchr)) {
        pchr++;
    }

    /* trim spaces at the end */
    echr = buffer + buffer_len;
    while (echr > buffer && isspace (*(echr - 1))) {
        echr--;
    }
    echr[0] = '\0';

    if (NULL != suffix && (uintptr_t) (echr - buffer) > strlen (suffix)) {
        size_t suffix_len = strlen (suffix);

        echr -= suffix_len;

        if (0 == strncmp (echr, suffix, strlen(suffix))) {
            do {
                echr--;
            } while (isspace (*echr));
            echr[1] = '\0';
        }
    }

    if (buffer != pchr) {
        /* move the trimmed string to the beginning of the buffer */
        memmove (buffer, pchr, strlen (pchr) + 1);
    }
}

static int save_param_name (void)
{
    if (key_buffer_len < strlen(prrte_util_keyval_yytext) + 1) {
        char *tmp;
        key_buffer_len = strlen(prrte_util_keyval_yytext) + 1;
        tmp = (char*)realloc(key_buffer, key_buffer_len);
        if (NULL == tmp) {
            free(key_buffer);
            key_buffer_len = 0;
            key_buffer = NULL;
            return PRRTE_ERR_TEMP_OUT_OF_RESOURCE;
        }
        key_buffer = tmp;
    }

    prrte_string_copy (key_buffer, prrte_util_keyval_yytext, key_buffer_len);

    return PRRTE_SUCCESS;
}

static int add_to_env_str(char *var, char *val)
{
    int sz, varsz, valsz;
    void *tmp;

    if (NULL == var) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (NULL != env_str) {
        varsz = strlen(var);
        valsz = (NULL != val) ? strlen(val) : 0;
        sz = strlen(env_str)+varsz+valsz+2;
        if (envsize <= sz) {
            envsize *=2;

            tmp = realloc(env_str, envsize);
            if (NULL == tmp) {
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            env_str = tmp;
        }
        strcat(env_str, ";");
    } else {
        env_str = calloc(1, envsize);
        if (NULL == env_str) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    strcat(env_str, var);
    if (NULL != val) {
        strcat(env_str, "=");
        strcat(env_str, val);
    }

    return PRRTE_SUCCESS;
}

static int parse_line_new(prrte_keyval_parse_state_t first_val)
{
    prrte_keyval_parse_state_t val;
    char *tmp;
    int rc;

    val = first_val;
    while (PRRTE_UTIL_KEYVAL_PARSE_NEWLINE != val && PRRTE_UTIL_KEYVAL_PARSE_DONE != val) {
        rc = save_param_name ();
        if (PRRTE_SUCCESS != rc) {
            return rc;
        }

        if (PRRTE_UTIL_KEYVAL_PARSE_MCAVAR == val) {
            trim_name (key_buffer, "-mca", NULL);
            trim_name (key_buffer, "--mca", NULL);

            val = prrte_util_keyval_yylex();
            if (PRRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
                if (NULL != prrte_util_keyval_yytext) {
                    tmp = strdup(prrte_util_keyval_yytext);
                    if ('\'' == tmp[0] || '\"' == tmp[0]) {
                        trim_name (tmp, "\'", "\'");
                        trim_name (tmp, "\"", "\"");
                    }
                    keyval_callback(key_buffer, tmp);
                    free(tmp);
                }
            } else {
                parse_error(4);
                return PRRTE_ERROR;
            }
        } else if (PRRTE_UTIL_KEYVAL_PARSE_ENVEQL == val) {
            trim_name (key_buffer, "-x", "=");
            trim_name (key_buffer, "--x", NULL);

            val = prrte_util_keyval_yylex();
            if (PRRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
                add_to_env_str(key_buffer, prrte_util_keyval_yytext);
            } else {
                parse_error(5);
                return PRRTE_ERROR;
            }
        } else if (PRRTE_UTIL_KEYVAL_PARSE_ENVVAR == val) {
            trim_name (key_buffer, "-x", "=");
            trim_name (key_buffer, "--x", NULL);
            add_to_env_str(key_buffer, NULL);
        } else {
            /* we got something unexpected.  Bonk! */
            parse_error(6);
            return PRRTE_ERROR;
        }

        val = prrte_util_keyval_yylex();
    }

    return PRRTE_SUCCESS;
}
