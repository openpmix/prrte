/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PRRTE_COMPRESS_BASE_H
#define PRRTE_COMPRESS_BASE_H

#include "prrte_config.h"
#include "src/mca/compress/compress.h"
#include "src/util/prrte_environ.h"

#include "src/mca/base/base.h"

/*
 * Global functions for MCA overall COMPRESS
 */

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    size_t compress_limit;
} prrte_compress_base_t;

PRRTE_EXPORT extern prrte_compress_base_t prrte_compress_base;

    /**
     * Initialize the COMPRESS MCA framework
     *
     * @retval PRRTE_SUCCESS Upon success
     * @retval PRRTE_ERROR   Upon failures
     *
     * This function is invoked during prrte_init();
     */
    PRRTE_EXPORT int prrte_compress_base_open(prrte_mca_base_open_flag_t flags);

    /**
     * Select an available component.
     *
     * @retval PRRTE_SUCCESS Upon Success
     * @retval PRRTE_NOT_FOUND If no component can be selected
     * @retval PRRTE_ERROR Upon other failure
     *
     */
    PRRTE_EXPORT int prrte_compress_base_select(void);

    /**
     * Finalize the COMPRESS MCA framework
     *
     * @retval PRRTE_SUCCESS Upon success
     * @retval PRRTE_ERROR   Upon failures
     *
     * This function is invoked during prrte_finalize();
     */
    PRRTE_EXPORT int prrte_compress_base_close(void);

    /**
     * Globals
     */
    PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_compress_base_framework;
    PRRTE_EXPORT extern prrte_compress_base_component_t prrte_compress_base_selected_component;
    PRRTE_EXPORT extern prrte_compress_base_module_t prrte_compress;

    /**
     *
     */
    PRRTE_EXPORT int prrte_compress_base_tar_create(char ** target);
    PRRTE_EXPORT int prrte_compress_base_tar_extract(char ** target);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PRRTE_COMPRESS_BASE_H */
