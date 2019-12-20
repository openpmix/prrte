/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/mca/base/base.h"
#include "src/mca/compress/base/base.h"

#include "src/mca/compress/base/static-components.h"

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

prrte_compress_base_module_t prrte_compress = {
    NULL, /* init             */
    NULL, /* finalize         */
    NULL, /* compress         */
    NULL, /* compress_nb      */
    NULL, /* decompress       */
    NULL,  /* decompress_nb    */
    compress_block,
    decompress_block
};
prrte_compress_base_t prrte_compress_base = {0};

prrte_compress_base_component_t prrte_compress_base_selected_component = {{0}};

static int prrte_compress_base_register(prrte_mca_base_register_flag_t flags);

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, compress, "COMPRESS MCA",
                                 prrte_compress_base_register, prrte_compress_base_open,
                                 prrte_compress_base_close, prrte_compress_base_static_components, 0);

static int prrte_compress_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_compress_base.compress_limit = 4096;
    (void) prrte_mca_base_var_register("prrte", "compress", "base", "limit",
                                       "Threshold beyond which data will be compressed",
                                       PRRTE_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_compress_base.compress_limit);

    return PRRTE_SUCCESS;
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
int prrte_compress_base_open(prrte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_compress_base_framework, flags);
}

int prrte_compress_base_close(void)
{
    /* Call the component's finalize routine */
    if( NULL != prrte_compress.finalize ) {
        prrte_compress.finalize();
    }

    /* Close all available modules that are open */
    return prrte_mca_base_framework_components_close (&prrte_compress_base_framework, NULL);
}
