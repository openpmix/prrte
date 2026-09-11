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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_net.h"
#include "src/util/prte_show_help.h"
#include "src/util/rankfile/rankfile.h"
#include "src/util/textfile.h"

static void rf_map_construct(prte_rankfile_map_t *ptr)
{
    ptr->node_name = NULL;
    ptr->slot_list = NULL;
}
static void rf_map_destruct(prte_rankfile_map_t *ptr)
{
    if (NULL != ptr->node_name) {
        free(ptr->node_name);
    }
    if (NULL != ptr->slot_list) {
        free(ptr->slot_list);
    }
}
PMIX_CLASS_INSTANCE(prte_rankfile_map_t, pmix_object_t, rf_map_construct, rf_map_destruct);

/* the file and line the messages name, so that every call site does not
 * have to carry them */
static const char *cur_rankfile_name = NULL;
static int cur_rankfile_line = 0;

static void rankfile_syntax_error(const char *offender)
{
    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true,
                   cur_rankfile_name, cur_rankfile_line, offender);
}

/*
 * Is this field a name we will take for a node?  The same question the
 * hostfile parser asks, and for the same reason: the scanner this replaces
 * sorted names into four token types and this parser listed all four in one
 * switch arm, so what it actually needed to know was only whether the field
 * is made of characters a name may contain.  A relative "+n<K>" is a name
 * here too -- it is stored as written and resolved by the mapper.
 */
static bool valid_nodename(const char *s)
{
    size_t i;

    if ('\0' == s[0]) {
        return false;
    }
    if ('+' == s[0]) {
        if ('n' != s[1] && 'N' != s[1]) {
            return false;
        }
        if ('\0' == s[2]) {
            return false;
        }
        for (i = 2; '\0' != s[i]; i++) {
            if (!isdigit((unsigned char) s[i])) {
                return false;
            }
        }
        return true;
    }
    for (i = 0; '\0' != s[i]; i++) {
        if (!isalnum((unsigned char) s[i]) && NULL == strchr("_-.,:*@", s[i])) {
            return false;
        }
    }
    return true;
}

static int parse_rank(const char *value, int *result)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(value, &end, 10);
    if (0 != errno || end == value || '\0' != *end || 0 > v || INT_MAX < v) {
        return PRTE_ERR_BAD_PARAM;
    }
    *result = (int) v;
    return PRTE_SUCCESS;
}

/*
 * Take the node name out of a "<host>" or "<user>@<host>" field.
 *
 * The user half is dropped.  That is what this parser has always done, and
 * it is worth knowing that it differs from the hostfile parser, which keeps
 * it as a node attribute: a rankfile record has nowhere to put it.  Saying
 * "username=" outright is refused as unsupported, so the two spellings of
 * the same thing get two different answers; changing that means giving the
 * record somewhere to keep it.
 */
static char *node_from_entry(const char *entry)
{
    char **argv;
    char *name = NULL;
    int cnt;

    argv = PMIx_Argv_split(entry, '@');
    cnt = PMIx_Argv_count(argv);
    if (1 == cnt) {
        name = strdup(argv[0]);
    } else if (2 == cnt) {
        name = strdup(argv[1]);
    }
    PMIx_Argv_free(argv);
    return name;
}

int prte_util_parse_rankfile(const char *rankfile, pmix_pointer_array_t *rankmap,
                             int *num_ranks)
{
    prte_textfile_t tf;
    prte_node_t *hnp_node;
    prte_rankfile_map_t *rfmap;
    char **fields;
    char *node_name = NULL;
    char *ptr;
    int rc, rank, i;

    cur_rankfile_name = rankfile;
    cur_rankfile_line = 0;

    rc = prte_textfile_open(&tf, rankfile);
    if (PRTE_SUCCESS != rc) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "no-rankfile", true,
                       prte_tool_basename, rankfile, prte_tool_basename);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* the name to substitute for any entry that turns out to be this
     * machine, so that a rankfile naming the head node by any of its names
     * lands on the same node object the daemon reports itself as */
    hnp_node = (prte_node_t *) (prte_node_pool->addr[0]);

    /*
     * Every line is "rank <N> = <host>", optionally followed by
     * "slot = <list>".  The scanner this replaces produced a token stream
     * in which those four things were four separate cases carrying state
     * between them, reset on a newline token -- which is to say it was
     * reading lines, the hard way.
     */
    while (NULL != (fields = prte_textfile_next(&tf))) {
        cur_rankfile_line = tf.lineno;
        rc = PRTE_ERR_BAD_PARAM;

        if (0 != strcmp("rank", fields[0])) {
            rankfile_syntax_error(fields[0]);
            goto cleanup;
        }
        if (NULL == fields[1] || PRTE_SUCCESS != parse_rank(fields[1], &rank)) {
            rankfile_syntax_error((NULL == fields[1]) ? fields[0] : fields[1]);
            goto cleanup;
        }
        if (NULL == fields[2] || 0 != strcmp("=", fields[2])) {
            rankfile_syntax_error((NULL == fields[2]) ? fields[1] : fields[2]);
            goto cleanup;
        }
        if (NULL == fields[3] || !valid_nodename(fields[3])) {
            rankfile_syntax_error((NULL == fields[3]) ? fields[2] : fields[3]);
            goto cleanup;
        }

        /*
         * Refuse a rank the file has already placed.
         *
         * The check this replaces lived on the "slot=" path alone, so two
         * lines naming the same rank and then stopping were accepted in
         * silence: the second record overwrote the first in the map -- and
         * leaked it, the slot being replaced without releasing what was
         * there -- the rank count was incremented for both, and the file's
         * answer was quietly the last one.  A rankfile that says rank 0
         * twice should not be answered by picking one.
         */
        rfmap = (prte_rankfile_map_t *) pmix_pointer_array_get_item(rankmap, rank);
        if (NULL != rfmap) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-assign",
                           true, rank, rfmap->node_name, rankfile);
            goto cleanup;
        }

        node_name = node_from_entry(fields[3]);
        if (NULL == node_name) {
            /* more than one "@": neither a hostname nor a "user@hostname" */
            rankfile_syntax_error(fields[3]);
            goto cleanup;
        }

        /* strip the FQDN if we are not keeping them, but never off an
         * address, whose dots are not domain separators */
        if (!prte_keep_fqdn_hostnames && !pmix_net_isaddr(node_name)) {
            if (NULL != (ptr = strchr(node_name, '.'))) {
                *ptr = '\0';
            }
        }

        rfmap = PMIX_NEW(prte_rankfile_map_t);
        if (prte_check_host_is_local(node_name)) {
            rfmap->node_name = strdup(hnp_node->name);
        } else {
            rfmap->node_name = strdup(node_name);
        }
        free(node_name);
        node_name = NULL;

        /* "slot = <list>", if the line carries one.  "slots" is taken as a
         * synonym, as it always has been. */
        i = 4;
        if (NULL != fields[i]) {
            if (0 == strcmp("username", fields[i]) || 0 == strcmp("user-name", fields[i])
                || 0 == strcmp("user_name", fields[i])) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt",
                               "not-supported-rankfile", true, "USERNAME", rankfile);
                PMIX_RELEASE(rfmap);
                goto cleanup;
            }
            if ((0 != strcmp("slot", fields[i]) && 0 != strcmp("slots", fields[i]))
                || NULL == fields[i + 1] || 0 != strcmp("=", fields[i + 1])
                || NULL == fields[i + 2]) {
                rankfile_syntax_error(fields[i]);
                PMIX_RELEASE(rfmap);
                goto cleanup;
            }
            rfmap->slot_list = strdup(fields[i + 2]);
            i += 3;
            if (NULL != fields[i]) {
                rankfile_syntax_error(fields[i]);
                PMIX_RELEASE(rfmap);
                goto cleanup;
            }
        }

        pmix_pointer_array_set_item(rankmap, rank, rfmap);
        (*num_ranks)++;
    }
    rc = PRTE_SUCCESS;

cleanup:
    if (NULL != node_name) {
        free(node_name);
    }
    prte_textfile_close(&tf);
    cur_rankfile_name = NULL;
    cur_rankfile_line = 0;
    return rc;
}
