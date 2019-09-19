/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
 * The PRRTE Compress framework has been created to provide an abstract interface
 * to the compression agent library on the host machine. This fromework is useful
 * when distributing files that can be compressed before sending to dimish the
 * load on the network.
 *
 */

#ifndef MCA_COMPRESS_H
#define MCA_COMPRESS_H

#include "prrte_config.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/class/prrte_object.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/**
 * Module initialization function.
 * Returns PRRTE_SUCCESS
 */
typedef int (*prrte_compress_base_module_init_fn_t)
     (void);

/**
 * Module finalization function.
 * Returns PRRTE_SUCCESS
 */
typedef int (*prrte_compress_base_module_finalize_fn_t)
     (void);

/**
 * Compress the file provided
 *
 * Arguments:
 *   fname   = Filename to compress
 *   cname   = Compressed filename
 *   postfix = postfix added to filename to create compressed filename
 * Returns:
 *   PRRTE_SUCCESS on success, ow PRRTE_ERROR
 */
typedef int (*prrte_compress_base_module_compress_fn_t)
    (char * fname, char **cname, char **postfix);

typedef int (*prrte_compress_base_module_compress_nb_fn_t)
    (char * fname, char **cname, char **postfix, pid_t *child_pid);

/**
 * Decompress the file provided
 *
 * Arguments:
 *   fname = Filename to compress
 *   cname = Compressed filename
 * Returns:
 *   PRRTE_SUCCESS on success, ow PRRTE_ERROR
 */
typedef int (*prrte_compress_base_module_decompress_fn_t)
    (char * cname, char **fname);
typedef int (*prrte_compress_base_module_decompress_nb_fn_t)
    (char * cname, char **fname, pid_t *child_pid);

/**
 * Compress a string
 *
 * Arguments:
 *
 */
typedef bool (*prrte_compress_base_module_compress_string_fn_t)(uint8_t *inbytes,
                                                               size_t inlen,
                                                               uint8_t **outbytes,
                                                               size_t *olen);
typedef bool (*prrte_compress_base_module_decompress_string_fn_t)(uint8_t **outbytes, size_t olen,
                                                                 uint8_t *inbytes, size_t len);


/**
 * Structure for COMPRESS components.
 */
struct prrte_compress_base_component_2_0_0_t {
    /** MCA base component */
    prrte_mca_base_component_t base_version;
    /** MCA base data */
    prrte_mca_base_component_data_t base_data;

    /** Verbosity Level */
    int verbose;
    /** Output Handle for prrte_output */
    int output_handle;
    /** Default Priority */
    int priority;
};
typedef struct prrte_compress_base_component_2_0_0_t prrte_compress_base_component_2_0_0_t;
typedef struct prrte_compress_base_component_2_0_0_t prrte_compress_base_component_t;

/**
 * Structure for COMPRESS modules
 */
struct prrte_compress_base_module_1_0_0_t {
    /** Initialization Function */
    prrte_compress_base_module_init_fn_t           init;
    /** Finalization Function */
    prrte_compress_base_module_finalize_fn_t       finalize;

    /** Compress interface */
    prrte_compress_base_module_compress_fn_t       compress;
    prrte_compress_base_module_compress_nb_fn_t    compress_nb;

    /** Decompress Interface */
    prrte_compress_base_module_decompress_fn_t     decompress;
    prrte_compress_base_module_decompress_nb_fn_t  decompress_nb;

    /* COMPRESS STRING */
    prrte_compress_base_module_compress_string_fn_t      compress_block;
    prrte_compress_base_module_decompress_string_fn_t    decompress_block;
};
typedef struct prrte_compress_base_module_1_0_0_t prrte_compress_base_module_1_0_0_t;
typedef struct prrte_compress_base_module_1_0_0_t prrte_compress_base_module_t;

PRRTE_EXPORT extern prrte_compress_base_module_t prrte_compress;

/**
 * Macro for use in components that are of type COMPRESS
 */
#define PRRTE_COMPRESS_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("compress", 2, 0, 0)

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PRRTE_COMPRESS_H */

