/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that just spins, with vpid 3 aborting - provides mechanism for testing
 * abnormal program termination
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>

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

    i = 0;
    while (1) {
        i++;
        pi = i / 3.14159256;
        if (i > 10000) i = 0;
        if ((PRRTE_PROC_MY_NAME->vpid == 3 ||
             (prrte_process_info.num_procs <= 3 && PRRTE_PROC_MY_NAME->vpid == 0))
            && i == 9995) {
            prrte_errmgr.abort(rc, NULL);
        }
    }

    return 0;
}
