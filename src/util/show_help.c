/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "src/mca/iof/base/base.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"
#include "src/util/output.h"
#include "src/util/pmix_printf.h"
#include "src/util/show_help.h"
#include "src/util/show_help_lex.h"

bool prte_help_want_aggregate = false;

/*
 * Private variables
 */
static const char *default_filename = "help-messages";
static const char *dash_line
    = "--------------------------------------------------------------------------\n";
static char **search_dirs = NULL;
static bool show_help_initialized = false;

/*
 * Local functions
 */
static void prte_show_help_cbfunc(pmix_status_t status, void *cbdata)
{
    prte_log_info_t *info = (prte_log_info_t *) cbdata;
    if(PMIX_SUCCESS != status && PMIX_OPERATION_SUCCEEDED != status) {
        fprintf(stderr, "%s", info -> msg);
    }
    PMIX_INFO_DESTRUCT(info -> info);
    if(info -> dirs) {
        PMIX_INFO_DESTRUCT(info -> dirs);
    }
    free(info -> msg);
    free(info);
}

static void local_delivery(const char *file, const char *topic, char *msg) {

    pmix_info_t *info, *dirs;
    int ninfo = 0, ndirs = 0;
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[ninfo++], PMIX_LOG_STDERR, msg, PMIX_STRING);

    prte_log_info_t *cbdata = calloc(1, sizeof(prte_log_info_t));
    if(prte_help_want_aggregate) {
        PMIX_INFO_CREATE(dirs, 3);
        PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_LOG_AGG, &prte_help_want_aggregate, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_LOG_KEY, file, PMIX_STRING);
        PMIX_INFO_LOAD(&dirs[ndirs++], PMIX_LOG_VAL, topic, PMIX_STRING);
        cbdata -> dirs = dirs;
    }

    cbdata -> info = info;
    cbdata -> msg  = msg;

    prte_status_t rc = PMIx_Log_nb(info, ninfo, dirs, ndirs, prte_show_help_cbfunc, cbdata);
    if(PMIX_SUCCESS != rc) {
        PMIX_INFO_DESTRUCT(info);
        if(prte_help_want_aggregate) {
            PMIX_INFO_DESTRUCT(dirs);
        }
        free(msg);
        free(cbdata);
    }
}

int prte_show_help_init(void)
{
    if (show_help_initialized) {
        return PRTE_SUCCESS;
    }

    pmix_argv_append_nosize(&search_dirs, prte_install_dirs.prtedatadir);
    show_help_initialized = true;
    return PRTE_SUCCESS;
}

void prte_show_help_finalize(void)
{
    if (!show_help_initialized) {
        return;
    }

    /* Shutdown show_help, showing final messages */
    if (PRTE_PROC_IS_MASTER) {
        show_help_initialized = false;
        return;
    }

    /* destruct the search list */
    if (NULL != search_dirs) {
        pmix_argv_free(search_dirs);
        search_dirs = NULL;
    }
    show_help_initialized = false;
}

/*
 * Make one big string with all the lines.  This isn't the most
 * efficient method in the world, but we're going for clarity here --
 * not optimization.  :-)
 */
static int array2string(char **outstring, int want_error_header, char **lines)
{
    int i, count;
    size_t len;

    /* See how much space we need */

    len = want_error_header ? 2 * strlen(dash_line) : 0;
    count = pmix_argv_count(lines);
    for (i = 0; i < count; ++i) {
        if (NULL == lines[i]) {
            break;
        }
        len += strlen(lines[i]) + 1;
    }

    /* Malloc it out */

    (*outstring) = (char *) malloc(len + 1);
    if (NULL == *outstring) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* Fill the big string */

    *(*outstring) = '\0';
    if (want_error_header) {
        strcat(*outstring, dash_line);
    }
    for (i = 0; i < count; ++i) {
        if (NULL == lines[i]) {
            break;
        }
        strcat(*outstring, lines[i]);
        strcat(*outstring, "\n");
    }
    if (want_error_header) {
        strcat(*outstring, dash_line);
    }

    return PRTE_SUCCESS;
}


/*
 * Find the right file to open
 */
static int open_file(const char *base, const char *topic)
{
    char *filename;
    char *err_msg = NULL;
    char *tmp;
    size_t base_len;
    int i, rc;

    /* If no filename was supplied, use the default */

    if (NULL == base) {
        base = default_filename;
    }

    /* if this is called prior to someone initializing the system,
     * then don't try to look
     */
    if (NULL != search_dirs) {
        /* Try to open the file.  If we can't find it, try it with a .txt
         * extension.
         */
        for (i = 0; NULL != search_dirs[i]; i++) {
            filename = pmix_os_path(false, search_dirs[i], base, NULL);
            prte_show_help_yyin = fopen(filename, "r");
            if (NULL == prte_show_help_yyin) {
                pmix_asprintf(&err_msg, "%s: %s", filename, strerror(errno));
                base_len = strlen(base);
                if (4 > base_len || 0 != strcmp(base + base_len - 4, ".txt")) {
                    free(filename);
                    pmix_asprintf(&filename, "%s%s%s.txt", search_dirs[i], PRTE_PATH_SEP, base);
                    prte_show_help_yyin = fopen(filename, "r");
                }
            }
            free(filename);
            if (NULL != prte_show_help_yyin) {
                break;
            }
        }
    }

    /* If we still couldn't open it, then something is wrong */
    if (NULL == prte_show_help_yyin) {
        pmix_asprintf(&tmp, "%sSorry!  You were supposed to get help about:\n    %s\nBut I couldn't open "
                      "the help file:\n    %s.  Sorry!\n%s",
                      dash_line, topic, err_msg, dash_line);
        local_delivery(topic, err_msg, tmp);
        free(err_msg);
        return PRTE_ERR_NOT_FOUND;
    }

    if (NULL != err_msg) {
        free(err_msg);
    }

    /* Set the buffer */

    prte_show_help_init_buffer(prte_show_help_yyin);

    /* Happiness */

    return PRTE_SUCCESS;
}

/*
 * In the file that has already been opened, find the topic that we're
 * supposed to output
 */
static int find_topic(const char *base, const char *topic)
{
    int token, ret;
    char *tmp;

    /* Examine every topic */

    while (1) {
        token = prte_show_help_yylex();
        switch (token) {
        case PRTE_SHOW_HELP_PARSE_TOPIC:
            tmp = strdup(prte_show_help_yytext);
            if (NULL == tmp) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            tmp[strlen(tmp) - 1] = '\0';
            ret = strcmp(tmp + 1, topic);
            free(tmp);
            if (0 == ret) {
                return PRTE_SUCCESS;
            }
            break;

        case PRTE_SHOW_HELP_PARSE_MESSAGE:
            break;

        case PRTE_SHOW_HELP_PARSE_DONE:
            pmix_asprintf(&tmp, "%sSorry!  You were supposed to get help about:\n    %s\nfrom the file:\n  "
                          "  %s\nBut I couldn't find that topic in the file.  Sorry!\n%s",
                          dash_line, topic, base, dash_line);
            local_delivery(topic, base, tmp);
            return PRTE_ERR_NOT_FOUND;

        default:
            break;
        }
    }

    /* Never get here */
}

/*
 * We have an open file, and we're pointed at the right topic.  So
 * read in all the lines in the topic and make a list of them.
 */
static int read_topic(char ***array)
{
    int token, rc;

    while (1) {
        token = prte_show_help_yylex();
        switch (token) {
        case PRTE_SHOW_HELP_PARSE_MESSAGE:
            /* pmix_argv_append_nosize does strdup(prte_show_help_yytext) */
            rc = pmix_argv_append_nosize(array, prte_show_help_yytext);
            if (rc != PRTE_SUCCESS) {
                return rc;
            }
            break;

        default:
            return PRTE_SUCCESS;
        }
    }

    /* Never get here */
}

static int load_array(char ***array, const char *filename, const char *topic)
{
    int ret;

    if (PRTE_SUCCESS != (ret = open_file(filename, topic))) {
        return ret;
    }

    ret = find_topic(filename, topic);
    if (PRTE_SUCCESS == ret) {
        ret = read_topic(array);
    }

    fclose(prte_show_help_yyin);
    prte_show_help_yylex_destroy();

    if (PRTE_SUCCESS != ret) {
        pmix_argv_free(*array);
    }

    return ret;
}

char *prte_show_help_vstring(const char *filename, const char *topic, int want_error_header,
                             va_list arglist)
{
    int rc;
    char *single_string, *output, **array = NULL;

    /* Load the message */
    if (PRTE_SUCCESS != (rc = load_array(&array, filename, topic))) {
        return NULL;
    }

    /* Convert it to a single raw string */
    rc = array2string(&single_string, want_error_header, array);

    if (PRTE_SUCCESS == rc) {
        /* Apply the formatting to make the final output string */
        pmix_vasprintf(&output, single_string, arglist);
        free(single_string);
    }

    pmix_argv_free(array);
    return (PRTE_SUCCESS == rc) ? output : NULL;
}

char *prte_show_help_string(const char *filename, const char *topic, int want_error_handler, ...)
{
    char *output;
    va_list arglist;

    va_start(arglist, want_error_handler);
    output = prte_show_help_vstring(filename, topic, want_error_handler, arglist);
    va_end(arglist);

    return output;
}

int prte_show_help(const char *filename, const char *topic, int want_error_header, ...)
{
    va_list arglist;
    int rc;
    char *output;

    va_start(arglist, want_error_header);
    output = prte_show_help_vstring(filename, topic, want_error_header, arglist);
    va_end(arglist);

    /* If nothing came back, there's nothing to do */
    if (NULL == output) {
        return PRTE_SUCCESS;
    }

    rc = prte_show_help_norender(filename, topic, want_error_header, output);
    return rc;
}

int prte_show_help_add_dir(const char *directory)
{
    pmix_argv_append_nosize(&search_dirs, directory);
    return PRTE_SUCCESS;
}

int prte_show_help_norender(const char *filename, const char *topic,
                            int want_error_header, const char *output)
{
    int rc = PRTE_SUCCESS;
    int8_t have_output = 1;
    PRTE_HIDE_UNUSED_PARAMS(want_error_header);

    /* pass to the PMIx server in case we have a tool
     * attached to us */
    if(output) {
        // strdup() it - so show_help owns a copy of this string,
        // and let the caller do what they want with the original string.
        local_delivery(filename, topic, strdup(output));
    }

CLEANUP:
    return rc;
}
