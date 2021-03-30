/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2012-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "src/class/prte_list.h"
#include "src/class/prte_object.h"
#include "src/threads/mutex.h"
#include "src/util/argv.h"
#include "src/util/cmd_line.h"
#include "src/util/output.h"
#include "src/util/prte_environ.h"

#include "constants.h"
#include "src/mca/base/prte_mca_base_var.h"

/*
 * Some usage message constants
 *
 * Max width for param listings before the description will be listed
 * on the next line
 */
#define PARAM_WIDTH 37
/*
 * Max length of any line in the usage message
 */
#define MAX_WIDTH 110

static void option_constructor(prte_cmd_line_option_t *cmd);
static void option_destructor(prte_cmd_line_option_t *cmd);

PRTE_CLASS_INSTANCE(prte_cmd_line_option_t, prte_list_item_t, option_constructor,
                    option_destructor);

static void param_constructor(prte_cmd_line_param_t *cmd);
static void param_destructor(prte_cmd_line_param_t *cmd);
PRTE_CLASS_INSTANCE(prte_cmd_line_param_t, prte_list_item_t, param_constructor, param_destructor);

/*
 * Instantiate the prte_cmd_line_t class
 */
static void cmd_line_constructor(prte_cmd_line_t *cmd);
static void cmd_line_destructor(prte_cmd_line_t *cmd);
PRTE_CLASS_INSTANCE(prte_cmd_line_t, prte_object_t, cmd_line_constructor, cmd_line_destructor);

/*
 * Private variables
 */
static char special_empty_token[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, '\0'};

/*
 * Private functions
 */
static int make_opt(prte_cmd_line_t *cmd, prte_cmd_line_init_t *e);
static void free_parse_results(prte_cmd_line_t *cmd);
static prte_value_t *set_dest(prte_cmd_line_option_t *option, char *sval);
static void fill(const prte_cmd_line_option_t *a, char result[3][BUFSIZ]);
static int qsort_callback(const void *a, const void *b);
static char *build_parsable(prte_cmd_line_option_t *option);

/*
 * Create an entire command line handle from a table
 */
int prte_cmd_line_create(prte_cmd_line_t *cmd, prte_cmd_line_init_t *table)
{
    int ret = PRTE_SUCCESS;

    /* Check bozo case */

    if (NULL == cmd) {
        return PRTE_ERR_BAD_PARAM;
    }
    PRTE_CONSTRUCT(cmd, prte_cmd_line_t);

    if (NULL != table) {
        ret = prte_cmd_line_add(cmd, table);
    }
    return ret;
}

/* Add a table to an existing cmd line object */
int prte_cmd_line_add(prte_cmd_line_t *cmd, prte_cmd_line_init_t *table)
{
    int i, ret;

    /* Ensure we got a table */
    if (NULL == table) {
        return PRTE_SUCCESS;
    }

    /* Loop through the table */

    for (i = 0;; ++i) {
        /* Is this the end? */
        if ('\0' == table[i].ocl_cmd_short_name && NULL == table[i].ocl_cmd_long_name) {
            break;
        }

        /* Nope -- it's an entry.  Process it. */
        ret = make_opt(cmd, &table[i]);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}
/*
 * Append a command line entry to the previously constructed command line
 */
int prte_cmd_line_make_opt_mca(prte_cmd_line_t *cmd, prte_cmd_line_init_t entry)
{
    /* Ensure we got an entry */
    if ('\0' == entry.ocl_cmd_short_name && NULL == entry.ocl_cmd_long_name) {
        return PRTE_SUCCESS;
    }

    return make_opt(cmd, &entry);
}

/*
 * Create a command line option, --long-name and/or -s (short name).
 */
int prte_cmd_line_make_opt3(prte_cmd_line_t *cmd, char short_name, const char *long_name,
                            int num_params, const char *desc, prte_cmd_line_otype_t otype)
{
    prte_cmd_line_init_t e;

    e.ocl_cmd_short_name = short_name;
    e.ocl_cmd_long_name = long_name;

    e.ocl_num_params = num_params;

    e.ocl_variable_type = PRTE_CMD_LINE_TYPE_NULL;

    e.ocl_description = desc;
    e.ocl_otype = otype;

    return make_opt(cmd, &e);
}

/*
 * Parse a command line according to a pre-built PRTE command line
 * handle.
 */
int prte_cmd_line_parse(prte_cmd_line_t *cmd, bool ignore_unknown, bool ignore_unknown_option,
                        int argc, char **argv)
{
    int i, j, orig;
    prte_cmd_line_option_t *option;
    prte_cmd_line_param_t *param;
    bool is_unknown_option;
    bool is_unknown_token;
    bool is_option;
    bool have_help_option = false;
    bool printed_error = false;
    bool help_without_arg = false;
    prte_cmd_line_init_t e;
    prte_value_t *val;

    /* Bozo check */

    if (0 == argc || NULL == argv) {
        return PRTE_SUCCESS;
    }

    /* Thread serialization */

    prte_mutex_lock(&cmd->lcl_mutex);

    /* Free any parsed results that are already on this handle */

    free_parse_results(cmd);

    /* Analyze each token */

    cmd->lcl_argc = argc;
    cmd->lcl_argv = prte_argv_copy(argv);

    /* Check up front: do we have a --help option? */
    memset(&e, 0, sizeof(prte_cmd_line_init_t));
    e.ocl_cmd_long_name = "help";
    option = prte_cmd_line_find_option(cmd, &e);
    if (NULL != option) {
        have_help_option = true;
    }

    /* Now traverse the easy-to-parse sequence of tokens.  Note that
       incrementing i must happen elsewhere; it can't be the third
       clause in the "if" statement. */

    param = NULL;
    option = NULL;
    for (i = 1; i < cmd->lcl_argc;) {
        is_unknown_option = false;
        is_unknown_token = false;
        is_option = false;

        /* Are we done?  i.e., did we find the special "--" token?  If
           so, copy everything beyond it into the tail (i.e., don't
           bother copying the "--" into the tail). */

        if (0 == strcmp(cmd->lcl_argv[i], "--")) {
            ++i;
            while (i < cmd->lcl_argc) {
                if (0 != strcmp(cmd->lcl_argv[i], "&") || '>' != cmd->lcl_argv[i][0]
                    || '<' != cmd->lcl_argv[i][0]) {
                    prte_argv_append(&cmd->lcl_tail_argc, &cmd->lcl_tail_argv, cmd->lcl_argv[i]);
                }
                ++i;
            }

            break;
        }

        /* is it the ampersand or an output redirection? if so,
         * then that should be at the end of the cmd line - either
         * way, we ignore it */
        else if (0 == strcmp(cmd->lcl_argv[i], "&") || '>' == cmd->lcl_argv[i][0]
                 || '<' == cmd->lcl_argv[i][0]) {
            ++i;
            continue;
        }

        /* If it's not an option, then this is an error.  Note that
           this is different than an unrecognized token; an
           unrecognized option is *always* an error. */

        else if ('-' != cmd->lcl_argv[i][0]) {
            is_unknown_token = true;
        }

        /* Nope, this is supposedly an option.  Is it a long name? */

        else if (0 == strncmp(cmd->lcl_argv[i], "--", 2)) {
            is_option = true;
            memset(&e, 0, sizeof(prte_cmd_line_init_t));
            e.ocl_cmd_long_name = &cmd->lcl_argv[i][2];
            option = prte_cmd_line_find_option(cmd, &e);
        }

        /* Is it the special-cased "-np" name? */
        else if (0 == strcmp(cmd->lcl_argv[i], "-np")) {
            is_option = true;
            memset(&e, 0, sizeof(prte_cmd_line_init_t));
            e.ocl_cmd_long_name = &cmd->lcl_argv[i][1];
            option = prte_cmd_line_find_option(cmd, &e);
        }

        /* It could be a short name.  Is it? */

        else {
            if (2 < strlen(cmd->lcl_argv[i])) {
                is_unknown_option = true;
            } else {
                memset(&e, 0, sizeof(prte_cmd_line_init_t));
                e.ocl_cmd_short_name = cmd->lcl_argv[i][1];
                option = prte_cmd_line_find_option(cmd, &e);

                /* If we didn't find it, then it is an unknown option */
                if (NULL == option) {
                    is_unknown_option = true;
                } else {
                    is_option = true;
                }
            }
        }

        /* If we figured out above that this is an option, handle it */

        if (is_option) {
            if (NULL == option) {
                is_unknown_option = true;
            } else {
                is_unknown_option = false;
                orig = i;
                ++i;

                /* Pull down the following parameters that belong to
                   this option.  If we run out of parameters, then
                   print an error and return. */

                param = PRTE_NEW(prte_cmd_line_param_t);
                if (NULL == param) {
                    prte_mutex_unlock(&cmd->lcl_mutex);
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                param->clp_arg = cmd->lcl_argv[i];
                param->clp_option = option;

                /* If we have any parameters to this option, pull down
                   tokens starting one beyond the token that we just
                   recognized */

                for (j = 0; j < option->clo_num_params; ++j, ++i) {
                    /* If we run out of parameters, error, unless its a help request
                       which has no arguments */
                    if (i >= cmd->lcl_argc) {
                        /* If this is a help or version request, can have no arguments */
                        if (NULL != option->clo_long_name
                            && (0 == strcmp(option->clo_long_name, "help")
                                || 0 == strcmp(option->clo_long_name, "version"))) {
                            help_without_arg = true;
                            continue;
                        }
                        fprintf(stderr,
                                "%s: Error: option \"%s\" did not "
                                "have enough parameters (%d)\n",
                                cmd->lcl_argv[0], cmd->lcl_argv[orig], option->clo_num_params);
                        if (have_help_option) {
                            fprintf(stderr, "Type '%s --help' for usage.\n", cmd->lcl_argv[0]);
                        }
                        PRTE_RELEASE(param);
                        printed_error = true;
                        goto error;
                    } else {
                        if (0 == strcmp(cmd->lcl_argv[i], special_empty_token)) {
                            fprintf(stderr,
                                    "%s: Error: option \"%s\" did not "
                                    "have enough parameters (%d)\n",
                                    cmd->lcl_argv[0], cmd->lcl_argv[orig], option->clo_num_params);
                            if (have_help_option) {
                                fprintf(stderr, "Type '%s --help' for usage.\n", cmd->lcl_argv[0]);
                            }
                            PRTE_RELEASE(param);
                            printed_error = true;
                            goto error;
                        }

                        /* Otherwise, save this parameter */

                        else {
                            /* Save in the argv on the param entry */
                            if (NULL == (val = set_dest(option, cmd->lcl_argv[i]))) {
                                PRTE_RELEASE(param);
                                printed_error = true;
                                goto error;
                            }
                            prte_list_append(&param->clp_values, &val->super);
                        }
                    }
                }

                /* If there are no options to this command or it is
                   a help request with no argument, check if it is a
                   boolean option and set it accordingly. */

                if (PRTE_CMD_LINE_TYPE_BOOL == option->clo_type
                    && (0 == option->clo_num_params || help_without_arg)) {
                    val = PRTE_NEW(prte_value_t);
                    val->value.type = PMIX_BOOL;
                    if (0 == strncasecmp(cmd->lcl_argv[orig], "t", 1)
                        || 0 != atoi(cmd->lcl_argv[orig])) {
                        val->value.data.flag = true;
                    } else {
                        val->value.data.flag = false;
                    }
                    prte_list_append(&param->clp_values, &val->super);
                }

                /* If we succeeded in all that, save the param to the
                   list on the prte_cmd_line_t handle */

                if (NULL != param) {
                    prte_list_append(&cmd->lcl_params, &param->super);
                }
            }
        }

        /* If we figured out above that this was an unknown option,
           handle it.  Copy everything (including the current token)
           into the tail.  If we're not ignoring unknowns, then print
           an error and return. */
        if (is_unknown_option || is_unknown_token) {
            if (!ignore_unknown || (is_unknown_option && !ignore_unknown_option)) {
                fprintf(stderr, "%s: Error: unknown option \"%s\"\n", cmd->lcl_argv[0],
                        cmd->lcl_argv[i]);
                printed_error = true;
                if (have_help_option) {
                    fprintf(stderr, "Type '%s --help' for usage.\n", cmd->lcl_argv[0]);
                }
            }
        error:
            while (i < cmd->lcl_argc) {
                prte_argv_append(&cmd->lcl_tail_argc, &cmd->lcl_tail_argv, cmd->lcl_argv[i]);
                ++i;
            }

            /* Because i has advanced, we'll fall out of the loop */
        }
    }

    /* Thread serialization */

    prte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    if (printed_error) {
        return PRTE_ERR_SILENT;
    }

    return PRTE_SUCCESS;
}

static char *headers[] = {"/*****      General Options      *****/",
                          "/*****       Debug Options       *****/",
                          "/*****      Output Options       *****/",
                          "/*****       Input Options       *****/",
                          "/*****      Mapping Options      *****/",
                          "/*****      Ranking Options      *****/",
                          "/*****      Binding Options      *****/",
                          "/*****     Developer Options     *****/",
                          "/*****      Launch Options       *****/",
                          "/*****  Fault Tolerance Options  *****/",
                          "/*****    DVM-Specific Options   *****/",
                          "/*****   Currently Unsupported   *****/"};

/*
 * Return a consolidated "usage" message for a PRTE command line handle.
 */
char *prte_cmd_line_get_usage_msg(prte_cmd_line_t *cmd, bool parseable)
{
    size_t i, len;
    size_t j;
    char **argv;
    char *ret, temp[MAX_WIDTH * 2 - 1], line[MAX_WIDTH * 2];
    char *start, *desc, *ptr;
    prte_list_item_t *item;
    prte_cmd_line_option_t *option, **sorted;
    prte_cmd_line_otype_t otype;
    bool found;

    /* Thread serialization */

    prte_mutex_lock(&cmd->lcl_mutex);

    /* Make an argv of all the usage strings */
    argv = NULL;
    ret = NULL;

    for (otype = PRTE_CMD_LINE_OTYPE_GENERAL; otype < PRTE_CMD_LINE_OTYPE_NULL; otype++) {
        found = false;
        /* First, take the original list and sort it */
        sorted = (prte_cmd_line_option_t **) malloc(sizeof(prte_cmd_line_option_t *)
                                                    * prte_list_get_size(&cmd->lcl_options[otype]));
        if (NULL == sorted) {
            prte_mutex_unlock(&cmd->lcl_mutex);
            prte_argv_free(argv);
            return NULL;
        }
        i = 0;
        PRTE_LIST_FOREACH(item, &cmd->lcl_options[otype], prte_list_item_t)
        {
            sorted[i++] = (prte_cmd_line_option_t *) item;
        }
        qsort(sorted, i, sizeof(prte_cmd_line_option_t *), qsort_callback);

        /* add all non-NULL descriptions */
        for (j = 0; j < prte_list_get_size(&cmd->lcl_options[otype]); j++) {
            option = sorted[j];
            if (parseable) {
                if (!found) {
                    /* we have at least one instance, so add the header for this type */
                    prte_argv_append_nosize(&argv, headers[otype]);
                    prte_argv_append_nosize(&argv, " ");
                    found = true;
                }
                ret = build_parsable(option);
                prte_argv_append_nosize(&argv, ret);
                free(ret);
                ret = NULL;
            } else if (NULL != option->clo_description) {
                bool filled = false;

                if (!found) {
                    /* we have at least one instance, so add the header for this type */
                    prte_argv_append_nosize(&argv, headers[otype]);
                    prte_argv_append_nosize(&argv, " ");
                    found = true;
                }
                /* Build up the output line */
                memset(line, 0, sizeof(line));
                if ('\0' != option->clo_short_name) {
                    line[0] = '-';
                    line[1] = option->clo_short_name;
                    filled = true;
                } else {
                    line[0] = ' ';
                    line[1] = ' ';
                }
                if (NULL != option->clo_long_name) {
                    if (filled) {
                        strncat(line, "|", sizeof(line) - 1);
                    } else {
                        strncat(line, " ", sizeof(line) - 1);
                    }
                    strncat(line, "--", sizeof(line) - 1);
                    strncat(line, option->clo_long_name, sizeof(line) - 1);
                }
                strncat(line, " ", sizeof(line) - 1);
                for (i = 0; (int) i < option->clo_num_params; ++i) {
                    len = sizeof(temp);
                    snprintf(temp, len, "<arg%d> ", (int) i);
                    strncat(line, temp, sizeof(line) - 1);
                }
                if (option->clo_num_params > 0) {
                    strncat(line, " ", sizeof(line) - 1);
                }

                /* If we're less than param width, then start adding the
                   description to this line.  Otherwise, finish this line
                   and start adding the description on the next line. */

                if (strlen(line) > PARAM_WIDTH) {
                    prte_argv_append_nosize(&argv, line);

                    /* Now reset the line to be all blanks up to
                       PARAM_WIDTH so that we can start adding the
                       description */

                    memset(line, ' ', PARAM_WIDTH);
                    line[PARAM_WIDTH] = '\0';
                } else {

                    /* Add enough blanks to the end of the line so that we
                       can start adding the description */

                    for (i = strlen(line); i < PARAM_WIDTH; ++i) {
                        line[i] = ' ';
                    }
                    line[i] = '\0';
                }

                /* Loop over adding the description to the array, breaking
                   the string at most at MAX_WIDTH characters.  We need a
                   modifyable description (for simplicity), so strdup the
                   clo_description (because it's likely a compiler
                   constant, and may barf if we write temporary \0's in
                   the middle). */

                desc = strdup(option->clo_description);
                if (NULL == desc) {
                    free(sorted);
                    prte_argv_free(argv);
                    prte_mutex_unlock(&cmd->lcl_mutex);
                    return strdup("");
                }
                start = desc;
                len = strlen(desc);
                do {

                    /* Trim off leading whitespace */

                    while (isspace(*start) && start < desc + len) {
                        ++start;
                    }
                    if (start >= desc + len) {
                        break;
                    }

                    /* Last line */

                    if (strlen(start) < (MAX_WIDTH - PARAM_WIDTH)) {
                        strncat(line, start, sizeof(line) - 1);
                        prte_argv_append_nosize(&argv, line);
                        break;
                    }

                    /* We have more than 1 line's worth left -- find this
                       line's worth and add it to the array.  Then reset
                       and loop around to get the next line's worth. */

                    for (ptr = start + (MAX_WIDTH - PARAM_WIDTH); ptr > start; --ptr) {
                        if (isspace(*ptr)) {
                            *ptr = '\0';
                            strncat(line, start, sizeof(line) - 1);
                            prte_argv_append_nosize(&argv, line);

                            start = ptr + 1;
                            memset(line, ' ', PARAM_WIDTH);
                            line[PARAM_WIDTH] = '\0';
                            break;
                        }
                    }

                    /* If we got all the way back to the beginning of the
                       string, then go forward looking for a whitespace
                       and break there. */

                    if (ptr == start) {
                        for (ptr = start + (MAX_WIDTH - PARAM_WIDTH); ptr < start + len; ++ptr) {
                            if (isspace(*ptr)) {
                                *ptr = '\0';

                                strncat(line, start, sizeof(line) - 1);
                                prte_argv_append_nosize(&argv, line);

                                start = ptr + 1;
                                memset(line, ' ', PARAM_WIDTH);
                                line[PARAM_WIDTH] = '\0';
                                break;
                            }
                        }

                        /* If we reached the end of the string with no
                           whitespace, then just add it on and be done */

                        if (ptr >= start + len) {
                            strncat(line, start, sizeof(line) - 1);
                            prte_argv_append_nosize(&argv, line);
                            start = desc + len + 1;
                        }
                    }
                } while (start < desc + len);
                free(desc);
            }
        }
        free(sorted);
        if (found) {
            /* add a spacer */
            prte_argv_append_nosize(&argv, " ");
            prte_argv_append_nosize(&argv, " ");
            prte_argv_append_nosize(&argv, " ");
        }
    }
    if (NULL != argv) {
        ret = prte_argv_join(argv, '\n');
        prte_argv_free(argv);
    } else {
        ret = strdup("");
    }

    /* Thread serialization */
    prte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    return ret;
}

/*
 * Test if a given option was taken on the parsed command line.
 */
bool prte_cmd_line_is_taken(prte_cmd_line_t *cmd, const char *opt)
{
    return (prte_cmd_line_get_ninsts(cmd, opt) > 0);
}

/*
 * Return the number of instances of an option found during parsing.
 */
int prte_cmd_line_get_ninsts(prte_cmd_line_t *cmd, const char *opt)
{
    int ret;
    prte_cmd_line_param_t *param;
    prte_cmd_line_option_t *option;
    prte_cmd_line_init_t e;

    /* Thread serialization */

    prte_mutex_lock(&cmd->lcl_mutex);

    /* Find the corresponding option.  If we find it, look through all
       the parsed params and see if we have any matches. */

    ret = 0;
    memset(&e, 0, sizeof(prte_cmd_line_init_t));
    if (1 < strlen(opt)) {
        e.ocl_cmd_long_name = opt;
    } else {
        e.ocl_cmd_short_name = opt[0];
    }
    option = prte_cmd_line_find_option(cmd, &e);
    if (NULL != option) {
        PRTE_LIST_FOREACH(param, &cmd->lcl_params, prte_cmd_line_param_t)
        {
            if (param->clp_option == option) {
                ++ret;
            }
        }
    }

    /* Thread serialization */

    prte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */

    return ret;
}

/*
 * Return a specific parameter for a specific instance of a option
 * from the parsed command line.
 */
prte_value_t *prte_cmd_line_get_param(prte_cmd_line_t *cmd, const char *opt, int inst, int idx)
{
    int num_found, ninst;
    prte_cmd_line_param_t *param;
    prte_cmd_line_option_t *option;
    prte_cmd_line_init_t e;
    prte_value_t *val;

    /* Thread serialization */
    prte_mutex_lock(&cmd->lcl_mutex);

    /* Find the corresponding option.  If we find it, look through all
       the parsed params and see if we have any matches. */

    memset(&e, 0, sizeof(prte_cmd_line_init_t));
    if (1 < strlen(opt)) {
        e.ocl_cmd_long_name = opt;
    } else {
        e.ocl_cmd_short_name = opt[0];
    }
    option = prte_cmd_line_find_option(cmd, &e);
    if (NULL != option) {
        ninst = 0;
        /* scan thru the found params */
        PRTE_LIST_FOREACH(param, &cmd->lcl_params, prte_cmd_line_param_t)
        {
            if (param->clp_option == option) {
                if (ninst == inst) {
                    /* scan thru the found values for this option */
                    num_found = 0;
                    PRTE_LIST_FOREACH(val, &param->clp_values, prte_value_t)
                    {
                        if (num_found == idx) {
                            prte_mutex_unlock(&cmd->lcl_mutex);
                            return val;
                        }
                        ++num_found;
                    }
                }
                ++ninst;
            }
        }
    }

    /* Thread serialization */
    prte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    return NULL;
}

/*
 * Return the entire "tail" of unprocessed argv from a PRTE command
 * line handle.
 */
int prte_cmd_line_get_tail(prte_cmd_line_t *cmd, int *tailc, char ***tailv)
{
    if (NULL != cmd) {
        prte_mutex_lock(&cmd->lcl_mutex);
        *tailc = cmd->lcl_tail_argc;
        *tailv = prte_argv_copy(cmd->lcl_tail_argv);
        prte_mutex_unlock(&cmd->lcl_mutex);
        return PRTE_SUCCESS;
    } else {
        return PRTE_ERROR;
    }
}

/**************************************************************************
 * Static functions
 **************************************************************************/

static void option_constructor(prte_cmd_line_option_t *o)
{
    o->clo_short_name = '\0';
    o->clo_long_name = NULL;
    o->clo_num_params = 0;
    o->clo_description = NULL;

    o->clo_type = PRTE_CMD_LINE_TYPE_NULL;
    o->clo_otype = PRTE_CMD_LINE_OTYPE_NULL;
}

static void option_destructor(prte_cmd_line_option_t *o)
{
    if (NULL != o->clo_long_name) {
        free(o->clo_long_name);
    }
    if (NULL != o->clo_description) {
        free(o->clo_description);
    }
}

static void param_constructor(prte_cmd_line_param_t *p)
{
    p->clp_arg = NULL;
    p->clp_option = NULL;
    PRTE_CONSTRUCT(&p->clp_values, prte_list_t);
}

static void param_destructor(prte_cmd_line_param_t *p)
{
    PRTE_LIST_DESTRUCT(&p->clp_values);
}

static void cmd_line_constructor(prte_cmd_line_t *cmd)
{
    int i;

    /* Initialize the mutex.  Since we're creating (and therefore the
       only thread that has this instance), there's no need to lock it
       right now. */

    PRTE_CONSTRUCT(&cmd->lcl_mutex, prte_recursive_mutex_t);

    /* Initialize the lists */
    for (i = 0; i < PRTE_CMD_OPTIONS_MAX; i++) {
        PRTE_CONSTRUCT(&cmd->lcl_options[i], prte_list_t);
    }
    PRTE_CONSTRUCT(&cmd->lcl_params, prte_list_t);

    /* Initialize the argc/argv pairs */

    cmd->lcl_argc = 0;
    cmd->lcl_argv = NULL;
    cmd->lcl_tail_argc = 0;
    cmd->lcl_tail_argv = NULL;
}

static void cmd_line_destructor(prte_cmd_line_t *cmd)
{
    int i;

    /* Free the contents of the options list (do not free the list
       itself; it was not allocated from the heap) */
    for (i = 0; i < PRTE_CMD_OPTIONS_MAX; i++) {
        PRTE_LIST_DESTRUCT(&cmd->lcl_options[i]);
    }

    /* Free any parsed results - destructs the list object */
    free_parse_results(cmd);
    PRTE_DESTRUCT(&cmd->lcl_params);

    /* Destroy the mutex */
    PRTE_DESTRUCT(&cmd->lcl_mutex);
}

static int make_opt(prte_cmd_line_t *cmd, prte_cmd_line_init_t *e)
{
    prte_cmd_line_option_t *option;

    /* Bozo checks */

    if (NULL == cmd) {
        return PRTE_ERR_BAD_PARAM;
    } else if ('\0' == e->ocl_cmd_short_name && NULL == e->ocl_cmd_long_name) {
        return PRTE_ERR_BAD_PARAM;
    } else if (e->ocl_num_params < 0) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* see if the option already exists */
    if (NULL != prte_cmd_line_find_option(cmd, e)) {
        prte_output(0, "Duplicate cmd line entry %c:%s",
                    ('\0' == e->ocl_cmd_short_name) ? ' ' : e->ocl_cmd_short_name,
                    (NULL == e->ocl_cmd_long_name) ? "NULL" : e->ocl_cmd_long_name);
        return PRTE_ERR_BAD_PARAM;
    }

    /* Allocate and fill an option item */
    option = PRTE_NEW(prte_cmd_line_option_t);
    if (NULL == option) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    option->clo_short_name = e->ocl_cmd_short_name;
    if (NULL != e->ocl_cmd_long_name) {
        option->clo_long_name = strdup(e->ocl_cmd_long_name);
    }
    option->clo_num_params = e->ocl_num_params;
    if (NULL != e->ocl_description) {
        option->clo_description = strdup(e->ocl_description);
    }

    option->clo_type = e->ocl_variable_type;
    option->clo_otype = e->ocl_otype;

    /* Append the item, serializing thread access */

    prte_mutex_lock(&cmd->lcl_mutex);
    prte_list_append(&cmd->lcl_options[option->clo_otype], &option->super);
    prte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */

    return PRTE_SUCCESS;
}

static void free_parse_results(prte_cmd_line_t *cmd)
{
    /* Free the contents of the params list (do not free the list
       itself; it was not allocated from the heap) */
    PRTE_LIST_DESTRUCT(&cmd->lcl_params);
    PRTE_CONSTRUCT(&cmd->lcl_params, prte_list_t);

    /* Free the argv's */
    if (NULL != cmd->lcl_argv) {
        prte_argv_free(cmd->lcl_argv);
    }
    cmd->lcl_argv = NULL;
    cmd->lcl_argc = 0;

    if (NULL != cmd->lcl_tail_argv) {
        prte_argv_free(cmd->lcl_tail_argv);
    }
    cmd->lcl_tail_argv = NULL;
    cmd->lcl_tail_argc = 0;
}

prte_cmd_line_option_t *prte_cmd_line_find_option(prte_cmd_line_t *cmd, prte_cmd_line_init_t *e)
{
    int i;
    prte_cmd_line_option_t *option;

    /* Iterate through the list of options hanging off the
     * prte_cmd_line_t and see if we find a match in single-char
     * or long names */
    for (i = 0; i < PRTE_CMD_OPTIONS_MAX; i++) {
        PRTE_LIST_FOREACH(option, &cmd->lcl_options[i], prte_cmd_line_option_t)
        {
            if ((NULL != option->clo_long_name && NULL != e->ocl_cmd_long_name
                 && 0 == strcmp(e->ocl_cmd_long_name, option->clo_long_name))
                || ('\0' != e->ocl_cmd_short_name
                    && e->ocl_cmd_short_name == option->clo_short_name)) {
                return option;
            }
        }
    }

    /* Not found */

    return NULL;
}

static prte_value_t *set_dest(prte_cmd_line_option_t *option, char *sval)
{
    size_t i;
    prte_value_t *val;

    /* Set variable */
    switch (option->clo_type) {
    case PRTE_CMD_LINE_TYPE_STRING:
        val = PRTE_NEW(prte_value_t);
        val->value.type = PMIX_STRING;
        /* check for quotes and remove them */
        if ('\"' == sval[0] && '\"' == sval[strlen(sval) - 1]) {
            val->value.data.string = strdup(&sval[1]);
            val->value.data.string[strlen(val->value.data.string) - 1] = '\0';
        } else {
            val->value.data.string = strdup(sval);
        }
        return val;

    case PRTE_CMD_LINE_TYPE_INT:
        /* check to see that the value given to us truly is an int */
        for (i = 0; i < strlen(sval); i++) {
            if (!isdigit(sval[i]) && '-' != sval[i]) {
                /* show help isn't going to be available yet, so just
                 * print the msg
                 */
                fprintf(stderr, "------------------------------------------------------------------"
                                "----------\n");
                fprintf(stderr, "PRTE has detected that a parameter given to a command line\n");
                fprintf(stderr, "option does not match the expected format:\n\n");
                if (NULL != option->clo_long_name) {
                    fprintf(stderr, "  Option: %s\n", option->clo_long_name);
                } else if ('\0' != option->clo_short_name) {
                    fprintf(stderr, "  Option: %c\n", option->clo_short_name);
                } else {
                    fprintf(stderr, "  Option: <unknown>\n");
                }
                fprintf(stderr, "  Param:  %s\n\n", sval);
                fprintf(stderr, "This is frequently caused by omitting to provide the parameter\n");
                fprintf(stderr, "to an option that requires one. Please check the command line and "
                                "try again.\n");
                fprintf(stderr, "------------------------------------------------------------------"
                                "----------\n");
                return NULL;
            }
        }
        val = PRTE_NEW(prte_value_t);
        val->value.type = PMIX_INT;
        val->value.data.integer = strtol(sval, NULL, 10);
        return val;

    case PRTE_CMD_LINE_TYPE_SIZE_T:
        /* check to see that the value given to us truly is a size_t */
        for (i = 0; i < strlen(sval); i++) {
            if (!isdigit(sval[i]) && '-' != sval[i]) {
                /* show help isn't going to be available yet, so just
                 * print the msg
                 */
                fprintf(stderr, "------------------------------------------------------------------"
                                "----------\n");
                fprintf(stderr, "PRTE has detected that a parameter given to a command line\n");
                fprintf(stderr, "option does not match the expected format:\n\n");
                if (NULL != option->clo_long_name) {
                    fprintf(stderr, "  Option: %s\n", option->clo_long_name);
                } else if ('\0' != option->clo_short_name) {
                    fprintf(stderr, "  Option: %c\n", option->clo_short_name);
                } else {
                    fprintf(stderr, "  Option: <unknown>\n");
                }
                fprintf(stderr, "  Param:  %s\n\n", sval);
                fprintf(stderr, "This is frequently caused by omitting to provide the parameter\n");
                fprintf(stderr, "to an option that requires one. Please check the command line and "
                                "try again.\n");
                fprintf(stderr, "------------------------------------------------------------------"
                                "----------\n");
                return NULL;
            }
        }
        val = PRTE_NEW(prte_value_t);
        val->value.type = PMIX_SIZE;
        val->value.data.integer = strtol(sval, NULL, 10);
        return val;

    case PRTE_CMD_LINE_TYPE_BOOL:
        val = PRTE_NEW(prte_value_t);
        val->value.type = PMIX_BOOL;
        if (0 == strncasecmp(sval, "t", 1) || 0 != atoi(sval)) {
            val->value.data.flag = true;
        } else {
            val->value.data.flag = false;
        }
        return val;

    default:
        return NULL;
    }
}

/*
 * Helper function to qsort_callback
 */
static void fill(const prte_cmd_line_option_t *a, char result[3][BUFSIZ])
{
    int i = 0;

    result[0][0] = '\0';
    result[1][0] = '\0';
    result[2][0] = '\0';

    if ('\0' != a->clo_short_name) {
        snprintf(&result[i][0], BUFSIZ, "%c", a->clo_short_name);
        ++i;
    }
    if (NULL != a->clo_long_name) {
        snprintf(&result[i][0], BUFSIZ, "%s", a->clo_long_name);
        ++i;
    }
}

static int qsort_callback(const void *aa, const void *bb)
{
    int ret, i;
    char str1[3][BUFSIZ], str2[3][BUFSIZ];
    const prte_cmd_line_option_t *a = *((const prte_cmd_line_option_t **) aa);
    const prte_cmd_line_option_t *b = *((const prte_cmd_line_option_t **) bb);

    /* Icky comparison of command line options.  There are multiple
       forms of each command line option, so we first have to check
       which forms each option has.  Compare, in order: short name,
       single-dash name, long name. */

    fill(a, str1);
    fill(b, str2);

    for (i = 0; i < 3; ++i) {
        if (0 != (ret = strcasecmp(str1[i], str2[i]))) {
            return ret;
        }
    }

    /* Shrug -- they must be equal */

    return 0;
}

/*
 * Helper function to build a parsable string for the help
 * output.
 */
static char *build_parsable(prte_cmd_line_option_t *option)
{
    char *line;
    int length;

    length = snprintf(NULL, 0, "%c:%s:%d:%s\n", option->clo_short_name, option->clo_long_name,
                      option->clo_num_params, option->clo_description);

    line = (char *) malloc(length * sizeof(char));

    if ('\0' == option->clo_short_name) {
        snprintf(line, length, "0:%s:%d:%s\n", option->clo_long_name, option->clo_num_params,
                 option->clo_description);
    } else {
        snprintf(line, length, "%c:%s:%d:%s\n", option->clo_short_name, option->clo_long_name,
                 option->clo_num_params, option->clo_description);
    }

    return line;
}
