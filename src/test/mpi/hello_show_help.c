/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>
#include "mpi.h"
#include "src/util/output.h"

#include "src/util/show_help.h"

int main(int argc, char* argv[])
{
    int rank, size;
    int stream;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (0 == rank) {
        opal_output(0, "============================================================================");
        opal_output(0, "This test ensures that the aggregation functionality of the prrte_show_help\nsystem is working properly.  It outputs a bogus warning about prrte_init(),\nand contains sleep statements to ensure that the timer is firiing properly\nin the HNP and aggregates messages properly.  The total sleep time is\n(3 * num_procs).  You should see:\n\n - aggregation messages from the HNP every five seconds or so\n - a total of (2 * num_procs) messages");
        opal_output(0, "============================================================================");
    }
    MPI_Barrier(MPI_COMM_WORLD);

    prrte_show_help("help-prrte-runtime.txt",
                   "prrte_init:startup:internal-failure", true,
                   "Nothing", "PRRTE_EVERYTHING_IS_PEACHY", "42");
    sleep(rank * 3);

    prrte_show_help("help-prrte-runtime.txt",
                   "prrte_init:startup:internal-failure", true,
                   "Duplicate prrte_show_help detection",
                   "PRRTE_SHOW_HELP_DUPLICATE_FAILED", "99999");

    MPI_Barrier(MPI_COMM_WORLD);

    if (0 == rank) {
        opal_output(0, "============================================================================");
        opal_output(0, "The test is now complete.  Please verify that the HNP output all the required\nmessages (you may see 1 or 2 more messages from the HNP after this message).");
        opal_output(0, "============================================================================");
    }
    MPI_Finalize();

    return 0;
}
