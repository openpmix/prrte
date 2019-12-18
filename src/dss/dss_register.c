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
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/dss/dss_internal.h"

int prrte_dss_register(prrte_dss_pack_fn_t pack_fn,
                      prrte_dss_unpack_fn_t unpack_fn,
                      prrte_dss_copy_fn_t copy_fn,
                      prrte_dss_compare_fn_t compare_fn,
                      prrte_dss_print_fn_t print_fn,
                      bool structured,
                      const char *name, prrte_data_type_t *type)
{
    prrte_dss_type_info_t *info, *ptr;
    int32_t i;

    /* Check for bozo cases */

    if (NULL == pack_fn || NULL == unpack_fn || NULL == copy_fn || NULL == compare_fn ||
        NULL == print_fn || NULL == name || NULL == type) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* check if this entry already exists - if so, error - we do NOT allow multiple type registrations */
    for (i=0; i < prrte_pointer_array_get_size(&prrte_dss_types); i++) {
        ptr = prrte_pointer_array_get_item(&prrte_dss_types, i);
        if (NULL != ptr) {
            /* check if the name exists */
            if (0 == strcmp(ptr->odti_name, name)) {
                return PRRTE_ERR_DATA_TYPE_REDEF;
            }
            /* check if the specified type exists */
            if (*type > 0 && ptr->odti_type == *type) {
                return PRRTE_ERR_DATA_TYPE_REDEF;
            }
        }
    }

    /* if type is given (i.e., *type > 0), then just use it.
     * otherwise, it is an error
     */
    if (0 >= *type) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Add a new entry to the table */
    info = (prrte_dss_type_info_t*) PRRTE_NEW(prrte_dss_type_info_t);
    if (NULL == info) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    info->odti_type = *type;
    info->odti_name = strdup(name);
    info->odti_pack_fn = pack_fn;
    info->odti_unpack_fn = unpack_fn;
    info->odti_copy_fn = copy_fn;
    info->odti_compare_fn = compare_fn;
    info->odti_print_fn = print_fn;
    info->odti_structured = structured;

    return prrte_pointer_array_set_item(&prrte_dss_types, *type, info);
}
