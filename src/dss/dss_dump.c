/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
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

#include "src/util/output.h"

#include "src/dss/dss_internal.h"

int prrte_dss_dump(int output_stream, void *src, prrte_data_type_t type)
{
    char *sptr;
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss.print(&sptr, NULL, src, type))) {
        return rc;
    }

    prrte_output(output_stream, "%s", sptr);
    free(sptr);

    return PRRTE_SUCCESS;
}


void prrte_dss_dump_data_types(int output)
{
    prrte_dss_type_info_t *ptr;
    prrte_data_type_t j;
    int32_t i;

    prrte_output(output, "DUMP OF REGISTERED DATA TYPES");

    j = 0;
    for (i=0; i < prrte_pointer_array_get_size(&prrte_dss_types); i++) {
        ptr = prrte_pointer_array_get_item(&prrte_dss_types, i);
        if (NULL != ptr) {
            j++;
            /* print out the info */
            prrte_output(output, "\tIndex: %lu\tData type: %lu\tName: %s",
                        (unsigned long)j,
                        (unsigned long)ptr->odti_type,
                        ptr->odti_name);
        }
    }
}

