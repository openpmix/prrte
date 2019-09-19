/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_DL_BASE_H
#define PRRTE_DL_BASE_H

#include "prrte_config.h"
#include "src/mca/dl/dl.h"
#include "src/util/prrte_environ.h"

#include "src/mca/base/base.h"
#include "src/mca/base/prrte_mca_base_framework.h"

BEGIN_C_DECLS

/**
 * Globals
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_dl_base_framework;
PRRTE_EXPORT extern prrte_dl_base_component_t
*prrte_dl_base_selected_component;
PRRTE_EXPORT extern prrte_dl_base_module_t *prrte_dl;


/**
 * Initialize the DL MCA framework
 *
 * @retval PRRTE_SUCCESS Upon success
 * @retval PRRTE_ERROR   Upon failures
 *
 * This function is invoked during prrte_init();
 */
PRRTE_EXPORT int prrte_dl_base_open(prrte_mca_base_open_flag_t flags);

/**
 * Select an available component.
 *
 * @retval PRRTE_SUCCESS Upon Success
 * @retval PRRTE_NOT_FOUND If no component can be selected
 * @retval PRRTE_ERROR Upon other failure
 *
 */
PRRTE_EXPORT int prrte_dl_base_select(void);

/**
 * Finalize the DL MCA framework
 *
 * @retval PRRTE_SUCCESS Upon success
 * @retval PRRTE_ERROR   Upon failures
 *
 * This function is invoked during prrte_finalize();
 */
PRRTE_EXPORT int prrte_dl_base_close(void);

/**
 * Open a DSO
 *
 * (see prrte_dl_base_module_open_ft_t in src/mca/dl/dl.h for
 * documentation of this function)
 */
PRRTE_EXPORT int prrte_dl_open(const char *fname,
                               bool use_ext, bool private_namespace,
                               prrte_dl_handle_t **handle, char **err_msg);

/**
 * Lookup a symbol in a DSO
 *
 * (see prrte_dl_base_module_lookup_ft_t in src/mca/dl/dl.h for
 * documentation of this function)
 */
PRRTE_EXPORT int prrte_dl_lookup(prrte_dl_handle_t *handle,
                                 const char *symbol,
                                 void **ptr, char **err_msg);

/**
 * Close a DSO
 *
 * (see prrte_dl_base_module_close_ft_t in src/mca/dl/dl.h for
 * documentation of this function)
 */
PRRTE_EXPORT int prrte_dl_close(prrte_dl_handle_t *handle);

/**
 * Iterate over files in a path
 *
 * (see prrte_dl_base_module_foreachfile_ft_t in src/mca/dl/dl.h for
 * documentation of this function)
 */
PRRTE_EXPORT int prrte_dl_foreachfile(const char *search_path,
                                      int (*cb_func)(const char *filename,
                                                     void *context),
                                      void *context);

END_C_DECLS

#endif /* PRRTE_DL_BASE_H */
