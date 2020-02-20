/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
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
#include "src/mca/prtecompress/prtecompress.h"
#include "src/mca/prtecompress/base/base.h"
#include "prtecompress_zlib.h"

/*
 * Public string for version number
 */
const char *prrte_prtecompress_zlib_component_version_string =
"PRRTE COMPRESS zlib MCA component version " PRRTE_VERSION;
/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_prtecompress_base_component_t prrte_prtecompress_zlib_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component itzlib
     */
    .base_version = {
        PRRTE_COMPRESS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .mca_component_name = "zlib",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        .mca_query_component = prrte_prtecompress_zlib_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    }
};

/*
 * Zlib module
 */
static prrte_prtecompress_base_module_t loc_module = {
    /** Initialization Function */
    .init = prrte_prtecompress_zlib_module_init,
    /** Finalization Function */
    .finalize = prrte_prtecompress_zlib_module_finalize,

    /** Compress Function */
    .compress_block = prrte_prtecompress_zlib_compress_block,

    /** Deprtecompress Function */
    .decompress_block = prrte_prtecompress_zlib_uncompress_block,
};

int prrte_prtecompress_zlib_component_query(prrte_mca_base_module_t **module, int *priority)
{
    *module   = (prrte_mca_base_module_t *)&loc_module;
    *priority = 10;
    return PRRTE_SUCCESS;
}

