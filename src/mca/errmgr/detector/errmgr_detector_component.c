/*
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "errmgr_detector.h"

/*
 * Public string for version number
 */
const char *prrte_errmgr_detector_component_version_string =
"PRRTE ERRMGR detector MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int errmgr_detector_register(void);
static int errmgr_detector_open(void);
static int errmgr_detector_close(void);
static int errmgr_detector_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_errmgr_base_component_t prrte_errmgr_detector_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component detector
     */
    .base_version = {
        PRRTE_ERRMGR_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "detector",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = errmgr_detector_open,
        .mca_close_component = errmgr_detector_close,
        .mca_query_component = errmgr_detector_component_query,
        .mca_register_component_params = errmgr_detector_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int errmgr_detector_register(void)
{
    prrte_mca_base_component_t *c = &prrte_errmgr_detector_component.base_version;
    if ( PRRTE_PROC_IS_DAEMON )
        my_priority = 1005;
    else
        my_priority = 0;
    (void) prrte_mca_base_component_var_register(c, "priority",
            "Priority of the detector errmgr component",
            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
            PRRTE_INFO_LVL_9,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    prrte_errmgr_detector_enable_flag = true;
    (void) prrte_mca_base_component_var_register(c, "enable",
            "Enable/disable detector in errmgr component",
            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
            PRRTE_INFO_LVL_9,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_errmgr_detector_enable_flag);

    prrte_errmgr_heartbeat_period = 5.0;
    (void) prrte_mca_base_component_var_register(c, "heartbeat_period",
            "Set heartbeat period for ring detector in errmgr component",
            PRRTE_MCA_BASE_VAR_TYPE_DOUBLE, NULL, 0, 0,
            PRRTE_INFO_LVL_9,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_errmgr_heartbeat_period);

    prrte_errmgr_heartbeat_timeout = 10.0;
    (void) prrte_mca_base_component_var_register(c, "heartbeat_timeout",
            "Set heartbeat timeout for ring detector in errmgr component",
            PRRTE_MCA_BASE_VAR_TYPE_DOUBLE, NULL, 0, 0,
            PRRTE_INFO_LVL_9,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_errmgr_heartbeat_timeout);

    return PRRTE_SUCCESS;
}

static int errmgr_detector_open(void)
{
    return PRRTE_SUCCESS;
}

static int errmgr_detector_close(void)
{
    return PRRTE_SUCCESS;
}

static int errmgr_detector_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* used by DVM masters */
    if ( PRRTE_PROC_IS_DAEMON ) {
        *priority = my_priority;
        *module = (prrte_mca_base_module_t *)&prrte_errmgr_detector_module;
        return PRRTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return PRRTE_ERROR;
}
