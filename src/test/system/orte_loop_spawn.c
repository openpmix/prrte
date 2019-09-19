/*file .c : spawned  the file Exe*/
#include <stdio.h>
#include <unistd.h>

#include "constants.h"

#include "src/util/argv.h"

#include "src/mca/plm/plm.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"

int main(int argc, char* argv[])
{
    int rc;
    prrte_job_t *jdata;
    prrte_app_context_t *app;
    char cwd[1024];
    int iter;

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "couldn't init prrte - error code %d\n", rc);
        return rc;
    }

    for (iter = 0; iter < 1000; ++iter) {
        /* setup the job object */
        jdata = PRRTE_NEW(prrte_job_t);
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NON_PRRTE_JOB, PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);

        /* create an app_context that defines the app to be run */
        app = PRRTE_NEW(prrte_app_context_t);
        app->app = strdup("hostname");
        opal_argv_append_nosize(&app->argv, "hostname");
        app->num_procs = 1;

        getcwd(cwd, sizeof(cwd));
        app->cwd = strdup(cwd);

        /* add the app to the job data */
        opal_pointer_array_add(jdata->apps, app);
        jdata->num_apps = 1;

        fprintf(stderr, "Parent: spawning child %d\n", iter);
        if (PRRTE_SUCCESS != (rc = prrte_plm.spawn(jdata))) {
            PRRTE_ERROR_LOG(rc);
            exit(1);
        }
    }

    /* All done */
    prrte_finalize();
    return 0;
}
