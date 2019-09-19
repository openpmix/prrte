/* -*- C -*-
 *
 * $HEADER$
 *
 * Generate a key for PSM transports
 */

#include <stdio.h>
#include "constants.h"
#include "src/runtime/runtime.h"

#include "src/util/pre_condition_transports.h"

int main(int argc, char* argv[])
{
    prrte_job_t *jdata;
    prrte_app_context_t *app;
    int i;

    if (PRRTE_SUCCESS != prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI)) {
        fprintf(stderr, "Failed prrte_init\n");
        exit(1);
    }

    jdata = PRRTE_NEW(prrte_job_t);
    app = PRRTE_NEW(prrte_app_context_t);
    opal_pointer_array_set_item(jdata->apps, 0, app);
    jdata->num_apps = 1;

    if (PRRTE_SUCCESS != prrte_pre_condition_transports(jdata)) {
        fprintf(stderr, "Failed to generate PSM key\n");
        exit(1);
    }

    for (i=0; NULL != app->env[i]; i++) {
        if (0 == strncmp(PRRTE_MCA_PREFIX"prrte_precondition_transports", app->env[i],
                         strlen(PRRTE_MCA_PREFIX"prrte_precondition_transports"))) {
            fprintf(stderr, "%s\n", app->env[i]);
            break;
        }
    }

    PRRTE_RELEASE(jdata);

    if (PRRTE_SUCCESS != prrte_finalize()) {
        fprintf(stderr, "Failed prrte_finalize\n");
        exit(1);
    }
    return 0;
}
