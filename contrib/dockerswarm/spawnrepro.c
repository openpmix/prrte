/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * spawnrepro -- mimics mpi4py's TestSpawnMultipleSelf sequence: every rank
 * repeatedly spawns a multi-app job off MPI_COMM_SELF, barriers on the
 * resulting intercommunicator, and disconnects.  Acts as its own child.
 *
 * This is the MPI-level shape, and the level matters.  The equivalent written
 * against PMIx directly (spawnloop --self) does not express two of the things
 * this does: the barrier on the *intercommunicator*, which is a collective
 * over the union of the parent proc and the whole child job, and
 * MPI_Comm_disconnect, which is what actually dissolves that assemblage.
 * Those are the operations whose ordering against job termination is under
 * test, so a PMIx-level approximation of "spawn, connect, disconnect" can run
 * clean while this does not.
 *
 * Every rank spawns off COMM_SELF, so N ranks means N independent spawn
 * requests in flight at the DVM master at once, each of which joins its
 * session before it has been named.  That concurrency is the point.
 *
 * Build:  mpicc -DSELF_PATH='"/abs/path/to/binary"' -o binary spawnrepro.c
 * Run:    mpiexec -n 2 ./binary [iterations]
 */

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

#if defined(__SANITIZE_ADDRESS__)
/* Turn LeakSanitizer off from inside the program.
 *
 * The environment cannot do this job here.  ASAN_OPTIONS reaches the ranks
 * mpiexec starts (with -x), but NOT the ones this program spawns: a spawned
 * job's environment comes from the spawn request, not from its parent's
 * shell.  So the children exit non-zero on LeakSanitizer's report, the
 * runtime reads that as the child job failing, and the connected-assemblage
 * rule then terminates the parent - which looks exactly like the runtime
 * defect being hunted, and is not.  (The leaks are real, but they are in
 * MPI_Init's own setup and are not what this test is about.)
 *
 * __asan_default_options is the documented hook for exactly this, and it is
 * read before main, in every process running this binary. */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_leaks=0";
}
#endif

#ifndef SELF_PATH
#    error "SELF_PATH must name this binary"
#endif

int main(int argc, char *argv[])
{
    MPI_Comm parent;
    int rank, iters = 8;

    MPI_Init(&argc, &argv);
    MPI_Comm_get_parent(&parent);

    if (MPI_COMM_NULL != parent) {
        /* child: exactly what test/spawn_child.py does */
        MPI_Barrier(parent);
        MPI_Comm_disconnect(&parent);
        MPI_Finalize();
        return 0;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (argc > 1) {
        iters = atoi(argv[1]);
    }

    for (int i = 0; i < iters; ++i) {
        char *cmds[2] = {(char *) SELF_PATH, (char *) SELF_PATH};
        char **argvs[2] = {MPI_ARGV_NULL, MPI_ARGV_NULL};
        int maxprocs[2] = {1, 2};
        MPI_Info infos[2] = {MPI_INFO_NULL, MPI_INFO_NULL};
        int errcodes[3];
        MPI_Comm child;

        MPI_Barrier(MPI_COMM_SELF);
        MPI_Comm_spawn_multiple(2, cmds, argvs, maxprocs, infos, 0, MPI_COMM_SELF, &child,
                                errcodes);
        MPI_Barrier(child);
        MPI_Comm_disconnect(&child);
        MPI_Barrier(MPI_COMM_SELF);

        if (0 == rank) {
            printf("iteration %d ok\n", i);
            fflush(stdout);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (0 == rank) {
        printf("all %d iterations ok\n", iters);
        fflush(stdout);
    }
    MPI_Finalize();
    return 0;
}
