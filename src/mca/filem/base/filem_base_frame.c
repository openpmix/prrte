/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"

#include "src/mca/filem/base/static-components.h"

/*
 * Globals
 */
PRRTE_EXPORT prrte_filem_base_module_t prrte_filem = {
    .filem_init = prrte_filem_base_module_init,
    .filem_finalize = prrte_filem_base_module_finalize,
    .put = prrte_filem_base_none_put,
    .put_nb = prrte_filem_base_none_put_nb,
    .get = prrte_filem_base_none_get,
    .get_nb = prrte_filem_base_none_get_nb,
    .rm = prrte_filem_base_none_rm,
    .rm_nb = prrte_filem_base_none_rm_nb,
    .wait = prrte_filem_base_none_wait,
    .wait_all = prrte_filem_base_none_wait_all,
    .preposition_files = prrte_filem_base_none_preposition_files,
    .link_local_files = prrte_filem_base_none_link_local_files
};
bool prrte_filem_base_is_active = false;

static int prrte_filem_base_close(void)
{
    /* Close the selected component */
    if( NULL != prrte_filem.filem_finalize ) {
        prrte_filem.filem_finalize();
    }

    return prrte_mca_base_framework_components_close(&prrte_filem_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
static int prrte_filem_base_open(prrte_mca_base_open_flag_t flags)
{
     /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_filem_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, filem, NULL, NULL, prrte_filem_base_open, prrte_filem_base_close,
                                 prrte_filem_base_static_components, 0);
