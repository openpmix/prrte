#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/param.h>


#include <pmix.h>

int main(int argc, char* argv[])
{
    pmix_status_t rc;
    int size;
    pid_t pid;
    pmix_proc_t myproc;
    pmix_proc_t proc;
    pmix_app_t app;
    pmix_proc_t peers[2];
    char hostname[1024];
    pmix_value_t *val = NULL;
    pmix_nspace_t nspace;

    pid = getpid();
    gethostname(hostname, 1024);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    PMIX_VALUE_GET_NUMBER(rc, val, size, int);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: get size number failed: %s\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    PMIX_VALUE_RELEASE(val);

    printf("[%s:%u pid %ld] of %d starting up on node %s!\n",
           myproc.nspace, myproc.rank, (long)pid, size, hostname);

    rc = PMIx_Get(&myproc, PMIX_PARENT_ID, NULL, 0, &val);
    /* If we don't find it, then we're the parent */
    if (PMIX_SUCCESS != rc) {
        pid = getpid();
        printf("Parent [pid %ld] about to spawn!\n", (long)pid);
        PMIX_APP_CONSTRUCT(&app);
        app.cmd = strdup(argv[0]);
        PMIX_ARGV_APPEND(rc, app.argv, argv[0]);
        app.maxprocs = 3;
        rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
        if (PMIX_SUCCESS != rc) {
            printf("Child failed to spawn\n");
            return rc;
        }
        printf("Parent done with spawn\n");
        /* connect to the children */
    }
    /* Otherwise, we're the child */
    else {
        printf("Hello from the child %s.%u of %d on host %s pid %ld\n",
               myproc.nspace, myproc.rank, size, hostname, (long)pid);
    }

done:
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "%d: exiting\n", pid);
    return 0;
}
