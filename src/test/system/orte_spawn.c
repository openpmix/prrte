/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include <stdio.h>

#include "src/util/argv.h"

#include "src/util/proc_info.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"

#define MY_TAG 12345

int main(int argc, char* argv[])
{
    int rc;
    prrte_job_t *jdata;
    prrte_app_context_t *app;
    char cwd[1024];
    prrte_process_name_t name;
    struct iovec msg;
    prrte_vpid_t i;


    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "couldn't init prrte - error code %d\n", rc);
        return rc;
    }

    /* setup the job object */
    jdata = PRRTE_NEW(prrte_job_t);
    prrte_set_attribute(&jdata->attributes, PRRTE_JOB_NON_PRRTE_JOB, PRRTE_ATTR_GLOBAL, NULL, PRRTE_BOOL);

    /* create an app_context that defines the app to be run */
    app = PRRTE_NEW(prrte_app_context_t);
    app->app = strdup("hostname");
    opal_argv_append_nosize(&app->argv, "hostname");
    app->num_procs = 3;

    getcwd(cwd, sizeof(cwd));
    app->cwd = strdup(cwd);
    /*===================================*/
    char *host_list = "vm,vm3,vm4";
    prrte_set_attribute(&app->attributes, PRRTE_APP_DASH_HOST, PRRTE_ATTR_GLOBAL, host_list, PRRTE_STRING);
    /*==================================*/

    /* add the app to the job data */
    opal_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;
#if 0
    /* setup a map object */
    jdata->map = PRRTE_NEW(prrte_job_map_t);
    jdata->map->display_map = true;
#endif
    /* launch the job */
    fprintf(stderr, "Parent: spawning children!\n");
    if (PRRTE_SUCCESS != (rc = prrte_plm.spawn(jdata))) {
        PRRTE_ERROR_LOG(rc);
        prrte_finalize();
        return 1;
    }
    fprintf(stderr, "Parent: children spawned!\n");

#if 0
    /* send messages to all children - this will verify that we know their contact info */
    name.jobid = jdata->jobid;
    i = 1;
    msg.iov_base = (void *) &i;
    msg.iov_len  = sizeof(i);
    for (i=0; i < app->num_procs; i++) {
        name.vpid = i;

        fprintf(stderr, "Parent: sending message to child %s\n", PRRTE_NAME_PRINT(&name));
        if (0 > (rc = prrte_rml.send(&name, &msg, 1, MY_TAG, 0))) {
            PRRTE_ERROR_LOG(rc);
        }
    }
#endif

    /* All done */
    prrte_finalize();
    return 0;
}
