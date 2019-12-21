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
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "constants.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/mca/pstat/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */
#include "src/mca/pstat/base/static-components.h"

/* unsupported functions */
static int prrte_pstat_base_unsupported_init(void);
static int prrte_pstat_base_unsupported_query(pid_t pid, prrte_pstats_t *stats, prrte_node_stats_t *nstats);
static int prrte_pstat_base_unsupported_finalize(void);

/*
 * Globals
 */
prrte_pstat_base_component_t *prrte_pstat_base_component = NULL;
prrte_pstat_base_module_t prrte_pstat = {
    prrte_pstat_base_unsupported_init,
    prrte_pstat_base_unsupported_query,
    prrte_pstat_base_unsupported_finalize
};

/* Use default register/open/close functions */
static int prrte_pstat_base_close(void)
{
    /* let the selected module finalize */
    if (NULL != prrte_pstat.finalize) {
            prrte_pstat.finalize();
    }

    return prrte_mca_base_framework_components_close(&prrte_pstat_base_framework, NULL);
}

static int prrte_pstat_base_open(prrte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_pstat_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, pstat, "process statistics", NULL,
                                 prrte_pstat_base_open, prrte_pstat_base_close,
                                 prrte_pstat_base_static_components, 0);

static int prrte_pstat_base_unsupported_init(void)
{
    return PRRTE_ERR_NOT_SUPPORTED;
}

static int prrte_pstat_base_unsupported_query(pid_t pid, prrte_pstats_t *stats, prrte_node_stats_t *nstats)
{
    return PRRTE_ERR_NOT_SUPPORTED;
}

static int prrte_pstat_base_unsupported_finalize(void)
{
    return PRRTE_ERR_NOT_SUPPORTED;
}
