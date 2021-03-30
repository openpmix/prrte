/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>

#include "src/runtime/runtime.h"

#include "src/class/prte_list.h"
#include "src/class/prte_pointer_array.h"

#include "src/util/argv.h"
#include "src/util/cmd_line.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"

#include "src/include/frameworks.h"

#include "src/mca/prteinstalldirs/prteinstalldirs.h"

#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/tools/prte_info/pinfo.h"

/*
 * Public variables
 */

static void component_map_construct(prte_info_component_map_t *map)
{
    map->type = NULL;
}
static void component_map_destruct(prte_info_component_map_t *map)
{
    if (NULL != map->type) {
        free(map->type);
    }
    /* the type close functions will release the
     * list of components
     */
}
PRTE_CLASS_INSTANCE(prte_info_component_map_t, prte_list_item_t, component_map_construct,
                    component_map_destruct);

prte_pointer_array_t prte_component_map = {{0}};

/*
 * Private variables
 */

static bool opened_components = false;

static int info_register_framework(prte_mca_base_framework_t *framework,
                                   prte_pointer_array_t *component_map)
{
    prte_info_component_map_t *map;
    int rc;

    rc = prte_mca_base_framework_register(framework, PRTE_MCA_BASE_REGISTER_ALL);
    if (PRTE_SUCCESS != rc && PRTE_ERR_BAD_PARAM != rc) {
        return rc;
    }

    if (NULL != component_map) {
        map = PRTE_NEW(prte_info_component_map_t);
        map->type = strdup(framework->framework_name);
        map->components = &framework->framework_components;
        map->failed_components = &framework->framework_failed_components;
        prte_pointer_array_add(component_map, map);
    }

    return rc;
}

static int register_project_frameworks(const char *project_name,
                                       prte_mca_base_framework_t **frameworks,
                                       prte_pointer_array_t *component_map)
{
    int i, rc = PRTE_SUCCESS;

    for (i = 0; NULL != frameworks[i]; i++) {
        if (PRTE_SUCCESS != (rc = info_register_framework(frameworks[i], component_map))) {
            if (PRTE_ERR_BAD_PARAM == rc) {
                fprintf(stderr,
                        "\nA \"bad parameter\" error was encountered when opening the %s %s "
                        "framework\n",
                        project_name, frameworks[i]->framework_name);
                fprintf(stderr, "The output received from that framework includes the following "
                                "parameters:\n\n");
            } else if (PRTE_ERR_NOT_AVAILABLE != rc) {
                fprintf(stderr, "%s_info_register: %s failed\n", project_name,
                        frameworks[i]->framework_name);
                rc = PRTE_ERROR;
            } else {
                continue;
            }

            break;
        }
    }

    return rc;
}

static int register_framework_params(prte_pointer_array_t *component_map)
{
    int rc;

    /* Register mca/base parameters */
    if (PRTE_SUCCESS != prte_mca_base_open()) {
        prte_show_help("help-prte_info.txt", "lib-call-fail", true, "mca_base_open", __FILE__,
                       __LINE__);
        return PRTE_ERROR;
    }

    /* Register the PRTE layer's MCA parameters */
    if (PRTE_SUCCESS != (rc = prte_register_params())) {
        fprintf(stderr, "prte_info_register: prte_register_params failed\n");
        return rc;
    }

    return register_project_frameworks("prte", prte_frameworks, component_map);
}

void prte_info_components_open(void)
{
    if (opened_components) {
        return;
    }

    opened_components = true;

    /* init the map */
    PRTE_CONSTRUCT(&prte_component_map, prte_pointer_array_t);
    prte_pointer_array_init(&prte_component_map, 256, INT_MAX, 128);

    register_framework_params(&prte_component_map);
}

/*
 * Not to be confused with prte_info_close_components.
 */
void prte_info_components_close(void)
{
    int i;
    prte_info_component_map_t *map;

    if (!opened_components) {
        return;
    }

    for (i = 0; NULL != prte_frameworks[i]; i++) {
        (void) prte_mca_base_framework_close(prte_frameworks[i]);
    }

    for (i = 0; i < prte_component_map.size; i++) {
        if (NULL
            != (map = (prte_info_component_map_t *) prte_pointer_array_get_item(&prte_component_map,
                                                                                i))) {
            PRTE_RELEASE(map);
        }
    }

    PRTE_DESTRUCT(&prte_component_map);

    opened_components = false;
}
