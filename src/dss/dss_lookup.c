/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/dss/dss_internal.h"

char *prrte_dss_lookup_data_type(prrte_data_type_t type)
{
    prrte_dss_type_info_t *info;
    char *name;

    info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, type);
    if (NULL != info) { /* type found on list */
        name = strdup(info->odti_name);
        return name;
    }

    return NULL;
}
