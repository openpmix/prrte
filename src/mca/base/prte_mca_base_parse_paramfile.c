/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_vari.h"
#include "src/mca/mca.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_keyval_parse.h"

static void save_value(const char *file, int lineno, const char *name, const char *value);

static pmix_list_t *_param_list;

int prte_mca_base_parse_paramfile(const char *paramfile, pmix_list_t *list)
{
    int rc;

    _param_list = list;

    rc = pmix_util_keyval_parse(paramfile, save_value);
    return prte_pmix_convert_status(rc);
}

static void save_value(const char *file, int lineno, const char *name, const char *value)
{
    prte_mca_base_var_file_value_t *fv;
    bool found = false;

    /* First traverse through the list and ensure that we don't
       already have a param of this name.  If we do, just replace the
       value. */

    PMIX_LIST_FOREACH (fv, _param_list, prte_mca_base_var_file_value_t) {
        if (0 == strcmp(name, fv->mbvfv_var)) {
            if (NULL != fv->mbvfv_value) {
                free(fv->mbvfv_value);
            }
            found = true;
            break;
        }
    }

    if (!found) {
        /* We didn't already have the param, so append it to the list */
        fv = PMIX_NEW(prte_mca_base_var_file_value_t);
        if (NULL == fv) {
            return;
        }

        fv->mbvfv_var = strdup(name);
        pmix_list_append(_param_list, &fv->super);
    }

    fv->mbvfv_value = value ? strdup(value) : NULL;
    fv->mbvfv_file = strdup(file);
    fv->mbvfv_lineno = lineno;
}
