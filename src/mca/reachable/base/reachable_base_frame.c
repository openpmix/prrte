/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "src/include/constants.h"

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/mca/reachable/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * components and the definition of an array of pointers to each
 * module's public mca_base_module_t struct.
 */

#include "src/mca/reachable/base/static-components.h"

prrte_reachable_base_module_t prrte_reachable = {0};

static int prrte_reachable_base_frame_register(prrte_mca_base_register_flag_t flags)
{
    return PRRTE_SUCCESS;
}

static int prrte_reachable_base_frame_close(void)
{
    return prrte_mca_base_framework_components_close(&prrte_reachable_base_framework, NULL);
}

static int prrte_reachable_base_frame_open(prrte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_reachable_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, reachable, "PRRTE Reachability Framework",
                                 prrte_reachable_base_frame_register,
                                 prrte_reachable_base_frame_open,
                                 prrte_reachable_base_frame_close,
                                 prrte_reachable_base_static_components, 0);
