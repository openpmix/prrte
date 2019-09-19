/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/mca/compress/compress.h"
#include "src/mca/compress/base/base.h"
#include "compress_zlib.h"

/*
 * Public string for version number
 */
const char *prrte_compress_zlib_component_version_string =
"PRRTE COMPRESS zlib MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int compress_zlib_register (void);
static int compress_zlib_open(void);
static int compress_zlib_close(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_compress_zlib_component_t mca_compress_zlib_component = {
    /* First do the base component stuff */
    {
        /* Handle the general mca_component_t struct containing
         *  meta information about the component itzlib
         */
        .base_version = {
            PRRTE_COMPRESS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "zlib",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = compress_zlib_open,
            .mca_close_component = compress_zlib_close,
            .mca_query_component = prrte_compress_zlib_component_query,
            .mca_register_component_params = compress_zlib_register
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        .verbose = 0,
        .output_handle = -1,
    }
};

/*
 * Zlib module
 */
static prrte_compress_base_module_t loc_module = {
    /** Initialization Function */
    .init = prrte_compress_zlib_module_init,
    /** Finalization Function */
    .finalize = prrte_compress_zlib_module_finalize,

    /** Compress Function */
    .compress_block = prrte_compress_zlib_compress_block,

    /** Decompress Function */
    .decompress_block = prrte_compress_zlib_uncompress_block,
};

static int compress_zlib_register (void)
{
    int ret;

    mca_compress_zlib_component.super.priority = 50;
    ret = prrte_mca_base_component_var_register (&mca_compress_zlib_component.super.base_version,
                                           "priority", "Priority of the COMPRESS zlib component "
                                           "(default: 50)", PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                           PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                           PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                           &mca_compress_zlib_component.super.priority);
    if (0 > ret) {
        return ret;
    }

    mca_compress_zlib_component.super.verbose = 0;
    ret = prrte_mca_base_component_var_register (&mca_compress_zlib_component.super.base_version,
                                           "verbose",
                                           "Verbose level for the COMPRESS zlib component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_SETTABLE,
                                           PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_LOCAL,
                                           &mca_compress_zlib_component.super.verbose);
    return (0 > ret) ? ret : PRRTE_SUCCESS;
}

static int compress_zlib_open(void)
{
    /* If there is a custom verbose level for this component than use it
     * otherwise take our parents level and output channel
     */
    if ( 0 != mca_compress_zlib_component.super.verbose) {
        mca_compress_zlib_component.super.output_handle = prrte_output_open(NULL);
        prrte_output_set_verbosity(mca_compress_zlib_component.super.output_handle,
                                  mca_compress_zlib_component.super.verbose);
    } else {
        mca_compress_zlib_component.super.output_handle = prrte_compress_base_framework.framework_output;
    }

    /*
     * Debug output
     */
    prrte_output_verbose(10, mca_compress_zlib_component.super.output_handle,
                        "compress:zlib: open()");
    prrte_output_verbose(20, mca_compress_zlib_component.super.output_handle,
                        "compress:zlib: open: priority = %d",
                        mca_compress_zlib_component.super.priority);
    prrte_output_verbose(20, mca_compress_zlib_component.super.output_handle,
                        "compress:zlib: open: verbosity = %d",
                        mca_compress_zlib_component.super.verbose);
    return PRRTE_SUCCESS;
}

static int compress_zlib_close(void)
{
    return PRRTE_SUCCESS;
}

int prrte_compress_zlib_component_query(prrte_mca_base_module_t **module, int *priority)
{
    *module   = (prrte_mca_base_module_t *)&loc_module;
    *priority = mca_compress_zlib_component.super.priority;

    return PRRTE_SUCCESS;
}

