/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/util/hostfile/hostfile.h"
#include "src/util/textfile.h"

static const char *cur_hostfile_name = NULL;
static int cur_hostfile_line = 0;

/*
 * Refuse a line, naming the file, the line and the text that was wrong
 * with it.  The messages this replaces named a token number instead of
 * the text -- a lexer's internal numbering, which told a user nothing
 * they could act on and no longer exists.
 */
static void hostfile_parse_error(const char *offender)
{
    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "parse_error", true,
                   cur_hostfile_name, cur_hostfile_line, offender);
}

/*
 * The keywords a host entry may carry, and every spelling of each that
 * has ever been accepted.  Anything else in the keyword position is an
 * error -- including "boards", "sockets" and "cores", which the lexer had
 * dedicated rules for and the parser has never had a case for.
 */
typedef enum {
    HKEY_UNKNOWN = 0,
    HKEY_SLOTS,
    HKEY_SLOTS_MAX,
    HKEY_USERNAME,
    HKEY_PORT
} hostfile_key_t;

static hostfile_key_t hostfile_keyword(const char *word)
{
    static const struct {
        const char *spelling;
        hostfile_key_t key;
    } table[] = {{"slots", HKEY_SLOTS},
                 {"cpu", HKEY_SLOTS},
                 {"count", HKEY_SLOTS},
                 {"slots-max", HKEY_SLOTS_MAX},
                 {"slots_max", HKEY_SLOTS_MAX},
                 {"max-slots", HKEY_SLOTS_MAX},
                 {"max_slots", HKEY_SLOTS_MAX},
                 {"cpu-max", HKEY_SLOTS_MAX},
                 {"cpu_max", HKEY_SLOTS_MAX},
                 {"max-cpu", HKEY_SLOTS_MAX},
                 {"max_cpu", HKEY_SLOTS_MAX},
                 {"count-max", HKEY_SLOTS_MAX},
                 {"count_max", HKEY_SLOTS_MAX},
                 {"max-count", HKEY_SLOTS_MAX},
                 {"max_count", HKEY_SLOTS_MAX},
                 {"username", HKEY_USERNAME},
                 {"user-name", HKEY_USERNAME},
                 {"user_name", HKEY_USERNAME},
                 {"port", HKEY_PORT},
                 {NULL, HKEY_UNKNOWN}};
    int i;

    for (i = 0; NULL != table[i].spelling; i++) {
        if (0 == strcmp(word, table[i].spelling)) {
            return table[i].key;
        }
    }
    return HKEY_UNKNOWN;
}

/*
 * Is this field a name we will take for a node?
 *
 * This is the union of everything the lexer's name rules matched: plain
 * names, names with dots, IPv4 and IPv6 addresses, any of them optionally
 * carrying a "user@" in front and a leading "^" to exclude.  The lexer
 * sorted those into four different token types, which is most of what
 * made it complicated -- and neither this file nor the rankfile parser
 * ever looked at which type it got.  What matters is only that the field
 * is made of characters a name may contain, so that a quoted string or a
 * stray "$" is still refused.
 */
static bool valid_nodename(const char *s)
{
    size_t i = 0;

    if ('^' == s[0]) {
        i = 1;
    }
    if ('\0' == s[i]) {
        return false;
    }
    for (; '\0' != s[i]; i++) {
        if (!isalnum((unsigned char) s[i]) && NULL == strchr("_-.,:*@", s[i])) {
            return false;
        }
    }
    return true;
}

/*
 * "+n<N>" names a node by its position in the pool and "+e", or "+e:<N>",
 * asks for empty ones.  Both letters are taken in either case: the lexer
 * accepted "+E" but only lower-case "+n", so the parser's own test for
 * 'N' could never fire, and there was no reason for the two forms to
 * differ.
 */
static bool valid_relative(const char *s)
{
    size_t i;

    if ('+' != s[0]) {
        return false;
    }
    if ('n' == s[1] || 'N' == s[1]) {
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
    if ('e' == s[1] || 'E' == s[1]) {
        if ('\0' == s[2]) {
            return true;
        }
        if (':' != s[2] || '\0' == s[3]) {
            return false;
        }
        for (i = 3; '\0' != s[i]; i++) {
            if (!isdigit((unsigned char) s[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* Read a count. Refuses anything that is not entirely digits, which the
 * lexer did by simply not matching it as a number. */
static int parse_count(const char *value, int *result)
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
 * Split a host entry into its optional username and the node name.
 *
 * A name may carry the account to reach it with, written in front and
 * separated by a single "@".  Anything with a second "@" is neither a
 * hostname nor a "user@hostname", and has to be refused by name, file and
 * line like every other failure here.
 */
static int hostfile_parse_username(const char *value, char **username, char **node_name)
{
    char **argv;
    int cnt;

    argv = PMIx_Argv_split(value, '@');
    cnt = PMIx_Argv_count(argv);
    if (1 == cnt) {
        *node_name = strdup(argv[0]);
    } else if (2 == cnt) {
        *username = strdup(argv[0]);
        *node_name = strdup(argv[1]);
    } else {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "user-host", true,
                       cur_hostfile_name, cur_hostfile_line, value);
        PMIx_Argv_free(argv);
        return PRTE_ERR_SILENT;
    }
    PMIx_Argv_free(argv);
    return PRTE_SUCCESS;
}

/* Put a node on the exclude list, or note another name for one already
 * there.  The leading "^" has been stripped by the caller. */
static void hostfile_exclude(const char *node_name, const char *username, pmix_list_t *exclude)
{
    prte_node_t *node;

    PMIX_OUTPUT_VERBOSE((3, prte_ras_base_framework.framework_output,
                         "%s hostfile: node %s is being excluded",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node_name));

    node = prte_node_match(exclude, node_name);
    if (NULL == node) {
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(node_name);
        if (NULL != username) {
            prte_set_attribute(&node->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL,
                               (void *) username, PMIX_STRING);
        }
        pmix_list_append(exclude, &node->super);
    } else if (0 != strcmp(node_name, node->name)) {
        /* the node name may not match the prior entry, so keep it */
        PMIx_Argv_append_unique_nosize(&node->aliases, (char *) node_name);
    }
}

/*
 * Parse one line: a host entry, optionally followed by "<key> = <value>"
 * groups.  Also accepts a rankfile's "rank <N>=<host> ..." line, from
 * which it takes only the host.
 */
static int hostfile_parse_line(char **fields, pmix_list_t *updates, pmix_list_t *exclude,
                               bool keep_all)
{
    prte_node_t *node;
    bool got_max = false;
    bool rank_form = false;
    char *node_name = NULL;
    char *username = NULL;
    const char *entry;
    hostfile_key_t key;
    int i, rc, count;

    entry = fields[0];
    i = 1;

    /*
     * A rankfile line read as a hostfile.  The rank means nothing here --
     * what is wanted is the node it names, which is whatever follows the
     * first "=" -- and the rest of the line is the rankfile's business.
     */
    if (0 == strcmp("rank", entry)) {
        for (i = 1; NULL != fields[i]; i++) {
            if (0 == strcmp("=", fields[i])) {
                break;
            }
        }
        if (NULL == fields[i] || NULL == fields[i + 1]) {
            /* the line ended before the "=" that must follow a rank, or
             * before the name that must follow the "=" */
            hostfile_parse_error(fields[0]);
            return PRTE_ERR_SILENT;
        }
        entry = fields[i + 1];
        rank_form = true;
        i = -1; /* nothing further on the line is ours */
    }

    if (valid_relative(entry)) {
        /* Store it for the caller to resolve against the node pool.  The
         * rest of the line still applies: a "+n<K> slots=2" is how an
         * allocation is subdivided, and the count has to reach the
         * placeholder or there is nothing for the caller to subdivide by. */
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(entry);
        pmix_list_append(updates, &node->super);
        goto options;
    }

    if (!valid_nodename(entry)) {
        hostfile_parse_error(entry);
        return PRTE_ERR_SILENT;
    }

    rc = hostfile_parse_username(entry, &username, &node_name);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* A leading "^" excludes the named host.  Remove it so the name is
     * usable, and put the node on the exclude list. */
    if ('^' == node_name[0]) {
        memmove(node_name, node_name + 1, strlen(node_name));
        if (prte_check_host_is_local(node_name)) {
            free(node_name);
            node_name = strdup(prte_process_info.nodename);
        }
        hostfile_exclude(node_name, username, exclude);
        free(node_name);
        free(username);
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((3, prte_ras_base_framework.framework_output,
                         "%s hostfile: node %s is being included - keep all is %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node_name,
                         keep_all ? "TRUE" : "FALSE"));

    /* see if this is another name for us */
    if (prte_check_host_is_local(node_name)) {
        free(node_name);
        node_name = strdup(prte_process_info.nodename);
    }

    if ((!rank_form && keep_all) || NULL == (node = prte_node_match(updates, node_name))) {
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(node_name);
        node->slots = 1;
        if (NULL != username) {
            prte_set_attribute(&node->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL, username,
                               PMIX_STRING);
        }
        pmix_list_append(updates, &node->super);
    } else {
        /* this node was already found once - add a slot and mark slots as "given" */
        node->slots++;
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
        if (0 != strcmp(node_name, node->name)) {
            PMIx_Argv_append_unique_nosize(&node->aliases, node_name);
        }
    }
    free(node_name);
    free(username);
    username = NULL;

    if (rank_form) {
        /* the slot count is the one the rankfile implied, so say it was
         * given and leave the rest of the line alone */
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
        PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                             "%s hostfile: node %s slots %d nodes-given TRUE",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name, node->slots));
        return PRTE_SUCCESS;
    }

options:
    /* the rest of the line is "<key> = <value>" groups */
    while (0 <= i && NULL != fields[i]) {
        key = hostfile_keyword(fields[i]);
        if (HKEY_UNKNOWN == key || NULL == fields[i + 1] || 0 != strcmp("=", fields[i + 1])
            || NULL == fields[i + 2]) {
            hostfile_parse_error(fields[i]);
            goto refuse;
        }

        switch (key) {
        case HKEY_USERNAME:
            prte_set_attribute(&node->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL,
                               fields[i + 2], PMIX_STRING);
            break;

        case HKEY_PORT:
            if (PRTE_SUCCESS != parse_count(fields[i + 2], &count)) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "port", true,
                               cur_hostfile_name, fields[i + 2]);
                goto refuse;
            }
            prte_set_attribute(&node->attributes, PRTE_NODE_PORT, PRTE_ATTR_LOCAL, &count,
                               PMIX_INT);
            break;

        case HKEY_SLOTS:
            if (PRTE_SUCCESS != parse_count(fields[i + 2], &count)) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "slots", true,
                               cur_hostfile_name, fields[i + 2]);
                goto refuse;
            }
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
                /* multiple definitions were given for the slot count */
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "slots-given", true,
                               cur_hostfile_name, node->name);
                goto refuse;
            }
            node->slots = count;
            PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
            /* ensure that slots_max >= slots */
            if (0 != node->slots_max && node->slots_max < node->slots) {
                node->slots_max = node->slots;
            }
            break;

        case HKEY_SLOTS_MAX:
            if (PRTE_SUCCESS != parse_count(fields[i + 2], &count)) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "max_slots", true,
                               cur_hostfile_name, fields[i + 2]);
                goto refuse;
            }
            if (count < node->slots) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "max_slots_lt", true,
                               cur_hostfile_name, node->slots, count);
                goto refuse;
            }
            if (node->slots_max != count) {
                node->slots_max = count;
                got_max = true;
            }
            break;

        default:
            hostfile_parse_error(fields[i]);
            goto refuse;
        }
        i += 3;
    }

    if (got_max && !PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)) {
        node->slots = node->slots_max;
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
    }

    return PRTE_SUCCESS;

refuse:
    pmix_list_remove_item(updates, &node->super);
    PMIX_RELEASE(node);
    return PRTE_ERR_SILENT;
}

/**
 * Parse the specified file into a node list.
 */
static int hostfile_parse(const char *hostfile, pmix_list_t *updates, pmix_list_t *exclude,
                          bool keep_all)
{
    prte_textfile_t tf;
    char **fields;
    int rc;

    cur_hostfile_name = hostfile;
    cur_hostfile_line = 0;

    rc = prte_textfile_open(&tf, hostfile);
    if (PRTE_ERR_BAD_PARAM == rc) {
        /* the path exists but is not a regular file -- a directory, most
         * likely, which a user reaches by naming one ("--hostfile /tmp")
         * or through a path variable that expanded to one */
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "not-a-file", true,
                       hostfile);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }
    if (PRTE_SUCCESS != rc) {
        if (NULL == prte_default_hostfile || 0 != strcmp(prte_default_hostfile, hostfile)) {
            /* not the default hostfile, so not finding it is an error */
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "no-hostfile", true,
                           hostfile);
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }
        if (prte_default_hostfile_given) {
            /* it is the default hostfile, but it was asked for by name */
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "no-hostfile", true,
                           hostfile);
            rc = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }
        /* otherwise, not finding it is okay */
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    while (NULL != (fields = prte_textfile_next(&tf))) {
        cur_hostfile_line = tf.lineno;
        rc = hostfile_parse_line(fields, updates, exclude, keep_all);
        if (PRTE_SUCCESS != rc) {
            goto cleanup;
        }
    }
    rc = PRTE_SUCCESS;

cleanup:
    prte_textfile_close(&tf);
    cur_hostfile_name = NULL;
    cur_hostfile_line = 0;
    return rc;
}

/**
 * Parse the provided hostfile and add the nodes to the list.
 */

int prte_util_add_hostfile_nodes(pmix_list_t *nodes, char *hostfile)
{
    pmix_list_t exclude, adds;
    pmix_list_item_t *item;
    int rc, i;
    prte_node_t *nd, *node;
    bool found;

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s hostfile: checking hostfile %s for nodes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostfile));

    PMIX_CONSTRUCT(&exclude, pmix_list_t);
    PMIX_CONSTRUCT(&adds, pmix_list_t);

    /* parse the hostfile and add any new contents to the list */
    if (PRTE_SUCCESS != (rc = hostfile_parse(hostfile, &adds, &exclude, false))) {
        goto cleanup;
    }

    /* check for any relative node directives */
    PMIX_LIST_FOREACH(node, &adds, prte_node_t) {
        if ('+' == node->name[0]) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:relative-syntax", true, node->name);
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }
    }

    /* remove from the list of nodes those that are in the exclude list */
    while (NULL != (item = pmix_list_remove_first(&exclude))) {
        nd = (prte_node_t *) item;
        /* check for matches on nodes */
        PMIX_LIST_FOREACH(node, &adds, prte_node_t) {
            if (prte_nptr_match(nd, node)) {
                /* match - remove it */
                pmix_list_remove_item(&adds, &node->super);
                PMIX_RELEASE(node);
                break;
            }
        }
        PMIX_RELEASE(item);
    }

    /* transfer across all unique nodes */
    while (NULL != (item = pmix_list_remove_first(&adds))) {
        nd = (prte_node_t *) item;
        found = false;
        PMIX_LIST_FOREACH(node, nodes, prte_node_t) {
            if (prte_nptr_match(nd, node)) {
                found = true;
                break;
            }
        }
        if (found) {
            /* add this node name as alias */
            PMIx_Argv_append_unique_nosize(&node->aliases, nd->name);
            /* ensure all other aliases are also transferred */
            if (NULL != nd->aliases) {
                for (i=0; NULL != nd->aliases[i]; i++) {
                    PMIx_Argv_append_unique_nosize(&node->aliases, nd->aliases[i]);
                }
            }
           PMIX_RELEASE(item);
        } else {
            pmix_list_append(nodes, &nd->super);
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                 "%s hostfile: adding node %s slots %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nd->name, nd->slots));
        }
    }

cleanup:
    PMIX_LIST_DESTRUCT(&exclude);
    PMIX_LIST_DESTRUCT(&adds);

    return rc;
}

/* Parse the provided hostfile and filter the nodes that are
 * on the input list, removing those that
 * are not found in the hostfile
 */
int prte_util_filter_hostfile_nodes(pmix_list_t *nodes, char *hostfile, bool remove)
{
    pmix_list_t newnodes, exclude;
    pmix_list_item_t *item1, *item2, *next, *item3;
    prte_node_t *node_from_list, *node_from_file, *node_from_pool, *node3;
    int rc = PRTE_SUCCESS;
    char *cptr;
    int num_empty, nodeidx;
    bool want_all_empty = false;
    pmix_list_t keep;
    bool found;

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s hostfile: filtering nodes through hostfile %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostfile));

    /* parse the hostfile and create local list of findings */
    PMIX_CONSTRUCT(&newnodes, pmix_list_t);
    PMIX_CONSTRUCT(&exclude, pmix_list_t);
    if (PRTE_SUCCESS != (rc = hostfile_parse(hostfile, &newnodes, &exclude, false))) {
        PMIX_DESTRUCT(&newnodes);
        PMIX_DESTRUCT(&exclude);
        return rc;
    }

    /* if the hostfile was empty, then treat it as a no-op filter */
    if (0 == pmix_list_get_size(&newnodes)) {
        PMIX_DESTRUCT(&newnodes);
        PMIX_DESTRUCT(&exclude);
        /* indicate that the hostfile was empty */
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* remove from the list of newnodes those that are in the exclude list
     * since we could have added duplicate names above due to the */
    while (NULL != (item1 = pmix_list_remove_first(&exclude))) {
        node_from_file = (prte_node_t *) item1;
        /* check for matches on nodes */
        for (item2 = pmix_list_get_first(&newnodes); item2 != pmix_list_get_end(&newnodes);
             item2 = pmix_list_get_next(item2)) {
            prte_node_t *node = (prte_node_t *) item2;
            if (prte_nptr_match(node_from_file, node)) {
                /* match - remove it */
                pmix_list_remove_item(&newnodes, item2);
                PMIX_RELEASE(item2);
                break;
            }
        }
        PMIX_RELEASE(item1);
    }

    /* now check our nodes and keep or mark those that match. We can
     * destruct our hostfile list as we go since this won't be needed
     */
    PMIX_CONSTRUCT(&keep, pmix_list_t);
    while (NULL != (item2 = pmix_list_remove_first(&newnodes))) {
        node_from_file = (prte_node_t *) item2;

        next = pmix_list_get_next(item2);

        /* see if this is a relative node syntax */
        if ('+' == node_from_file->name[0]) {
            /* see if we specified empty nodes */
            if ('e' == node_from_file->name[1] || 'E' == node_from_file->name[1]) {
                /* request for empty nodes - do they want
                 * all of them?
                 */
                if (NULL != (cptr = strchr(node_from_file->name, ':'))) {
                    /* the colon indicates a specific # are requested */
                    cptr++; /* step past : */
                    num_empty = strtol(cptr, NULL, 10);
                } else {
                    /* want them all - set num_empty to max */
                    num_empty = INT_MAX;
                    want_all_empty = true;
                }
                /* search the list of nodes provided to us and find those
                 * that are empty
                 */
                item1 = pmix_list_get_first(nodes);
                while (0 < num_empty && item1 != pmix_list_get_end(nodes)) {
                    node_from_list = (prte_node_t *) item1;
                    next = pmix_list_get_next(item1); /* keep our place */
                    if (0 == node_from_list->slots_inuse) {
                        /* check to see if this node is explicitly called
                         * out later - if so, don't use it here
                         */
                        for (item3 = pmix_list_get_first(&newnodes);
                             item3 != pmix_list_get_end(&newnodes);
                             item3 = pmix_list_get_next(item3)) {
                            node3 = (prte_node_t *) item3;
                            if (prte_nptr_match(node3, node_from_list)) {
                                /* match - don't use it */
                                goto skipnode;
                            }
                        }
                        if (remove) {
                            /* remove item from list */
                            pmix_list_remove_item(nodes, item1);
                            /* xfer to keep list */
                            pmix_list_append(&keep, item1);
                        } else {
                            /* mark as included */
                            PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                        }
                        --num_empty;
                    }
                skipnode:
                    item1 = next;
                }
                /* did they get everything they wanted? */
                if (!want_all_empty && 0 < num_empty) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:not-enough-empty", true,
                                   num_empty);
                    rc = PRTE_ERR_SILENT;
                    goto cleanup;
                }
            } else if ('n' == node_from_file->name[1] || 'N' == node_from_file->name[1]) {
                /* they want a specific relative node #, so
                 * look it up on global pool
                 */
                nodeidx = strtol(&node_from_file->name[2], NULL, 10);
                /* "+n#" indexes the ALLOCATION from zero. The head node
                 * always occupies pool slot 0, so when it is not part of
                 * the allocation the pool is offset by one and the index
                 * has to be adjusted - otherwise "+n0" in a hostfile names
                 * a node the job was never given, and every index is one
                 * node adrift of the same index given to --host.
                 * prte_util_get_ordered_host_list() and dash-host's
                 * parse_dash_host() both make this adjustment. */
                if (!prte_hnp_is_allocated) {
                    nodeidx++;
                }
                node_from_pool = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, nodeidx);
                if (NULL == node_from_pool) {
                    /* this is an error */
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:relative-node-not-found", true,
                                   nodeidx, node_from_file->name);
                    rc = PRTE_ERR_SILENT;
                    goto cleanup;
                }
                /* search the list of nodes provided to us and find it */
                for (item1 = pmix_list_get_first(nodes); item1 != pmix_list_get_end(nodes);
                     item1 = pmix_list_get_next(item1)) {
                    node_from_list = (prte_node_t *) item1;
                    if (prte_nptr_match(node_from_pool, node_from_list)) {
                        if (remove) {
                            /* match - remove item from list */
                            pmix_list_remove_item(nodes, item1);
                            /* xfer to keep list */
                            pmix_list_append(&keep, item1);
                        } else {
                            /* mark as included */
                            PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                        }
                        break;
                    }
                }
            } else {
                /* invalid relative node syntax */
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:invalid-relative-node-syntax", true,
                               node_from_file->name);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            }
        } else {
            /* we are looking for a specific node on the list
             * search the provided list of nodes to see if this
             * one is found
             */
            found = false;
            for (item1 = pmix_list_get_first(nodes); item1 != pmix_list_get_end(nodes);
                 item1 = pmix_list_get_next(item1)) {
                node_from_list = (prte_node_t *) item1;
                /* we have converted all aliases for ourself
                 * to our own detected nodename */
                if (prte_nptr_match(node_from_file, node_from_list)) {
                    /* if the slot count here is less than the
                     * total slots avail on this node, set it
                     * to the specified count - this allows people
                     * to subdivide an allocation.
                     *
                     * The nodes on this list are the pool's own objects, so
                     * the smaller count has to be handed back when the map is
                     * done: the "slots=" says how many slots THIS job may
                     * have on the node, not how big the node is, exactly as a
                     * "-host node:N" does. Left unrecorded, one job's hostfile
                     * shrank the node for every job the DVM ran afterwards -
                     * jobs that never named the hostfile - and the allocation
                     * could only ever get smaller, with nothing short of
                     * restarting the DVM to put it back.
                     *
                     * Only do this when we are selecting the nodes a job will
                     * map onto ("remove"), because that is the one caller
                     * running inside prte_rmaps_base_map_job(), which restores
                     * what it recorded before it returns. The record list is a
                     * framework global, not a per-job one, and a DVM maps one
                     * job while another is still forming its daemons - so an
                     * entry made anywhere else is one some unrelated job's map
                     * would put back. The other caller, the VM setup, is only
                     * marking which nodes are to host a daemon; it never maps
                     * and reads no slot count, so it has nothing to resize for.
                     */
                    if (remove
                        && PRTE_FLAG_TEST(node_from_file, PRTE_NODE_FLAG_SLOTS_GIVEN)
                        && node_from_file->slots < node_from_list->slots) {
                        prte_rmaps_base_record_resize(node_from_list, node_from_list->slots);
                        node_from_list->slots = node_from_file->slots;
                    }
                    if (remove) {
                        /* remove the node from the list */
                        pmix_list_remove_item(nodes, item1);
                        /* xfer it to keep list */
                        pmix_list_append(&keep, item1);
                    } else {
                        /* mark as included */
                        PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                    }
                    found = true;
                    break;
                }
            }
            /* if the host in the newnode list wasn't found,
             * then that is an error we need to report to the
             * user and abort
             */
            if (!found) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:extra-node-not-found", true, hostfile,
                               node_from_file->name);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            }
        }
        /* cleanup the newnode list */
        PMIX_RELEASE(item2);
    }

    /* if we still have entries on our hostfile list, then
     * there were requested hosts that were not in our allocation.
     * This is an error - report it to the user and return an error
     */
    if (0 != pmix_list_get_size(&newnodes)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "not-all-mapped-alloc", true, hostfile);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }

    if (!remove) {
        /* all done */
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    /* clear the rest of the nodes list */
    while (NULL != (item1 = pmix_list_remove_first(nodes))) {
        PMIX_RELEASE(item1);
    }

    /* the nodes list has been cleared - rebuild it in order */
    while (NULL != (item1 = pmix_list_remove_first(&keep))) {
        pmix_list_append(nodes, item1);
    }

cleanup:
    /* "keep" holds nodes this routine took *off* the caller's list, so on
     * any path that does not put them back they are the caller's nodes being
     * dropped on the floor - every error return used to leak them, along
     * with the two lists themselves */
    PMIX_LIST_DESTRUCT(&keep);
    PMIX_LIST_DESTRUCT(&newnodes);
    PMIX_LIST_DESTRUCT(&exclude);

    return rc;
}

int prte_util_get_ordered_host_list(pmix_list_t *nodes, char *hostfile)
{
    pmix_list_t exclude;
    pmix_list_item_t *item, *itm, *item2, *item1;
    char *cptr;
    int num_empty, i, nodeidx, startempty = 0;
    bool want_all_empty = false;
    prte_node_t *node_from_pool, *newnode;
    int rc;

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s hostfile: creating ordered list of hosts from hostfile %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostfile));

    PMIX_CONSTRUCT(&exclude, pmix_list_t);

    /* parse the hostfile and add the contents to the list, keeping duplicates */
    if (PRTE_SUCCESS != (rc = hostfile_parse(hostfile, nodes, &exclude, true))) {
        goto cleanup;
    }

    /* parse the nodes to process any relative node directives */
    item2 = pmix_list_get_first(nodes);
    while (item2 != pmix_list_get_end(nodes)) {
        prte_node_t *node = (prte_node_t *) item2;

        /* save the next location in case this one gets removed */
        item1 = pmix_list_get_next(item2);

        if ('+' != node->name[0]) {
            item2 = item1;
            continue;
        }

        /* see if we specified empty nodes */
        if ('e' == node->name[1] || 'E' == node->name[1]) {
            /* request for empty nodes - do they want
             * all of them?
             */
            if (NULL != (cptr = strchr(node->name, ':'))) {
                /* the colon indicates a specific # are requested */
                cptr++; /* step past : */
                num_empty = strtol(cptr, NULL, 10);
            } else {
                /* want them all - set num_empty to max */
                num_empty = INT_MAX;
                want_all_empty = true;
            }
            /* insert empty nodes into newnodes list in place of the current item.
             * since item1 is the next item, we insert in front of it
             */
            if (!prte_hnp_is_allocated && 0 == startempty) {
                startempty = 1;
            }
            for (i = startempty; 0 < num_empty && i < prte_node_pool->size; i++) {
                node_from_pool = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
                if (NULL == node_from_pool) {
                    continue;
                }
                if (0 == node_from_pool->slots_inuse) {
                    newnode = PMIX_NEW(prte_node_t);
                    newnode->name = strdup(node_from_pool->name);
                    /* if the slot count here is less than the
                     * total slots avail on this node, set it
                     * to the specified count - this allows people
                     * to subdivide an allocation.
                     *
                     * Only if the hostfile actually gave a count, though: a
                     * bare "+e" is a placeholder node whose slots are still
                     * the constructor's zero, and taking that as "subdivide
                     * to zero slots" handed back nodes with no slots at all.
                     */
                    if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)
                        && node->slots < node_from_pool->slots) {
                        newnode->slots = node->slots;
                    } else {
                        newnode->slots = node_from_pool->slots;
                    }
                    PRTE_FLAG_SET(newnode, PRTE_NODE_FLAG_SLOTS_GIVEN);
                    pmix_list_insert_pos(nodes, item1, &newnode->super);
                    /* track number added */
                    --num_empty;
                }
            }
            /* bookmark where we stopped in case they ask for more */
            startempty = i;
            /* did they get everything they wanted? */
            if (!want_all_empty && 0 < num_empty) {
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:not-enough-empty", true, num_empty);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            }
            /* since we have expanded the provided node, remove
             * it from list
             */
            pmix_list_remove_item(nodes, item2);
            PMIX_RELEASE(item2);
        } else if ('n' == node->name[1] || 'N' == node->name[1]) {
            /* they want a specific relative node #, so
             * look it up on global pool
             */
            nodeidx = strtol(&node->name[2], NULL, 10);
            /* if the HNP is not allocated, then we need to
             * adjust the index as the node pool is offset
             * by one
             */
            if (!prte_hnp_is_allocated) {
                nodeidx++;
            }
            /* see if that location is filled */
            node_from_pool = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, nodeidx);
            if (NULL == node_from_pool) {
                /* this is an error */
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:relative-node-not-found", true,
                               nodeidx, node->name);
                rc = PRTE_ERR_SILENT;
                goto cleanup;
            }
            /* create the node object */
            newnode = PMIX_NEW(prte_node_t);
            newnode->name = strdup(node_from_pool->name);
            /* if the slot count here is less than the
             * total slots avail on this node, set it
             * to the specified count - this allows people
             * to subdivide an allocation. As with "+e" above, a bare
             * "+n<K>" carries no count of its own, so only honor one that
             * the hostfile actually gave.
             */
            if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_SLOTS_GIVEN)
                && node->slots < node_from_pool->slots) {
                newnode->slots = node->slots;
            } else {
                newnode->slots = node_from_pool->slots;
            }
            PRTE_FLAG_SET(newnode, PRTE_NODE_FLAG_SLOTS_GIVEN);
            /* insert it before item1 */
            pmix_list_insert_pos(nodes, item1, &newnode->super);
            /* since we have expanded the provided node, remove
             * it from list
             */
            pmix_list_remove_item(nodes, item2);
            PMIX_RELEASE(item2);
        } else {
            /* invalid relative node syntax */
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:invalid-relative-node-syntax", true,
                           node->name);
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }

        /* move to next */
        item2 = item1;
    }

    /* remove from the list of nodes those that are in the exclude list.
     * This list keeps duplicates, so every match has to be removed - which
     * means the successor has to be saved before the item is released, not
     * read out of it afterwards. */
    while (NULL != (item = pmix_list_remove_first(&exclude))) {
        prte_node_t *exnode = (prte_node_t *) item;
        /* check for matches on nodes */
        itm = pmix_list_get_first(nodes);
        while (itm != pmix_list_get_end(nodes)) {
            prte_node_t *node = (prte_node_t *) itm;
            pmix_list_item_t *nxt = pmix_list_get_next(itm);
            if (prte_nptr_match(exnode, node)) {
                /* match - remove it */
                pmix_list_remove_item(nodes, itm);
                PMIX_RELEASE(itm);
            }
            itm = nxt;
        }
        PMIX_RELEASE(item);
    }

cleanup:
    PMIX_DESTRUCT(&exclude);

    return rc;
}
