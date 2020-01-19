/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/show_help.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/mca/schizo/base/base.h"
/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/schizo/base/static-components.h"

/*
 * Global variables
 */
prrte_schizo_base_t prrte_schizo_base = {{{0}}};
prrte_schizo_base_module_t prrte_schizo = {
    .define_cli = prrte_schizo_base_define_cli,
    .parse_cli = prrte_schizo_base_parse_cli,
    .parse_proxy_cli = prrte_schizo_base_parse_proxy_cli,
    .parse_env = prrte_schizo_base_parse_env,
    .allow_run_as_root = prrte_schizo_base_allow_run_as_root,
    .wrap_args = prrte_schizo_base_wrap_args,
    .setup_app = prrte_schizo_base_setup_app,
    .setup_fork = prrte_schizo_base_setup_fork,
    .setup_child = prrte_schizo_base_setup_child,
    .get_remaining_time = prrte_schizo_base_get_remaining_time,
    .finalize = prrte_schizo_base_finalize
};

static char *personalities = NULL;

static int prrte_schizo_base_register(prrte_mca_base_register_flag_t flags)
{
    /* pickup any defined personalities */
    personalities = strdup("prrte");
    prrte_mca_base_var_register("prrte", "schizo", "base", "personalities",
                                "Comma-separated list of personalities",
                                PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &personalities);
    return PRRTE_SUCCESS;
}

static int prrte_schizo_base_close(void)
{
    /* cleanup globals */
    PRRTE_LIST_DESTRUCT(&prrte_schizo_base.active_modules);
    if (NULL != prrte_schizo_base.personalities) {
        prrte_argv_free(prrte_schizo_base.personalities);
    }

    return prrte_mca_base_framework_components_close(&prrte_schizo_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_schizo_base_open(prrte_mca_base_open_flag_t flags)
{
    int rc;

    /* init the globals */
    PRRTE_CONSTRUCT(&prrte_schizo_base.active_modules, prrte_list_t);
    prrte_schizo_base.personalities = NULL;
    if (NULL != personalities) {
        prrte_schizo_base.personalities = prrte_argv_split(personalities, ',');
    }

    /* Open up all available components */
    rc = prrte_mca_base_framework_components_open(&prrte_schizo_base_framework, flags);

    /* All done */
    return rc;
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, schizo, "PRRTE Schizo Subsystem",
                                 prrte_schizo_base_register,
                                 prrte_schizo_base_open, prrte_schizo_base_close,
                                 prrte_schizo_base_static_components, 0);

PRRTE_CLASS_INSTANCE(prrte_schizo_base_active_module_t,
                   prrte_list_item_t,
                   NULL, NULL);
