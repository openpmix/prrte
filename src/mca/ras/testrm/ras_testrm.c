/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_list.h"
#include "src/runtime/prte_globals.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/proc_info.h"
#include "ras_testrm.h"

/*
 * Local functions
 */
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);
static int finalize(void);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_testrm_module = {
    .scheduler_owned = false,
    .allocate = allocate,
    .finalize = finalize
};

static int allocate(prte_job_t *jdata, pmix_list_t *nodes)
{
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(jdata);

    /* the component's own configuration, so read as ourselves */
    rc = prte_util_add_hostfile_nodes(nodes, prte_mca_ras_testrm_component.hostfile,
                                      prte_process_info.euid, prte_process_info.egid);
    return rc;
}

/*
 * There's really nothing to do here
 */
static int finalize(void)
{
    return PRTE_SUCCESS;
}
