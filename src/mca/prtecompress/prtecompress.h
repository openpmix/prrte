/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 *
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
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
 * Compression Framework
 *
 * General Description:
 *
 * The PRTE Compress framework has been created to provide an abstract interface
 * to the prtecompression agent library on the host machine. This fromework is useful
 * when distributing files that can be prtecompressed before sending to dimish the
 * load on the network.
 *
 */

#ifndef MCA_COMPRESS_H
#define MCA_COMPRESS_H

#include "prte_config.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/class/prte_object.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/**
 * Module initialization function.
 * Returns PRTE_SUCCESS
 */
typedef int (*prte_prtecompress_base_module_init_fn_t)
     (void);

/**
 * Module finalization function.
 * Returns PRTE_SUCCESS
 */
typedef int (*prte_prtecompress_base_module_finalize_fn_t)
     (void);

/**
 * Compress the file provided
 *
 * Arguments:
 *   fname   = Filename to prtecompress
 *   cname   = Compressed filename
 *   postfix = postfix added to filename to create prtecompressed filename
 * Returns:
 *   PRTE_SUCCESS on success, ow PRTE_ERROR
 */
typedef int (*prte_prtecompress_base_module_compress_fn_t)
    (char * fname, char **cname, char **postfix);

typedef int (*prte_prtecompress_base_module_compress_nb_fn_t)
    (char * fname, char **cname, char **postfix, pid_t *child_pid);

/**
 * Deprtecompress the file provided
 *
 * Arguments:
 *   fname = Filename to prtecompress
 *   cname = Compressed filename
 * Returns:
 *   PRTE_SUCCESS on success, ow PRTE_ERROR
 */
typedef int (*prte_prtecompress_base_module_decompress_fn_t)
    (char * cname, char **fname);
typedef int (*prte_prtecompress_base_module_decompress_nb_fn_t)
    (char * cname, char **fname, pid_t *child_pid);

/**
 * Compress a string
 *
 * Arguments:
 *
 */
typedef bool (*prte_prtecompress_base_module_compress_string_fn_t)(uint8_t *inbytes,
                                                               size_t inlen,
                                                               uint8_t **outbytes,
                                                               size_t *olen);
typedef bool (*prte_prtecompress_base_module_decompress_string_fn_t)(uint8_t **outbytes, size_t olen,
                                                                 uint8_t *inbytes, size_t len);


/**
 * Structure for COMPRESS components.
 */
struct prte_prtecompress_base_component_2_0_0_t {
    /** MCA base component */
    prte_mca_base_component_t base_version;
    /** MCA base data */
    prte_mca_base_component_data_t base_data;

    /** Verbosity Level */
    int verbose;
    /** Output Handle for prte_output */
    int output_handle;
    /** Default Priority */
    int priority;
};
typedef struct prte_prtecompress_base_component_2_0_0_t prte_prtecompress_base_component_2_0_0_t;
typedef struct prte_prtecompress_base_component_2_0_0_t prte_prtecompress_base_component_t;

/**
 * Structure for COMPRESS modules
 */
struct prte_prtecompress_base_module_1_0_0_t {
    /** Initialization Function */
    prte_prtecompress_base_module_init_fn_t           init;
    /** Finalization Function */
    prte_prtecompress_base_module_finalize_fn_t       finalize;

    /** Compress interface */
    prte_prtecompress_base_module_compress_fn_t       compress;
    prte_prtecompress_base_module_compress_nb_fn_t    compress_nb;

    /** Deprtecompress Interface */
    prte_prtecompress_base_module_decompress_fn_t     decompress;
    prte_prtecompress_base_module_decompress_nb_fn_t  decompress_nb;

    /* COMPRESS STRING */
    prte_prtecompress_base_module_compress_string_fn_t      compress_block;
    prte_prtecompress_base_module_decompress_string_fn_t    decompress_block;
};
typedef struct prte_prtecompress_base_module_1_0_0_t prte_prtecompress_base_module_1_0_0_t;
typedef struct prte_prtecompress_base_module_1_0_0_t prte_prtecompress_base_module_t;

PRTE_EXPORT extern prte_prtecompress_base_module_t prte_compress;

/**
 * Macro for use in components that are of type COMPRESS
 */
#define PRTE_COMPRESS_BASE_VERSION_2_0_0 \
    PRTE_MCA_BASE_VERSION_2_1_0("prtecompress", 2, 0, 0)

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PRTE_COMPRESS_H */

