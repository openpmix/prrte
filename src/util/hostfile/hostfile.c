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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "src/class/prrte_list.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/mca/installdirs/installdirs.h"

#include "src/util/show_help.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prrte_globals.h"

#include "src/util/hostfile/hostfile_lex.h"
#include "src/util/hostfile/hostfile.h"


static const char *cur_hostfile_name = NULL;

static void hostfile_parse_error(int token)
{
    switch (token) {
    case PRRTE_HOSTFILE_STRING:
        prrte_show_help("help-hostfile.txt", "parse_error_string",
                       true,
                       cur_hostfile_name,
                       prrte_util_hostfile_line,
                       token,
                       prrte_util_hostfile_value.sval);
        break;
    case PRRTE_HOSTFILE_IPV4:
    case PRRTE_HOSTFILE_IPV6:
    case PRRTE_HOSTFILE_INT:
        prrte_show_help("help-hostfile.txt", "parse_error_int",
                       true,
                       cur_hostfile_name,
                       prrte_util_hostfile_line,
                       token,
                       prrte_util_hostfile_value.ival);
        break;
     default:
        prrte_show_help("help-hostfile.txt", "parse_error",
                       true,
                       cur_hostfile_name,
                       prrte_util_hostfile_line,
                       token );
        break;
    }
}

 /**
  * Return the integer following an = (actually may only return positive ints)
  */
static int hostfile_parse_int(void)
{
    if (PRRTE_HOSTFILE_EQUAL != prrte_util_hostfile_lex())
        return -1;
    if (PRRTE_HOSTFILE_INT != prrte_util_hostfile_lex())
        return -1;
    return prrte_util_hostfile_value.ival;
}

/**
 * Return the string following an = (option to a keyword)
 */
static char *hostfile_parse_string(void)
{
    int rc;
    if (PRRTE_HOSTFILE_EQUAL != prrte_util_hostfile_lex()){
        return NULL;
    }
    rc = prrte_util_hostfile_lex();
    if (PRRTE_HOSTFILE_STRING != rc){
        return NULL;
    }
    return strdup(prrte_util_hostfile_value.sval);
}

static prrte_node_t* hostfile_lookup(prrte_list_t* nodes, const char* name)
{
    prrte_list_item_t* item;
    for(item =  prrte_list_get_first(nodes);
        item != prrte_list_get_end(nodes);
        item =  prrte_list_get_next(item)) {
        prrte_node_t* node = (prrte_node_t*)item;
        if (strcmp(node->name, name) == 0) {
            return node;
        }
    }
    return NULL;
}

static int hostfile_parse_line(int token, prrte_list_t* updates,
                               prrte_list_t* exclude, bool keep_all)
{
    int rc;
    prrte_node_t* node;
    bool got_max = false;
    char* value;
    char **argv;
    char* node_name = NULL;
    char* username = NULL;
    int cnt;
    int number_of_slots = 0;
    char buff[64];

    if (PRRTE_HOSTFILE_STRING == token ||
        PRRTE_HOSTFILE_HOSTNAME == token ||
        PRRTE_HOSTFILE_INT == token ||
        PRRTE_HOSTFILE_IPV4 == token ||
        PRRTE_HOSTFILE_IPV6 == token) {

        if(PRRTE_HOSTFILE_INT == token) {
            snprintf(buff, 64, "%d", prrte_util_hostfile_value.ival);
            value = buff;
        } else {
            value = prrte_util_hostfile_value.sval;
        }
        argv = prrte_argv_split (value, '@');

        cnt = prrte_argv_count (argv);
        if (1 == cnt) {
            node_name = strdup(argv[0]);
        } else if (2 == cnt) {
            username = strdup(argv[0]);
            node_name = strdup(argv[1]);
        } else {
            prrte_output(0, "WARNING: Unhandled user@host-combination\n"); /* XXX */
        }
        prrte_argv_free (argv);

        // Strip off the FQDN if present, ignore IP addresses
        if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(node_name) ) {
            char *ptr;
            if (NULL != (ptr = strchr(node_name, '.'))) {
                *ptr = '\0';
            }
        }

        /* if the first letter of the name is '^', then this is a node
         * to be excluded. Remove the ^ character so the nodename is
         * usable, and put it on the exclude list
         */
        if ('^' == node_name[0]) {
            int i, len;
            len = strlen(node_name);
            for (i=1; i < len; i++) {
                node_name[i-1] = node_name[i];
            }
            node_name[len-1] = '\0';  /* truncate */

            PRRTE_OUTPUT_VERBOSE((3, prrte_ras_base_framework.framework_output,
                                 "%s hostfile: node %s is being excluded",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node_name));

            /* see if this is another name for us */
            if (prrte_check_host_is_local(node_name)) {
                /* Nodename has been allocated, that is for sure */
                free (node_name);
                node_name = strdup(prrte_process_info.nodename);
            }

            /* Do we need to make a new node object?  First check to see
               if it's already in the exclude list */
            if (NULL == (node = hostfile_lookup(exclude, node_name))) {
                node = PRRTE_NEW(prrte_node_t);
                node->name = node_name;
                if (NULL != username) {
                    prrte_set_attribute(&node->attributes, PRRTE_NODE_USERNAME, PRRTE_ATTR_LOCAL, username, PRRTE_STRING);
                }
                prrte_list_append(exclude, &node->super);
            } else {
                free(node_name);
            }
            return PRRTE_SUCCESS;
        }

        /* this is not a node to be excluded, so we need to process it and
         * add it to the "include" list. See if this host is actually us.
         */
        if (prrte_check_host_is_local(node_name)) {
            /* Nodename has been allocated, that is for sure */
            free (node_name);
            node_name = strdup(prrte_process_info.nodename);
        }

        PRRTE_OUTPUT_VERBOSE((3, prrte_ras_base_framework.framework_output,
                             "%s hostfile: node %s is being included - keep all is %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node_name,
                             keep_all ? "TRUE" : "FALSE"));

        /* Do we need to make a new node object? */
        if (keep_all || NULL == (node = hostfile_lookup(updates, node_name))) {
            node = PRRTE_NEW(prrte_node_t);
            node->name = node_name;
            node->slots = 1;
            if (NULL != username) {
                prrte_set_attribute(&node->attributes, PRRTE_NODE_USERNAME, PRRTE_ATTR_LOCAL, username, PRRTE_STRING);
            }
            prrte_list_append(updates, &node->super);
        } else {
            /* this node was already found once - add a slot and mark slots as "given" */
            node->slots++;
            PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
            free(node_name);
        }
    } else if (PRRTE_HOSTFILE_RELATIVE == token) {
        /* store this for later processing */
        node = PRRTE_NEW(prrte_node_t);
        node->name = strdup(prrte_util_hostfile_value.sval);
        prrte_list_append(updates, &node->super);
    } else if (PRRTE_HOSTFILE_RANK == token) {
        /* we can ignore the rank, but we need to extract the node name. we
         * first need to shift over to the other side of the equal sign as
         * this is where the node name will be
         */
        while (!prrte_util_hostfile_done &&
               PRRTE_HOSTFILE_EQUAL != token) {
            token = prrte_util_hostfile_lex();
        }
        if (prrte_util_hostfile_done) {
            /* bad syntax somewhere */
            return PRRTE_ERROR;
        }
        /* next position should be the node name */
        token = prrte_util_hostfile_lex();
        if(PRRTE_HOSTFILE_INT == token) {
            snprintf(buff, 64, "%d", prrte_util_hostfile_value.ival);
            value = buff;
        } else {
            value = prrte_util_hostfile_value.sval;
        }

        argv = prrte_argv_split (value, '@');

        cnt = prrte_argv_count (argv);
        if (1 == cnt) {
            node_name = strdup(argv[0]);
        } else if (2 == cnt) {
            username = strdup(argv[0]);
            node_name = strdup(argv[1]);
        } else {
            prrte_output(0, "WARNING: Unhandled user@host-combination\n"); /* XXX */
        }
        prrte_argv_free (argv);

        // Strip off the FQDN if present, ignore IP addresses
        if( !prrte_keep_fqdn_hostnames && !prrte_net_isaddr(node_name) ) {
            char *ptr;
            if (NULL != (ptr = strchr(node_name, '.'))) {
                *ptr = '\0';
            }
        }

        /* Do we need to make a new node object? */
        if (NULL == (node = hostfile_lookup(updates, node_name))) {
            node = PRRTE_NEW(prrte_node_t);
            node->name = node_name;
            node->slots = 1;
            if (NULL != username) {
                prrte_set_attribute(&node->attributes, PRRTE_NODE_USERNAME, PRRTE_ATTR_LOCAL, username, PRRTE_STRING);
            }
            prrte_list_append(updates, &node->super);
        } else {
            /* add a slot */
            node->slots++;
            free(node_name);
        }
        PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                             "%s hostfile: node %s slots %d nodes-given %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), node->name, node->slots,
                             PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN) ? "TRUE" : "FALSE"));
        /* mark the slots as "given" since we take them as being the
         * number specified via the rankfile
         */
        PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
        /* skip to end of line */
        while (!prrte_util_hostfile_done &&
               PRRTE_HOSTFILE_NEWLINE != token) {
            token = prrte_util_hostfile_lex();
        }
        return PRRTE_SUCCESS;
    } else {
        hostfile_parse_error(token);
        return PRRTE_ERROR;
    }
    free(username);

    while (!prrte_util_hostfile_done) {
        token = prrte_util_hostfile_lex();

        switch (token) {
        case PRRTE_HOSTFILE_DONE:
            goto done;

        case PRRTE_HOSTFILE_NEWLINE:
            goto done;

        case PRRTE_HOSTFILE_USERNAME:
            username = hostfile_parse_string();
            if (NULL != username) {
                prrte_set_attribute(&node->attributes, PRRTE_NODE_USERNAME, PRRTE_ATTR_LOCAL, username, PRRTE_STRING);
                free(username);
            }
            break;

        case PRRTE_HOSTFILE_PORT:
            rc = hostfile_parse_int();
            if (rc < 0) {
                prrte_show_help("help-hostfile.txt", "port",
                               true,
                               cur_hostfile_name, rc);
                return PRRTE_ERROR;
            }
            prrte_set_attribute(&node->attributes, PRRTE_NODE_PORT, PRRTE_ATTR_LOCAL, &rc, PRRTE_INT);
            break;

        case PRRTE_HOSTFILE_COUNT:
        case PRRTE_HOSTFILE_CPU:
        case PRRTE_HOSTFILE_SLOTS:
            rc = hostfile_parse_int();
            if (rc < 0) {
                prrte_show_help("help-hostfile.txt", "slots",
                               true,
                               cur_hostfile_name, rc);
                prrte_list_remove_item(updates, &node->super);
                PRRTE_RELEASE(node);
                return PRRTE_ERROR;
            }
            if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
                /* multiple definitions were given for the
                 * slot count - this is not allowed
                 */
                prrte_show_help("help-hostfile.txt", "slots-given",
                               true,
                               cur_hostfile_name, node->name);
                prrte_list_remove_item(updates, &node->super);
                PRRTE_RELEASE(node);
                return PRRTE_ERROR;
            }
            node->slots = rc;
            PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);

            /* Ensure that slots_max >= slots */
            if (node->slots_max != 0 && node->slots_max < node->slots) {
                node->slots_max = node->slots;
            }
            break;

        case PRRTE_HOSTFILE_SLOTS_MAX:
            rc = hostfile_parse_int();
            if (rc < 0) {
                prrte_show_help("help-hostfile.txt", "max_slots",
                               true,
                               cur_hostfile_name, ((size_t) rc));
                prrte_list_remove_item(updates, &node->super);
                PRRTE_RELEASE(node);
                return PRRTE_ERROR;
            }
            /* Only take this update if it puts us >= node_slots */
            if (rc >= node->slots) {
                if (node->slots_max != rc) {
                    node->slots_max = rc;
                    got_max = true;
                }
            } else {
                prrte_show_help("help-hostfile.txt", "max_slots_lt",
                               true,
                               cur_hostfile_name, node->slots, rc);
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                prrte_list_remove_item(updates, &node->super);
                PRRTE_RELEASE(node);
                return PRRTE_ERROR;
            }
            break;

        case PRRTE_HOSTFILE_STRING:
        case PRRTE_HOSTFILE_INT:
            /* just ignore it */
            break;

        default:
            hostfile_parse_error(token);
            prrte_list_remove_item(updates, &node->super);
            PRRTE_RELEASE(node);
            return PRRTE_ERROR;
        }
        if (number_of_slots > node->slots) {
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            prrte_list_remove_item(updates, &node->super);
            PRRTE_RELEASE(node);
            return PRRTE_ERROR;
        }
    }

 done:
    if (got_max && !PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_SLOTS_GIVEN)) {
        node->slots = node->slots_max;
        PRRTE_FLAG_SET(node, PRRTE_NODE_FLAG_SLOTS_GIVEN);
    }

    return PRRTE_SUCCESS;
}


/**
 * Parse the specified file into a node list.
 */

static int hostfile_parse(const char *hostfile, prrte_list_t* updates,
                          prrte_list_t* exclude, bool keep_all)
{
    int token;
    int rc = PRRTE_SUCCESS;


    cur_hostfile_name = hostfile;

    prrte_util_hostfile_done = false;
    prrte_util_hostfile_in = fopen(hostfile, "r");
    if (NULL == prrte_util_hostfile_in) {
        if (NULL == prrte_default_hostfile ||
            0 != strcmp(prrte_default_hostfile, hostfile)) {
            /* not the default hostfile, so not finding it
             * is an error
             */
            prrte_show_help("help-hostfile.txt", "no-hostfile", true, hostfile);
            rc = PRRTE_ERR_SILENT;
            goto unlock;
        }
        /* if this is the default hostfile and it was given,
         * then it's an error
         */
        if (prrte_default_hostfile_given) {
            prrte_show_help("help-hostfile.txt", "no-hostfile", true, hostfile);
            rc = PRRTE_ERR_NOT_FOUND;
            goto unlock;
        }
        /* otherwise, not finding it is okay */
        rc = PRRTE_SUCCESS;
        goto unlock;
    }

    while (!prrte_util_hostfile_done) {
        token = prrte_util_hostfile_lex();

        switch (token) {
        case PRRTE_HOSTFILE_DONE:
            prrte_util_hostfile_done = true;
            break;

        case PRRTE_HOSTFILE_NEWLINE:
            break;

        /*
         * This looks odd, since we have several forms of host-definitions:
         *   hostname              just plain as it is, being a PRRTE_HOSTFILE_STRING
         *   IP4s and user@IPv4s
         *   hostname.domain and user@hostname.domain
         */
        case PRRTE_HOSTFILE_STRING:
        case PRRTE_HOSTFILE_INT:
        case PRRTE_HOSTFILE_HOSTNAME:
        case PRRTE_HOSTFILE_IPV4:
        case PRRTE_HOSTFILE_IPV6:
        case PRRTE_HOSTFILE_RELATIVE:
        case PRRTE_HOSTFILE_RANK:
            rc = hostfile_parse_line(token, updates, exclude, keep_all);
            if (PRRTE_SUCCESS != rc) {
                goto unlock;
            }
            break;

        default:
            hostfile_parse_error(token);
            goto unlock;
        }
    }
    fclose(prrte_util_hostfile_in);
    prrte_util_hostfile_in = NULL;
    prrte_util_hostfile_lex_destroy();

unlock:
    cur_hostfile_name = NULL;

    return rc;
}


/**
 * Parse the provided hostfile and add the nodes to the list.
 */

int prrte_util_add_hostfile_nodes(prrte_list_t *nodes,
                                 char *hostfile)
{
    prrte_list_t exclude, adds;
    prrte_list_item_t *item, *itm;
    int rc;
    prrte_node_t *nd, *node;
    bool found;

    PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                         "%s hostfile: checking hostfile %s for nodes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hostfile));

    PRRTE_CONSTRUCT(&exclude, prrte_list_t);
    PRRTE_CONSTRUCT(&adds, prrte_list_t);

    /* parse the hostfile and add any new contents to the list */
    if (PRRTE_SUCCESS != (rc = hostfile_parse(hostfile, &adds, &exclude, false))) {
        goto cleanup;
    }

    /* check for any relative node directives */
    for (item = prrte_list_get_first(&adds);
         item != prrte_list_get_end(&adds);
         item = prrte_list_get_next(item)) {
        node=(prrte_node_t*)item;

        if ('+' == node->name[0]) {
            prrte_show_help("help-hostfile.txt", "hostfile:relative-syntax",
                           true, node->name);
            rc = PRRTE_ERR_SILENT;
            goto cleanup;
        }
    }

    /* remove from the list of nodes those that are in the exclude list */
    while (NULL != (item = prrte_list_remove_first(&exclude))) {
        nd = (prrte_node_t*)item;
        /* check for matches on nodes */
        for (itm = prrte_list_get_first(&adds);
             itm != prrte_list_get_end(&adds);
             itm = prrte_list_get_next(itm)) {
            node = (prrte_node_t*)itm;
            if (0 == strcmp(nd->name, node->name)) {
                /* match - remove it */
                prrte_list_remove_item(&adds, itm);
                PRRTE_RELEASE(itm);
                break;
            }
        }
        PRRTE_RELEASE(item);
    }

    /* transfer across all unique nodes */
    while (NULL != (item = prrte_list_remove_first(&adds))) {
        nd = (prrte_node_t*)item;
        found = false;
        for (itm = prrte_list_get_first(nodes);
             itm != prrte_list_get_end(nodes);
             itm = prrte_list_get_next(itm)) {
            node = (prrte_node_t*)itm;
            if (0 == strcmp(nd->name, node->name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            prrte_list_append(nodes, &nd->super);
            PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                                 "%s hostfile: adding node %s slots %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), nd->name, nd->slots));
        } else {
            PRRTE_RELEASE(item);
        }
    }

cleanup:
    PRRTE_LIST_DESTRUCT(&exclude);
    PRRTE_LIST_DESTRUCT(&adds);

    return rc;
}

/* Parse the provided hostfile and filter the nodes that are
 * on the input list, removing those that
 * are not found in the hostfile
 */
int prrte_util_filter_hostfile_nodes(prrte_list_t *nodes,
                                    char *hostfile,
                                    bool remove)
{
    prrte_list_t newnodes, exclude;
    prrte_list_item_t *item1, *item2, *next, *item3;
    prrte_node_t *node_from_list, *node_from_file, *node_from_pool, *node3;
    int rc = PRRTE_SUCCESS;
    char *cptr;
    int num_empty, nodeidx;
    bool want_all_empty = false;
    prrte_list_t keep;
    bool found;

    PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                        "%s hostfile: filtering nodes through hostfile %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hostfile));

    /* parse the hostfile and create local list of findings */
    PRRTE_CONSTRUCT(&newnodes, prrte_list_t);
    PRRTE_CONSTRUCT(&exclude, prrte_list_t);
    if (PRRTE_SUCCESS != (rc = hostfile_parse(hostfile, &newnodes, &exclude, false))) {
        PRRTE_DESTRUCT(&newnodes);
        PRRTE_DESTRUCT(&exclude);
        return rc;
    }

    /* if the hostfile was empty, then treat it as a no-op filter */
    if (0 == prrte_list_get_size(&newnodes)) {
        PRRTE_DESTRUCT(&newnodes);
        PRRTE_DESTRUCT(&exclude);
        /* indicate that the hostfile was empty */
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* remove from the list of newnodes those that are in the exclude list
     * since we could have added duplicate names above due to the */
    while (NULL != (item1 = prrte_list_remove_first(&exclude))) {
        node_from_file = (prrte_node_t*)item1;
        /* check for matches on nodes */
        for (item2 = prrte_list_get_first(&newnodes);
             item2 != prrte_list_get_end(&newnodes);
             item2 = prrte_list_get_next(item2)) {
            prrte_node_t *node = (prrte_node_t*)item2;
            if (0 == strcmp(node_from_file->name, node->name)) {
                /* match - remove it */
                prrte_list_remove_item(&newnodes, item2);
                PRRTE_RELEASE(item2);
                break;
            }
        }
        PRRTE_RELEASE(item1);
    }

    /* now check our nodes and keep or mark those that match. We can
     * destruct our hostfile list as we go since this won't be needed
     */
    PRRTE_CONSTRUCT(&keep, prrte_list_t);
    while (NULL != (item2 = prrte_list_remove_first(&newnodes))) {
        node_from_file = (prrte_node_t*)item2;

        next = prrte_list_get_next(item2);

        /* see if this is a relative node syntax */
        if ('+' == node_from_file->name[0]) {
            /* see if we specified empty nodes */
            if ('e' == node_from_file->name[1] ||
                'E' == node_from_file->name[1]) {
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
                item1 = prrte_list_get_first(nodes);
                while (0 < num_empty && item1 != prrte_list_get_end(nodes)) {
                    node_from_list = (prrte_node_t*)item1;
                    next = prrte_list_get_next(item1);  /* keep our place */
                    if (0 == node_from_list->slots_inuse) {
                        /* check to see if this node is explicitly called
                         * out later - if so, don't use it here
                         */
                        for (item3 = prrte_list_get_first(&newnodes);
                             item3 != prrte_list_get_end(&newnodes);
                             item3 = prrte_list_get_next(item3)) {
                            node3 = (prrte_node_t*)item3;
                            if (0 == strcmp(node3->name, node_from_list->name)) {
                                /* match - don't use it */
                                goto skipnode;
                            }
                        }
                        if (remove) {
                            /* remove item from list */
                            prrte_list_remove_item(nodes, item1);
                            /* xfer to keep list */
                            prrte_list_append(&keep, item1);
                        } else {
                            /* mark as included */
                            PRRTE_FLAG_SET(node_from_list, PRRTE_NODE_FLAG_MAPPED);
                        }
                        --num_empty;
                    }
                skipnode:
                    item1 = next;
                }
                /* did they get everything they wanted? */
                if (!want_all_empty && 0 < num_empty) {
                    prrte_show_help("help-hostfile.txt", "hostfile:not-enough-empty",
                                   true, num_empty);
                    rc = PRRTE_ERR_SILENT;
                    goto cleanup;
                }
            } else if ('n' == node_from_file->name[1] ||
                       'N' == node_from_file->name[1]) {
                /* they want a specific relative node #, so
                 * look it up on global pool
                 */
                nodeidx = strtol(&node_from_file->name[2], NULL, 10);
                if (NULL == (node_from_pool = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, nodeidx))) {
                    /* this is an error */
                    prrte_show_help("help-hostfile.txt", "hostfile:relative-node-not-found",
                                   true, nodeidx, node_from_file->name);
                    rc = PRRTE_ERR_SILENT;
                    goto cleanup;
                }
                /* search the list of nodes provided to us and find it */
                for (item1 = prrte_list_get_first(nodes);
                     item1 != prrte_list_get_end(nodes);
                     item1 = prrte_list_get_next(nodes)) {
                    node_from_list = (prrte_node_t*)item1;
                    if (0 == strcmp(node_from_list->name, node_from_pool->name)) {
                        if (remove) {
                            /* match - remove item from list */
                            prrte_list_remove_item(nodes, item1);
                            /* xfer to keep list */
                            prrte_list_append(&keep, item1);
                        } else {
                            /* mark as included */
                            PRRTE_FLAG_SET(node_from_list, PRRTE_NODE_FLAG_MAPPED);
                        }
                        break;
                    }
                }
            } else {
                /* invalid relative node syntax */
                prrte_show_help("help-hostfile.txt", "hostfile:invalid-relative-node-syntax",
                               true, node_from_file->name);
                rc = PRRTE_ERR_SILENT;
                goto cleanup;
            }
        } else {
            /* we are looking for a specific node on the list
             * search the provided list of nodes to see if this
             * one is found
             */
            found = false;
            for (item1 = prrte_list_get_first(nodes);
                 item1 != prrte_list_get_end(nodes);
                 item1 = prrte_list_get_next(item1)) {
                node_from_list = (prrte_node_t*)item1;
                /* we have converted all aliases for ourself
                 * to our own detected nodename, so no need
                 * to check for interfaces again - a simple
                 * strcmp will suffice */
                if (0 == strcmp(node_from_file->name, node_from_list->name)) {
                    /* if the slot count here is less than the
                     * total slots avail on this node, set it
                     * to the specified count - this allows people
                     * to subdivide an allocation
                     */
                    if (PRRTE_FLAG_TEST(node_from_file, PRRTE_NODE_FLAG_SLOTS_GIVEN) &&
                        node_from_file->slots < node_from_list->slots) {
                        node_from_list->slots = node_from_file->slots;
                    }
                    if (remove) {
                        /* remove the node from the list */
                        prrte_list_remove_item(nodes, item1);
                        /* xfer it to keep list */
                        prrte_list_append(&keep, item1);
                    } else {
                        /* mark as included */
                        PRRTE_FLAG_SET(node_from_list, PRRTE_NODE_FLAG_MAPPED);
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
                prrte_show_help("help-hostfile.txt", "hostfile:extra-node-not-found",
                               true, hostfile, node_from_file->name);
                rc = PRRTE_ERR_SILENT;
                goto cleanup;
            }
        }
        /* cleanup the newnode list */
        PRRTE_RELEASE(item2);
    }

    /* if we still have entries on our hostfile list, then
     * there were requested hosts that were not in our allocation.
     * This is an error - report it to the user and return an error
     */
    if (0 != prrte_list_get_size(&newnodes)) {
        prrte_show_help("help-hostfile.txt", "not-all-mapped-alloc",
                       true, hostfile);
        while (NULL != (item1 = prrte_list_remove_first(&newnodes))) {
            PRRTE_RELEASE(item1);
        }
        PRRTE_DESTRUCT(&newnodes);
        return PRRTE_ERR_SILENT;
    }

    if (!remove) {
        /* all done */
        PRRTE_DESTRUCT(&newnodes);
        return PRRTE_SUCCESS;
    }

    /* clear the rest of the nodes list */
    while (NULL != (item1 = prrte_list_remove_first(nodes))) {
        PRRTE_RELEASE(item1);
    }

    /* the nodes list has been cleared - rebuild it in order */
    while (NULL != (item1 = prrte_list_remove_first(&keep))) {
        prrte_list_append(nodes, item1);
    }

cleanup:
    PRRTE_DESTRUCT(&newnodes);

    return rc;
}

int prrte_util_get_ordered_host_list(prrte_list_t *nodes,
                                    char *hostfile)
{
    prrte_list_t exclude;
    prrte_list_item_t *item, *itm, *item2, *item1;
    char *cptr;
    int num_empty, i, nodeidx, startempty=0;
    bool want_all_empty=false;
    prrte_node_t *node_from_pool, *newnode;
    int rc;

    PRRTE_OUTPUT_VERBOSE((1, prrte_ras_base_framework.framework_output,
                         "%s hostfile: creating ordered list of hosts from hostfile %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), hostfile));

    PRRTE_CONSTRUCT(&exclude, prrte_list_t);

    /* parse the hostfile and add the contents to the list, keeping duplicates */
    if (PRRTE_SUCCESS != (rc = hostfile_parse(hostfile, nodes, &exclude, true))) {
        goto cleanup;
    }

    /* parse the nodes to process any relative node directives */
    item2 = prrte_list_get_first(nodes);
    while (item2 != prrte_list_get_end(nodes)) {
        prrte_node_t *node=(prrte_node_t*)item2;

        /* save the next location in case this one gets removed */
        item1 = prrte_list_get_next(item2);

        if ('+' != node->name[0]) {
            item2 = item1;
            continue;
        }

        /* see if we specified empty nodes */
        if ('e' == node->name[1] ||
            'E' == node->name[1]) {
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
            if (!prrte_hnp_is_allocated && 0 == startempty) {
               startempty = 1;
            }
            for (i=startempty; 0 < num_empty && i < prrte_node_pool->size; i++) {
                if (NULL == (node_from_pool = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, i))) {
                    continue;
                }
                if (0 == node_from_pool->slots_inuse) {
                    newnode = PRRTE_NEW(prrte_node_t);
                    newnode->name = strdup(node_from_pool->name);
                    /* if the slot count here is less than the
                     * total slots avail on this node, set it
                     * to the specified count - this allows people
                     * to subdivide an allocation
                     */
                    if (node->slots < node_from_pool->slots) {
                        newnode->slots = node->slots;
                    } else {
                        newnode->slots = node_from_pool->slots;
                    }
                    prrte_list_insert_pos(nodes, item1, &newnode->super);
                    /* track number added */
                    --num_empty;
                }
            }
            /* bookmark where we stopped in case they ask for more */
            startempty = i;
            /* did they get everything they wanted? */
            if (!want_all_empty && 0 < num_empty) {
                prrte_show_help("help-hostfile.txt", "hostfile:not-enough-empty",
                               true, num_empty);
                rc = PRRTE_ERR_SILENT;
                goto cleanup;
            }
            /* since we have expanded the provided node, remove
             * it from list
             */
            prrte_list_remove_item(nodes, item2);
            PRRTE_RELEASE(item2);
        } else if ('n' == node->name[1] ||
                   'N' == node->name[1]) {
            /* they want a specific relative node #, so
             * look it up on global pool
             */
            nodeidx = strtol(&node->name[2], NULL, 10);
            /* if the HNP is not allocated, then we need to
             * adjust the index as the node pool is offset
             * by one
             */
            if (!prrte_hnp_is_allocated) {
                nodeidx++;
            }
            /* see if that location is filled */
            if (NULL == (node_from_pool = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, nodeidx))) {
                /* this is an error */
                prrte_show_help("help-hostfile.txt", "hostfile:relative-node-not-found",
                               true, nodeidx, node->name);
                rc = PRRTE_ERR_SILENT;
                goto cleanup;
            }
            /* create the node object */
            newnode = PRRTE_NEW(prrte_node_t);
            newnode->name = strdup(node_from_pool->name);
            /* if the slot count here is less than the
             * total slots avail on this node, set it
             * to the specified count - this allows people
             * to subdivide an allocation
             */
            if (node->slots < node_from_pool->slots) {
                newnode->slots = node->slots;
            } else {
                newnode->slots = node_from_pool->slots;
            }
            /* insert it before item1 */
            prrte_list_insert_pos(nodes, item1, &newnode->super);
            /* since we have expanded the provided node, remove
             * it from list
             */
            prrte_list_remove_item(nodes, item2);
            PRRTE_RELEASE(item2);
        } else {
            /* invalid relative node syntax */
            prrte_show_help("help-hostfile.txt", "hostfile:invalid-relative-node-syntax",
                           true, node->name);
            rc = PRRTE_ERR_SILENT;
            goto cleanup;
        }

        /* move to next */
        item2 = item1;
    }

    /* remove from the list of nodes those that are in the exclude list */
    while(NULL != (item = prrte_list_remove_first(&exclude))) {
        prrte_node_t *exnode = (prrte_node_t*)item;
        /* check for matches on nodes */
        for (itm = prrte_list_get_first(nodes);
             itm != prrte_list_get_end(nodes);
             itm = prrte_list_get_next(itm)) {
            prrte_node_t *node=(prrte_node_t*)itm;
            if (0 == strcmp(exnode->name, node->name)) {
                /* match - remove it */
                prrte_list_remove_item(nodes, itm);
                PRRTE_RELEASE(itm);
                /* have to cycle through the entire list as we could
                 * have duplicates
                 */
            }
        }
        PRRTE_RELEASE(item);
    }

cleanup:
    PRRTE_DESTRUCT(&exclude);

    return rc;
}
