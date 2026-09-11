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
#include "src/util/rankfile/rankfile_lex.h"

static char *rankfile_parse_string_or_int(void);

static void rf_map_construct(prte_rankfile_map_t *ptr)
{
    ptr->node_name = NULL;
    memset(ptr->slot_list, (char) 0x00, PRTE_RANKFILE_MAX_SLOTS);
}
static void rf_map_destruct(prte_rankfile_map_t *ptr)
{
    if (NULL != ptr->node_name) {
        free(ptr->node_name);
    }
}
PMIX_CLASS_INSTANCE(prte_rankfile_map_t, pmix_object_t, rf_map_construct, rf_map_destruct);

int prte_util_parse_rankfile(const char *rankfile, pmix_pointer_array_t *rankmap,
                             int *num_ranks)
{
    int token;
    int rc = PRTE_SUCCESS;
    int cnt;
    char *node_name = NULL;
    char **argv;
    char buff[PRTE_RANKFILE_MAX_SLOTS];
    char *value;
    int rank = -1;
    int i;
    prte_node_t *hnp_node;
    prte_rankfile_map_t *rfmap = NULL;
    pmix_pointer_array_t *assigned_ranks_array;
    char tmp_rank_assignment[PRTE_RANKFILE_MAX_SLOTS];

    /* keep track of rank assignments */
    assigned_ranks_array = PMIX_NEW(pmix_pointer_array_t);
    rc = pmix_pointer_array_init(assigned_ranks_array,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                 PRTE_GLOBAL_ARRAY_MAX_SIZE,
                                 PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(assigned_ranks_array);
        return PRTE_ERROR;
    }

    /* get the hnp node's info */
    hnp_node = (prte_node_t *) (prte_node_pool->addr[0]);

    prte_util_rankfile_done = false;
    prte_util_rankfile_in = fopen(rankfile, "r");

    if (NULL == prte_util_rankfile_in) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "no-rankfile", true,
                       prte_tool_basename, rankfile, prte_tool_basename);
        rc = PRTE_ERR_NOT_FOUND;
        goto unlock;
    }

    while (!prte_util_rankfile_done) {
        token = prte_util_rankfile_lex();

        switch (token) {
        case PRTE_RANKFILE_ERROR:
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_QUOTED_STRING:
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "not-supported-rankfile", true,
                           "QUOTED_STRING", rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_NEWLINE:
            rank = -1;
            if (NULL != node_name) {
                free(node_name);
            }
            node_name = NULL;
            rfmap = NULL;
            break;
        case PRTE_RANKFILE_RANK:
            token = prte_util_rankfile_lex();
            if (PRTE_RANKFILE_INT == token) {
                rank = prte_util_rankfile_value.ival;
                rfmap = PMIX_NEW(prte_rankfile_map_t);
                pmix_pointer_array_set_item(rankmap, rank, rfmap);
                (*num_ranks)++; // keep track of number of provided ranks
            } else {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            break;
        case PRTE_RANKFILE_USERNAME:
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "not-supported-rankfile", true, "USERNAME",
                           rankfile);
            rc = PRTE_ERR_BAD_PARAM;
            PRTE_ERROR_LOG(rc);
            goto unlock;
        case PRTE_RANKFILE_EQUAL:
            if (rank < 0) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }
            token = prte_util_rankfile_lex();
            switch (token) {
            case PRTE_RANKFILE_HOSTNAME:
            case PRTE_RANKFILE_IPV4:
            case PRTE_RANKFILE_IPV6:
            case PRTE_RANKFILE_STRING:
            case PRTE_RANKFILE_INT:
            case PRTE_RANKFILE_RELATIVE:
                if (PRTE_RANKFILE_INT == token) {
                    snprintf(buff,PRTE_RANKFILE_MAX_SLOTS,  "%d", prte_util_rankfile_value.ival);
                    value = buff;
                } else {
                    value = prte_util_rankfile_value.sval;
                }
                argv = PMIx_Argv_split(value, '@');
                cnt = PMIx_Argv_count(argv);
                if (NULL != node_name) {
                    free(node_name);
                }
                if (1 == cnt) {
                    node_name = strdup(argv[0]);
                } else if (2 == cnt) {
                    node_name = strdup(argv[1]);
                } else {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    PMIx_Argv_free(argv);
                    node_name = NULL;
                    goto unlock;
                }
                PMIx_Argv_free(argv);

                // Strip off the FQDN if present, ignore IP addresses
                if (!prte_keep_fqdn_hostnames && !pmix_net_isaddr(node_name)) {
                    char *ptr;
                    if (NULL != (ptr = strchr(node_name, '.'))) {
                        *ptr = '\0';
                    }
                }

                /* check the rank item */
                if (NULL == rfmap) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                    rc = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(rc);
                    goto unlock;
                }
                /* check if this is the local node */
                if (prte_check_host_is_local(node_name)) {
                    rfmap->node_name = strdup(hnp_node->name);
                } else {
                    rfmap->node_name = strdup(node_name);
                }
            }
            break;
        case PRTE_RANKFILE_SLOT:
            if (NULL == node_name || rank < 0
                || NULL == (value = rankfile_parse_string_or_int())) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                goto unlock;
            }

            /* check for a duplicate rank assignment. The record has to be
             * filed under the rank it describes, and it has to be a copy:
             * indexing every record at 0 (and pointing them all at one reused
             * stack buffer) meant a rankfile whose first "slot=" line was for
             * any rank but 0 rejected its own rank 0 line as a duplicate, and
             * a genuine duplicate of any other rank went unnoticed */
            if (NULL != pmix_pointer_array_get_item(assigned_ranks_array, rank)) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-assign", true, rank,
                               pmix_pointer_array_get_item(assigned_ranks_array, rank), rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                free(value);
                goto unlock;
            } else {
                /* prepare rank assignment string for the help message in case of a bad-assign */
                snprintf(tmp_rank_assignment, PRTE_RANKFILE_MAX_SLOTS, "%s slot=%s", node_name, value);
                pmix_pointer_array_set_item(assigned_ranks_array, rank,
                                            strdup(tmp_rank_assignment));
            }

            /* check the rank item */
            if (NULL == rfmap) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-rmaps_rank_file.txt", "bad-syntax", true, rankfile);
                rc = PRTE_ERR_BAD_PARAM;
                PRTE_ERROR_LOG(rc);
                free(value);
                goto unlock;
            }
            for (i = 0; i < PRTE_RANKFILE_MAX_SLOTS && '\0' != value[i]; i++) {
                rfmap->slot_list[i] = value[i];
            }
            free(value);
            break;
        }
    }
unlock:
    /* every exit has to give the file and the lexer back - the error paths
     * used to jump straight past the close, leaking a descriptor and the
     * lexer's buffers for every malformed rankfile */
    if (NULL != prte_util_rankfile_in) {
        fclose(prte_util_rankfile_in);
        prte_util_rankfile_in = NULL;
        prte_util_rankfile_lex_destroy();
    }
    if (NULL != node_name) {
        free(node_name);
    }
    for (i = 0; i < assigned_ranks_array->size; i++) {
        value = (char *) pmix_pointer_array_get_item(assigned_ranks_array, i);
        if (NULL != value) {
            free(value);
        }
    }
    PMIX_RELEASE(assigned_ranks_array);
    return rc;
}

static char *rankfile_parse_string_or_int(void)
{
    int rc;
    char tmp_str[PRTE_RANKFILE_MAX_SLOTS];

    if (PRTE_RANKFILE_EQUAL != prte_util_rankfile_lex()) {
        return NULL;
    }

    rc = prte_util_rankfile_lex();
    switch (rc) {
    case PRTE_RANKFILE_STRING:
        return strdup(prte_util_rankfile_value.sval);
    case PRTE_RANKFILE_INT:
        snprintf(tmp_str, PRTE_RANKFILE_MAX_SLOTS, "%d", prte_util_rankfile_value.ival);
        return strdup(tmp_str);
    default:
        return NULL;
    }
}
