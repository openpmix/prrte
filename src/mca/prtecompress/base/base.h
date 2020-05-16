/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 *
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PRTE_COMPRESS_BASE_H
#define PRTE_COMPRESS_BASE_H

#include "prte_config.h"
#include "src/mca/prtecompress/prtecompress.h"
#include "src/util/prte_environ.h"

#include "src/mca/base/base.h"

/*
 * Global functions for MCA overall COMPRESS
 */

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    size_t prtecompress_limit;
} prte_prtecompress_base_t;

PRTE_EXPORT extern prte_prtecompress_base_t prte_prtecompress_base;

    /**
     * Initialize the COMPRESS MCA framework
     *
     * @retval PRTE_SUCCESS Upon success
     * @retval PRTE_ERROR   Upon failures
     *
     * This function is invoked during prte_init();
     */
    PRTE_EXPORT int prte_prtecompress_base_open(prte_mca_base_open_flag_t flags);

    /**
     * Select an available component.
     *
     * @retval PRTE_SUCCESS Upon Success
     * @retval PRTE_NOT_FOUND If no component can be selected
     * @retval PRTE_ERROR Upon other failure
     *
     */
    PRTE_EXPORT int prte_prtecompress_base_select(void);

    /**
     * Finalize the COMPRESS MCA framework
     *
     * @retval PRTE_SUCCESS Upon success
     * @retval PRTE_ERROR   Upon failures
     *
     * This function is invoked during prte_finalize();
     */
    PRTE_EXPORT int prte_prtecompress_base_close(void);

    /**
     * Globals
     */
    PRTE_EXPORT extern prte_mca_base_framework_t prte_prtecompress_base_framework;
    PRTE_EXPORT extern prte_prtecompress_base_component_t prte_prtecompress_base_selected_component;
    PRTE_EXPORT extern prte_prtecompress_base_module_t prte_compress;

    /**
     *
     */
    PRTE_EXPORT int prte_prtecompress_base_tar_create(char ** target);
    PRTE_EXPORT int prte_prtecompress_base_tar_extract(char ** target);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PRTE_COMPRESS_BASE_H */
