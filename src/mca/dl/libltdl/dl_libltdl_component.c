/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, Inc.  All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
#include "src/mca/dl/dl.h"
#include "src/mca/base/mca_base_var.h"
#include "src/util/argv.h"

#include "dl_libltdl.h"


/*
 * Public string showing the sysinfo ompi_linux component version number
 */
const char *prrte_dl_libltdl_component_version_string =
    "PRRTE dl libltdl MCA component version " PRRTE_VERSION;


/*
 * Local functions
 */
static int libltdl_component_register(void);
static int libltdl_component_open(void);
static int libltdl_component_close(void);
static int libltdl_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prrte_dl_libltdl_component_t prrte_dl_libltdl_component = {

    /* Fill in the mca_dl_base_component_t */
    .base = {

        /* First, the mca_component_t struct containing meta information
           about the component itself */
        .base_version = {
            PRRTE_DL_BASE_VERSION_1_0_0,

            /* Component name and version */
            .mca_component_name = "libltdl",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component functions */
            .mca_register_component_params = libltdl_component_register,
            .mca_open_component = libltdl_component_open,
            .mca_close_component = libltdl_component_close,
            .mca_query_component = libltdl_component_query,
        },

        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        /* The dl framework members */
        .priority = 50
    }

    /* Now fill in the libltdl component-specific members */
};


static int libltdl_component_register(void)
{
    /* Register an info param indicating whether we have lt_dladvise
       support or not */
    bool supported = PRRTE_INT_TO_BOOL(PRRTE_DL_LIBLTDL_HAVE_LT_DLADVISE);
    prrte_mca_base_component_var_register(&prrte_dl_libltdl_component.base.base_version,
                                    "have_lt_dladvise",
                                    "Whether the version of libltdl that this component is built against supports lt_dladvise functionality or not",
                                    PRRTE_MCA_BASE_VAR_TYPE_BOOL,
                                    NULL,
                                    0,
                                    PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY,
                                    PRRTE_INFO_LVL_7,
                                    PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                    &supported);

    return PRRTE_SUCCESS;
}

static int libltdl_component_open(void)
{
    if (lt_dlinit()) {
        return PRRTE_ERROR;
    }

#if PRRTE_DL_LIBLTDL_HAVE_LT_DLADVISE
    prrte_dl_libltdl_component_t *c = &prrte_dl_libltdl_component;

    if (lt_dladvise_init(&c->advise_private_noext)) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_private_ext) ||
        lt_dladvise_ext(&c->advise_private_ext)) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_noext) ||
        lt_dladvise_global(&c->advise_public_noext)) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_ext) ||
        lt_dladvise_global(&c->advise_public_ext) ||
        lt_dladvise_ext(&c->advise_public_ext)) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
#endif

    return PRRTE_SUCCESS;
}


static int libltdl_component_close(void)
{
#if PRRTE_DL_LIBLTDL_HAVE_LT_DLADVISE
    prrte_dl_libltdl_component_t *c = &prrte_dl_libltdl_component;

    lt_dladvise_destroy(&c->advise_private_noext);
    lt_dladvise_destroy(&c->advise_private_ext);
    lt_dladvise_destroy(&c->advise_public_noext);
    lt_dladvise_destroy(&c->advise_public_ext);
#endif

    lt_dlexit();

    return PRRTE_SUCCESS;
}


static int libltdl_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* The priority value is somewhat meaningless here; by
       src/mca/dl/configure.m4, there's at most one component
       available. */
    *priority = prrte_dl_libltdl_component.base.priority;
    *module = &prrte_dl_libltdl_module.super;

    return PRRTE_SUCCESS;
}
