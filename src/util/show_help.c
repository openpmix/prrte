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
 * Copyright (c) 2008-2018 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

#include "src/runtime/prrte_globals.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/mca/iof/iof.h"
#include "src/mca/rml/rml.h"
#include "src/util/show_help.h"
#include "src/util/show_help_lex.h"
#include "src/util/printf.h"
#include "src/util/argv.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/pmix/pmix-internal.h"

bool prrte_help_want_aggregate = false;

/*
 * Private variables
 */
static const char *default_filename = "help-messages";
static const char *dash_line = "--------------------------------------------------------------------------\n";
static int output_stream = -1;
static char **search_dirs = NULL;
static bool show_help_initialized = false;

/* List items for holding (filename, topic) tuples */
typedef struct {
    prrte_list_item_t super;
    /* The filename */
    char *tli_filename;
    /* The topic */
    char *tli_topic;
    /* List of process names that have displayed this (filename, topic) */
    prrte_list_t tli_processes;
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
    PRRTE_CONSTRUCT(&(obj->tli_processes), prrte_list_t);
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
    PRRTE_LIST_DESTRUCT(&(obj->tli_processes));
}
static PRRTE_CLASS_INSTANCE(tuple_list_item_t, prrte_list_item_t,
                          tuple_list_item_constructor,
                          tuple_list_item_destructor);

/* List of (filename, topic) tuples that have already been displayed */
static prrte_list_t abd_tuples;

/* How long to wait between displaying duplicate show_help notices */
static struct timeval show_help_interval = { 5, 0 };

/* Timer for displaying duplicate help message notices */
static time_t show_help_time_last_displayed = 0;
static bool show_help_timer_set = false;
static prrte_event_t show_help_timer_event;

/*
 * Local functions
 */
static void show_accumulated_duplicates(int fd, short event, void *context);
static char* xml_format(unsigned char *input);
static int show_help(const char *filename, const char *topic,
                     const char *output, prrte_process_name_t *sender);

int prrte_show_help_init(void)
{
    prrte_output_stream_t lds;

    if (show_help_initialized) {
        return PRRTE_SUCCESS;
    }

    PRRTE_CONSTRUCT(&lds, prrte_output_stream_t);
    lds.lds_want_stderr = true;
    output_stream = prrte_output_open(&lds);
    PRRTE_DESTRUCT(&lds);

    PRRTE_CONSTRUCT(&abd_tuples, prrte_list_t);

    prrte_argv_append_nosize(&search_dirs, prrte_install_dirs.prrtedatadir);
    show_help_initialized = true;
    return PRRTE_SUCCESS;
}

void prrte_show_help_finalize (void)
{
    if (!show_help_initialized) {
        return;
    }

    /* Shutdown show_help, showing final messages */
    if (PRRTE_PROC_IS_MASTER) {
        show_accumulated_duplicates(0, 0, NULL);
        PRRTE_LIST_DESTRUCT(&abd_tuples);
        if (show_help_timer_set) {
            prrte_event_evtimer_del(&show_help_timer_event);
        }
        show_help_initialized = false;
        return;
    }

    prrte_output_close(output_stream);
    output_stream = -1;
    PRRTE_LIST_DESTRUCT(&abd_tuples);

    /* destruct the search list */
    if (NULL != search_dirs) {
        prrte_argv_free(search_dirs);
        search_dirs = NULL;
    }
    show_help_initialized = false;
}

/*
 * Make one big string with all the lines.  This isn't the most
 * efficient method in the world, but we're going for clarity here --
 * not optimization.  :-)
 */
static int array2string(char **outstring,
                        int want_error_header, char **lines)
{
    int i, count;
    size_t len;

    /* See how much space we need */

    len = want_error_header ? 2 * strlen(dash_line) : 0;
    count = prrte_argv_count(lines);
    for (i = 0; i < count; ++i) {
        if (NULL == lines[i]) {
            break;
        }
        len += strlen(lines[i]) + 1;
    }

    /* Malloc it out */

    (*outstring) = (char*) malloc(len + 1);
    if (NULL == *outstring) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
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

    return PRRTE_SUCCESS;
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
        for (i=0; NULL != search_dirs[i]; i++) {
            filename = prrte_os_path( false, search_dirs[i], base, NULL );
            prrte_show_help_yyin = fopen(filename, "r");
            if (NULL == prrte_show_help_yyin) {
                prrte_asprintf(&err_msg, "%s: %s", filename, strerror(errno));
                base_len = strlen(base);
                if (4 > base_len || 0 != strcmp(base + base_len - 4, ".txt")) {
                    free(filename);
                    prrte_asprintf(&filename, "%s%s%s.txt", search_dirs[i], PRRTE_PATH_SEP, base);
                    prrte_show_help_yyin = fopen(filename, "r");
                }
            }
            free(filename);
            if (NULL != prrte_show_help_yyin) {
                break;
            }
        }
    }

    /* If we still couldn't open it, then something is wrong */
    if (NULL == prrte_show_help_yyin) {
        prrte_output(output_stream, "%sSorry!  You were supposed to get help about:\n    %s\nBut I couldn't open the help file:\n    %s.  Sorry!\n%s", dash_line, topic, err_msg, dash_line);
        free(err_msg);
        return PRRTE_ERR_NOT_FOUND;
    }

    if (NULL != err_msg) {
        free(err_msg);
    }

    /* Set the buffer */

    prrte_show_help_init_buffer(prrte_show_help_yyin);

    /* Happiness */

    return PRRTE_SUCCESS;
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
        token = prrte_show_help_yylex();
        switch (token) {
        case PRRTE_SHOW_HELP_PARSE_TOPIC:
            tmp = strdup(prrte_show_help_yytext);
            if (NULL == tmp) {
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            tmp[strlen(tmp) - 1] = '\0';
            ret = strcmp(tmp + 1, topic);
            free(tmp);
            if (0 == ret) {
                return PRRTE_SUCCESS;
            }
            break;

        case PRRTE_SHOW_HELP_PARSE_MESSAGE:
            break;

        case PRRTE_SHOW_HELP_PARSE_DONE:
            prrte_output(output_stream, "%sSorry!  You were supposed to get help about:\n    %s\nfrom the file:\n    %s\nBut I couldn't find that topic in the file.  Sorry!\n%s", dash_line, topic, base, dash_line);
            return PRRTE_ERR_NOT_FOUND;
            break;

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
        token = prrte_show_help_yylex();
        switch (token) {
        case PRRTE_SHOW_HELP_PARSE_MESSAGE:
            /* prrte_argv_append_nosize does strdup(prrte_show_help_yytext) */
            rc = prrte_argv_append_nosize(array, prrte_show_help_yytext);
            if (rc != PRRTE_SUCCESS) {
                return rc;
            }
            break;

        default:
            return PRRTE_SUCCESS;
            break;
        }
    }

    /* Never get here */
}


static int load_array(char ***array, const char *filename, const char *topic)
{
    int ret;

    if (PRRTE_SUCCESS != (ret = open_file(filename, topic))) {
        return ret;
    }

    ret = find_topic(filename, topic);
    if (PRRTE_SUCCESS == ret) {
        ret = read_topic(array);
    }

    fclose(prrte_show_help_yyin);
    prrte_show_help_yylex_destroy ();

    if (PRRTE_SUCCESS != ret) {
        prrte_argv_free(*array);
    }

    return ret;
}

char *prrte_show_help_vstring(const char *filename, const char *topic,
                             int want_error_header, va_list arglist)
{
    int rc;
    char *single_string, *output, **array = NULL;

    /* Load the message */
    if (PRRTE_SUCCESS != (rc = load_array(&array, filename, topic))) {
        return NULL;
    }

    /* Convert it to a single raw string */
    rc = array2string(&single_string, want_error_header, array);

    if (PRRTE_SUCCESS == rc) {
        /* Apply the formatting to make the final output string */
        prrte_vasprintf(&output, single_string, arglist);
        free(single_string);
    }

    prrte_argv_free(array);
    return (PRRTE_SUCCESS == rc) ? output : NULL;
}

char *prrte_show_help_string(const char *filename, const char *topic,
                            int want_error_handler, ...)
{
    char *output;
    va_list arglist;

    va_start(arglist, want_error_handler);
    output = prrte_show_help_vstring(filename, topic, want_error_handler,
                                    arglist);
    va_end(arglist);

    return output;
}

int prrte_show_vhelp(const char *filename, const char *topic,
                     int want_error_header, va_list arglist)
{
    char *output;

    /* Convert it to a single string */
    output = prrte_show_help_vstring(filename, topic, want_error_header,
                                    arglist);

    /* If we got a single string, output it with formatting */
    if (NULL != output) {
        prrte_output(output_stream, "%s", output);
        free(output);
    }

    return (NULL == output) ? PRRTE_ERROR : PRRTE_SUCCESS;
}

int prrte_show_help(const char *filename, const char *topic,
                    int want_error_header, ...)
{
    va_list arglist;
    int rc;
    char *output;

    va_start(arglist, want_error_header);
    output = prrte_show_help_vstring(filename, topic, want_error_header,
                                    arglist);
    va_end(arglist);

    /* If nothing came back, there's nothing to do */
    if (NULL == output) {
        return PRRTE_SUCCESS;
    }

    rc = prrte_show_help_norender(filename, topic, want_error_header, output);
    free(output);
    return rc;
}

int prrte_show_help_add_dir(const char *directory)
{
    prrte_argv_append_nosize(&search_dirs, directory);
    return PRRTE_SUCCESS;
}

int prrte_show_help_norender(const char *filename, const char *topic,
                            int want_error_header, const char *output)
{
    int rc = PRRTE_SUCCESS;
    int8_t have_output = 1;
    prrte_buffer_t *buf;
    bool am_inside = false;

    /* if we are the HNP, or the RML has not yet been setup,
     * or ROUTED has not been setup,
     * or we weren't given an HNP, then all we can do is process this locally
     */
    if (PRRTE_PROC_IS_MASTER) {
        rc = show_help(filename, topic, output, PRRTE_PROC_MY_NAME);
        goto CLEANUP;
    } else {
        if (NULL == prrte_rml.send_buffer_nb ||
            NULL == prrte_routed.get_route ||
            NULL == prrte_process_info.my_hnp_uri) {
            rc = show_help(filename, topic, output, PRRTE_PROC_MY_NAME);
            goto CLEANUP;
        }
    }

    /* otherwise, we relay the output message to
     * the HNP for processing
     */

    /* JMS Note that we *may* have a recursion situation here where
       the RML could call show_help.  Need to think about this
       properly, but put a safeguard in here for sure for the time
       being. */
    if (am_inside) {
        rc = show_help(filename, topic, output, PRRTE_PROC_MY_NAME);
    } else {
        am_inside = true;

        /* build the message to the HNP */
        buf = PRRTE_NEW(prrte_buffer_t);
        /* pack the filename of the show_help text file */
        prrte_dss.pack(buf, &filename, 1, PRRTE_STRING);
        /* pack the topic tag */
        prrte_dss.pack(buf, &topic, 1, PRRTE_STRING);
        /* pack the flag that we have a string */
        prrte_dss.pack(buf, &have_output, 1, PRRTE_INT8);
        /* pack the resulting string */
        prrte_dss.pack(buf, &output, 1, PRRTE_STRING);

        /* if we are a daemon, then send it via RML to the HNP */
        if (PRRTE_PROC_IS_DAEMON) {
            /* send it to the HNP */
            if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                                              PRRTE_RML_TAG_SHOW_HELP,
                                                              prrte_rml_send_callback, NULL))) {
                PRRTE_RELEASE(buf);
                /* okay, that didn't work, output locally  */
                prrte_output(output_stream, "%s", output);
            } else {
                rc = PRRTE_SUCCESS;
            }
        } else {
            /* if we are not a daemon (i.e., we are an app) and if PMIx
             * support for "log" is available, then use that channel */
            pmix_status_t ret;
            pmix_info_t info;
            pmix_byte_object_t pbo;
            int32_t nsize;

            prrte_dss.unload(buf, (void**)&pbo.bytes, &nsize);
            pbo.size = nsize;
            PMIX_INFO_LOAD(&info, PRRTE_PMIX_SHOW_HELP, &pbo, PMIX_BYTE_OBJECT);
            ret = PMIx_Log(&info, 1, NULL, 0);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            }
            PMIX_INFO_DESTRUCT(&info);
            PRRTE_RELEASE(buf);
            rc = PRRTE_SUCCESS;
            goto CLEANUP;
        }
        am_inside = false;
    }

  CLEANUP:
    return rc;
}

int prrte_show_help_suppress(const char *filename, const char *topic)
{
    int rc = PRRTE_SUCCESS;
    int8_t have_output = 0;

    if (prrte_execute_quiet) {
        return PRRTE_SUCCESS;
    }

    /* If we are the HNP, or the RML has not yet been setup, or ROUTED
       has not been setup, or we weren't given an HNP, then all we can
       do is process this locally. */
    if (PRRTE_PROC_IS_MASTER ||
        NULL == prrte_rml.send_buffer_nb ||
        NULL == prrte_routed.get_route ||
        NULL == prrte_process_info.my_hnp_uri) {
        rc = show_help(filename, topic, NULL, PRRTE_PROC_MY_NAME);
    }

    /* otherwise, we relay the output message to
     * the HNP for processing
     */
    else {
        prrte_buffer_t *buf;
        static bool am_inside = false;

        /* JMS Note that we *may* have a recursion situation here where
           the RML could call show_help.  Need to think about this
           properly, but put a safeguard in here for sure for the time
           being. */
        if (am_inside) {
            rc = show_help(filename, topic, NULL, PRRTE_PROC_MY_NAME);
        } else {
            am_inside = true;

            /* build the message to the HNP */
            buf = PRRTE_NEW(prrte_buffer_t);
            /* pack the filename of the show_help text file */
            prrte_dss.pack(buf, &filename, 1, PRRTE_STRING);
            /* pack the topic tag */
            prrte_dss.pack(buf, &topic, 1, PRRTE_STRING);
            /* pack the flag that we DO NOT have a string */
            prrte_dss.pack(buf, &have_output, 1, PRRTE_INT8);
            /* send it to the HNP */
            if (PRRTE_SUCCESS != (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                                              PRRTE_RML_TAG_SHOW_HELP,
                                                              prrte_rml_send_callback, NULL))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(buf);
                /* okay, that didn't work, just process locally error, just ignore return  */
                show_help(filename, topic, NULL, PRRTE_PROC_MY_NAME);
            }
            am_inside = false;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * Returns PRRTE_SUCCESS if the strings match; PRRTE_ERROR otherwise.
 */
static int match(const char *a, const char *b)
{
    int rc = PRRTE_ERROR;
    char *p1, *p2, *tmp1 = NULL, *tmp2 = NULL;
    size_t min;

    /* Check straight string match first */
    if (0 == strcmp(a, b)) return PRRTE_SUCCESS;

    if (NULL != strchr(a, '*') || NULL != strchr(b, '*')) {
        tmp1 = strdup(a);
        if (NULL == tmp1) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        tmp2 = strdup(b);
        if (NULL == tmp2) {
            free(tmp1);
            return PRRTE_ERR_OUT_OF_RESOURCE;
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
            rc = PRRTE_SUCCESS;
        }
        free(tmp1);
        free(tmp2);
        return rc;
    }

    /* No match */
    return PRRTE_ERROR;
}

/*
 * Check to see if a given (filename, topic) tuple has been displayed
 * already.  Return PRRTE_SUCCESS if so, or PRRTE_ERR_NOT_FOUND if not.
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
static int get_tli(const char *filename, const char *topic,
                   tuple_list_item_t **tli)
{
    /* Search the list for a duplicate. */
    PRRTE_LIST_FOREACH(*tli, &abd_tuples, tuple_list_item_t) {
        if (PRRTE_SUCCESS == match((*tli)->tli_filename, filename) &&
            PRRTE_SUCCESS == match((*tli)->tli_topic, topic)) {
            return PRRTE_SUCCESS;
        }
    }

    /* Nope, we didn't find it -- make a new one */
    *tli = PRRTE_NEW(tuple_list_item_t);
    if (NULL == *tli) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    (*tli)->tli_filename = strdup(filename);
    (*tli)->tli_topic = strdup(topic);
    prrte_list_append(&abd_tuples, &((*tli)->super));
    return PRRTE_ERR_NOT_FOUND;
}

static void show_accumulated_duplicates(int fd, short event, void *context)
{
    time_t now = time(NULL);
    tuple_list_item_t *tli;
    char *tmp, *output;

    /* Loop through all the messages we've displayed and see if any
       processes have sent duplicates that have not yet been displayed
       yet */
    PRRTE_LIST_FOREACH(tli, &abd_tuples, tuple_list_item_t) {
        if (tli->tli_display &&
            tli->tli_count_since_last_display > 0) {
            static bool first = true;
            if (prrte_xml_output) {
                prrte_asprintf(&tmp, "%d more process%s sent help message %s / %s",
                         tli->tli_count_since_last_display,
                         (tli->tli_count_since_last_display > 1) ? "es have" : " has",
                         tli->tli_filename, tli->tli_topic);
                output = xml_format((unsigned char*)tmp);
                free(tmp);
                fprintf(prrte_xml_fp, "%s", output);
                free(output);
            } else {
                prrte_output(0, "%d more process%s sent help message %s / %s",
                            tli->tli_count_since_last_display,
                            (tli->tli_count_since_last_display > 1) ? "es have" : " has",
                            tli->tli_filename, tli->tli_topic);
            }
            tli->tli_count_since_last_display = 0;

            if (first) {
               if (prrte_xml_output) {
                    fprintf(prrte_xml_fp, "<stderr>Set MCA parameter \"prrte_base_help_aggregate\" to 0 to see all help / error messages</stderr>\n");
                    fflush(prrte_xml_fp);
                } else {
                    prrte_output(0, "Set MCA parameter \"prrte_base_help_aggregate\" to 0 to see all help / error messages");
                }
                first = false;
            }
        }
    }

    show_help_time_last_displayed = now;
    show_help_timer_set = false;
}

/* dealing with special characters in xml output */
static char* xml_format(unsigned char *input)
{
    int i, j, k, len, outlen;
    char *output, qprint[10];
    char *endtag="</stderr>";
    char *starttag="<stderr>";
    int endtaglen, starttaglen;
    bool endtagged = false;

    len = strlen((char*)input);
    /* add some arbitrary size padding */
    output = (char*)malloc((len+1024)*sizeof(char));
    if (NULL == output) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return (char*)input; /* default to no xml formatting */
    }
    memset(output, 0, len+1024);
    outlen = len+1023;
    endtaglen = strlen(endtag);
    starttaglen = strlen(starttag);

    /* start at the beginning */
    k=0;

    /* start with the tag */
    for (j=0; j < starttaglen && k < outlen; j++) {
        output[k++] = starttag[j];
    }

    for (i=0; i < len; i++) {
        if ('&' == input[i]) {
            if (k+5 >= outlen) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                goto error;
            }
            snprintf(qprint, 10, "&amp;");
            for (j=0; j < (int)strlen(qprint) && k < outlen; j++) {
                output[k++] = qprint[j];
            }
        } else if ('<' == input[i]) {
            if (k+4 >= outlen) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                goto error;
            }
            snprintf(qprint, 10, "&lt;");
            for (j=0; j < (int)strlen(qprint) && k < outlen; j++) {
                output[k++] = qprint[j];
            }
        } else if ('>' == input[i]) {
            if (k+4 >= outlen) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                goto error;
            }
            snprintf(qprint, 10, "&gt;");
            for (j=0; j < (int)strlen(qprint) && k < outlen; j++) {
                output[k++] = qprint[j];
            }
        } else if (input[i] < 32 || input[i] > 127) {
            /* this is a non-printable character, so escape it too */
            if (k+7 >= outlen) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                goto error;
            }
            snprintf(qprint, 10, "&#%03d;", (int)input[i]);
            for (j=0; j < (int)strlen(qprint) && k < outlen; j++) {
                output[k++] = qprint[j];
            }
            /* if this was a \n, then we also need to break the line with the end tag */
            if ('\n' == input[i] && (k+endtaglen+1) < outlen) {
                /* we need to break the line with the end tag */
                for (j=0; j < endtaglen && k < outlen-1; j++) {
                    output[k++] = endtag[j];
                }
                /* move the <cr> over */
                output[k++] = '\n';
                /* if this isn't the end of the input buffer, add a new start tag */
                if (i < len-1 && (k+starttaglen) < outlen) {
                    for (j=0; j < starttaglen && k < outlen; j++) {
                        output[k++] = starttag[j];
                        endtagged = false;
                    }
                } else {
                    endtagged = true;
                }
            }
        } else {
            output[k++] = input[i];
        }
    }

    if (!endtagged) {
        /* need to add an endtag */
        for (j=0; j < endtaglen && k < outlen-1; j++) {
            output[k++] = endtag[j];
        }
        output[k++] = '\n';
    }

    return output;

error:
    /* if we couldn't complete the processing for
     * some reason, return the unprocessed input
     * so at least the message gets out!
     */
    free(output);
    return (char*)input;
}

static int show_help(const char *filename, const char *topic,
                     const char *output, prrte_process_name_t *sender)
{
    int rc;
    tuple_list_item_t *tli = NULL;
    prrte_namelist_t *pnli;
    time_t now = time(NULL);

    /* If we're aggregating, check for duplicates.  Otherwise, don't
       track duplicates at all and always display the message. */
    if (prrte_help_want_aggregate) {
        rc = get_tli(filename, topic, &tli);
    } else {
        rc = PRRTE_ERR_NOT_FOUND;
    }

    /* If there's no output string (i.e., this is a control message
       asking us to suppress), then skip to the end. */
    if (NULL == output) {
        tli->tli_display = false;
        goto after_output;
    }

    /* Was it already displayed? */
    if (PRRTE_SUCCESS == rc) {
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
            prrte_event_evtimer_set(prrte_event_base, &show_help_timer_event,
                                   show_accumulated_duplicates, NULL);
            prrte_event_evtimer_add(&show_help_timer_event, &show_help_interval);
            show_help_timer_set = true;
        }
    }
    /* Not already displayed */
    else if (PRRTE_ERR_NOT_FOUND == rc) {
        if (NULL != prrte_iof.output) {
            prrte_iof.output(sender, PRRTE_IOF_STDDIAG, output);
        } else {
            if (prrte_xml_output) {
                char *tmp;
                tmp = xml_format((unsigned char*)output);
                fprintf(prrte_xml_fp, "%s", tmp);
                fflush(prrte_xml_fp);
                free(tmp);
            } else {
                prrte_output(output_stream, "%s", output);
            }
        }
        if (!show_help_timer_set) {
            show_help_time_last_displayed = now;
        }
    }
    /* Some other error occurred */
    else {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

 after_output:
    /* If we're aggregating, add this process name to the list */
    if (prrte_help_want_aggregate) {
        pnli = PRRTE_NEW(prrte_namelist_t);
        if (NULL == pnli) {
            rc = PRRTE_ERR_OUT_OF_RESOURCE;
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        pnli->name = *sender;
        prrte_list_append(&(tli->tli_processes), &(pnli->super));
    }
    return PRRTE_SUCCESS;
}

/* Note that this function is called from ess/hnp, so don't make it
   static */
void prrte_show_help_recv(int status, prrte_process_name_t* sender,
                         prrte_buffer_t *buffer, prrte_rml_tag_t tag,
                         void* cbdata)
{
    char *output=NULL;
    char *filename=NULL, *topic=NULL;
    int32_t n;
    int8_t have_output;
    int rc;

    PRRTE_OUTPUT_VERBOSE((5, prrte_debug_output,
                         "%s got show_help from %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(sender)));

    /* unpack the filename of the show_help text file */
    n = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &filename, &n, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the topic tag */
    n = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &topic, &n, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the flag */
    n = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &have_output, &n, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* If we have an output string, unpack it */
    if (have_output) {
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &output, &n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
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
