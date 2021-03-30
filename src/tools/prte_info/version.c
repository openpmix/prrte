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
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <string.h>

#include "src/include/version.h"
#include "src/mca/base/base.h"
#include "src/util/printf.h"

#include "pmix.h"
#include "src/tools/prte_info/pinfo.h"

/*
 * Public variables
 */

const char *prte_info_ver_full = "full";
const char *prte_info_ver_major = "major";
const char *prte_info_ver_minor = "minor";
const char *prte_info_ver_release = "release";
const char *prte_info_ver_greek = "greek";
const char *prte_info_ver_repo = "repo";

/*
 * Private variables
 */

static const char *prte_info_ver_all = "all";
static const char *prte_info_ver_mca = "mca";
static const char *prte_info_ver_type = "type";
static const char *prte_info_ver_component = "component";

/*
 * Private functions
 */

static void show_mca_version(const prte_mca_base_component_t *component, const char *scope,
                             const char *ver_type);
static char *make_version_str(const char *scope, int major, int minor, int release,
                              const char *greek, const char *repo);

void prte_info_show_prte_version(const char *scope)
{
    char *tmp, *tmp2;

    prte_asprintf(&tmp, "%s:version:full", prte_info_type_prte);
    tmp2 = make_version_str(scope, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,
                            PRTE_GREEK_VERSION, PRTE_REPO_REV);
    prte_info_out("PRTE", tmp, tmp2);
    free(tmp);
    free(tmp2);
    prte_asprintf(&tmp, "%s:version:repo", prte_info_type_prte);
    prte_info_out("PRTE repo revision", tmp, PRTE_REPO_REV);
    free(tmp);
    prte_asprintf(&tmp, "%s:version:release_date", prte_info_type_prte);
    prte_info_out("PRTE release date", tmp, PRTE_RELEASE_DATE);
    free(tmp);

    prte_info_out("PMIx", "pmix:version:full", PMIx_Get_version());
}

/*
 * do_version
 *
 * Determines the version information related to the prte components
 * being used.
 * Accepts:
 *      - want_all: True if all components' info is required.
 *      - cmd_line: The constructed command line argument
 */
void prte_info_do_version(bool want_all, prte_cmd_line_t *cmd_line)
{
    unsigned int count;
    size_t i;
    char *arg1, *scope, *type, *component;
    char *pos;
    int j;
    prte_value_t *pval;

    prte_info_components_open();

    if (want_all) {
        prte_info_show_prte_version(prte_info_ver_full);
        for (j = 0; j < mca_types.size; ++j) {
            if (NULL == (pos = (char *) prte_pointer_array_get_item(&mca_types, j))) {
                continue;
            }
            prte_info_show_component_version(pos, prte_info_component_all, prte_info_ver_full,
                                             prte_info_type_all);
        }
    } else {
        count = prte_cmd_line_get_ninsts(cmd_line, "show-version");
        for (i = 0; i < count; ++i) {
            pval = prte_cmd_line_get_param(cmd_line, "show-version", (int) i, 0);
            arg1 = pval->value.data.string;
            pval = prte_cmd_line_get_param(cmd_line, "show-version", (int) i, 1);
            scope = pval->value.data.string;

            /* Version of PRTE */

            if (0 == strcmp(prte_info_type_prte, arg1)) {
                prte_info_show_prte_version(scope);
            }

            /* Specific type and component */

            else if (NULL != (pos = strchr(arg1, ':'))) {
                *pos = '\0';
                type = arg1;
                pos++;
                component = pos;

                prte_info_show_component_version(type, component, scope, prte_info_ver_all);

            }

            /* All components of a specific type */

            else {
                prte_info_show_component_version(arg1, prte_info_component_all, scope,
                                                 prte_info_ver_all);
            }
        }
    }
}

/*
 * Show all the components of a specific type/component combo (component may be
 * a wildcard)
 */
void prte_info_show_component_version(const char *type_name, const char *component_name,
                                      const char *scope, const char *ver_type)
{
    bool want_all_components = false;
    bool found;
    prte_list_item_t *item;
    prte_mca_base_component_list_item_t *cli;
    const prte_mca_base_component_t *component;
    prte_list_t *components;
    int j;
    char *pos;
    prte_info_component_map_t *map;

    /* see if all components wanted */
    if (0 == strcmp(prte_info_type_all, component_name)) {
        want_all_components = true;
    }

    /* Check to see if the type is valid */
    for (found = false, j = 0; j < mca_types.size; ++j) {
        if (NULL == (pos = (char *) prte_pointer_array_get_item(&mca_types, j))) {
            continue;
        }
        if (0 == strcmp(pos, type_name)) {
            found = true;
            break;
        }
    }

    if (!found) {
        exit(1);
    }

    /* Now that we have a valid type, find the right component list */
    components = NULL;
    for (j = 0; j < prte_component_map.size; j++) {
        if (NULL
            == (map = (prte_info_component_map_t *) prte_pointer_array_get_item(&prte_component_map,
                                                                                j))) {
            continue;
        }
        if (0 == strcmp(type_name, map->type)) {
            /* found it! */
            components = map->components;
            break;
        }
    }

    if (NULL != components) {
        if (prte_list_get_size(components) > 0) {
            for (item = prte_list_get_first(components); prte_list_get_end(components) != item;
                 item = prte_list_get_next(item)) {
                cli = (prte_mca_base_component_list_item_t *) item;
                component = cli->cli_component;
                if (want_all_components
                    || 0 == strcmp(component->mca_component_name, component_name)) {
                    show_mca_version(component, scope, ver_type);
                }
            }
        }
    }
}

/*
 * Given a component, display its relevant version(s)
 */
static void show_mca_version(const prte_mca_base_component_t *component, const char *scope,
                             const char *ver_type)
{
    bool printed;
    bool want_mca = false;
    bool want_type = false;
    bool want_component = false;
    char *message, *content;
    char *mca_version;
    char *api_version;
    char *component_version;
    char *tmp;

    if (0 == strcmp(ver_type, prte_info_ver_all) || 0 == strcmp(ver_type, prte_info_ver_mca)) {
        want_mca = true;
    }

    if (0 == strcmp(ver_type, prte_info_ver_all) || 0 == strcmp(ver_type, prte_info_ver_type)) {
        want_type = true;
    }

    if (0 == strcmp(ver_type, prte_info_ver_all)
        || 0 == strcmp(ver_type, prte_info_ver_component)) {
        want_component = true;
    }

    mca_version = make_version_str(scope, component->mca_major_version,
                                   component->mca_minor_version, component->mca_release_version, "",
                                   "");
    api_version = make_version_str(scope, component->mca_type_major_version,
                                   component->mca_type_minor_version,
                                   component->mca_type_release_version, "", "");
    component_version = make_version_str(scope, component->mca_component_major_version,
                                         component->mca_component_minor_version,
                                         component->mca_component_release_version, "", "");

    if (prte_info_pretty) {
        prte_asprintf(&message, "MCA %s", component->mca_type_name);
        printed = false;
        prte_asprintf(&content, "%s (", component->mca_component_name);

        if (want_mca) {
            prte_asprintf(&tmp, "%sMCA v%s", content, mca_version);
            free(content);
            content = tmp;
            printed = true;
        }

        if (want_type) {
            if (printed) {
                prte_asprintf(&tmp, "%s, ", content);
                free(content);
                content = tmp;
            }
            prte_asprintf(&tmp, "%sAPI v%s", content, api_version);
            free(content);
            content = tmp;
            printed = true;
        }

        if (want_component) {
            if (printed) {
                prte_asprintf(&tmp, "%s, ", content);
                free(content);
                content = tmp;
            }
            prte_asprintf(&tmp, "%sComponent v%s", content, component_version);
            free(content);
            content = tmp;
            printed = true;
        }
        if (NULL != content) {
            prte_asprintf(&tmp, "%s)", content);
            free(content);
        } else {
            prte_asprintf(&tmp, ")");
        }

        prte_info_out(message, NULL, tmp);
        free(message);
        free(tmp);

    } else {
        prte_asprintf(&message, "mca:%s:%s:version", component->mca_type_name,
                      component->mca_component_name);
        if (want_mca) {
            prte_asprintf(&tmp, "mca:%s", mca_version);
            prte_info_out(NULL, message, tmp);
            free(tmp);
        }
        if (want_type) {
            prte_asprintf(&tmp, "api:%s", api_version);
            prte_info_out(NULL, message, tmp);
            free(tmp);
        }
        if (want_component) {
            prte_asprintf(&tmp, "component:%s", component_version);
            prte_info_out(NULL, message, tmp);
            free(tmp);
        }
        free(message);
    }

    free(mca_version);
    free(api_version);
    free(component_version);
}

static char *make_version_str(const char *scope, int major, int minor, int release,
                              const char *greek, const char *repo)
{
    char *str = NULL, *tmp;
    char temp[BUFSIZ];

    temp[BUFSIZ - 1] = '\0';
    if (0 == strcmp(scope, prte_info_ver_full) || 0 == strcmp(scope, prte_info_ver_all)) {
        snprintf(temp, BUFSIZ - 1, "%d.%d", major, minor);
        str = strdup(temp);
        if (release > 0) {
            snprintf(temp, BUFSIZ - 1, ".%d", release);
            prte_asprintf(&tmp, "%s%s", str, temp);
            free(str);
            str = tmp;
        }
        if (NULL != greek) {
            prte_asprintf(&tmp, "%s%s", str, greek);
            free(str);
            str = tmp;
        }
        if (NULL != repo) {
            prte_asprintf(&tmp, "%s%s", str, repo);
            free(str);
            str = tmp;
        }
    } else if (0 == strcmp(scope, prte_info_ver_major)) {
        snprintf(temp, BUFSIZ - 1, "%d", major);
    } else if (0 == strcmp(scope, prte_info_ver_minor)) {
        snprintf(temp, BUFSIZ - 1, "%d", minor);
    } else if (0 == strcmp(scope, prte_info_ver_release)) {
        snprintf(temp, BUFSIZ - 1, "%d", release);
    } else if (0 == strcmp(scope, prte_info_ver_greek)) {
        str = strdup(greek);
    } else if (0 == strcmp(scope, prte_info_ver_repo)) {
        str = strdup(repo);
    }

    if (NULL == str) {
        str = strdup(temp);
    }

    return str;
}
