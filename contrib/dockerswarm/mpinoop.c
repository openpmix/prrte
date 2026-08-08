/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* An MPI probe for how much of the modex a job actually needs.
 *
 * The point is NOT to time MPI_Init.  Open MPI resolves a remote peer the
 * first time it talks to it, not at startup: `mpi_add_procs_cutoff' defaults
 * to 0 so the pre-add-everybody branch never runs, and ob1 only demands the
 * whole world when a BTL declares MCA_BTL_FLAGS_SINGLE_ADD_PROCS (TCP does
 * that only with more than one interface plus threads).  So MPI_Init adds
 * the node-local peers and nothing else, and every remote proc materializes
 * later through ompi_proc_for_name() when a communication operation first
 * names it - which is precisely the design direct modex exists to serve.
 *
 * That makes a bare MPI_Init/MPI_Finalize a *bad* probe for any question
 * about what a fence should distribute: it never touches a remote peer, so it
 * measures the fence and nothing else, and every answer looks identical.
 * What separates them is FIRST TOUCH, and the two patterns that bracket it:
 *
 *   --ring   each rank sendrecv's with its two neighbours only.  This is the
 *            cheap end - the pattern a sideways-sharing design would aim at,
 *            and the one a real neighbour exchange actually produces.
 *   --all    every rank exchanges with every other rank, point to point.
 *            This is the adversarial case for on-demand retrieval, because
 *            it is exactly the modex a "fetch only what you need" design
 *            declined to move up front.
 *
 * --all is deliberately NOT MPI_Alltoall.  A tuned MPI_Alltoall on one int
 * per peer runs Bruck and contacts about log2(N) partners, so it touches
 * nowhere near every proc and would understate this case badly - measured:
 * over 16 ranks it left a daemon fetching six peers where the honest answer
 * is every peer it does not already hold.  An explicit exchange with each
 * rank in turn is the only way to guarantee first contact with all of them.
 *
 * Note also that the MPI_Barrier lining each phase up is itself a
 * communication: a recursive-doubling barrier resolves log2(N) partners
 * before the timed section starts.  That is charged to the barrier, not to
 * the phase, which is correct - but it means the "touch" figure is the cost
 * of the peers the barrier did NOT already resolve.
 *
 * Each phase is timed separately, so "what did MPI_Init cost" and "what did
 * first contact cost" are never added together.  With neither flag this is
 * still just init and finalize, which is the honest baseline.
 *
 * Build with the mpicc of the Open MPI under test; run it under that Open
 * MPI's prterun (or PRRTE's, which is the same thing when Open MPI was
 * configured --with-prrte=external).
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* NOT MPI_Wtime: it may not be called before MPI_Init, and MPI_Init is one of
 * the things timed here.  Every container shares one host kernel clock, so
 * these are directly comparable across ranks the same way scaletest's are. */
static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    double t0, t1, t2, t3 = 0.0, t4 = 0.0;
    int rank, size, verbose = 0, ring = 0, all = 0;
    double touch_ms = 0.0, repeat_ms = 0.0;

    for (int i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else if (0 == strcmp(argv[i], "--ring")) {
            ring = 1;
        } else if (0 == strcmp(argv[i], "--all")) {
            all = 1;
        }
    }

    t0 = now_ms();
    MPI_Init(&argc, &argv);
    t1 = now_ms();

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (ring && 1 < size) {
        int left = (rank + size - 1) % size;
        int right = (rank + 1) % size;
        int out = rank, in = -1;

        /* Everyone lined up first, so the timing is first contact and not
         * whoever was slowest out of MPI_Init. */
        MPI_Barrier(MPI_COMM_WORLD);
        t2 = now_ms();
        MPI_Sendrecv(&out, 1, MPI_INT, right, 0, &in, 1, MPI_INT, left, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Sendrecv(&out, 1, MPI_INT, left, 1, &in, 1, MPI_INT, right, 1,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        t3 = now_ms();
        touch_ms = t3 - t2;

        /* The same exchange again, now that both peers are resolved.  The
         * difference between the two is what the resolution cost - which is
         * the number this probe exists to produce. */
        MPI_Barrier(MPI_COMM_WORLD);
        t3 = now_ms();
        MPI_Sendrecv(&out, 1, MPI_INT, right, 2, &in, 1, MPI_INT, left, 2,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Sendrecv(&out, 1, MPI_INT, left, 3, &in, 1, MPI_INT, right, 3,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        t4 = now_ms();
        repeat_ms = t4 - t3;
    } else if (all && 1 < size) {
        int *sbuf = (int *) malloc((size_t) size * sizeof(int));
        int *rbuf = (int *) malloc((size_t) size * sizeof(int));
        MPI_Request *req = (MPI_Request *) malloc(2 * (size_t) size * sizeof(MPI_Request));

        if (NULL == sbuf || NULL == rbuf || NULL == req) {
            fprintf(stderr, "rank %d: out of memory\n", rank);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        for (int i = 0; i < size; i++) {
            sbuf[i] = rank;
        }

        /* Explicit point to point with EVERY other rank, so every peer is
         * genuinely first-touched.  See the note above on why MPI_Alltoall
         * is not good enough here. */
        MPI_Barrier(MPI_COMM_WORLD);
        t2 = now_ms();
        {
            int n = 0;
            for (int i = 0; i < size; i++) {
                if (i == rank) {
                    continue;
                }
                MPI_Irecv(&rbuf[i], 1, MPI_INT, i, 10, MPI_COMM_WORLD, &req[n++]);
                MPI_Isend(&sbuf[i], 1, MPI_INT, i, 10, MPI_COMM_WORLD, &req[n++]);
            }
            MPI_Waitall(n, req, MPI_STATUSES_IGNORE);
        }
        t3 = now_ms();
        touch_ms = t3 - t2;

        MPI_Barrier(MPI_COMM_WORLD);
        t3 = now_ms();
        {
            int n = 0;
            for (int i = 0; i < size; i++) {
                if (i == rank) {
                    continue;
                }
                MPI_Irecv(&rbuf[i], 1, MPI_INT, i, 11, MPI_COMM_WORLD, &req[n++]);
                MPI_Isend(&sbuf[i], 1, MPI_INT, i, 11, MPI_COMM_WORLD, &req[n++]);
            }
            MPI_Waitall(n, req, MPI_STATUSES_IGNORE);
        }
        t4 = now_ms();
        repeat_ms = t4 - t3;

        free(sbuf);
        free(rbuf);
        free(req);
    }

    if (verbose) {
        printf("MPINOOP rank %d/%d init %.3f ms touch %.3f ms repeat %.3f ms\n",
               rank, size, t1 - t0, touch_ms, repeat_ms);
        fflush(stdout);
    }

    MPI_Finalize();
    t2 = now_ms();

    if (0 == rank) {
        printf("MPINOOP %d ranks %s init %.3f ms touch %.3f ms repeat %.3f ms finalize %.3f ms\n",
               size, ring ? "ring" : (all ? "all" : "none"),
               t1 - t0, touch_ms, repeat_ms, t2 - (all || ring ? t4 : t1));
        fflush(stdout);
    }
    return 0;
}
