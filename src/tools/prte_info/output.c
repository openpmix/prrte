/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <signal.h>
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif
#include <ctype.h>

#include "src/tools/prte_info/pinfo.h"

#include "src/util/pmix_show_help.h"

#define PRTE_max(a, b) (((a) > (b)) ? (a) : (b))

/*
 * Private variables - set some reasonable screen size defaults
 */

static int centerpoint = 24;
static int screen_width = 78;

/*
 * Prints the passed integer in a pretty or parsable format.
 */
void prte_info_out(const char *pretty_message, const char *plain_message, const char *value)
{
    size_t i, len, max_value_width;
    char *spaces = NULL;
    char *filler = NULL;
    char *pos, *v, savev, *v_to_free;

#ifdef HAVE_ISATTY
    /* If we have isatty(), if this is not a tty, then disable
     * wrapping for grep-friendly behavior
     */
    if (0 == isatty(STDOUT_FILENO)) {
        screen_width = INT_MAX;
    }
#endif

#ifdef TIOCGWINSZ
    if (screen_width < INT_MAX) {
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, (char *) &size) >= 0) {
            screen_width = size.ws_col;
        }
    }
#endif

    /* Strip leading and trailing whitespace from the string value */
    v = v_to_free = strdup(value);
    len = strlen(v);
    if (isspace(v[0])) {
        char *newv;
        i = 0;
        while (isspace(v[i]) && i < len) {
            ++i;
        }
        newv = strdup(v + i);
        free(v_to_free);
        v_to_free = v = newv;
        len = strlen(v);
    }
    if (len > 0 && isspace(v[len - 1])) {
        i = len - 1;
        /* Note that i is size_t (unsigned), so we can't check for i
           >= 0.  But we don't need to, because if the value was all
           whitespace, stripping whitespace from the left (above)
           would have resulted in an empty string, and we wouldn't
           have gotten into this block. */
        while (isspace(v[i]) && i > 0) {
            --i;
        }
        v[i] = '\0';
    }

    if (prte_info_pretty && NULL != pretty_message) {
        if (centerpoint > (int) strlen(pretty_message)) {
            pmix_asprintf(&spaces, "%*s", centerpoint - (int) strlen(pretty_message), " ");
        } else {
            spaces = strdup("");
#if PRTE_ENABLE_DEBUG
            if (centerpoint < (int) strlen(pretty_message)) {
                pmix_show_help("help-prte-info.txt", "developer warning: field too long", false,
                               pretty_message, centerpoint);
            }
#endif
        }
        max_value_width = screen_width - strlen(spaces) - strlen(pretty_message) - 2;
        if (0 < strlen(pretty_message)) {
            pmix_asprintf(&filler, "%s%s: ", spaces, pretty_message);
        } else {
            pmix_asprintf(&filler, "%s  ", spaces);
        }
        free(spaces);
        spaces = NULL;

        while (true) {
            if (strlen(v) < max_value_width) {
                printf("%s%s\n", filler, v);
                break;
            } else {
                pmix_asprintf(&spaces, "%*s", centerpoint + 2, " ");

                /* Work backwards to find the first space before
                 * max_value_width
                 */
                savev = v[max_value_width];
                v[max_value_width] = '\0';
                pos = (char *) strrchr(v, (int) ' ');
                v[max_value_width] = savev;
                if (NULL == pos) {
                    /* No space found < max_value_width.  Look for the first
                     * space after max_value_width.
                     */
                    pos = strchr(&v[max_value_width], ' ');

                    if (NULL == pos) {

                        /* There's just no spaces.  So just print it and be done. */

                        printf("%s%s\n", filler, v);
                        break;
                    } else {
                        *pos = '\0';
                        printf("%s%s\n", filler, v);
                        v = pos + 1;
                    }
                } else {
                    *pos = '\0';
                    printf("%s%s\n", filler, v);
                    v = pos + 1;
                }

                /* Reset for the next iteration */
                free(filler);
                filler = strdup(spaces);
                free(spaces);
                spaces = NULL;
            }
        }
        if (NULL != filler) {
            free(filler);
        }
        if (NULL != spaces) {
            free(spaces);
        }
    } else {
        if (NULL != plain_message && 0 < strlen(plain_message)) {
            printf("%s:%s\n", plain_message, value);
        } else {
            printf("  %s\n", value);
        }
    }
    if (NULL != v_to_free) {
        free(v_to_free);
    }
}

void prte_info_out_int(const char *pretty_message, const char *plain_message, int value)
{
    char *valstr;

    pmix_asprintf(&valstr, "%d", (int) value);
    prte_info_out(pretty_message, plain_message, valstr);
    free(valstr);
}
