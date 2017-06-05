/*
 * Copyright (c) 2017-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "orte_config.h"
#include "orte/constants.h"

#include "orte/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/base.h"

#include "orte/mca/rml/rml.h"
#include "orte/mca/state/state.h"

#include "orte/mca/propagate/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */
#include "orte/mca/propagate/base/static-components.h"

/*
 * Global
 */
orte_propagate_base_module_t orte_propagate = {
    NULL,
    NULL,
    NULL,
    NULL
};

static int orte_propagate_base_close(void)
{
    /* Close the selected component */
    if( NULL != orte_propagate.finalize ) {
        orte_propagate.finalize();
    }

    return mca_base_framework_components_close(&orte_propagate_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int orte_propagate_base_open(mca_base_open_flag_t flags)
{
    return mca_base_framework_components_open(&orte_propagate_base_framework, flags);
}

MCA_BASE_FRAMEWORK_DECLARE(orte, propagate, "PROPAGATE", NULL,
        orte_propagate_base_open,
        orte_propagate_base_close,
        mca_propagate_base_static_components, 0);

