/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of applications
 */

#include <stdio.h>
#include "prrte/constants.h"
#include "prrte/runtime/runtime.h"

int main(int argc, char* argv[])
{
    if (PRRTE_SUCCESS != prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI)) {
        fprintf(stderr, "Failed prrte_init\n");
        exit(1);
    }

    if (PRRTE_SUCCESS != prrte_finalize()) {
        fprintf(stderr, "Failed prrte_finalize\n");
        exit(1);
    }
    return 0;
}
