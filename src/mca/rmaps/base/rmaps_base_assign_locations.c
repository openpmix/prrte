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
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"


int prrte_rmaps_base_assign_locations(prrte_job_t *jdata)
{
    int rc;
    prrte_rmaps_base_selected_module_t *mod;

    prrte_output_verbose(5, prrte_rmaps_base_framework.framework_output,
                        "mca:rmaps: assigning locations for job %s",
                        PRRTE_JOBID_PRINT(jdata->jobid));

    /* cycle thru the available mappers until one agrees to assign
     * locations for the job
     */
    if (1 == prrte_list_get_size(&prrte_rmaps_base.selected_modules)) {
        /* forced selection */
        mod = (prrte_rmaps_base_selected_module_t*)prrte_list_get_first(&prrte_rmaps_base.selected_modules);
        jdata->map->req_mapper = strdup(mod->component->mca_component_name);
    }
    PRRTE_LIST_FOREACH(mod, &prrte_rmaps_base.selected_modules, prrte_rmaps_base_selected_module_t) {
        if (NULL == mod->module->assign_locations) {
            continue;
        }
        if (PRRTE_SUCCESS == (rc = mod->module->assign_locations(jdata))) {
            return rc;
        }
        /* mappers return "next option" if they didn't attempt to
         * process the job. anything else is a true error.
         */
        if (PRRTE_ERR_TAKE_NEXT_OPTION != rc) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* if we get here without doing the assignments, then that's an error */
    prrte_show_help("help-prrte-rmaps-base.txt", "failed-assignments", true,
                   prrte_process_info.nodename,
                   prrte_rmaps_base_print_mapping(jdata->map->mapping));
    return PRRTE_ERROR;
}
