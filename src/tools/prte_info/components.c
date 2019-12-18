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
 * Copyright (c) 2006-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdlib.h>
#include <string.h>

#include "src/runtime/runtime.h"

#include "src/class/prrte_list.h"
#include "src/class/prrte_pointer_array.h"

#include "src/util/output.h"
#include "src/util/cmd_line.h"
#include "src/util/error.h"
#include "src/util/argv.h"
#include "src/util/show_help.h"
#include "src/util/printf.h"
#include "src/dss/dss.h"

#include "src/include/frameworks.h"

#include "src/mca/installdirs/installdirs.h"

#include "src/mca/base/prrte_mca_base_component_repository.h"
#include "src/tools/prte_info/pinfo.h"

/*
 * Public variables
 */

static void component_map_construct(prrte_info_component_map_t *map)
{
    map->type = NULL;
}
static void component_map_destruct(prrte_info_component_map_t *map)
{
    if (NULL != map->type) {
        free(map->type);
    }
    /* the type close functions will release the
     * list of components
     */
}
PRRTE_CLASS_INSTANCE(prrte_info_component_map_t,
                   prrte_list_item_t,
                   component_map_construct,
                   component_map_destruct);

prrte_pointer_array_t prrte_component_map = {{0}};

/*
 * Private variables
 */

static bool opened_components = false;


static int info_register_framework (prrte_mca_base_framework_t *framework, prrte_pointer_array_t *component_map)
{
    prrte_info_component_map_t *map;
    int rc;

    rc = prrte_mca_base_framework_register(framework, PRRTE_MCA_BASE_REGISTER_ALL);
    if (PRRTE_SUCCESS != rc && PRRTE_ERR_BAD_PARAM != rc) {
        return rc;
    }

    if (NULL != component_map) {
        map = PRRTE_NEW(prrte_info_component_map_t);
        map->type = strdup(framework->framework_name);
        map->components = &framework->framework_components;
        map->failed_components = &framework->framework_failed_components;
        prrte_pointer_array_add(component_map, map);
    }

    return rc;
}

static int register_project_frameworks (const char *project_name, prrte_mca_base_framework_t **frameworks,
                                        prrte_pointer_array_t *component_map)
{
    int i, rc=PRRTE_SUCCESS;

    for (i=0; NULL != frameworks[i]; i++) {
        if (PRRTE_SUCCESS != (rc = info_register_framework(frameworks[i], component_map))) {
            if (PRRTE_ERR_BAD_PARAM == rc) {
                fprintf(stderr, "\nA \"bad parameter\" error was encountered when opening the %s %s framework\n",
                        project_name, frameworks[i]->framework_name);
                fprintf(stderr, "The output received from that framework includes the following parameters:\n\n");
            } else if (PRRTE_ERR_NOT_AVAILABLE != rc) {
                fprintf(stderr, "%s_info_register: %s failed\n", project_name, frameworks[i]->framework_name);
                rc = PRRTE_ERROR;
            } else {
                continue;
            }

            break;
        }
    }

    return rc;
}

static int register_framework_params(prrte_pointer_array_t *component_map)
{
    int rc;

    /* Register mca/base parameters */
    if( PRRTE_SUCCESS != prrte_mca_base_open() ) {
        prrte_show_help("help-prrte_info.txt", "lib-call-fail", true, "mca_base_open", __FILE__, __LINE__ );
        return PRRTE_ERROR;
    }

    /* Register the PRRTE layer's MCA parameters */
    if (PRRTE_SUCCESS != (rc = prrte_register_params())) {
        fprintf(stderr, "prrte_info_register: prrte_register_params failed\n");
        return rc;
    }

    return register_project_frameworks("prrte", prrte_frameworks, component_map);
}

void prrte_info_components_open(void)
{
    if (opened_components) {
        return;
    }

    opened_components = true;

    /* init the map */
    PRRTE_CONSTRUCT(&prrte_component_map, prrte_pointer_array_t);
    prrte_pointer_array_init(&prrte_component_map, 256, INT_MAX, 128);

    register_framework_params(&prrte_component_map);
}

/*
 * Not to be confused with prrte_info_close_components.
 */
void prrte_info_components_close(void)
{
    int i;
    prrte_info_component_map_t *map;

    if (!opened_components) {
        return;
    }

    for (i=0; NULL != prrte_frameworks[i]; i++) {
        (void) prrte_mca_base_framework_close(prrte_frameworks[i]);
    }

    for (i=0; i < prrte_component_map.size; i++) {
        if (NULL != (map = (prrte_info_component_map_t*)prrte_pointer_array_get_item(&prrte_component_map, i))) {
            PRRTE_RELEASE(map);
        }
    }

    PRRTE_DESTRUCT(&prrte_component_map);

    opened_components = false;
}
