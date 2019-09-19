/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * This file is a simple set of wrappers around the selected PRRTE DL
 * component (it's a compile-time framework with, at most, a single
 * component; see dl.h for details).
 */

#include "prrte_config.h"

#include "src/include/constants.h"

#include "src/mca/dl/base/base.h"


int prrte_dl_open(const char *fname,
                 bool use_ext, bool private_namespace,
                 prrte_dl_handle_t **handle, char **err_msg)
{
    *handle = NULL;

    if (NULL != prrte_dl && NULL != prrte_dl->open) {
        return prrte_dl->open(fname, use_ext, private_namespace,
                             handle, err_msg);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_lookup(prrte_dl_handle_t *handle,
                   const char *symbol,
                   void **ptr, char **err_msg)
{
    if (NULL != prrte_dl && NULL != prrte_dl->lookup) {
        return prrte_dl->lookup(handle, symbol, ptr, err_msg);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_close(prrte_dl_handle_t *handle)
{
    if (NULL != prrte_dl && NULL != prrte_dl->close) {
        return prrte_dl->close(handle);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}

int prrte_dl_foreachfile(const char *search_path,
                        int (*cb_func)(const char *filename, void *context),
                        void *context)
{
    if (NULL != prrte_dl && NULL != prrte_dl->foreachfile) {
        return prrte_dl->foreachfile(search_path, cb_func, context);
    }

    return PRRTE_ERR_NOT_SUPPORTED;
}
