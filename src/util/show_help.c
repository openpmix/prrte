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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/mca/iof/iof.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/mca/rml/rml.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/argv.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/util/show_help_lex.h"

bool prte_help_want_aggregate = false;

/*
 * Private variables
 */
static const char *default_filename = "help-messages";
static const char *dash_line
    = "--------------------------------------------------------------------------\n";
static int output_stream = -1;
static char **search_dirs = NULL;
static bool show_help_initialized = false;

/* List items for holding (filename, topic) tuples */
typedef struct {
    prte_list_item_t super;
    /* The filename */
    char *tli_filename;
    /* The topic */
    char *tli_topic;
    /* List of process names that have displayed this (filename, topic) */
    prte_list_t tli_processes;
    /* Time this message was displayed */
    time_t tli_time_displayed;
    /* Count of processes since last display (i.e., "new" processes
       that have showed this message that have not yet been output) */
    int tli_count_since_last_display;
    /* Do we want to display these? */
    bool tli_display;
} tuple_list_item_t;
static void tuple_list_item_constructor(tuple_list_item_t *obj)
{
    obj->tli_filename = NULL;
    obj->tli_topic = NULL;
    PRTE_CONSTRUCT(&(obj->tli_processes), prte_list_t);
    obj->tli_time_displayed = time(NULL);
    obj->tli_count_since_last_display = 0;
    obj->tli_display = true;
}

static void tuple_list_item_destructor(tuple_list_item_t *obj)
{
    if (NULL != obj->tli_filename) {
        free(obj->tli_filename);
    }
    if (NULL != obj->tli_topic) {
        free(obj->tli_topic);
    }
    PRTE_LIST_DESTRUCT(&(obj->tli_processes));
}
static PRTE_CLASS_INSTANCE(tuple_list_item_t, prte_list_item_t, tuple_list_item_constructor,
                           tuple_list_item_destructor);

/* List of (filename, topic) tuples that have already been displayed */
static prte_list_t abd_tuples;

/* How long to wait between displaying duplicate show_help notices */
static struct timeval show_help_interval = {5, 0};

/* Timer for displaying duplicate help message notices */
static time_t show_help_time_last_displayed = 0;
static bool show_help_timer_set = false;
static prte_event_t show_help_timer_event;

/*
 * Local functions
 */
static void show_accumulated_duplicates(int fd, short event, void *context);
static int show_help(const char *filename, const char *topic, const char *output,
                     pmix_proc_t *sender);

int prte_show_help_init(void)
{
    prte_output_stream_t lds;

    if (show_help_initialized) {
        return PRTE_SUCCESS;
    }

    PRTE_CONSTRUCT(&lds, prte_output_stream_t);
    lds.lds_want_stderr = true;
    output_stream = prte_output_open(&lds);
    PRTE_DESTRUCT(&lds);

    PRTE_CONSTRUCT(&abd_tuples, prte_list_t);

    prte_argv_append_nosize(&search_dirs, prte_install_dirs.prtedatadir);
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
        show_accumulated_duplicates(0, 0, NULL);
        PRTE_LIST_DESTRUCT(&abd_tuples);
        if (show_help_timer_set) {
            prte_event_evtimer_del(&show_help_timer_event);
        }
        show_help_initialized = false;
        return;
    }

    prte_output_close(output_stream);
    output_stream = -1;
    PRTE_LIST_DESTRUCT(&abd_tuples);

    /* destruct the search list */
    if (NULL != search_dirs) {
        prte_argv_free(search_dirs);
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
    count = prte_argv_count(lines);
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
    size_t base_len;
    int i;

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
            filename = prte_os_path(false, search_dirs[i], base, NULL);
            prte_show_help_yyin = fopen(filename, "r");
            if (NULL == prte_show_help_yyin) {
                prte_asprintf(&err_msg, "%s: %s", filename, strerror(errno));
                base_len = strlen(base);
                if (4 > base_len || 0 != strcmp(base + base_len - 4, ".txt")) {
                    free(filename);
                    prte_asprintf(&filename, "%s%s%s.txt", search_dirs[i], PRTE_PATH_SEP, base);
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
        prte_output(output_stream,
                    "%sSorry!  You were supposed to get help about:\n    %s\nBut I couldn't open "
                    "the help file:\n    %s.  Sorry!\n%s",
                    dash_line, topic, err_msg, dash_line);
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
            prte_output(output_stream,
                        "%sSorry!  You were supposed to get help about:\n    %s\nfrom the file:\n  "
                        "  %s\nBut I couldn't find that topic in the file.  Sorry!\n%s",
                        dash_line, topic, base, dash_line);
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
            /* prte_argv_append_nosize does strdup(prte_show_help_yytext) */
            rc = prte_argv_append_nosize(array, prte_show_help_yytext);
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
        prte_argv_free(*array);
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
        prte_vasprintf(&output, single_string, arglist);
        free(single_string);
    }

    prte_argv_free(array);
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

int prte_show_vhelp(const char *filename, const char *topic, int want_error_header, va_list arglist)
{
    char *output;

    /* Convert it to a single string */
    output = prte_show_help_vstring(filename, topic, want_error_header, arglist);

    /* If we got a single string, output it with formatting */
    if (NULL != output) {
        prte_output(output_stream, "%s", output);
        free(output);
    }

    return (NULL == output) ? PRTE_ERROR : PRTE_SUCCESS;
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
    free(output);
    return rc;
}

int prte_show_help_add_dir(const char *directory)
{
    prte_argv_append_nosize(&search_dirs, directory);
    return PRTE_SUCCESS;
}

int prte_show_help_norender(const char *filename, const char *topic, int want_error_header,
                            const char *output)
{
    int rc = PRTE_SUCCESS;
    int8_t have_output = 1;
    pmix_data_buffer_t *buf;
    bool am_inside = false;

    /* if we are the HNP, or the RML has not yet been setup,
     * or ROUTED has not been setup,
     * or we weren't given an HNP, then all we can do is process this locally
     */
    if (PRTE_PROC_IS_MASTER || NULL == prte_rml.send_buffer_nb || NULL == prte_routed.get_route
        || NULL == prte_process_info.my_hnp_uri) {
        rc = show_help(filename, topic, output, PRTE_PROC_MY_NAME);
        goto CLEANUP;
    }

    /* otherwise, we relay the output message to
     * the HNP for processing
     */

    /* JMS Note that we *may* have a recursion situation here where
       the RML could call show_help.  Need to think about this
       properly, but put a safeguard in here for sure for the time
       being. */
    if (am_inside) {
        rc = show_help(filename, topic, output, PRTE_PROC_MY_NAME);
    } else {
        am_inside = true;

        /* build the message to the HNP */
        PMIX_DATA_BUFFER_CREATE(buf);
        /* pack the filename of the show_help text file */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &filename, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto CLEANUP;
        }
        /* pack the topic tag */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &topic, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto CLEANUP;
        }
        /* pack the flag that we have a string */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &have_output, 1, PMIX_INT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto CLEANUP;
        }
        /* pack the resulting string */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &output, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            goto CLEANUP;
        }

        /* send it via RML to the HNP */

        if (PRTE_SUCCESS
            != (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_SHOW_HELP,
                                             prte_rml_send_callback, NULL))) {
            PMIX_DATA_BUFFER_RELEASE(buf);
            /* okay, that didn't work, output locally  */
            prte_output(output_stream, "%s", output);
        } else {
            rc = PRTE_SUCCESS;
        }
        am_inside = false;
    }

CLEANUP:
    return rc;
}

int prte_show_help_suppress(const char *filename, const char *topic)
{
    int rc = PRTE_SUCCESS;
    int8_t have_output = 0;
    pmix_data_buffer_t *buf;
    static bool am_inside = false;

    if (prte_execute_quiet) {
        return PRTE_SUCCESS;
    }

    /* If we are the HNP, or the RML has not yet been setup, or ROUTED
       has not been setup, or we weren't given an HNP, then all we can
       do is process this locally. */
    if (PRTE_PROC_IS_MASTER || NULL == prte_rml.send_buffer_nb || NULL == prte_routed.get_route
        || NULL == prte_process_info.my_hnp_uri) {
        rc = show_help(filename, topic, NULL, PRTE_PROC_MY_NAME);
        return rc;
    }

    /* otherwise, we relay the output message to
     * the HNP for processing
     */

    /* JMS Note that we *may* have a recursion situation here where
       the RML could call show_help.  Need to think about this
       properly, but put a safeguard in here for sure for the time
       being. */
    if (am_inside) {
        rc = show_help(filename, topic, NULL, PRTE_PROC_MY_NAME);
    } else {
        am_inside = true;

        /* build the message to the HNP */
        PMIX_DATA_BUFFER_CREATE(buf);
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &filename, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return PRTE_SUCCESS;
        }
        /* pack the topic tag */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &topic, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return PRTE_SUCCESS;
        }
        /* pack the flag that we DO NOT have a string */
        rc = PMIx_Data_pack(PRTE_PROC_MY_NAME, buf, &have_output, 1, PMIX_INT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return PRTE_SUCCESS;
        }
        /* send it to the HNP */
        if (PRTE_SUCCESS
            != (rc = prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_SHOW_HELP,
                                             prte_rml_send_callback, NULL))) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            /* okay, that didn't work, just process locally error, just ignore return  */
            show_help(filename, topic, NULL, PRTE_PROC_MY_NAME);
        }
        am_inside = false;
    }

    return PRTE_SUCCESS;
}

/*
 * Returns PRTE_SUCCESS if the strings match; PRTE_ERROR otherwise.
 */
static int match(const char *a, const char *b)
{
    int rc = PRTE_ERROR;
    char *p1, *p2, *tmp1 = NULL, *tmp2 = NULL;
    size_t min;

    /* Check straight string match first */
    if (0 == strcmp(a, b))
        return PRTE_SUCCESS;

    if (NULL != strchr(a, '*') || NULL != strchr(b, '*')) {
        tmp1 = strdup(a);
        if (NULL == tmp1) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        tmp2 = strdup(b);
        if (NULL == tmp2) {
            free(tmp1);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        p1 = strchr(tmp1, '*');
        p2 = strchr(tmp2, '*');

        if (NULL != p1) {
            *p1 = '\0';
        }
        if (NULL != p2) {
            *p2 = '\0';
        }
        min = strlen(tmp1);
        if (strlen(tmp2) < min) {
            min = strlen(tmp2);
        }
        if (0 == min || 0 == strncmp(tmp1, tmp2, min)) {
            rc = PRTE_SUCCESS;
        }
        free(tmp1);
        free(tmp2);
        return rc;
    }

    /* No match */
    return PRTE_ERROR;
}

/*
 * Check to see if a given (filename, topic) tuple has been displayed
 * already.  Return PRTE_SUCCESS if so, or PRTE_ERR_NOT_FOUND if not.
 *
 * Always return a tuple_list_item_t representing this (filename,
 * topic) entry in the list of "already been displayed tuples" (if it
 * wasn't in the list already, this function will create a new entry
 * in the list and return it).
 *
 * Note that a list is not an overly-efficient mechanism for this kind
 * of data.  The assupmtion is that there will only be a small numebr
 * of (filename, topic) tuples displayed so the storage required will
 * be fairly small, and linear searches will be fast enough.
 */
static int get_tli(const char *filename, const char *topic, tuple_list_item_t **tli)
{
    /* Search the list for a duplicate. */
    PRTE_LIST_FOREACH(*tli, &abd_tuples, tuple_list_item_t)
    {
        if (PRTE_SUCCESS == match((*tli)->tli_filename, filename)
            && PRTE_SUCCESS == match((*tli)->tli_topic, topic)) {
            return PRTE_SUCCESS;
        }
    }

    /* Nope, we didn't find it -- make a new one */
    *tli = PRTE_NEW(tuple_list_item_t);
    if (NULL == *tli) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    (*tli)->tli_filename = strdup(filename);
    (*tli)->tli_topic = strdup(topic);
    prte_list_append(&abd_tuples, &((*tli)->super));
    return PRTE_ERR_NOT_FOUND;
}

static void show_accumulated_duplicates(int fd, short event, void *context)
{
    time_t now = time(NULL);
    tuple_list_item_t *tli;

    /* Loop through all the messages we've displayed and see if any
       processes have sent duplicates that have not yet been displayed
       yet */
    PRTE_LIST_FOREACH(tli, &abd_tuples, tuple_list_item_t)
    {
        if (tli->tli_display && tli->tli_count_since_last_display > 0) {
            static bool first = true;
            prte_output(0, "%d more process%s sent help message %s / %s",
                        tli->tli_count_since_last_display,
                        (tli->tli_count_since_last_display > 1) ? "es have" : " has",
                        tli->tli_filename, tli->tli_topic);
            tli->tli_count_since_last_display = 0;

            if (first) {
                prte_output(0, "Set MCA parameter \"prte_base_help_aggregate\" to 0 to see all "
                               "help / error messages");
                first = false;
            }
        }
    }

    show_help_time_last_displayed = now;
    show_help_timer_set = false;
}

static int show_help(const char *filename, const char *topic, const char *output,
                     pmix_proc_t *sender)
{
    int rc;
    tuple_list_item_t *tli = NULL;
    prte_namelist_t *pnli;
    time_t now = time(NULL);

    /* If we're aggregating, check for duplicates.  Otherwise, don't
       track duplicates at all and always display the message. */
    if (prte_help_want_aggregate) {
        rc = get_tli(filename, topic, &tli);
    } else {
        rc = PRTE_ERR_NOT_FOUND;
    }

    /* If there's no output string (i.e., this is a control message
       asking us to suppress), then skip to the end. */
    if (NULL == output) {
        tli->tli_display = false;
        goto after_output;
    }

    /* Was it already displayed? */
    if (PRTE_SUCCESS == rc) {
        /* Yes.  But do we want to print anything?  That's complicated.

           We always show the first message of a given (filename,
           topic) tuple as soon as it arrives.  But we don't want to
           show duplicate notices often, because we could get overrun
           with them.  So we want to gather them up and say "We got N
           duplicates" every once in a while.

           And keep in mind that at termination, we'll unconditionally
           show all accumulated duplicate notices.

           A simple scheme is as follows:
           - when the first of a (filename, topic) tuple arrives
             - print the message
             - if a timer is not set, set T=now
           - when a duplicate (filename, topic) tuple arrives
             - if now>(T+5) and timer is not set (due to
               non-pre-emptiveness of our libevent, a timer *could* be
               set!)
               - print all accumulated duplicates
               - reset T=now
             - else if a timer was not set, set the timer for T+5
             - else if a timer was set, do nothing (just wait)
           - set T=now when the timer expires
        */
        ++tli->tli_count_since_last_display;
        if (now > show_help_time_last_displayed + 5 && !show_help_timer_set) {
            show_accumulated_duplicates(0, 0, NULL);
        } else if (!show_help_timer_set) {
            prte_event_evtimer_set(prte_event_base, &show_help_timer_event,
                                   show_accumulated_duplicates, NULL);
            prte_event_evtimer_add(&show_help_timer_event, &show_help_interval);
            show_help_timer_set = true;
        }
    }
    /* Not already displayed */
    else if (PRTE_ERR_NOT_FOUND == rc) {
        if (NULL != prte_iof.output) {
            /* send it to any connected tools */
            prte_iof.output(sender, PRTE_IOF_STDDIAG, output);
        }
        prte_output(output_stream, "%s", output);
        if (!show_help_timer_set) {
            show_help_time_last_displayed = now;
        }
    }
    /* Some other error occurred */
    else {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

after_output:
    /* If we're aggregating, add this process name to the list */
    if (prte_help_want_aggregate) {
        pnli = PRTE_NEW(prte_namelist_t);
        if (NULL == pnli) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        pnli->name = *sender;
        prte_list_append(&(tli->tli_processes), &(pnli->super));
    }
    return PRTE_SUCCESS;
}

/* Note that this function is called from ess/hnp, so don't make it
   static */
void prte_show_help_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata)
{
    char *output = NULL;
    char *filename = NULL, *topic = NULL;
    int32_t n;
    int8_t have_output;
    int rc;

    PRTE_OUTPUT_VERBOSE((5, prte_debug_output, "%s got show_help from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)));

    /* unpack the filename of the show_help text file */
    n = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buffer, &filename, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the topic tag */
    n = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buffer, &topic, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the flag */
    n = 1;
    rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buffer, &have_output, &n, PMIX_INT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* If we have an output string, unpack it */
    if (have_output) {
        n = 1;
        rc = PMIx_Data_unpack(PRTE_PROC_MY_NAME, buffer, &output, &n, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* Send it to show_help */
    rc = show_help(filename, topic, output, sender);

cleanup:
    if (NULL != output) {
        free(output);
    }
    if (NULL != filename) {
        free(filename);
    }
    if (NULL != topic) {
        free(topic);
    }
}
