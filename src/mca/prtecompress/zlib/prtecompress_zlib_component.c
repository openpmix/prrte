/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "constants.h"
#include "src/mca/prtecompress/prtecompress.h"
#include "src/mca/prtecompress/base/base.h"
#include "prtecompress_zlib.h"

/*
 * Public string for version number
 */
const char *prte_prtecompress_zlib_component_version_string =
"PRTE COMPRESS zlib MCA component version " PRTE_VERSION;
/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prte_prtecompress_base_component_t prte_prtecompress_zlib_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component itzlib
     */
    .base_version = {
        PRTE_COMPRESS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .mca_component_name = "zlib",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),

        .mca_query_component = prte_prtecompress_zlib_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    }
};

/*
 * Zlib module
 */
static prte_prtecompress_base_module_t loc_module = {
    /** Initialization Function */
    .init = prte_prtecompress_zlib_module_init,
    /** Finalization Function */
    .finalize = prte_prtecompress_zlib_module_finalize,

    /** Compress Function */
    .compress_block = prte_prtecompress_zlib_compress_block,

    /** Deprtecompress Function */
    .decompress_block = prte_prtecompress_zlib_uncompress_block,
};

int prte_prtecompress_zlib_component_query(prte_mca_base_module_t **module, int *priority)
{
    *module   = (prte_mca_base_module_t *)&loc_module;
    *priority = 10;
    return PRTE_SUCCESS;
}

