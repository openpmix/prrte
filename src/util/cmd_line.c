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
 * Copyright (c) 2012-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "src/class/prrte_object.h"
#include "src/class/prrte_list.h"
#include "src/dss/dss_types.h"
#include "src/threads/mutex.h"
#include "src/util/argv.h"
#include "src/util/cmd_line.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"

#include "src/mca/base/prrte_mca_base_var.h"
#include "constants.h"


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

/*
 * Description of a command line option
 */
typedef struct prrte_cmd_line_option_t {
    prrte_list_item_t super;

    char clo_short_name;
    char *clo_long_name;

    int clo_num_params;
    char *clo_description;

    prrte_cmd_line_type_t clo_type;
    prrte_cmd_line_otype_t clo_otype;

} prrte_cmd_line_option_t;
static void option_constructor(prrte_cmd_line_option_t *cmd);
static void option_destructor(prrte_cmd_line_option_t *cmd);

PRRTE_CLASS_INSTANCE(prrte_cmd_line_option_t,
                   prrte_list_item_t,
                   option_constructor, option_destructor);

/*
 * An option that was used in the argv that was parsed
 */
typedef struct prrte_cmd_line_param_t {
    prrte_list_item_t super;

    /* Note that clp_arg points to storage "owned" by someone else; it
       has the original option string by reference, not by value.
       Hence, it should not be free()'ed. */

    char *clp_arg;

    /* Pointer to the existing option.  This is also by reference; it
       should not be free()ed. */

    prrte_cmd_line_option_t *clp_option;

    /* This is a list of all the parameters of this option.
       It is owned by this parameter, and should be freed when this
       param_t is freed. */

    prrte_list_t clp_values;
} prrte_cmd_line_param_t;
static void param_constructor(prrte_cmd_line_param_t *cmd);
static void param_destructor(prrte_cmd_line_param_t *cmd);
PRRTE_CLASS_INSTANCE(prrte_cmd_line_param_t,
                   prrte_list_item_t,
                   param_constructor, param_destructor);

/*
 * Instantiate the prrte_cmd_line_t class
 */
static void cmd_line_constructor(prrte_cmd_line_t *cmd);
static void cmd_line_destructor(prrte_cmd_line_t *cmd);
PRRTE_CLASS_INSTANCE(prrte_cmd_line_t,
                   prrte_object_t,
                   cmd_line_constructor,
                   cmd_line_destructor);

/*
 * Private variables
 */
static char special_empty_token[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, '\0'
};

/*
 * Private functions
 */
static int make_opt(prrte_cmd_line_t *cmd, prrte_cmd_line_init_t *e);
static void free_parse_results(prrte_cmd_line_t *cmd);
static int split_shorts(prrte_cmd_line_t *cmd,
                        char *token, char **args,
                        int *output_argc, char ***output_argv,
                        int *num_args_used, bool ignore_unknown);
static prrte_cmd_line_option_t *find_option(prrte_cmd_line_t *cmd,
                                            prrte_cmd_line_init_t *e) __prrte_attribute_nonnull__(1) __prrte_attribute_nonnull__(2);
static prrte_value_t*  set_dest(prrte_cmd_line_option_t *option, char *sval);
static void fill(const prrte_cmd_line_option_t *a, char result[3][BUFSIZ]);
static int qsort_callback(const void *a, const void *b);
static char *build_parsable(prrte_cmd_line_option_t *option);


/*
 * Create an entire command line handle from a table
 */
int prrte_cmd_line_create(prrte_cmd_line_t *cmd,
                         prrte_cmd_line_init_t *table)
{
    int ret = PRRTE_SUCCESS;

    /* Check bozo case */

    if (NULL == cmd) {
        return PRRTE_ERR_BAD_PARAM;
    }
    PRRTE_CONSTRUCT(cmd, prrte_cmd_line_t);

    if (NULL != table) {
        ret = prrte_cmd_line_add(cmd, table);
    }
    return ret;
}

/* Add a table to an existing cmd line object */
int prrte_cmd_line_add(prrte_cmd_line_t *cmd,
                      prrte_cmd_line_init_t *table)
{
    int i, ret;

    /* Ensure we got a table */
    if (NULL == table) {
        return PRRTE_SUCCESS;
    }

    /* Loop through the table */

    for (i = 0; ; ++i) {
        /* Is this the end? */
        if ('\0' == table[i].ocl_cmd_short_name &&
            NULL == table[i].ocl_cmd_long_name) {
            break;
        }

        /* Nope -- it's an entry.  Process it. */
        ret = make_opt(cmd, &table[i]);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}
/*
 * Append a command line entry to the previously constructed command line
 */
int prrte_cmd_line_make_opt_mca(prrte_cmd_line_t *cmd,
                               prrte_cmd_line_init_t entry)
{
    /* Ensure we got an entry */
    if ('\0' == entry.ocl_cmd_short_name &&
        NULL == entry.ocl_cmd_long_name) {
        return PRRTE_SUCCESS;
    }

    return make_opt(cmd, &entry);
}


/*
 * Create a command line option, --long-name and/or -s (short name).
 */
int prrte_cmd_line_make_opt3(prrte_cmd_line_t *cmd, char short_name,
                            const char *long_name,
                            int num_params, const char *desc,
                            prrte_cmd_line_otype_t otype)
{
    prrte_cmd_line_init_t e;

    e.ocl_cmd_short_name = short_name;
    e.ocl_cmd_long_name = long_name;

    e.ocl_num_params = num_params;

    e.ocl_variable_type = PRRTE_CMD_LINE_TYPE_NULL;

    e.ocl_description = desc;
    e.ocl_otype = otype;

    return make_opt(cmd, &e);
}


/*
 * Parse a command line according to a pre-built PRRTE command line
 * handle.
 */
int prrte_cmd_line_parse(prrte_cmd_line_t *cmd, bool ignore_unknown,
                         bool ignore_unknown_option, int argc, char **argv)
{
    int i, j, orig, ret;
    prrte_cmd_line_option_t *option;
    prrte_cmd_line_param_t *param;
    bool is_unknown_option;
    bool is_unknown_token;
    bool is_option;
    char **shortsv;
    int shortsc;
    int num_args_used;
    bool have_help_option = false;
    bool printed_error = false;
    bool help_without_arg = false;
    prrte_cmd_line_init_t e;
    prrte_value_t *val;

    /* Bozo check */

    if (0 == argc || NULL == argv) {
        return PRRTE_SUCCESS;
    }

    /* Thread serialization */

    prrte_mutex_lock(&cmd->lcl_mutex);

    /* Free any parsed results that are already on this handle */

    free_parse_results(cmd);

    /* Analyze each token */

    cmd->lcl_argc = argc;
    cmd->lcl_argv = prrte_argv_copy(argv);

    /* Check up front: do we have a --help option? */
    memset(&e, 0, sizeof(prrte_cmd_line_init_t));
    e.ocl_cmd_long_name = "help";
    option = find_option(cmd, &e);
    if (NULL != option) {
        have_help_option = true;
    }

    /* Now traverse the easy-to-parse sequence of tokens.  Note that
       incrementing i must happen elsewhere; it can't be the third
       clause in the "if" statement. */

    param = NULL;
    option = NULL;
    for (i = 1; i < cmd->lcl_argc; ) {
        is_unknown_option = false;
        is_unknown_token = false;
        is_option = false;

        /* Are we done?  i.e., did we find the special "--" token?  If
           so, copy everything beyond it into the tail (i.e., don't
           bother copying the "--" into the tail). */

        if (0 == strcmp(cmd->lcl_argv[i], "--")) {
            ++i;
            while (i < cmd->lcl_argc) {
                prrte_argv_append(&cmd->lcl_tail_argc, &cmd->lcl_tail_argv,
                                 cmd->lcl_argv[i]);
                ++i;
            }

            break;
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
            memset(&e, 0, sizeof(prrte_cmd_line_init_t));
            e.ocl_cmd_long_name = &cmd->lcl_argv[i][2];
            option = find_option(cmd, &e);
        }

        /* It could be a short name.  Is it? */

        else {
            memset(&e, 0, sizeof(prrte_cmd_line_init_t));
            e.ocl_cmd_short_name = cmd->lcl_argv[i][1];
            option = find_option(cmd, &e);

            /* If we didn't find it, try to split it into shorts.  If
               we find the short option, replace lcl_argv[i] and
               insert the rest into lcl_argv starting after position
               i.  If we don't find the short option, don't do
               anything to lcl_argv so that it can fall through to the
               error condition, below. */

            if (NULL == option) {
                shortsv = NULL;
                shortsc = 0;
                ret = split_shorts(cmd, cmd->lcl_argv[i] + 1,
                                   &(cmd->lcl_argv[i + 1]),
                                   &shortsc, &shortsv,
                                   &num_args_used, ignore_unknown);
                if (PRRTE_SUCCESS == ret) {
                    memset(&e, 0, sizeof(prrte_cmd_line_init_t));
                    e.ocl_cmd_short_name = shortsv[0][1];
                    option = find_option(cmd, &e);

                    if (NULL != option) {
                        prrte_argv_delete(&cmd->lcl_argc,
                                         &cmd->lcl_argv, i,
                                         1 + num_args_used);
                        prrte_argv_insert(&cmd->lcl_argv, i, shortsv);
                        cmd->lcl_argc = prrte_argv_count(cmd->lcl_argv);
                    } else {
                        is_unknown_option = true;
                    }
                    prrte_argv_free(shortsv);
                } else {
                    is_unknown_option = true;
                }
            }

            if (NULL != option) {
                is_option = true;
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
                   this option.  If we run out of parameters, or find
                   that any of them are the special_empty_param
                   (inserted by split_shorts()), then print an error
                   and return. */

                param = PRRTE_NEW(prrte_cmd_line_param_t);
                if (NULL == param) {
                    prrte_mutex_unlock(&cmd->lcl_mutex);
                    return PRRTE_ERR_OUT_OF_RESOURCE;
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
                        if (NULL != option->clo_long_name &&
                            (0 == strcmp(option->clo_long_name, "help") ||
                             0 == strcmp(option->clo_long_name, "version"))) {
                            help_without_arg = true;
                            continue;
                        }
                        fprintf(stderr, "%s: Error: option \"%s\" did not "
                                "have enough parameters (%d)\n",
                                cmd->lcl_argv[0],
                                cmd->lcl_argv[orig],
                                option->clo_num_params);
                        if (have_help_option) {
                            fprintf(stderr, "Type '%s --help' for usage.\n",
                                    cmd->lcl_argv[0]);
                        }
                        PRRTE_RELEASE(param);
                        printed_error = true;
                        goto error;
                    } else {
                        if (0 == strcmp(cmd->lcl_argv[i],
                                        special_empty_token)) {
                            fprintf(stderr, "%s: Error: option \"%s\" did not "
                                    "have enough parameters (%d)\n",
                                    cmd->lcl_argv[0],
                                    cmd->lcl_argv[orig],
                                    option->clo_num_params);
                            if (have_help_option) {
                                fprintf(stderr, "Type '%s --help' for usage.\n",
                                        cmd->lcl_argv[0]);
                            }
                            PRRTE_RELEASE(param);
                            printed_error = true;
                            goto error;
                        }

                        /* Otherwise, save this parameter */

                        else {
                            /* Save in the argv on the param entry */
                            if (NULL == (val = set_dest(option, cmd->lcl_argv[i]))) {
                                PRRTE_RELEASE(param);
                                printed_error = true;
                                goto error;
                            }
                            prrte_list_append(&param->clp_values, &val->super);
                        }
                    }
                }

                /* If there are no options to this command or it is
                   a help request with no argument, check if it is a
                   boolean option and set it accordingly. */

                if (PRRTE_CMD_LINE_TYPE_BOOL == option->clo_type &&
                    (0 == option->clo_num_params || help_without_arg)) {
                    val = PRRTE_NEW(prrte_value_t);
                    val->type = PRRTE_BOOL;
                    if (0 == strncasecmp(cmd->lcl_argv[orig], "t", 1) || 0 != atoi(cmd->lcl_argv[orig])) {
                        val->data.flag = true;
                    } else {
                        val->data.flag = false;
                    }
                    prrte_list_append(&param->clp_values, &val->super);
                }

                /* If we succeeded in all that, save the param to the
                   list on the prrte_cmd_line_t handle */

                if (NULL != param) {
                    prrte_list_append(&cmd->lcl_params, &param->super);
                }
            }
        }

        /* If we figured out above that this was an unknown option,
           handle it.  Copy everything (including the current token)
           into the tail.  If we're not ignoring unknowns, then print
           an error and return. */
        if (is_unknown_option || is_unknown_token) {
            if (!ignore_unknown || (is_unknown_option && !ignore_unknown_option)) {
                fprintf(stderr, "%s: Error: unknown option \"%s\"\n",
                        cmd->lcl_argv[0], cmd->lcl_argv[i]);
                printed_error = true;
                if (have_help_option) {
                    fprintf(stderr, "Type '%s --help' for usage.\n",
                            cmd->lcl_argv[0]);
                }
            }
        error:
            while (i < cmd->lcl_argc) {
                prrte_argv_append(&cmd->lcl_tail_argc, &cmd->lcl_tail_argv,
                                 cmd->lcl_argv[i]);
                ++i;
            }

            /* Because i has advanced, we'll fall out of the loop */
        }
    }

    /* Thread serialization */

    prrte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    if (printed_error) {
        return PRRTE_ERR_SILENT;
    }

    return PRRTE_SUCCESS;
}


static char *headers[] = {
    "/*****      General Options      *****/",
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
    "/*****   Currently Unsupported   *****/"
};

/*
 * Return a consolidated "usage" message for a PRRTE command line handle.
 */
char *prrte_cmd_line_get_usage_msg(prrte_cmd_line_t *cmd, bool parseable)
{
    size_t i, len;
    size_t j;
    char **argv;
    char *ret, temp[MAX_WIDTH * 2], line[MAX_WIDTH * 2];
    char *start, *desc, *ptr;
    prrte_list_item_t *item;
    prrte_cmd_line_option_t *option, **sorted;
    prrte_cmd_line_otype_t otype;
    bool found;

    /* Thread serialization */

    prrte_mutex_lock(&cmd->lcl_mutex);

    /* Make an argv of all the usage strings */
    argv = NULL;
    ret = NULL;

    for (otype=0; otype < PRRTE_CMD_LINE_OTYPE_NULL; otype++) {
        found = false;
        /* First, take the original list and sort it */
        sorted = (prrte_cmd_line_option_t**)malloc(sizeof(prrte_cmd_line_option_t *) *
                                             prrte_list_get_size(&cmd->lcl_options[otype]));
        if (NULL == sorted) {
            prrte_mutex_unlock(&cmd->lcl_mutex);
            prrte_argv_free(argv);
            return NULL;
        }
        i = 0;
        PRRTE_LIST_FOREACH(item, &cmd->lcl_options[otype], prrte_list_item_t) {
            sorted[i++] = (prrte_cmd_line_option_t *) item;
        }
        qsort(sorted, i, sizeof(prrte_cmd_line_option_t*), qsort_callback);

        /* add all non-NULL descriptions */
        for (j=0; j < prrte_list_get_size(&cmd->lcl_options[otype]); j++) {
            option = sorted[j];
            if (parseable) {
                if (!found) {
                    /* we have at least one instance, so add the header for this type */
                    prrte_argv_append_nosize(&argv, headers[otype]);
                    prrte_argv_append_nosize(&argv, " ");
                    found = true;
                }
                ret = build_parsable(option);
                prrte_argv_append_nosize(&argv, ret);
                free(ret);
                ret = NULL;
            } else if (NULL != option->clo_description) {
                bool filled = false;

                if (!found) {
                    /* we have at least one instance, so add the header for this type */
                    prrte_argv_append_nosize(&argv, headers[otype]);
                    prrte_argv_append_nosize(&argv, " ");
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
                for (i = 0; (int)i < option->clo_num_params; ++i) {
                    len = sizeof(temp);
                    snprintf(temp, len, "<arg%d> ", (int)i);
                    strncat(line, temp, sizeof(line) - 1);
                }
                if (option->clo_num_params > 0) {
                    strncat(line, " ", sizeof(line) - 1);
                }

                /* If we're less than param width, then start adding the
                   description to this line.  Otherwise, finish this line
                   and start adding the description on the next line. */

                if (strlen(line) > PARAM_WIDTH) {
                    prrte_argv_append_nosize(&argv, line);

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
                    prrte_argv_free(argv);
                    prrte_mutex_unlock(&cmd->lcl_mutex);
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
                        prrte_argv_append_nosize(&argv, line);
                        break;
                    }

                    /* We have more than 1 line's worth left -- find this
                       line's worth and add it to the array.  Then reset
                       and loop around to get the next line's worth. */

                    for (ptr = start + (MAX_WIDTH - PARAM_WIDTH);
                         ptr > start; --ptr) {
                        if (isspace(*ptr)) {
                            *ptr = '\0';
                            strncat(line, start, sizeof(line) - 1);
                            prrte_argv_append_nosize(&argv, line);

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
                        for (ptr = start + (MAX_WIDTH - PARAM_WIDTH);
                             ptr < start + len; ++ptr) {
                            if (isspace(*ptr)) {
                                *ptr = '\0';

                                strncat(line, start, sizeof(line) - 1);
                                prrte_argv_append_nosize(&argv, line);

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
                            prrte_argv_append_nosize(&argv, line);
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
            prrte_argv_append_nosize(&argv, " ");
            prrte_argv_append_nosize(&argv, " ");
            prrte_argv_append_nosize(&argv, " ");
        }
    }
    if (NULL != argv) {
        ret = prrte_argv_join(argv, '\n');
        prrte_argv_free(argv);
    } else {
        ret = strdup("");
    }

    /* Thread serialization */
    prrte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    return ret;
}


/*
 * Test if a given option was taken on the parsed command line.
 */
bool prrte_cmd_line_is_taken(prrte_cmd_line_t *cmd, const char *opt)
{
    return (prrte_cmd_line_get_ninsts(cmd, opt) > 0);
}


/*
 * Return the number of instances of an option found during parsing.
 */
int prrte_cmd_line_get_ninsts(prrte_cmd_line_t *cmd, const char *opt)
{
    int ret;
    prrte_cmd_line_param_t *param;
    prrte_cmd_line_option_t *option;
    prrte_cmd_line_init_t e;

    /* Thread serialization */

    prrte_mutex_lock(&cmd->lcl_mutex);

    /* Find the corresponding option.  If we find it, look through all
       the parsed params and see if we have any matches. */

    ret = 0;
    memset(&e, 0, sizeof(prrte_cmd_line_init_t));
    if (1 < strlen(opt)) {
        e.ocl_cmd_long_name = opt;
    } else {
        e.ocl_cmd_short_name = opt[0];
    }
    option = find_option(cmd, &e);
    if (NULL != option) {
        PRRTE_LIST_FOREACH(param, &cmd->lcl_params, prrte_cmd_line_param_t) {
            if (param->clp_option == option) {
                ++ret;
            }
        }
    }

    /* Thread serialization */

    prrte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */

    return ret;
}


/*
 * Return a specific parameter for a specific instance of a option
 * from the parsed command line.
 */
prrte_value_t *prrte_cmd_line_get_param(prrte_cmd_line_t *cmd,
                                        const char *opt,
                                        int inst, int idx)
{
    int num_found;
    prrte_cmd_line_param_t *param;
    prrte_cmd_line_option_t *option;
    prrte_cmd_line_init_t e;
    prrte_value_t *val;

    /* Thread serialization */
    prrte_mutex_lock(&cmd->lcl_mutex);

    /* Find the corresponding option.  If we find it, look through all
       the parsed params and see if we have any matches. */

    num_found = 0;
    memset(&e, 0, sizeof(prrte_cmd_line_init_t));
    if (1 < strlen(opt)) {
        e.ocl_cmd_long_name = opt;
    } else {
        e.ocl_cmd_short_name = opt[0];
    }
    option = find_option(cmd, &e);
    if (NULL != option) {
        /* scan thru the found params */
        PRRTE_LIST_FOREACH(param, &cmd->lcl_params, prrte_cmd_line_param_t) {
            if (param->clp_option == option) {
                /* scan thru the found values for this option */
                PRRTE_LIST_FOREACH(val, &param->clp_values, prrte_value_t) {
                    if (num_found == inst) {
                        prrte_mutex_unlock(&cmd->lcl_mutex);
                        return val;
                    }
                    ++num_found;
                }
            }
        }
    }

    /* Thread serialization */
    prrte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */
    return NULL;
}


/*
 * Return the entire "tail" of unprocessed argv from a PRRTE command
 * line handle.
 */
int prrte_cmd_line_get_tail(prrte_cmd_line_t *cmd, int *tailc, char ***tailv)
{
    if (NULL != cmd) {
        prrte_mutex_lock(&cmd->lcl_mutex);
        *tailc = cmd->lcl_tail_argc;
        *tailv = prrte_argv_copy(cmd->lcl_tail_argv);
        prrte_mutex_unlock(&cmd->lcl_mutex);
        return PRRTE_SUCCESS;
    } else {
        return PRRTE_ERROR;
    }
}


/**************************************************************************
 * Static functions
 **************************************************************************/

static void option_constructor(prrte_cmd_line_option_t *o)
{
    o->clo_short_name = '\0';
    o->clo_long_name = NULL;
    o->clo_num_params = 0;
    o->clo_description = NULL;

    o->clo_type = PRRTE_CMD_LINE_TYPE_NULL;
    o->clo_otype = PRRTE_CMD_LINE_OTYPE_NULL;
}


static void option_destructor(prrte_cmd_line_option_t *o)
{
    if (NULL != o->clo_long_name) {
        free(o->clo_long_name);
    }
    if (NULL != o->clo_description) {
        free(o->clo_description);
    }
}


static void param_constructor(prrte_cmd_line_param_t *p)
{
    p->clp_arg = NULL;
    p->clp_option = NULL;
    PRRTE_CONSTRUCT(&p->clp_values, prrte_list_t);
}


static void param_destructor(prrte_cmd_line_param_t *p)
{
    PRRTE_LIST_DESTRUCT(&p->clp_values);
}


static void cmd_line_constructor(prrte_cmd_line_t *cmd)
{
    int i;

    /* Initialize the mutex.  Since we're creating (and therefore the
       only thread that has this instance), there's no need to lock it
       right now. */

    PRRTE_CONSTRUCT(&cmd->lcl_mutex, prrte_recursive_mutex_t);

    /* Initialize the lists */
    for (i=0; i < PRRTE_CMD_OPTIONS_MAX; i++) {
        PRRTE_CONSTRUCT(&cmd->lcl_options[i], prrte_list_t);
    }
    PRRTE_CONSTRUCT(&cmd->lcl_params, prrte_list_t);

    /* Initialize the argc/argv pairs */

    cmd->lcl_argc = 0;
    cmd->lcl_argv = NULL;
    cmd->lcl_tail_argc = 0;
    cmd->lcl_tail_argv = NULL;
}


static void cmd_line_destructor(prrte_cmd_line_t *cmd)
{
    int i;

    /* Free the contents of the options list (do not free the list
       itself; it was not allocated from the heap) */
    for (i=0; i < PRRTE_CMD_OPTIONS_MAX; i++) {
        PRRTE_LIST_DESTRUCT(&cmd->lcl_options[i]);
    }

    /* Free any parsed results - destructs the list object */
    free_parse_results(cmd);
    PRRTE_DESTRUCT(&cmd->lcl_params);

    /* Destroy the mutex */
    PRRTE_DESTRUCT(&cmd->lcl_mutex);
}


static int make_opt(prrte_cmd_line_t *cmd, prrte_cmd_line_init_t *e)
{
    prrte_cmd_line_option_t *option;

    /* Bozo checks */

    if (NULL == cmd) {
        return PRRTE_ERR_BAD_PARAM;
    } else if ('\0' == e->ocl_cmd_short_name &&
               NULL == e->ocl_cmd_long_name) {
        return PRRTE_ERR_BAD_PARAM;
    } else if (e->ocl_num_params < 0) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* see if the option already exists */
    if (NULL != find_option(cmd, e)) {
        prrte_output(0, "Duplicate cmd line entry %c:%s",
                     ('\0' == e->ocl_cmd_short_name) ? ' ' : e->ocl_cmd_short_name,
                     (NULL == e->ocl_cmd_long_name) ? "NULL" : e->ocl_cmd_long_name);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Allocate and fill an option item */
    option = PRRTE_NEW(prrte_cmd_line_option_t);
    if (NULL == option) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
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

    prrte_mutex_lock(&cmd->lcl_mutex);
    prrte_list_append(&cmd->lcl_options[option->clo_otype], &option->super);
    prrte_mutex_unlock(&cmd->lcl_mutex);

    /* All done */

    return PRRTE_SUCCESS;
}


static void free_parse_results(prrte_cmd_line_t *cmd)
{
    /* Free the contents of the params list (do not free the list
       itself; it was not allocated from the heap) */
    PRRTE_LIST_DESTRUCT(&cmd->lcl_params);
    PRRTE_CONSTRUCT(&cmd->lcl_params, prrte_list_t);

    /* Free the argv's */
    if (NULL != cmd->lcl_argv) {
        prrte_argv_free(cmd->lcl_argv);
    }
    cmd->lcl_argv = NULL;
    cmd->lcl_argc = 0;

    if (NULL != cmd->lcl_tail_argv) {
        prrte_argv_free(cmd->lcl_tail_argv);
    }
    cmd->lcl_tail_argv = NULL;
    cmd->lcl_tail_argc = 0;
}


/*
 * Traverse a token and split it into individual letter options (the
 * token has already been certified to not be a long name and not be a
 * short name).  Ensure to differentiate the resulting options from
 * "single dash" names.
 */
static int split_shorts(prrte_cmd_line_t *cmd, char *token, char **args,
                        int *output_argc, char ***output_argv,
                        int *num_args_used, bool ignore_unknown)
{
    int i, j, len;
    prrte_cmd_line_option_t *option;
    char fake_token[3];
    int num_args;
    prrte_cmd_line_init_t e;

    /* Setup that we didn't use any of the args */

    num_args = prrte_argv_count(args);
    *num_args_used = 0;

    /* Traverse the token.  If it's empty (e.g., if someone passes a
       "-" token, which, since the upper level calls this function as
       (argv[i] + 1), will be empty by the time it gets down here),
       just return that we didn't find a short option. */

    len = (int)strlen(token);
    if (0 == len) {
        return PRRTE_ERR_BAD_PARAM;
    }
    fake_token[0] = '-';
    fake_token[2] = '\0';
    memset(&e, 0, sizeof(prrte_cmd_line_init_t));
    for (i = 0; i < len; ++i) {
        fake_token[1] = token[i];
        e.ocl_cmd_short_name = token[i];
        option = find_option(cmd, &e);

        /* If we don't find the option, either return an error or pass
           it through unmodified to the new argv */

        if (NULL == option) {
            if (!ignore_unknown) {
                return PRRTE_ERR_BAD_PARAM;
            } else {
                prrte_argv_append(output_argc, output_argv, fake_token);
            }
        }

        /* If we do find the option, copy it and all of its parameters
           to the output args.  If we run out of paramters (i.e., no
           more tokens in the original argv), that error will be
           handled at a higher level) */

        else {
            prrte_argv_append(output_argc, output_argv, fake_token);
            for (j = 0; j < option->clo_num_params; ++j) {
                if (*num_args_used < num_args) {
                    prrte_argv_append(output_argc, output_argv,
                                     args[*num_args_used]);
                    ++(*num_args_used);
                } else {
                    prrte_argv_append(output_argc, output_argv,
                                     special_empty_token);
                }
            }
        }
    }

    /* All done */

    return PRRTE_SUCCESS;
}


static prrte_cmd_line_option_t *find_option(prrte_cmd_line_t *cmd,
                                            prrte_cmd_line_init_t *e)
{
    int i;
    prrte_cmd_line_option_t *option;

    /* Iterate through the list of options hanging off the
     * prrte_cmd_line_t and see if we find a match in single-char
     * or long names */
    for (i=0; i < PRRTE_CMD_OPTIONS_MAX; i++) {
        PRRTE_LIST_FOREACH(option, &cmd->lcl_options[i], prrte_cmd_line_option_t) {
            if ((NULL != option->clo_long_name &&
                 NULL != e->ocl_cmd_long_name &&
                 0 == strcmp(e->ocl_cmd_long_name, option->clo_long_name))||
                ('\0' != e->ocl_cmd_short_name &&
                 e->ocl_cmd_short_name == option->clo_short_name)) {
                return option;
            }
        }
    }

    /* Not found */

    return NULL;
}


static prrte_value_t* set_dest(prrte_cmd_line_option_t *option, char *sval)
{
    size_t i;
    prrte_value_t *val;

    /* Set variable */
    switch(option->clo_type) {
        case PRRTE_CMD_LINE_TYPE_STRING:
            val = PRRTE_NEW(prrte_value_t);
            val->type = PRRTE_STRING;
            /* check for quotes and remove them */
            if ('\"' == sval[0] && '\"' == sval[strlen(sval)-1]) {
                val->data.string = strdup(&sval[1]);
                val->data.string[strlen(val->data.string)-1] = '\0';
            } else {
                val->data.string = strdup(sval);
            }
            return val;

        case PRRTE_CMD_LINE_TYPE_INT:
            /* check to see that the value given to us truly is an int */
            for (i=0; i < strlen(sval); i++) {
                if (!isdigit(sval[i]) && '-' != sval[i]) {
                    /* show help isn't going to be available yet, so just
                     * print the msg
                     */
                    fprintf(stderr, "----------------------------------------------------------------------------\n");
                    fprintf(stderr, "PRRTE has detected that a parameter given to a command line\n");
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
                    fprintf(stderr, "to an option that requires one. Please check the command line and try again.\n");
                    fprintf(stderr, "----------------------------------------------------------------------------\n");
                    return NULL;
                }
            }
            val = PRRTE_NEW(prrte_value_t);
            val->type = PRRTE_INT;
            val->data.integer = strtol(sval, NULL, 10);
            return val;

        case PRRTE_CMD_LINE_TYPE_SIZE_T:
            /* check to see that the value given to us truly is a size_t */
            for (i=0; i < strlen(sval); i++) {
                if (!isdigit(sval[i]) && '-' != sval[i]) {
                    /* show help isn't going to be available yet, so just
                     * print the msg
                     */
                    fprintf(stderr, "----------------------------------------------------------------------------\n");
                    fprintf(stderr, "PRRTE has detected that a parameter given to a command line\n");
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
                    fprintf(stderr, "to an option that requires one. Please check the command line and try again.\n");
                    fprintf(stderr, "----------------------------------------------------------------------------\n");
                    return NULL;
                }
            }
            val = PRRTE_NEW(prrte_value_t);
            val->type = PRRTE_SIZE;
            val->data.integer = strtol(sval, NULL, 10);
            return val;

        case PRRTE_CMD_LINE_TYPE_BOOL:
            val = PRRTE_NEW(prrte_value_t);
            val->type = PRRTE_BOOL;
            if (0 == strncasecmp(sval, "t", 1) || 0 != atoi(sval)) {
                val->data.flag = true;
            } else {
                val->data.flag = false;
            }
            return val;

        default:
            return NULL;
    }

    return NULL;
}


/*
 * Helper function to qsort_callback
 */
static void fill(const prrte_cmd_line_option_t *a, char result[3][BUFSIZ])
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
    const prrte_cmd_line_option_t *a = *((const prrte_cmd_line_option_t**) aa);
    const prrte_cmd_line_option_t *b = *((const prrte_cmd_line_option_t**) bb);

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
static char *build_parsable(prrte_cmd_line_option_t *option) {
    char *line;
    int length;

    length = snprintf(NULL, 0, "%c:%s:%d:%s\n", option->clo_short_name,
                      option->clo_long_name, option->clo_num_params, option->clo_description);

    line = (char *)malloc(length * sizeof(char));

    if('\0' == option->clo_short_name) {
        snprintf(line, length, "0:%s:%d:%s\n", option->clo_long_name,
                 option->clo_num_params, option->clo_description);
    } else {
        snprintf(line, length, "%c:%s:%d:%s\n", option->clo_short_name,
                 option->clo_long_name, option->clo_num_params, option->clo_description);
    }

    return line;
}
