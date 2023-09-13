/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/mca/base/pmix_mca_base_var.h"

#include "src/tools/psched/psched.h"

static int sched_base_verbose = -1;
void psched_scheduler_init(void)
{
    pmix_output_stream_t lds;

    pmix_mca_base_var_register("prte", "scheduler", "base", "verbose",
                               "Verbosity for debugging scheduler operations",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &sched_base_verbose);
    if (0 <= sched_base_verbose) {
        PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
        lds.lds_want_stdout = true;
        psched_globals.scheduler_output = pmix_output_open(&lds);
        PMIX_DESTRUCT(&lds);
        pmix_output_set_verbosity(psched_globals.scheduler_output, sched_base_verbose);
    }

    pmix_output_verbose(2, psched_globals.scheduler_output,
                        "%s scheduler:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
    return;
}

void psched_scheduler_finalize(void)
{
    return;
}
