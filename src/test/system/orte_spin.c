/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that just spins - provides mechanism for testing user-driven
 * abnormal program termination
 */
#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>

#include "src/event/event-internal.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/runtime/runtime.h"

int main(int argc, char* argv[])
{
    int i;
    float pi;

    if (PRRTE_SUCCESS != prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI)) {
        fprintf(stderr, "PRRTE_INIT FAILED\n");
        exit(1);
    }
    opal_output(0, "%s RUNNING", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    i = 0;
    while (1) {
        i++;
        pi = i / 3.14159256;
        if (i == 9995) {
            i=0;
        }
    }

    prrte_finalize();

    return 0;
}
