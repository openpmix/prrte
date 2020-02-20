/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * This file is a simple set of wrappers around the selected PRRTE DL
 * component (it's a compile-time framework with, at most, a single
 * component; see prtedl.h for details).
 */

#include "prrte_config.h"

#include "src/include/constants.h"

#include "src/mca/prtedl/base/base.h"


int prrte_dl_open(const char *fname,
                 bool use_ext, bool private_namespace,
                 prrte_dl_handle_t **handle, char **err_msg)
{
    *handle = NULL;

    if (NULL != prrte_prtedl && NULL != prrte_prtedl->open) {
        return prrte_prtedl->open(fname, use_ext, private_namespace,
                             handle, err_msg);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_lookup(prrte_dl_handle_t *handle,
                   const char *symbol,
                   void **ptr, char **err_msg)
{
    if (NULL != prrte_prtedl && NULL != prrte_prtedl->lookup) {
        return prrte_prtedl->lookup(handle, symbol, ptr, err_msg);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_close(prrte_dl_handle_t *handle)
{
    if (NULL != prrte_prtedl && NULL != prrte_prtedl->close) {
        return prrte_prtedl->close(handle);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_foreachfile(const char *search_path,
                        int (*cb_func)(const char *filename, void *context),
                        void *context)
{
    if (NULL != prrte_prtedl && NULL != prrte_prtedl->foreachfile) {
        return prrte_prtedl->foreachfile(search_path, cb_func, context);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}
