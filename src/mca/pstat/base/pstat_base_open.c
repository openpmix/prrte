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
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prte_config.h"

#include "constants.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/mca/pstat/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */
#include "src/mca/pstat/base/static-components.h"

/* unsupported functions */
static int prte_pstat_base_unsupported_init(void);
static int prte_pstat_base_unsupported_query(pid_t pid, prte_pstats_t *stats, prte_node_stats_t *nstats);
static int prte_pstat_base_unsupported_finalize(void);

/*
 * Globals
 */
prte_pstat_base_component_t *prte_pstat_base_component = NULL;
prte_pstat_base_module_t prte_pstat = {
    prte_pstat_base_unsupported_init,
    prte_pstat_base_unsupported_query,
    prte_pstat_base_unsupported_finalize
};

/* Use default register/open/close functions */
static int prte_pstat_base_close(void)
{
    /* let the selected module finalize */
    if (NULL != prte_pstat.finalize) {
            prte_pstat.finalize();
    }

    return prte_mca_base_framework_components_close(&prte_pstat_base_framework, NULL);
}

static int prte_pstat_base_open(prte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_pstat_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, pstat, "process statistics", NULL,
                                 prte_pstat_base_open, prte_pstat_base_close,
                                 prte_pstat_base_static_components, 0);

static int prte_pstat_base_unsupported_init(void)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

static int prte_pstat_base_unsupported_query(pid_t pid, prte_pstats_t *stats, prte_node_stats_t *nstats)
{
    return PRTE_ERR_NOT_SUPPORTED;
}

static int prte_pstat_base_unsupported_finalize(void)
{
    return PRTE_ERR_NOT_SUPPORTED;
}
