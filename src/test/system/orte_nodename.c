/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>

#include "src/class/opal_list.h"
#include "src/util/opal_environ.h"

#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/runtime.h"
#include "src/mca/ess/ess.h"

int main(int argc, char* argv[])
{
    int rc, i, restart=-1;
    char hostname[PRRTE_MAXHOSTNAMELEN], *rstrt;
    pid_t pid;

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "prrte_nodename: couldn't init prrte - error code %d\n", rc);
        return rc;
    }

    if (NULL != (rstrt = getenv("OMPI_MCA_prrte_num_restarts"))) {
        restart = strtol(rstrt, NULL, 10);
    }

    gethostname(hostname, sizeof(hostname));
    pid = getpid();

    printf("prrte_nodename: Node %s Name %s Pid %ld Restarts: %d\n",
           hostname, PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (long)pid, restart);

    for (i=0; NULL != environ[i]; i++) {
        if (0 == strncmp(environ[i], "OMPI_MCA", strlen("OMPI_MCA"))) {
            printf("\t%s\n", environ[i]);
        }
    }

    prrte_finalize();
    return 0;
}
