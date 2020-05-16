/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
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

/**
 * @file
 *
 * ZLIB COMPRESS component
 *
 * Uses the zlib library
 */

#ifndef PRTE_MCA_COMPRESS_ZLIB_EXPORT_H
#define PRTE_MCA_COMPRESS_ZLIB_EXPORT_H

#include "prte_config.h"

#include "src/util/output.h"

#include "src/mca/mca.h"
#include "src/mca/prtecompress/prtecompress.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    extern prte_prtecompress_base_component_t prte_prtecompress_zlib_component;

    int prte_prtecompress_zlib_component_query(prte_mca_base_module_t **module, int *priority);

    /*
     * Module functions
     */
    int prte_prtecompress_zlib_module_init(void);
    int prte_prtecompress_zlib_module_finalize(void);

    /*
     * Actual funcationality
     */
    bool prte_prtecompress_zlib_compress_block(uint8_t *inbytes,
                                           size_t inlen,
                                           uint8_t **outbytes,
                                           size_t *olen);
    bool prte_prtecompress_zlib_uncompress_block(uint8_t **outbytes, size_t olen,
                                             uint8_t *inbytes, size_t len);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PRTE_MCA_COMPRESS_ZLIB_EXPORT_H */
