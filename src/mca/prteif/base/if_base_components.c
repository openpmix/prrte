/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#include <ctype.h>

#include "constants.h"
#include "src/mca/mca.h"
#include "src/mca/prteif/base/base.h"
#include "src/mca/prteif/base/static-components.h"
#include "src/mca/prteif/prteif.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_net.h"
#include "src/util/output.h"
#include "src/util/show_help.h"

/* instantiate the global list of interfaces */
prte_list_t prte_if_list = PRTE_LIST_STATIC_INIT;
bool prte_if_retain_loopback = false;

static int prte_if_base_open(prte_mca_base_open_flag_t flags);
static int prte_if_base_close(void);
static void prte_if_construct(prte_if_t *obj);
static char **split_and_resolve(const char *orig_str, const char *name);

static bool frameopen = false;

/* instance the prte_if_t object */
PRTE_CLASS_INSTANCE(prte_if_t, prte_list_item_t, prte_if_construct, NULL);

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, prteif, NULL, NULL, prte_if_base_open,
                                prte_if_base_close, prte_prteif_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static int prte_if_base_open(prte_mca_base_open_flag_t flags)
{
    prte_if_t *selected_interface, *next;
    int i, kindex;
    struct sockaddr_storage my_ss;
    char **interfaces=NULL;
    bool including = false;
    bool excluding = false;
    int rc;

    if (frameopen) {
        return PRTE_SUCCESS;
    }
    frameopen = true;

    /* setup the global list */
    PRTE_CONSTRUCT(&prte_if_list, prte_list_t);

    rc = prte_mca_base_framework_components_open(&prte_prteif_base_framework, flags);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != prte_if_include) {
        interfaces = split_and_resolve(prte_if_include, "include");
        including = true;
        excluding = false;
    } else if (NULL != prte_if_exclude) {
        interfaces = split_and_resolve(prte_if_exclude, "exclude");
        including = false;
        excluding = true;
    }
    /* look at all available interfaces */
    PRTE_LIST_FOREACH_SAFE(selected_interface, next, &prte_if_list, prte_if_t)
    {
        i = selected_interface->if_index;
        kindex = selected_interface->if_kernel_index;
        memcpy((struct sockaddr *) &my_ss, &selected_interface->if_addr,
               MIN(sizeof(struct sockaddr_storage), sizeof(selected_interface->if_addr)));
        /* remove non-ip4/6 interfaces */
        if (AF_INET != my_ss.ss_family
#if PRTE_ENABLE_IPV6
            && AF_INET6 != my_ss.ss_family
#endif
            ) {
            prte_list_remove_item(&prte_if_list, &selected_interface->super);
            PRTE_RELEASE(selected_interface);
            continue;
        }
        prte_output_verbose(10, prte_prteif_base_framework.framework_output,
                            "WORKING INTERFACE %d KERNEL INDEX %d FAMILY: %s", i, kindex,
                            (AF_INET == my_ss.ss_family) ? "V4" : "V6");

        /* remove any virtual interfaces */
        if (0 == strncmp(selected_interface->if_name, "vir", 3)) {
            prte_list_remove_item(&prte_if_list, &selected_interface->super);
            PRTE_RELEASE(selected_interface);
            continue;
        }

        /* handle include/exclude directives */
        if (NULL != interfaces) {
            /* check for match */
            rc = prte_ifmatches(kindex, interfaces);
            /* if one of the network specifications isn't parseable, then
             * error out as we can't do what was requested
             */
            if (PRTE_ERR_NETWORK_NOT_PARSEABLE == rc) {
                prte_show_help("help-oob-tcp.txt", "not-parseable", true);
                pmix_argv_free(interfaces);
                return PRTE_ERR_BAD_PARAM;
            }
            /* if we are including, then remove this if not present */
            if (including) {
                if (PRTE_SUCCESS != rc) {
                    prte_output_verbose(
                                        20, prte_prteif_base_framework.framework_output,
                                        "%s oob:tcp:init rejecting interface %s (not in include list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    prte_list_remove_item(&prte_if_list, &selected_interface->super);
                    PRTE_RELEASE(selected_interface);
                    continue;
                }
            } else {
                /* we are excluding, so remove if present */
                if (PRTE_SUCCESS == rc) {
                    prte_output_verbose(20, prte_prteif_base_framework.framework_output,
                                        "%s oob:tcp:init rejecting interface %s (in exclude list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        selected_interface->if_name);
                    prte_list_remove_item(&prte_if_list, &selected_interface->super);
                    PRTE_RELEASE(selected_interface);
                   continue;
                }
            }
        } else {
            /* if no specific interfaces were provided, we remove the loopback
             * interface unless nothing else is available
             */
            if (1 < prte_ifcount() && prte_ifisloopback(i)) {
                prte_output_verbose(20, prte_prteif_base_framework.framework_output,
                                    "%s if: rejecting loopback interface %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    selected_interface->if_name);
                prte_list_remove_item(&prte_if_list, &selected_interface->super);
                PRTE_RELEASE(selected_interface);
                continue;
            }
        }

        /* Refs ticket #3019
         * it would probably be worthwhile to print out a warning if PRTE detects multiple
         * IP interfaces that are "up" on the same subnet (because that's a Bad Idea). Note
         * that we should only check for this after applying the relevant include/exclude
         * list MCA params. If we detect redundant ports, we can also automatically ignore
         * them so that applications won't hang.
         */
    }

    /* cleanup */
    if (NULL != interfaces) {
        pmix_argv_free(interfaces);
    }

    if (0 == prte_list_get_size(&prte_if_list)) {
        if (including) {
            prte_show_help("help-oob-tcp.txt", "no-included-found", true,
                           prte_if_include);
        } else if (excluding) {
            prte_show_help("help-oob-tcp.txt", "excluded-all", true,
                           prte_if_exclude);
        }
        return PRTE_ERR_NOT_AVAILABLE;
    }

    return PRTE_SUCCESS;
}

static int prte_if_base_close(void)
{
    prte_list_item_t *item;

    if (!frameopen) {
        return PRTE_SUCCESS;
    }
    frameopen = false;

    while (NULL != (item = prte_list_remove_first(&prte_if_list))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_if_list);

    return prte_mca_base_framework_components_close(&prte_prteif_base_framework, NULL);
}

static void prte_if_construct(prte_if_t *obj)
{
    memset(obj->if_name, 0, sizeof(obj->if_name));
    obj->if_index = -1;
    obj->if_kernel_index = (uint16_t) -1;
    obj->af_family = PF_UNSPEC;
    obj->if_flags = 0;
    obj->if_speed = 0;
    memset(&obj->if_addr, 0, sizeof(obj->if_addr));
    obj->if_mask = 0;
    obj->if_bandwidth = 0;
    memset(obj->if_mac, 0, sizeof(obj->if_mac));
    obj->ifmtu = 0;
}

/*
 * Go through a list of argv; if there are any subnet specifications
 * (a.b.c.d/e), resolve them to an interface name (Currently only
 * supporting IPv4).  If unresolvable, warn and remove.
 */
static char **split_and_resolve(const char *orig_str,
                                const char *name)
{
    int i, ret, if_index;
    char **argv, **interfaces, *str;
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;
    char if_name[PRTE_IF_NAMESIZE];
    bool found;

    /* Sanity check */
    if (NULL == orig_str) {
        return NULL;
    }

    argv = pmix_argv_split(orig_str, ',');
    interfaces = NULL;
    for (i = 0; NULL != argv[i]; ++i) {
        if (isalpha(argv[i][0])) {
            /* This is an interface name. If not already in the interfaces array, add it */
            pmix_argv_append_unique_nosize(&interfaces, argv[i]);
            prte_output_verbose(20,
                                prte_prteif_base_framework.framework_output,
                                "prteif:base: Using interface: %s ", argv[i]);
            continue;
        }
        /* Found a subnet notation.  Convert it to an IP
         address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        str = strchr(argv[i], '/');
        if (NULL == str) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true,
                           name, prte_process_info.nodename, argv[i],
                           "Invalid specification (missing \"/\")");
            continue;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr *) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i], &((struct sockaddr_in *) &argv_inaddr)->sin_addr);
        *str = '/';

        if (1 != ret) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true,
                           name, prte_process_info.nodename, argv[i],
                           "Invalid specification (inet_pton() failed)");
            continue;
        }
        prte_output_verbose(20, prte_prteif_base_framework.framework_output,
                            "%s if: Searching for %s address+prefix: %s / %u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), name,
                            pmix_net_get_hostname((struct sockaddr *) &argv_inaddr), argv_prefix);


        /* Go through all interfaces to see if we can find one or more matches */
        found = false;
        for (if_index = prte_ifbegin(); if_index >= 0;
             if_index = prte_ifnext(if_index)) {
            prte_ifindextoaddr(if_index,
                               (struct sockaddr*) &if_inaddr,
                               sizeof(if_inaddr));
            if (pmix_net_samenetwork(&argv_inaddr, &if_inaddr, argv_prefix)) {
                /* We found a match. If it's not already in the interfaces array,
                   add it. If it's already in the array, treat it as a match */
                found = true;
                prte_ifindextoname(if_index, if_name, sizeof(if_name));
                pmix_argv_append_unique_nosize(&interfaces, if_name);
                    prte_output_verbose(20,
                                        prte_prteif_base_framework.framework_output,
                                        "prteif:base: Found match: %s (%s)",
                                        pmix_net_get_hostname((struct sockaddr*) &if_inaddr),
                                        if_name);
            }
        }

        /* If we didn't find a match, keep trying */
        if (!found) {
            prte_show_help("help-oob-tcp.txt", "invalid if_inexclude", true, name,
                           prte_process_info.nodename, argv[i],
                           "Did not find interface matching this subnet");
        }
    }

    pmix_argv_free(argv);
    return interfaces;
}
