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
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "constants.h"
#include "src/runtime/runtime.h"
#include "src/threads/mutex.h"
#include "src/util/keyval/keyval_lex.h"
#include "src/util/keyval_parse.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"
#include <ctype.h>
#include <string.h>

int prte_util_keyval_parse_lineno = 0;

static char *key_buffer = NULL;
static size_t key_buffer_len = 0;
static prte_mutex_t keyval_mutex;

static int parse_line(const char *filename, prte_keyval_parse_fn_t callback);
static int parse_line_new(const char *filename, prte_keyval_parse_state_t first_val,
                          prte_keyval_parse_fn_t callback);
static void parse_error(int num, const char *filename);

static char *env_str = NULL;
static int envsize = 1024;

void prte_util_keyval_parse_finalize(void)
{
    free(key_buffer);
    key_buffer = NULL;
    key_buffer_len = 0;

    PRTE_DESTRUCT(&keyval_mutex);
}

int prte_util_keyval_parse_init(void)
{
    PRTE_CONSTRUCT(&keyval_mutex, prte_mutex_t);

    return PRTE_SUCCESS;
}

int prte_util_keyval_parse(const char *filename, prte_keyval_parse_fn_t callback)
{
    int val;
    int ret = PRTE_SUCCESS;
    ;

    prte_mutex_lock(&keyval_mutex);

    /* Open the prte */
    prte_util_keyval_yyin = fopen(filename, "r");
    if (NULL == prte_util_keyval_yyin) {
        ret = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    prte_util_keyval_parse_done = false;
    prte_util_keyval_yynewlines = 1;
    prte_util_keyval_init_buffer(prte_util_keyval_yyin);
    while (!prte_util_keyval_parse_done) {
        val = prte_util_keyval_yylex();
        switch (val) {
        case PRTE_UTIL_KEYVAL_PARSE_DONE:
            /* This will also set prte_util_keyval_parse_done to true, so just
               break here */
            break;

        case PRTE_UTIL_KEYVAL_PARSE_NEWLINE:
            /* blank line!  ignore it */
            break;

        case PRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD:
            parse_line(filename, callback);
            break;

        case PRTE_UTIL_KEYVAL_PARSE_MCAVAR:
        case PRTE_UTIL_KEYVAL_PARSE_ENVVAR:
        case PRTE_UTIL_KEYVAL_PARSE_ENVEQL:
            parse_line_new(filename, val, callback);
            break;

        default:
            /* anything else is an error */
            parse_error(1, filename);
            break;
        }
    }
    fclose(prte_util_keyval_yyin);
    prte_util_keyval_yylex_destroy();

cleanup:
    prte_mutex_unlock(&keyval_mutex);
    return ret;
}

static int parse_line(const char *filename, prte_keyval_parse_fn_t callback)
{
    int val;

    prte_util_keyval_parse_lineno = prte_util_keyval_yylineno;

    /* Save the name name */
    if (key_buffer_len < strlen(prte_util_keyval_yytext) + 1) {
        char *tmp;
        key_buffer_len = strlen(prte_util_keyval_yytext) + 1;
        tmp = (char *) realloc(key_buffer, key_buffer_len);
        if (NULL == tmp) {
            free(key_buffer);
            key_buffer_len = 0;
            key_buffer = NULL;
            return PRTE_ERR_TEMP_OUT_OF_RESOURCE;
        }
        key_buffer = tmp;
    }

    prte_string_copy(key_buffer, prte_util_keyval_yytext, key_buffer_len);

    /* The first thing we have to see is an "=" */

    val = prte_util_keyval_yylex();
    if (prte_util_keyval_parse_done || PRTE_UTIL_KEYVAL_PARSE_EQUAL != val) {
        parse_error(2, filename);
        return PRTE_ERROR;
    }

    /* Next we get the value */

    val = prte_util_keyval_yylex();
    if (PRTE_UTIL_KEYVAL_PARSE_SINGLE_WORD == val || PRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
        callback(filename, 0, key_buffer, prte_util_keyval_yytext);

        /* Now we need to see the newline */

        val = prte_util_keyval_yylex();
        if (PRTE_UTIL_KEYVAL_PARSE_NEWLINE == val || PRTE_UTIL_KEYVAL_PARSE_DONE == val) {
            return PRTE_SUCCESS;
        }
    }

    /* Did we get an EOL or EOF? */

    else if (PRTE_UTIL_KEYVAL_PARSE_DONE == val || PRTE_UTIL_KEYVAL_PARSE_NEWLINE == val) {
        callback(filename, 0, key_buffer, NULL);
        return PRTE_SUCCESS;
    }

    /* Nope -- we got something unexpected.  Bonk! */
    parse_error(3, filename);
    return PRTE_ERROR;
}

static void parse_error(int num, const char *filename)
{
    /* JMS need better error/warning message here */
    prte_output(0, "keyval parser: error %d reading file %s at line %d:\n  %s\n", num, filename,
                prte_util_keyval_yynewlines, prte_util_keyval_yytext);
}

static void trim_name(char *buffer, const char *prefix, const char *suffix)
{
    char *pchr, *echr;
    size_t buffer_len;

    if (NULL == buffer) {
        return;
    }

    buffer_len = strlen(buffer);

    pchr = buffer;
    if (NULL != prefix) {
        size_t prefix_len = strlen(prefix);

        if (0 == strncmp(buffer, prefix, prefix_len)) {
            pchr += prefix_len;
        }
    }

    /* trim spaces at the beginning */
    while (isspace(*pchr)) {
        pchr++;
    }

    /* trim spaces at the end */
    echr = buffer + buffer_len;
    while (echr > buffer && isspace(*(echr - 1))) {
        echr--;
    }
    echr[0] = '\0';

    if (NULL != suffix && (uintptr_t)(echr - buffer) > strlen(suffix)) {
        size_t suffix_len = strlen(suffix);

        echr -= suffix_len;

        if (0 == strncmp(echr, suffix, strlen(suffix))) {
            do {
                echr--;
            } while (isspace(*echr));
            echr[1] = '\0';
        }
    }

    if (buffer != pchr) {
        /* move the trimmed string to the beginning of the buffer */
        memmove(buffer, pchr, strlen(pchr) + 1);
    }
}

static int save_param_name(void)
{
    if (key_buffer_len < strlen(prte_util_keyval_yytext) + 1) {
        char *tmp;
        key_buffer_len = strlen(prte_util_keyval_yytext) + 1;
        tmp = (char *) realloc(key_buffer, key_buffer_len);
        if (NULL == tmp) {
            free(key_buffer);
            key_buffer_len = 0;
            key_buffer = NULL;
            return PRTE_ERR_TEMP_OUT_OF_RESOURCE;
        }
        key_buffer = tmp;
    }

    prte_string_copy(key_buffer, prte_util_keyval_yytext, key_buffer_len);

    return PRTE_SUCCESS;
}

static int add_to_env_str(char *var, char *val)
{
    int sz, varsz = 0, valsz = 0, new_envsize;
    void *tmp;

    if (NULL == var) {
        return PRTE_ERR_BAD_PARAM;
    }

    varsz = strlen(var);
    if (NULL != val) {
        valsz = strlen(val);
        /* account for '=' */
        valsz += 1;
    }
    sz = 0;
    if (NULL != env_str) {
        sz = strlen(env_str);
        /* account for ';' */
        sz += 1;
    }
    /* add required new size incl NULL byte */
    sz += varsz + valsz + 1;

    /* make sure we have sufficient space */
    new_envsize = envsize;
    while (new_envsize <= sz) {
        new_envsize *= 2;
    }

    if (NULL != env_str) {
        if (new_envsize > envsize) {
            tmp = realloc(env_str, new_envsize);
            if (NULL == tmp) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            env_str = tmp;
        }
        strcat(env_str, ";");
    } else {
        env_str = calloc(1, new_envsize);
        if (NULL == env_str) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    envsize = new_envsize;

    strcat(env_str, var);
    if (NULL != val) {
        strcat(env_str, "=");
        strcat(env_str, val);
    }

    return PRTE_SUCCESS;
}

static int parse_line_new(const char *filename, prte_keyval_parse_state_t first_val,
                          prte_keyval_parse_fn_t callback)
{
    prte_keyval_parse_state_t val;
    char *tmp;
    int rc;

    val = first_val;
    while (PRTE_UTIL_KEYVAL_PARSE_NEWLINE != val && PRTE_UTIL_KEYVAL_PARSE_DONE != val) {
        rc = save_param_name();
        if (PRTE_SUCCESS != rc) {
            return rc;
        }

        if (PRTE_UTIL_KEYVAL_PARSE_MCAVAR == val) {
            trim_name(key_buffer, "-mca", NULL);
            trim_name(key_buffer, "--mca", NULL);

            val = prte_util_keyval_yylex();
            if (PRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
                if (NULL != prte_util_keyval_yytext) {
                    tmp = strdup(prte_util_keyval_yytext);
                    if ('\'' == tmp[0] || '\"' == tmp[0]) {
                        trim_name(tmp, "\'", "\'");
                        trim_name(tmp, "\"", "\"");
                    }
                    callback(filename, 0, key_buffer, tmp);
                    free(tmp);
                }
            } else {
                parse_error(4, filename);
                return PRTE_ERROR;
            }
        } else if (PRTE_UTIL_KEYVAL_PARSE_ENVEQL == val) {
            trim_name(key_buffer, "-x", "=");
            trim_name(key_buffer, "--x", NULL);

            val = prte_util_keyval_yylex();
            if (PRTE_UTIL_KEYVAL_PARSE_VALUE == val) {
                add_to_env_str(key_buffer, prte_util_keyval_yytext);
            } else {
                parse_error(5, filename);
                return PRTE_ERROR;
            }
        } else if (PRTE_UTIL_KEYVAL_PARSE_ENVVAR == val) {
            trim_name(key_buffer, "-x", "=");
            trim_name(key_buffer, "--x", NULL);
            add_to_env_str(key_buffer, NULL);
        } else {
            /* we got something unexpected.  Bonk! */
            parse_error(6, filename);
            return PRTE_ERROR;
        }

        val = prte_util_keyval_yylex();
    }

    return PRTE_SUCCESS;
}
