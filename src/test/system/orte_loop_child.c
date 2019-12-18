#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "constants.h"

#include "src/util/proc_info.h"
#include "src/runtime/runtime.h"

int main( int argc, char **argv )
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI))) {
        fprintf(stderr, "couldn't init prrte - error code %d\n", rc);
        return rc;
    }
    sleep(1);
    prrte_finalize();

    return 0;
}
