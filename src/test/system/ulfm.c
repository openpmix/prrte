/* -*- C -*-
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

#include "src/mca/pmix/pmix.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"

int main(int argc, char* argv[])
{

    int i, rc;
    double pi;
    pid_t pid;
    char hostname[PRRTE_MAXHOSTNAMELEN];

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "prrte_abort: couldn't init prrte - error code %d\n", rc);
        return rc;
    }
    pid = getpid();
    gethostname(hostname, sizeof(hostname));

    if (1 < argc) {
        rc = strtol(argv[1], NULL, 10);
    } else {
        rc = 3;
    }

    printf("prrte_abort: Name %s Host: %s Pid %ld\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
              hostname, (long)pid);
    fflush(stdout);

    if (prrte_process_info.my_name.vpid == (prrte_process_info.num_procs-1)) {
        printf("ulfm[%ld]: exiting\n", (long)pid);
        exit(0);
    }

    printf("ulfm[%ld]: entering fence\n", (long)pid);
    /* everyone else enters barrier - this should complete */
    opal_pmix.fence(NULL, 0);
    return 0;
}
