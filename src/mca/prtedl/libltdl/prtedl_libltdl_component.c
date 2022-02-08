/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, Inc.  All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "constants.h"
#include "src/mca/base/mca_base_var.h"
#include "src/mca/prtedl/prtedl.h"
#include "src/util/pmix_argv.h"

#include "prtedl_libltdl.h"

/*
 * Public string showing the sysinfo ompi_linux component version number
 */
const char *prte_prtedl_libltdl_component_version_string
    = "PRTE prtedl libltdl MCA component version " PRTE_VERSION;

/*
 * Local functions
 */
static int libltdl_component_register(void);
static int libltdl_component_open(void);
static int libltdl_component_close(void);
static int libltdl_component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_prtedl_libltdl_component_t prte_prtedl_libltdl_component = {

    /* Fill in the mca_prtedl_base_component_t */
    .base = {

        /* First, the mca_component_t struct containing meta information
           about the component itself */
        .base_version = {
            PRTE_DL_BASE_VERSION_1_0_0,

            /* Component name and version */
            .mca_component_name = "libltdl",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),

            /* Component functions */
            .mca_register_component_params = libltdl_component_register,
            .mca_open_component = libltdl_component_open,
            .mca_close_component = libltdl_component_close,
            .mca_query_component = libltdl_component_query,
        },

        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        /* The prtedl framework members */
        .priority = 50
    }

    /* Now fill in the libltdl component-specific members */
};

static int libltdl_component_register(void)
{
    /* Register an info param indicating whether we have lt_dladvise
       support or not */
    bool supported = PRTE_INT_TO_BOOL(PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE);
    prte_mca_base_component_var_register(&prte_prtedl_libltdl_component.base.base_version,
                                         "have_lt_dladvise",
                                         "Whether the version of libltdl that this component is "
                                         "built against supports lt_dladvise functionality or not",
                                         PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                         PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY, PRTE_INFO_LVL_7,
                                         PRTE_MCA_BASE_VAR_SCOPE_CONSTANT, &supported);

    return PRTE_SUCCESS;
}

static int libltdl_component_open(void)
{
    if (lt_prtedlinit()) {
        return PRTE_ERROR;
    }

#if PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE
    prte_prtedl_libltdl_component_t *c = &prte_prtedl_libltdl_component;

    if (lt_dladvise_init(&c->advise_private_noext)) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_private_ext) || lt_dladvise_ext(&c->advise_private_ext)) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_noext) || lt_dladvise_global(&c->advise_public_noext)) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_ext) || lt_dladvise_global(&c->advise_public_ext)
        || lt_dladvise_ext(&c->advise_public_ext)) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
#endif

    return PRTE_SUCCESS;
}

static int libltdl_component_close(void)
{
#if PRTE_DL_LIBLTDL_HAVE_LT_DLADVISE
    prte_prtedl_libltdl_component_t *c = &prte_prtedl_libltdl_component;

    lt_dladvise_destroy(&c->advise_private_noext);
    lt_dladvise_destroy(&c->advise_private_ext);
    lt_dladvise_destroy(&c->advise_public_noext);
    lt_dladvise_destroy(&c->advise_public_ext);
#endif

    lt_prtedlexit();

    return PRTE_SUCCESS;
}

static int libltdl_component_query(prte_mca_base_module_t **module, int *priority)
{
    /* The priority value is somewhat meaningless here; by
       src/mca/prtedl/configure.m4, there's at most one component
       available. */
    *priority = prte_prtedl_libltdl_component.base.priority;
    *module = &prte_prtedl_libltdl_module.super;

    return PRTE_SUCCESS;
}
