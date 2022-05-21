/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

#include "src/rml/rml.h"
#include "src/mca/state/state.h"

#include "src/mca/propagate/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */
#include "src/mca/propagate/base/static-components.h"

/*
 * Global
 */
prte_propagate_base_module_t prte_propagate = {NULL, NULL, NULL, NULL};

static int prte_propagate_base_close(void)
{
    /* Close the selected component */
    if (NULL != prte_propagate.finalize) {
        prte_propagate.finalize();
    }

    return prte_mca_base_framework_components_close(&prte_propagate_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_propagate_base_open(prte_mca_base_open_flag_t flags)
{
    return prte_mca_base_framework_components_open(&prte_propagate_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, propagate, "PROPAGATE", NULL, prte_propagate_base_open,
                                prte_propagate_base_close, prte_propagate_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);
