/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_ODLS_BASE_H
#define MCA_ODLS_BASE_H

/*
 * includes
 */
#include "prte_config.h"

#include "src/mca/mca.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/odls/odls.h"

BEGIN_C_DECLS

/*
 * MCA framework
 */
PRTE_EXPORT extern pmix_mca_base_framework_t prte_odls_base_framework;
/*
 * Select an available component.
 */
PRTE_EXPORT int prte_odls_base_select(void);

PRTE_EXPORT void prte_odls_base_start_threads(prte_job_t *jdata);

PRTE_EXPORT void prte_odls_base_harvest_threads(void);

#define PRTE_ODLS_SET_ERROR(ns, s, j)                                                   \
do {                                                                                    \
    int _idx;                                                                           \
    prte_proc_t *_cld;                                                                  \
    unsigned int _j = (unsigned int)j;                                                  \
    for (_idx = 0; _idx < prte_local_children->size; _idx++) {                          \
        _cld = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, _idx);  \
        if (NULL == _cld) {                                                             \
            continue;                                                                   \
        }                                                                               \
        if (PMIX_CHECK_NSPACE(ns, _cld->name.nspace) &&                                 \
            (UINT_MAX == _j || _j == _cld->app_idx)) {                                  \
            _cld->exit_code = s;                                                        \
            PRTE_ACTIVATE_PROC_STATE(&_cld->name, PRTE_PROC_STATE_FAILED_TO_LAUNCH);    \
        }                                                                               \
    }                                                                                   \
} while(0)

END_C_DECLS
#endif
