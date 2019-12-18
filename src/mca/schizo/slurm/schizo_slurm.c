/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies Ltd.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/prrte_environ.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/mca/schizo/base/base.h"

#include "schizo_slurm.h"

static int get_remaining_time(uint32_t *timeleft);

prrte_schizo_base_module_t prrte_schizo_slurm_module = {
    .get_remaining_time = get_remaining_time
};

static int get_remaining_time(uint32_t *timeleft)
{
    char output[256], *cmd, *jobid, **res;
    FILE *fp;
    uint32_t tleft;
    size_t cnt;

    /* set the default */
    *timeleft = UINT32_MAX;

    if (NULL == (jobid = getenv("SLURM_JOBID"))) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }
    if (0 > prrte_asprintf(&cmd, "squeue -h -j %s -o %%L", jobid)) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    fp = popen(cmd, "r");
    if (NULL == fp) {
        free(cmd);
        return PRRTE_ERR_FILE_OPEN_FAILURE;
    }
    if (NULL == fgets(output, 256, fp)) {
        free(cmd);
        pclose(fp);
        return PRRTE_ERR_FILE_READ_FAILURE;
    }
    free(cmd);
    pclose(fp);
    /* the output is returned in a colon-delimited set of fields */
    res = prrte_argv_split(output, ':');
    cnt =  prrte_argv_count(res);
    tleft = strtol(res[cnt-1], NULL, 10); // has to be at least one field
    /* the next field would be minutes */
    if (1 < cnt) {
        tleft += 60 * strtol(res[cnt-2], NULL, 10);
    }
    /* next field would be hours */
    if (2 < cnt) {
        tleft += 3600 * strtol(res[cnt-3], NULL, 10);
    }
    /* next field is days */
    if (3 < cnt) {
        tleft += 24*3600 * strtol(res[cnt-4], NULL, 10);
    }
    /* if there are more fields than that, then it is infinite */
    if (4 < cnt) {
        tleft = UINT32_MAX;
    }
    prrte_argv_free(res);

    *timeleft = tleft;
    return PRRTE_SUCCESS;
}
