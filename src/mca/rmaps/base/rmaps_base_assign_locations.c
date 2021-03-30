/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"

int prte_rmaps_base_assign_locations(prte_job_t *jdata)
{
    int rc;
    prte_rmaps_base_selected_module_t *mod;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: assigning locations for job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* cycle thru the available mappers until one agrees to assign
     * locations for the job
     */
    if (1 == prte_list_get_size(&prte_rmaps_base.selected_modules)) {
        /* forced selection */
        mod = (prte_rmaps_base_selected_module_t *) prte_list_get_first(
            &prte_rmaps_base.selected_modules);
        jdata->map->req_mapper = strdup(mod->component->mca_component_name);
    }
    PRTE_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules, prte_rmaps_base_selected_module_t)
    {
        if (NULL == mod->module->assign_locations) {
            continue;
        }
        if (PRTE_SUCCESS == (rc = mod->module->assign_locations(jdata))) {
            return rc;
        }
        /* mappers return "next option" if they didn't attempt to
         * process the job. anything else is a true error.
         */
        if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* if we get here without doing the assignments, then that's an error */
    prte_show_help("help-prte-rmaps-base.txt", "failed-assignments", true,
                   prte_process_info.nodename, prte_rmaps_base_print_mapping(jdata->map->mapping));
    return PRTE_ERROR;
}
