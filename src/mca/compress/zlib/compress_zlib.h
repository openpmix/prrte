/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#ifndef MCA_COMPRESS_ZLIB_EXPORT_H
#define MCA_COMPRESS_ZLIB_EXPORT_H

#include "prrte_config.h"

#include "src/util/output.h"

#include "src/mca/mca.h"
#include "src/mca/compress/compress.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    extern prrte_compress_base_component_t prrte_compress_zlib_component;

    int prrte_compress_zlib_component_query(prrte_mca_base_module_t **module, int *priority);

    /*
     * Module functions
     */
    int prrte_compress_zlib_module_init(void);
    int prrte_compress_zlib_module_finalize(void);

    /*
     * Actual funcationality
     */
    bool prrte_compress_zlib_compress_block(uint8_t *inbytes,
                                           size_t inlen,
                                           uint8_t **outbytes,
                                           size_t *olen);
    bool prrte_compress_zlib_uncompress_block(uint8_t **outbytes, size_t olen,
                                             uint8_t *inbytes, size_t len);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* MCA_COMPRESS_ZLIB_EXPORT_H */
