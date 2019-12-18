/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include <stdio.h>
#include <unistd.h>

#include "src/dss/dss.h"
#include "src/util/opal_getcwd.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/comm/comm.h"
#include "src/util/hnp_contact.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/runtime.h"

int main(int argc, char* argv[])
{
    int rc=PRRTE_SUCCESS;
    prrte_job_t *jdata=NULL, **jobs=NULL;
    opal_list_t hnp_list;
    prrte_hnp_contact_t *hnp;
    prrte_std_cntr_t num_jobs, i;
    prrte_app_context_t *app;
    char cwd[PRRTE_PATH_MAX];

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_TOOL))) {
        fprintf(stderr, "prrte_tool: couldn't init prrte\n");
        return rc;
    }

    /***************
     * Initialize
     ***************/
    PRRTE_CONSTRUCT(&hnp_list, opal_list_t);

    /*
     * Get the directory listing
     */
    if (PRRTE_SUCCESS != (rc = prrte_list_local_hnps(&hnp_list, true) ) ) {
        fprintf(stderr, "prrte_tool: couldn't get list of HNP's on this system - error %s\n",
                PRRTE_ERROR_NAME(rc));
        goto cleanup;
    }

    /* if the list is empty, we can't do anything */
    if (opal_list_is_empty(&hnp_list)) {
        fprintf(stderr, "prrte_tool: no HNP's were found\n");
        goto cleanup;
    }

    /* take first one */
    hnp = (prrte_hnp_contact_t*)opal_list_remove_first(&hnp_list);

    /* create a job */
    jdata = PRRTE_NEW(prrte_job_t);

    /* create an app_context for this job */
    app = PRRTE_NEW(prrte_app_context_t);
    /* add the app to the job data */
    opal_pointer_array_add(jdata->apps, app);
    jdata->num_apps++;

    /* copy over the name of the executable */
    app->app = strdup("hostname");
    /* make sure it is also in argv[0]! */
    app->argv = (char**)malloc(2 * sizeof(char*));
    app->argv[0] = strdup(app->app);
    /* record the number of procs to be generated */
    app->num_procs = 1;
    /* setup the wd */
    opal_getcwd(cwd, PRRTE_PATH_MAX);
    app->cwd = strdup(cwd);

    /* spawn it */
    if (PRRTE_SUCCESS != (rc = prrte_util_comm_spawn_job(&hnp->name, jdata))) {
        PRRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* report out the jobid */
    fprintf(stderr, "prrte_tool: spawned jobid %s\n", PRRTE_JOBID_PRINT(jdata->jobid));
#if 0
    if (PRRTE_SUCCESS != (rc = prrte_util_comm_query_job_info(&hnp->name, PRRTE_JOBID_WILDCARD,
                                                             &num_jobs, &jobs))) {
        PRRTE_ERROR_LOG(rc);
    }
    printf("num jobs: %d\n", num_jobs);
    opal_dss.dump(0, jobs[0], PRRTE_JOB);
#endif

cleanup:
    if (NULL != jdata) PRRTE_RELEASE(jdata);
    if (NULL != jobs) {
        for (i=0; i < num_jobs; i++) PRRTE_RELEASE(jobs[i]);
        if (NULL != jobs) free(jobs);
    }
    prrte_finalize();
    return rc;
}
