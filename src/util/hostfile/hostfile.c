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
#include "src/runtime/prte_node_index.h"
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
 * "+n<N>" names a node by its position in the pool and "+e", or "+e:<N>",
 * asks for empty ones.  Both letters are taken in either case: the lexer
 * accepted "+E" but only lower-case "+n", so the parser's own test for
 * 'N' could never fire, and there was no reason for the two forms to
 * differ.
 */
static bool valid_relative(const char *s)
{
    size_t i;
    int v;

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
        /* the resolvers read the index into an int and may add one to it
         * to step over the head node's pool slot */
        return (PRTE_SUCCESS == parse_count(s + 2, &v) && INT_MAX > v);
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
        /* a count too large for an int came back from strtol() truncated,
         * possibly negative, which asked for no nodes and got none */
        return (PRTE_SUCCESS == parse_count(s + 3, &v));
    }
    return false;
}

/*
 * Split a host entry into its optional username and the node name.
 *
 * A name may carry the account to reach it with, written in front and
 * separated by a single "@".  Anything else with an "@" in it is neither a
 * hostname nor a "user@hostname", and has to be refused by name, file and
 * line like every other failure here.
 *
 * Do not reach for PMIx_Argv_split() to do this.  It drops empty fields, so
 * "someone@" is one field and "@hostA" and "someone@@hostA" are two, and
 * all three were accepted: the first as a node named "someone", which a
 * launcher then tries to reach.
 */
static int hostfile_parse_username(const char *value, char **username, char **node_name)
{
    const char *at;
    size_t len;

    at = strchr(value, '@');
    if (NULL == at) {
        *node_name = strdup(value);
        return PRTE_SUCCESS;
    }
    if (at == value || '\0' == at[1] || NULL != strchr(at + 1, '@')) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "user-host", true,
                       cur_hostfile_name, cur_hostfile_line, value);
        return PRTE_ERR_SILENT;
    }
    len = (size_t) (at - value);
    *username = (char *) malloc(len + 1);
    memcpy(*username, value, len);
    (*username)[len] = '\0';
    *node_name = strdup(at + 1);
    return PRTE_SUCCESS;
}

/* Put a node on the exclude list, or note another name for one already
 * there.  The leading "^" has been stripped by the caller. */
static void hostfile_exclude(const char *node_name, const char *username, pmix_list_t *exclude,
                             prte_node_index_t *eidx)
{
    prte_node_t *node;

    PMIX_OUTPUT_VERBOSE((3, prte_ras_base_framework.framework_output,
                         "%s hostfile: node %s is being excluded",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node_name));

    node = prte_node_index_match_name(eidx, node_name);
    if (NULL == node) {
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(node_name);
        if (NULL != username) {
            prte_set_attribute(&node->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL,
                               (void *) username, PMIX_STRING);
        }
        pmix_list_append(exclude, &node->super);
        prte_node_index_append(eidx, node);
    } else if (0 != strcmp(node_name, node->name)) {
        /* the node name may not match the prior entry, so keep it */
        PMIx_Argv_append_unique_nosize(&node->aliases, (char *) node_name);
        prte_node_index_add_alias(eidx, node, node_name);
    }
}

/*
 * Parse one line: a host entry, optionally followed by "<key> = <value>"
 * groups.  Also accepts a rankfile's "rank <N>=<host> ..." line, from
 * which it takes only the host.
 */
static int hostfile_parse_line(char **fields, pmix_list_t *updates, pmix_list_t *exclude,
                               prte_node_index_t *uidx, prte_node_index_t *eidx)
{
    prte_node_t *node;
    bool got_max = false;
    bool rank_form = false;
    bool excluded;
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
        /* Store it for the filter to resolve against the node pool.  The
         * rest of the line still applies: a "+n<K> slots=2" subdivides the
         * node it resolves to just as "hostA slots=2" does, and the count
         * has to reach the placeholder or there is nothing to subdivide by. */
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(entry);
        pmix_list_append(updates, &node->super);
        prte_node_index_append(uidx, node);
        goto options;
    }

    if (!valid_nodename(entry)) {
        hostfile_parse_error(entry);
        return PRTE_ERR_SILENT;
    }

    /* A leading "^" excludes the named host, and it leads the whole entry:
     * "^user@host" excludes host.  Take it off before the user is split
     * away, not after -- looking for it on the node name found it only when
     * there was no user, and otherwise gave the user a name starting with
     * "^" and counted the host as one more slot of a node to use. */
    excluded = ('^' == entry[0]);
    rc = hostfile_parse_username(excluded ? entry + 1 : entry, &username, &node_name);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    if (excluded) {
        if (prte_check_host_is_local(node_name)) {
            free(node_name);
            node_name = strdup(prte_process_info.nodename);
        }
        hostfile_exclude(node_name, username, exclude, eidx);
        free(node_name);
        free(username);
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((3, prte_ras_base_framework.framework_output,
                         "%s hostfile: node %s is being included",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node_name));

    /* see if this is another name for us */
    if (prte_check_host_is_local(node_name)) {
        free(node_name);
        node_name = strdup(prte_process_info.nodename);
    }

    if (NULL == (node = prte_node_index_match_name(uidx, node_name))) {
        node = PMIX_NEW(prte_node_t);
        node->name = strdup(node_name);
        node->slots = 1;
        if (NULL != username) {
            prte_set_attribute(&node->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL, username,
                               PMIX_STRING);
        }
        pmix_list_append(updates, &node->super);
        prte_node_index_append(uidx, node);
    } else {
        /* this node was already found once - add a slot and mark slots as "given".
         * A count already at INT_MAX (slots=2147483647) stays there */
        if (INT_MAX > node->slots) {
            node->slots++;
        }
        PRTE_FLAG_SET(node, PRTE_NODE_FLAG_SLOTS_GIVEN);
        if (0 != strcmp(node_name, node->name)) {
            PMIx_Argv_append_unique_nosize(&node->aliases, node_name);
            prte_node_index_add_alias(uidx, node, node_name);
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
    prte_node_index_remove(uidx, node);
    pmix_list_remove_item(updates, &node->super);
    PMIX_RELEASE(node);
    return PRTE_ERR_SILENT;
}

/**
 * Parse the specified file into a node list.
 */
static int hostfile_parse(const char *hostfile, pmix_list_t *updates, pmix_list_t *exclude,
                          uid_t uid, gid_t gid)
{
    prte_textfile_t tf;
    prte_node_index_t uidx, eidx;
    char **fields;
    int rc;

    cur_hostfile_name = hostfile;
    cur_hostfile_line = 0;

    /* each line is looked up against the lines before it, so look them up
     * in an index rather than by walking the lists: a walk per line made
     * reading the file quadratic in its length */
    prte_node_index_construct(&uidx);
    prte_node_index_construct(&eidx);
    if (PRTE_SUCCESS != (rc = prte_node_index_add_list(&uidx, updates)) ||
        PRTE_SUCCESS != (rc = prte_node_index_add_list(&eidx, exclude))) {
        prte_node_index_destruct(&uidx);
        prte_node_index_destruct(&eidx);
        return rc;
    }

    rc = prte_textfile_open(&tf, hostfile, uid, gid);
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
            rc = PRTE_ERR_SILENT;
            goto cleanup;
        }
        /* otherwise, not finding it is okay */
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    while (NULL != (fields = prte_textfile_next(&tf))) {
        cur_hostfile_line = tf.lineno;
        rc = hostfile_parse_line(fields, updates, exclude, &uidx, &eidx);
        if (PRTE_SUCCESS != rc) {
            goto cleanup;
        }
    }
    if (tf.failed) {
        /* the nodes read so far stay on the caller's lists, and the caller
         * disposes of them as it does after any other refusal */
        if (!prte_textfile_report_limit(&tf, PRTE_PROC_MY_NAME->nspace, hostfile)) {
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "read-error", true,
                           hostfile, tf.lineno);
        }
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }
    rc = PRTE_SUCCESS;

cleanup:
    prte_node_index_destruct(&uidx);
    prte_node_index_destruct(&eidx);
    prte_textfile_close(&tf);
    cur_hostfile_name = NULL;
    cur_hostfile_line = 0;
    return rc;
}

/**
 * Parse the provided hostfile and add the nodes to the list.
 */

int prte_util_add_hostfile_nodes(pmix_list_t *nodes, char *hostfile, uid_t uid, gid_t gid)
{
    pmix_list_t exclude, adds;
    pmix_list_item_t *item;
    prte_node_index_t aidx, nidx;
    int rc, i;
    prte_node_t *nd, *node;

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s hostfile: checking hostfile %s for nodes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostfile));

    PMIX_CONSTRUCT(&exclude, pmix_list_t);
    PMIX_CONSTRUCT(&adds, pmix_list_t);
    prte_node_index_construct(&aidx);
    prte_node_index_construct(&nidx);

    /* parse the hostfile and add any new contents to the list */
    if (PRTE_SUCCESS != (rc = hostfile_parse(hostfile, &adds, &exclude, uid, gid))) {
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

    /* remove from the list of nodes those that are in the exclude list -
     * each the first node on the list that it matches, found in an index
     * of the list rather than by walking it once per exclusion */
    if (PRTE_SUCCESS != (rc = prte_node_index_add_list(&aidx, &adds))) {
        goto cleanup;
    }
    while (NULL != (item = pmix_list_remove_first(&exclude))) {
        nd = (prte_node_t *) item;
        if (NULL != (node = prte_node_index_match_node(&aidx, nd))) {
            /* match - remove it */
            prte_node_index_remove(&aidx, node);
            pmix_list_remove_item(&adds, &node->super);
            PMIX_RELEASE(node);
        }
        PMIX_RELEASE(item);
    }

    /* transfer across all unique nodes - the caller's list grows as we
     * go, and the index grows with it */
    if (PRTE_SUCCESS != (rc = prte_node_index_add_list(&nidx, nodes))) {
        goto cleanup;
    }
    while (NULL != (item = pmix_list_remove_first(&adds))) {
        nd = (prte_node_t *) item;
        prte_node_index_remove(&aidx, nd);
        if (NULL != (node = prte_node_index_match_node(&nidx, nd))) {
            /* add this node name as alias */
            PMIx_Argv_append_unique_nosize(&node->aliases, nd->name);
            prte_node_index_add_alias(&nidx, node, nd->name);
            /* ensure all other aliases are also transferred */
            if (NULL != nd->aliases) {
                for (i=0; NULL != nd->aliases[i]; i++) {
                    PMIx_Argv_append_unique_nosize(&node->aliases, nd->aliases[i]);
                    prte_node_index_add_alias(&nidx, node, nd->aliases[i]);
                }
            }
           PMIX_RELEASE(item);
        } else {
            pmix_list_append(nodes, &nd->super);
            prte_node_index_append(&nidx, nd);
            PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                                 "%s hostfile: adding node %s slots %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nd->name, nd->slots));
        }
    }

cleanup:
    prte_node_index_destruct(&aidx);
    prte_node_index_destruct(&nidx);
    PMIX_LIST_DESTRUCT(&exclude);
    PMIX_LIST_DESTRUCT(&adds);

    return rc;
}

/*
 * Cap a node the filter selected to the count its hostfile entry gave, for
 * this job only.  The entry may have named the node outright or by position
 * ("+n<K>", "+e"); a count means the same thing either way, and the
 * positional forms used to have theirs dropped.
 *
 * If the slot count here is less than the total slots avail on this node,
 * set it to the specified count - this allows people to subdivide an
 * allocation.
 *
 * The nodes on the caller's list are the pool's own objects, so the smaller
 * count has to be handed back when the map is done: the "slots=" says how
 * many slots THIS job may have on the node, not how big the node is,
 * exactly as a "-host node:N" does. Left unrecorded, one job's hostfile
 * shrank the node for every job the DVM ran afterwards - jobs that never
 * named the hostfile - and the allocation could only ever get smaller, with
 * nothing short of restarting the DVM to put it back.
 *
 * Only do this when we are selecting the nodes a job will map onto
 * ("remove"), because that is the one caller running inside
 * prte_rmaps_base_map_job(), which restores what it recorded before it
 * returns. The record list is a framework global, not a per-job one, and a
 * DVM maps one job while another is still forming its daemons - so an entry
 * made anywhere else is one some unrelated job's map would put back. The
 * other caller, the VM setup, is only marking which nodes are to host a
 * daemon; it never maps and reads no slot count, so it has nothing to resize
 * for.
 */
static void hostfile_cap_for_job(prte_node_t *node_from_file, prte_node_t *node_from_list,
                                 bool remove)
{
    if (remove && PRTE_FLAG_TEST(node_from_file, PRTE_NODE_FLAG_SLOTS_GIVEN)
        && node_from_file->slots < node_from_list->slots) {
        prte_rmaps_base_record_resize(node_from_list, node_from_list->slots);
        node_from_list->slots = node_from_file->slots;
    }
}

/* A node taken off the caller's list is no longer one "+e" may take. */
static void hostfile_drop_empty(pmix_hash_table_t *where, pmix_pointer_array_t *empties,
                                prte_node_t *node)
{
    void *pos = NULL;

    if (PMIX_SUCCESS == pmix_hash_table_get_value_uint64(where, (uint64_t) (uintptr_t) node,
                                                         &pos) && NULL != pos) {
        pmix_pointer_array_set_item(empties, (int) ((intptr_t) pos - 1), NULL);
    }
}

/* Parse the provided hostfile and filter the nodes that are
 * on the input list, removing those that
 * are not found in the hostfile
 */
int prte_util_filter_hostfile_nodes(pmix_list_t *nodes, char *hostfile, bool remove, uid_t uid,
                                    gid_t gid)
{
    pmix_list_t newnodes, exclude;
    pmix_list_item_t *item1, *item2 = NULL, *next;
    prte_node_t *node_from_list, *node_from_file, *node_from_pool, *node3;
    int rc = PRTE_SUCCESS;
    char *cptr;
    int num_empty, nodeidx;
    bool want_all_empty = false;
    pmix_list_t keep;
    bool found;
    /* The lists are looked up once per entry of the file, so each is
     * indexed (prte_node_index_t) rather than walked: walking the caller's
     * list for every entry made the filter quadratic. nidx is the caller's
     * list, fidx the file's entries still to be resolved, xidx its
     * exclusions. */
    prte_node_index_t nidx, fidx, xidx;
    /* the caller's empty nodes in list order, for "+e"; see below */
    pmix_pointer_array_t empties;
    pmix_hash_table_t where;   /* each empty node's place in "empties", plus one */
    int next_empty = 0, e;
    bool all_empty_done = false;

    PMIX_OUTPUT_VERBOSE((1, prte_ras_base_framework.framework_output,
                         "%s hostfile: filtering nodes through hostfile %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), hostfile));

    /* parse the hostfile and create local list of findings.  These lists
     * hold nodes, so they go through PMIX_LIST_DESTRUCT on every exit: a
     * plain PMIX_DESTRUCT releases none of the items, and a hostfile that
     * failed on its tenth line leaked the nine nodes before it, per job. */
    PMIX_CONSTRUCT(&newnodes, pmix_list_t);
    PMIX_CONSTRUCT(&exclude, pmix_list_t);
    PMIX_CONSTRUCT(&keep, pmix_list_t);
    PMIX_CONSTRUCT(&empties, pmix_pointer_array_t);
    pmix_pointer_array_init(&empties, 8, INT_MAX, 8);
    PMIX_CONSTRUCT(&where, pmix_hash_table_t);
    pmix_hash_table_init(&where, 1024);
    prte_node_index_construct(&nidx);
    prte_node_index_construct(&fidx);
    prte_node_index_construct(&xidx);
    if (PRTE_SUCCESS != (rc = hostfile_parse(hostfile, &newnodes, &exclude, uid, gid))) {
        goto cleanup;
    }
    if (PRTE_SUCCESS != (rc = prte_node_index_add_list(&nidx, nodes)) ||
        PRTE_SUCCESS != (rc = prte_node_index_add_list(&fidx, &newnodes)) ||
        PRTE_SUCCESS != (rc = prte_node_index_add_list(&xidx, &exclude))) {
        goto cleanup;
    }

    if (0 == pmix_list_get_size(&newnodes)) {
        if (0 == pmix_list_get_size(&exclude)) {
            /* the hostfile was empty: treat it as a no-op filter */
            rc = PRTE_ERR_TAKE_NEXT_OPTION;
            goto cleanup;
        }
        /*
         * The hostfile only excludes.  It names no node to select, but it
         * is not empty and must not be taken for empty: that answer mapped
         * the job across every node, the excluded ones included.  What a
         * file of nothing but "^host" lines says is "any node but these",
         * so every node on the caller's list is selected except the ones it
         * excludes.  (Where the file also names nodes, an exclusion takes a
         * node back out of what it named, below.)
         */
        item1 = pmix_list_get_first(nodes);
        while (item1 != pmix_list_get_end(nodes)) {
            node_from_list = (prte_node_t *) item1;
            next = pmix_list_get_next(item1);
            found = (NULL != prte_node_index_match_node(&xidx, node_from_list));
            if (found) {
                if (remove) {
                    pmix_list_remove_item(nodes, item1);
                    PMIX_RELEASE(item1);
                }
            } else if (!remove) {
                PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
            }
            item1 = next;
        }
        rc = PRTE_SUCCESS;
        goto cleanup;
    }

    /* remove from the list of newnodes those that are in the exclude list
     * since we could have added duplicate names above due to the */
    while (NULL != (item1 = pmix_list_remove_first(&exclude))) {
        node_from_file = (prte_node_t *) item1;
        prte_node_index_remove(&xidx, node_from_file);
        /* the first entry it matches, as the walk this replaces found */
        if (NULL != (node3 = prte_node_index_match_node(&fidx, node_from_file))) {
            /* match - remove it */
            prte_node_index_remove(&fidx, node3);
            pmix_list_remove_item(&newnodes, &node3->super);
            PMIX_RELEASE(node3);
        }
        PMIX_RELEASE(item1);
    }
    item2 = NULL;

    /* "+e" takes the caller's empty nodes in list order. Gather them once:
     * scanning the whole list for each "+e" entry made a file of them
     * quadratic. slots_inuse does not change while we filter. */
    PMIX_LIST_FOREACH(node_from_list, nodes, prte_node_t) {
        if (0 == node_from_list->slots_inuse) {
            e = pmix_pointer_array_add(&empties, node_from_list);
            if (0 <= e) {
                pmix_hash_table_set_value_uint64(&where, (uint64_t) (uintptr_t) node_from_list,
                                                 (void *) (intptr_t) (e + 1));
            }
        }
    }

    /* now check our nodes and keep or mark those that match. We can
     * destruct our hostfile list as we go since this won't be needed
     */
    while (NULL != (item2 = pmix_list_remove_first(&newnodes))) {
        node_from_file = (prte_node_t *) item2;
        /* off the list, so no longer "called out later" by anything below */
        prte_node_index_remove(&fidx, node_from_file);

        /* see if this is a relative node syntax */
        if ('+' == node_from_file->name[0]) {
            /* see if we specified empty nodes */
            if ('e' == node_from_file->name[1] || 'E' == node_from_file->name[1]) {
                /* request for empty nodes - do they want
                 * all of them?
                 */
                if (NULL != (cptr = strchr(node_from_file->name, ':'))) {
                    /* the colon indicates a specific # are requested - and
                     * says so for this line, whatever an earlier "+e" asked */
                    cptr++; /* step past : */
                    num_empty = strtol(cptr, NULL, 10);
                    want_all_empty = false;
                } else {
                    /* want them all - set num_empty to max */
                    num_empty = INT_MAX;
                    want_all_empty = true;
                }
                /* A request for every empty node that follows one already
                 * served finds nothing it did not: whatever the first left
                 * behind was a node named later in the file, which is
                 * either still named later or has been resolved since. */
                if (want_all_empty && all_empty_done) {
                    num_empty = 0;
                }
                /* search the empty nodes provided to us, in list order.
                 * When selecting, a node taken - by "+e" or by name - has
                 * left the list and its place here is emptied, so a later
                 * "+e" may start past the taken ones at the front. One
                 * passed over because it is named later stays, and is
                 * looked at again. When only marking, nodes stay on the
                 * list and every "+e" looks at all of them, as before. */
                for (e = remove ? next_empty : 0; 0 < num_empty && e < empties.size; e++) {
                    node_from_list = (prte_node_t *) pmix_pointer_array_get_item(&empties, e);
                    if (NULL == node_from_list) {
                        continue;
                    }
                    /* check to see if this node is explicitly called
                     * out later - if so, don't use it here
                     */
                    if (NULL != prte_node_index_match_node(&fidx, node_from_list)) {
                        continue;
                    }
                    hostfile_cap_for_job(node_from_file, node_from_list, remove);
                    if (remove) {
                        /* remove item from list */
                        prte_node_index_remove(&nidx, node_from_list);
                        pmix_list_remove_item(nodes, &node_from_list->super);
                        /* xfer to keep list */
                        pmix_list_append(&keep, &node_from_list->super);
                        pmix_pointer_array_set_item(&empties, e, NULL);
                    } else {
                        /* mark as included */
                        PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                    }
                    --num_empty;
                }
                while (remove && next_empty < empties.size &&
                       NULL == pmix_pointer_array_get_item(&empties, next_empty)) {
                    next_empty++;
                }
                if (want_all_empty) {
                    all_empty_done = true;
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
                 * dash-host's parse_dash_host() makes the same
                 * adjustment. */
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
                /* find it on the list of nodes provided to us */
                found = false;
                if (NULL != (node_from_list = prte_node_index_match_node(&nidx, node_from_pool))) {
                    hostfile_cap_for_job(node_from_file, node_from_list, remove);
                    if (remove) {
                        /* match - remove item from list */
                        hostfile_drop_empty(&where, &empties, node_from_list);
                        prte_node_index_remove(&nidx, node_from_list);
                        pmix_list_remove_item(nodes, &node_from_list->super);
                        /* xfer to keep list */
                        pmix_list_append(&keep, &node_from_list->super);
                    } else {
                        /* mark as included */
                        PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                    }
                    found = true;
                }
                /* A node named by its position is as much a request as one
                 * named outright, and a node named outright that is not on
                 * the list is refused below.  This one used to be dropped
                 * without a word, so a hostfile of "+n1" and "+n2" mapped a
                 * job onto one node if the other was not available to it. */
                if (!found) {
                    prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-hostfile.txt", "hostfile:extra-node-not-found", true, hostfile,
                                   node_from_pool->name);
                    rc = PRTE_ERR_SILENT;
                    goto cleanup;
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
            /* we have converted all aliases for ourself
             * to our own detected nodename */
            if (NULL != (node_from_list = prte_node_index_match_node(&nidx, node_from_file))) {
                hostfile_cap_for_job(node_from_file, node_from_list, remove);
                if (remove) {
                    /* remove the node from the list */
                    hostfile_drop_empty(&where, &empties, node_from_list);
                    prte_node_index_remove(&nidx, node_from_list);
                    pmix_list_remove_item(nodes, &node_from_list->super);
                    /* xfer it to keep list */
                    pmix_list_append(&keep, &node_from_list->super);
                } else {
                    /* mark as included */
                    PRTE_FLAG_SET(node_from_list, PRTE_NODE_FLAG_MAPPED);
                }
                found = true;
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
        item2 = NULL;
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
    /* "item2" is the hostfile entry being resolved when a refusal jumped
     * here: it has been taken off newnodes, so nothing below would release
     * it, and each of those refusals used to leak it */
    if (NULL != item2) {
        PMIX_RELEASE(item2);
    }
    /* "keep" holds nodes this routine took *off* the caller's list, so on
     * any path that does not put them back they are the caller's nodes being
     * dropped on the floor - every error return used to leak them, along
     * with the two lists themselves */
    prte_node_index_destruct(&nidx);
    prte_node_index_destruct(&fidx);
    prte_node_index_destruct(&xidx);
    PMIX_DESTRUCT(&empties);
    PMIX_DESTRUCT(&where);
    PMIX_LIST_DESTRUCT(&keep);
    PMIX_LIST_DESTRUCT(&newnodes);
    PMIX_LIST_DESTRUCT(&exclude);

    return rc;
}
