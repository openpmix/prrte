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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/util/show_help.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/oob/oob.h"
#include "src/mca/oob/base/base.h"


/**
 * Function for selecting all runnable modules from those that are
 * available.
 *
 * Call the init function on all available modules.
 */
int prrte_oob_base_select(void)
{
    prrte_mca_base_component_list_item_t *cli, *cmp, *c2;
    prrte_oob_base_component_t *component, *c3;
    bool added;
    int i, rc;

    /* Query all available components and ask if their transport is available */
    PRRTE_LIST_FOREACH(cli, &prrte_oob_base_framework.framework_components, prrte_mca_base_component_list_item_t) {
        component = (prrte_oob_base_component_t *) cli->cli_component;

        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "mca:oob:select: checking available component %s",
                            component->oob_base.mca_component_name);

        /* If there's no query function, skip it */
        if (NULL == component->available) {
            prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                "mca:oob:select: Skipping component [%s]. It does not implement a query function",
                                component->oob_base.mca_component_name );
            continue;
        }

        /* Query the component */
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "mca:oob:select: Querying component [%s]",
                            component->oob_base.mca_component_name);

        rc = component->available();

        /* If the component is not available, then skip it as
         * it has no available interfaces
         */
        if (PRRTE_SUCCESS != rc && PRRTE_ERR_FORCE_SELECT != rc) {
            prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                "mca:oob:select: Skipping component [%s] - no available interfaces",
                                component->oob_base.mca_component_name );
            continue;
        }

        /* if it fails to startup, then skip it */
        if (PRRTE_SUCCESS != component->startup()) {
            prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                "mca:oob:select: Skipping component [%s] - failed to startup",
                                component->oob_base.mca_component_name );
            continue;
        }

        if (PRRTE_ERR_FORCE_SELECT == rc) {
            /* this component shall be the *only* component allowed
             * for use, so shutdown and remove any prior ones */
            while (NULL != (cmp = (prrte_mca_base_component_list_item_t*)prrte_list_remove_first(&prrte_oob_base.actives))) {
                c3 = (prrte_oob_base_component_t *) cmp->cli_component;
                if (NULL != c3->shutdown) {
                    c3->shutdown();
                }
                PRRTE_RELEASE(cmp);
            }
            c2 = PRRTE_NEW(prrte_mca_base_component_list_item_t);
            c2->cli_component = (prrte_mca_base_component_t*)component;
            prrte_list_append(&prrte_oob_base.actives, &c2->super);
            break;
        }

        /* record it, but maintain priority order */
        added = false;
        PRRTE_LIST_FOREACH(cmp, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
            c3 = (prrte_oob_base_component_t *) cmp->cli_component;
            if (c3->priority > component->priority) {
                continue;
            }
            prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                "mca:oob:select: Inserting component");
            c2 = PRRTE_NEW(prrte_mca_base_component_list_item_t);
            c2->cli_component = (prrte_mca_base_component_t*)component;
            prrte_list_insert_pos(&prrte_oob_base.actives,
                                 &cmp->super, &c2->super);
            added = true;
            break;
        }
        if (!added) {
            /* add to end */
            prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                                "mca:oob:select: Adding component to end");
            c2 = PRRTE_NEW(prrte_mca_base_component_list_item_t);
            c2->cli_component = (prrte_mca_base_component_t*)component;
            prrte_list_append(&prrte_oob_base.actives, &c2->super);
        }
    }

    if (0 == prrte_list_get_size(&prrte_oob_base.actives)) {
        /* no support available means we really cannot run */
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                            "mca:oob:select: Init failed to return any available transports");
        prrte_show_help("help-oob-base.txt", "no-interfaces-avail", true);
        return PRRTE_ERR_SILENT;
    }

    /* provide them an index so we can track their usability in a bitmap */
    i=0;
    PRRTE_LIST_FOREACH(cmp, &prrte_oob_base.actives, prrte_mca_base_component_list_item_t) {
        c3 = (prrte_oob_base_component_t *) cmp->cli_component;
        c3->idx = i++;
    }

    prrte_output_verbose(5, prrte_oob_base_framework.framework_output,
                        "mca:oob:select: Found %d active transports",
                        (int)prrte_list_get_size(&prrte_oob_base.actives));
    return PRRTE_SUCCESS;
}
