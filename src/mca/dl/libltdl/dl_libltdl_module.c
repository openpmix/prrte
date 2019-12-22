/*
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/mca/dl/dl.h"

#include "dl_libltdl.h"


static int libltdl_open(const char *fname, bool use_ext, bool private_namespace,
                       prrte_dl_handle_t **handle, char **err_msg)
{
    assert(handle);

    *handle = NULL;
    if (NULL != err_msg) {
        *err_msg = NULL;
    }

    lt_dlhandle local_handle;

#if PRRTE_DL_LIBLTDL_HAVE_LT_DLADVISE
    prrte_dl_libltdl_component_t *c = &prrte_dl_libltdl_component;

    if (use_ext && private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_private_ext);
    } else if (use_ext && !private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_public_ext);
    } else if (!use_ext && private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_private_noext);
    } else if (!use_ext && !private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_public_noext);
    }
#else
    if (use_ext) {
        local_handle = lt_dlopenext(fname);
    } else {
        local_handle = lt_dlopen(fname);
    }
#endif

    if (NULL != local_handle) {
        *handle = calloc(1, sizeof(prrte_dl_handle_t));
        (*handle)->ltdl_handle = local_handle;

#if PRRTE_ENABLE_DEBUG
        if( NULL != fname ) {
            (*handle)->filename = strdup(fname);
        }
        else {
            (*handle)->filename = strdup("(null)");
        }
#endif

        return PRRTE_SUCCESS;
    }

    if (NULL != err_msg) {
        *err_msg = (char*) lt_dlerror();
    }
    return PRRTE_ERROR;
}


static int libltdl_lookup(prrte_dl_handle_t *handle, const char *symbol,
                         void **ptr, char **err_msg)
{
    assert(handle);
    assert(handle->ltdl_handle);
    assert(symbol);
    assert(ptr);

    if (NULL != err_msg) {
        *err_msg = NULL;
    }

    *ptr = lt_dlsym(handle->ltdl_handle, symbol);
    if (NULL != *ptr) {
        return PRRTE_SUCCESS;
    }

    if (NULL != err_msg) {
        *err_msg = (char*) lt_dlerror();
    }
    return PRRTE_ERROR;
}


static int libltdl_close(prrte_dl_handle_t *handle)
{
    assert(handle);

    int ret;
    ret = lt_dlclose(handle->ltdl_handle);

#if PRRTE_ENABLE_DEBUG
    free(handle->filename);
#endif
    free(handle);

    return ret;
}

static int libltdl_foreachfile(const char *search_path,
                               int (*func)(const char *filename, void *data),
                               void *data)
{
    assert(search_path);
    assert(func);

    int ret = lt_dlforeachfile(search_path, func, data);
    return (0 == ret) ? PRRTE_SUCCESS : PRRTE_ERROR;
}


/*
 * Module definition
 */
prrte_dl_base_module_t prrte_dl_libltdl_module = {
    .open = libltdl_open,
    .lookup = libltdl_lookup,
    .close = libltdl_close,
    .foreachfile = libltdl_foreachfile
};
