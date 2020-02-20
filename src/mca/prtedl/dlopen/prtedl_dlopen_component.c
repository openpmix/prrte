/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
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
#include "src/mca/prtedl/prtedl.h"
#include "src/util/argv.h"

#include "prtedl_dlopen.h"


/*
 * Public string showing the sysinfo ompi_linux component version number
 */
const char *prrte_prtedl_dlopen_component_version_string =
    "PRRTE prtedl dlopen MCA component version " PRRTE_VERSION;


/*
 * Local functions
 */
static int dlopen_component_register(void);
static int dlopen_component_open(void);
static int dlopen_component_close(void);
static int dlopen_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prrte_prtedl_dlopen_component_t prrte_prtedl_dlopen_component = {

    /* Fill in the mca_prtedl_base_component_t */
    .base = {

        /* First, the mca_component_t struct containing meta information
           about the component itself */
        .base_version = {
            PRRTE_DL_BASE_VERSION_1_0_0,

            /* Component name and version */
            .mca_component_name = "dlopen",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component functions */
            .mca_register_component_params = dlopen_component_register,
            .mca_open_component = dlopen_component_open,
            .mca_close_component = dlopen_component_close,
            .mca_query_component = dlopen_component_query,
        },

        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        /* The prtedl framework members */
        .priority = 80
    },
};


static int dlopen_component_register(void)
{
    int ret;

    prrte_prtedl_dlopen_component.filename_suffixes_mca_storage = ".so,.dylib,.dll,.sl";
    ret =
        prrte_mca_base_component_var_register(&prrte_prtedl_dlopen_component.base.base_version,
                                        "filename_suffixes",
                                        "Comma-delimited list of filename suffixes that the PRRTE dlopen component will try",
                                        PRRTE_MCA_BASE_VAR_TYPE_STRING,
                                        NULL,
                                        0,
                                        PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                        PRRTE_INFO_LVL_5,
                                        PRRTE_MCA_BASE_VAR_SCOPE_LOCAL,
                                        &prrte_prtedl_dlopen_component.filename_suffixes_mca_storage);
    if (ret < 0) {
        return ret;
    }
    prrte_prtedl_dlopen_component.filename_suffixes =
        prrte_argv_split(prrte_prtedl_dlopen_component.filename_suffixes_mca_storage,
                        ',');

    return PRRTE_SUCCESS;
}

static int dlopen_component_open(void)
{
    return PRRTE_SUCCESS;
}


static int dlopen_component_close(void)
{
    if (NULL != prrte_prtedl_dlopen_component.filename_suffixes) {
        prrte_argv_free(prrte_prtedl_dlopen_component.filename_suffixes);
        prrte_prtedl_dlopen_component.filename_suffixes = NULL;
    }

    return PRRTE_SUCCESS;
}


static int dlopen_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* The priority value is somewhat meaningless here; by
       src/mca/prtedl/configure.m4, there's at most one component
       available. */
    *priority = prrte_prtedl_dlopen_component.base.priority;
    *module = &prrte_prtedl_dlopen_module.super;

    return PRRTE_SUCCESS;
}
