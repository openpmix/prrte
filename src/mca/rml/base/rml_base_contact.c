/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/util/argv.h"
#include "src/util/output.h"

#include "src/dss/dss.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/routed/routed.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/rml/rml.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/rml/base/base.h"


int prrte_rml_base_parse_uris(const char* uri,
                             prrte_process_name_t* peer,
                             char*** uris)
{
    int rc;

    /* parse the process name */
    char* cinfo = strdup(uri);
    char* ptr = strchr(cinfo, ';');
    if(NULL == ptr) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        free(cinfo);
        return PRRTE_ERR_BAD_PARAM;
    }
    *ptr = '\0';
    ptr++;
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_string_to_process_name(peer, cinfo))) {
        PRRTE_ERROR_LOG(rc);
        free(cinfo);
        return rc;
    }

    if (NULL != uris) {
        /* parse the remainder of the string into an array of uris */
        *uris = prrte_argv_split(ptr, ';');
    }
    free(cinfo);
    return PRRTE_SUCCESS;
}
