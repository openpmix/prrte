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
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/mca.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */

#include "src/mca/grpcomm/base/static-components.h"

/*
 * Global variables
 */
prte_grpcomm_base_t prte_grpcomm_base = {
    .context_id = UINT32_MAX
};

prte_grpcomm_base_module_t prte_grpcomm = {0};


static int prte_grpcomm_base_close(void)
{
    if (NULL != prte_grpcomm.finalize) {
        prte_grpcomm.finalize();
    }

    return pmix_mca_base_framework_components_close(&prte_grpcomm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_grpcomm_base_open(pmix_mca_base_open_flag_t flags)
{
    return pmix_mca_base_framework_components_open(&prte_grpcomm_base_framework, flags);
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, grpcomm, "GRPCOMM", NULL, prte_grpcomm_base_open,
                                prte_grpcomm_base_close, prte_grpcomm_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void grpcon(prte_pmix_grp_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->op = PMIX_GROUP_NONE;
    p->grpid = NULL;
    p->procs = NULL;
    p->nprocs = 0;
    p->directives = NULL;
    p->ndirs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void grpdes(prte_pmix_grp_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->grpid) {
        free(p->grpid);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(prte_pmix_grp_caddy_t,
                    pmix_object_t,
                    grpcon, grpdes);
