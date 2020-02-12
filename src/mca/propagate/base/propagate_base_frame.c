/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
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

#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"

#include "src/mca/propagate/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */
#include "src/mca/propagate/base/static-components.h"

/*
 * Global
 */
prrte_propagate_base_module_t prrte_propagate = {
    NULL,
    NULL,
    NULL,
    NULL
};

static int prrte_propagate_base_close(void)
{
    /* Close the selected component */
    if( NULL != prrte_propagate.finalize ) {
        prrte_propagate.finalize();
    }

    return prrte_mca_base_framework_components_close(&prrte_propagate_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_propagate_base_open(prrte_mca_base_open_flag_t flags)
{
    return prrte_mca_base_framework_components_open(&prrte_propagate_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, propagate, "PROPAGATE", NULL,
        prrte_propagate_base_open,
        prrte_propagate_base_close,
        prrte_propagate_base_static_components, 0);

