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

#include <string.h>

#include "src/class/pmix_list.h"

#include "src/runtime/prte_globals.h"
#include "src/util/prte_bootstrap.h"
#include "ras_boot.h"

// module functions
static int allocate(prte_job_t *jdata, pmix_list_t *nodes);

/*
 * Global variable
 */
prte_ras_base_module_t prte_ras_bootstrap_module = {
    .init = NULL,
    .allocate = allocate,
    .modify = NULL,
    .finalize = NULL
};

static int allocate(prte_job_t *jdata, pmix_list_t *ndlist)
{
    prte_bootstrap_config_t cfg;
    prte_node_t *ndptr;
    pmix_rank_t rank;
    int n, rc;
    PRTE_HIDE_UNUSED_PARAMS(jdata);

    /* read the same configuration file the daemons read */
    rc = prte_bootstrap_parse(&cfg);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* add each DVMNodes entry to the pool at its canonical rank.  The
     * controller (rank 0) is already represented in the node pool by the HNP
     * itself, so skip that entry if it appears in the list. */
    for (n = 0; NULL != cfg.nodes[n]; n++) {
        rc = prte_bootstrap_rank_of(&cfg, cfg.nodes[n], &rank);
        if (PRTE_SUCCESS != rc) {
            continue;
        }
        if (0 == rank) {
            /* the controller node - already the HNP at pool index 0 */
            continue;
        }
        ndptr = PMIX_NEW(prte_node_t);
        ndptr->name = strdup(cfg.nodes[n]);
        ndptr->index = (int) rank;
        pmix_list_append(ndlist, &ndptr->super);
    }

    prte_bootstrap_config_free(&cfg);
    return PRTE_SUCCESS;
}
