/* -*- C -*-
 *
 * $HEADER$
 *
 * Check error messages
 */

#include <stdio.h>
#include <unistd.h>

#include "src/runtime/runtime.h"
#include "src/mca/errmgr/errmgr.h"

int main(int argc, char* argv[])
{

    int rc, i;

    putenv("OMPI_MCA_prrte_report_silent_errors=1");

    if (0 > (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "prrte_abort: couldn't init prrte - error code %d\n", rc);
        return rc;
    }

    for (i=0; PRRTE_ERR_MAX < i; i--) {
        fprintf(stderr, "%d: %s\n", -1*i,
                (NULL == PRRTE_ERROR_NAME(i)) ? "NULL" : PRRTE_ERROR_NAME(i));
    }

    prrte_finalize();
    return 0;
}
