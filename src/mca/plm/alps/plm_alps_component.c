/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/base/prrte_mca_base_var.h"

#include "src/runtime/prrte_globals.h"

#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "plm_alps.h"


/*
 * Public string showing the plm ompi_alps component version number
 */
const char *prrte_plm_alps_component_version_string =
  "PRRTE alps plm MCA component version " PRRTE_VERSION;


/*
 * Local functions
 */
static int plm_alps_register(void);
static int plm_alps_open(void);
static int plm_alps_close(void);
static int prrte_plm_alps_component_query(prrte_mca_base_module_t **module, int *priority);


/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prrte_plm_alps_component_t prrte_plm_alps_component = {

    {
        /* First, the mca_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_PLM_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "alps",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = plm_alps_open,
            .mca_close_component = plm_alps_close,
            .mca_query_component = prrte_plm_alps_component_query,
            .mca_register_component_params = plm_alps_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }

    /* Other prrte_plm_alps_component_t items -- left uninitialized
       here; will be initialized in plm_alps_open() */
};


static int plm_alps_register(void)
{
    prrte_mca_base_component_t *comp = &prrte_plm_alps_component.super.base_version;

    prrte_plm_alps_component.debug = false;
    (void) prrte_mca_base_component_var_register (comp, "debug", "Enable debugging of alps plm",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_plm_alps_component.debug);

    if (prrte_plm_alps_component.debug == 0) {
        prrte_plm_alps_component.debug = prrte_debug_flag;
    }

    prrte_plm_alps_component.priority = 100;
    (void) prrte_mca_base_component_var_register (comp, "priority", "Default selection priority",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_plm_alps_component.priority);

    prrte_plm_alps_component.aprun_cmd = "aprun";
    (void) prrte_mca_base_component_var_register (comp, "aprun", "Command to run instead of aprun",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_plm_alps_component.aprun_cmd);

    prrte_plm_alps_component.custom_args = NULL;
    (void) prrte_mca_base_component_var_register (comp, "args", "Custom arguments to aprun",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_plm_alps_component.custom_args);
    return PRRTE_SUCCESS;
}

static int plm_alps_open(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_plm_alps_component_query(prrte_mca_base_module_t **module, int *priority)
{
#if CRAY_WLM_DETECT
    char slurm[]="SLURM";
    char *wlm_detected = NULL;

    wlm_detected = wlm_detect_get_active();

    /*
     * The content of wlm_detected.h indicates wlm_detect_get_active
     * may return NULL upon failure.  Resort to the suggested plan
     * B in that event.
     */

    if (NULL == wlm_detected) {
        wlm_detected = (char *)wlm_detect_get_default();
        PRRTE_OUTPUT_VERBOSE((10, prrte_plm_base_framework.framework_output,
                             "%s plm:alps: wlm_detect_get_active returned NULL, using %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wlm_detected));

    }

    if((NULL != wlm_detected) && !strcmp(slurm, wlm_detected)) {
        /* we are in a Cray SLURM environment, so we don't want
         * this plm component */
        *priority = 0;
        *module = NULL;
        return PRRTE_ERROR;
    }
#endif

    *priority = prrte_plm_alps_component.priority;
    *module = (prrte_mca_base_module_t *) &prrte_plm_alps_module;
    PRRTE_OUTPUT_VERBOSE((1, prrte_plm_base_framework.framework_output,
                        "%s plm:alps: available for selection",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    return PRRTE_SUCCESS;
}


static int plm_alps_close(void)
{
    return PRRTE_SUCCESS;
}
