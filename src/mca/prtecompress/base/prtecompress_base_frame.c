/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/mca/base/base.h"
#include "src/mca/prtecompress/base/base.h"

#include "src/mca/prtecompress/base/static-components.h"

/*
 * Globals
 */
static bool compress_block(uint8_t *inbytes,
                           size_t inlen,
                           uint8_t **outbytes,
                           size_t *olen)
{
    return false;
}

static bool decompress_block(uint8_t **outbytes, size_t olen,
                             uint8_t *inbytes, size_t len)
{
    return false;
}

prte_prtecompress_base_module_t prte_compress = {
    NULL, /* init             */
    NULL, /* finalize         */
    NULL, /* compress         */
    NULL, /* compress_nb      */
    NULL, /* decompress       */
    NULL,  /* decompress_nb    */
    compress_block,
    decompress_block
};
prte_prtecompress_base_t prte_prtecompress_base = {0};

prte_prtecompress_base_component_t prte_prtecompress_base_selected_component = {{0}};

static int prte_prtecompress_base_register(prte_mca_base_register_flag_t flags);

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, prtecompress, "COMPRESS MCA",
                                 prte_prtecompress_base_register, prte_prtecompress_base_open,
                                 prte_prtecompress_base_close, prte_prtecompress_base_static_components, 0);

static int prte_prtecompress_base_register(prte_mca_base_register_flag_t flags)
{
    prte_prtecompress_base.prtecompress_limit = 4096;
    (void) prte_mca_base_var_register("prte", "prtecompress", "base", "limit",
                                       "Threshold beyond which data will be prtecompressed",
                                       PRTE_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0, 0, PRTE_INFO_LVL_3,
                                       PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_prtecompress_base.prtecompress_limit);

    return PRTE_SUCCESS;
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
int prte_prtecompress_base_open(prte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_prtecompress_base_framework, flags);
}

int prte_prtecompress_base_close(void)
{
    /* Call the component's finalize routine */
    if( NULL != prte_compress.finalize ) {
        prte_compress.finalize();
    }

    /* Close all available modules that are open */
    return prte_mca_base_framework_components_close (&prte_prtecompress_base_framework, NULL);
}
