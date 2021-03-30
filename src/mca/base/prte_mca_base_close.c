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
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "constants.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

extern int prte_mca_base_opened;

/*
 * Main MCA shutdown.
 */

void prte_mca_base_close(void)
{
    assert(prte_mca_base_opened);
    if (--prte_mca_base_opened) {
        return;
    }

    /* deregister all MCA base parameters */
    int group_id = prte_mca_base_var_group_find("prte", "mca", "base");

    if (-1 < group_id) {
        prte_mca_base_var_group_deregister(group_id);
    }

    /* release the default paths */
    if (NULL != prte_mca_base_system_default_path) {
        free(prte_mca_base_system_default_path);
    }
    prte_mca_base_system_default_path = NULL;

    if (NULL != prte_mca_base_user_default_path) {
        free(prte_mca_base_user_default_path);
    }
    prte_mca_base_user_default_path = NULL;

    /* Close down the component repository */
    prte_mca_base_component_repository_finalize();

    /* Shut down the dynamic component finder */
    prte_mca_base_component_find_finalize();

    /* Close prte output stream 0 */
    prte_output_close(0);
}
