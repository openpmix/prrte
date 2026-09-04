/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * spawnloop -- call PMIx_Spawn many times from one long-lived process, from a
 * node that is NOT the DVM master.
 *
 * Every other spawn client here spawns once.  One spawn cannot reach the
 * state that only exists between spawns, and there is a good deal of it:
 *
 *   - The requesting daemon tracks each spawn in a slot of
 *     prte_pmix_server_globals.local_reqs (the "room number"), writes that
 *     index onto the job as PRTE_JOB_ROOM_NUM, and ships it to the master so
 *     the answer can be matched back.  Slots are recycled by
 *     pmix_pointer_array_add as requests retire, and the room number is never
 *     cleared from the job - so a second, later use of a job's stored room
 *     number does not fail, it completes whatever spawn now holds that slot.
 *     There IS a second user: prted_comm.c answers again out of
 *     DVM_CLEANUP_JOB_CMD when the job ends.  It is guarded, and the guard is
 *     a flag on the daemon's own copy of the job; repetition is what makes a
 *     window in that guard show up as a wrong or missing completion.
 *
 *   - The response only travels by RML - and the room number only means
 *     anything on a remote daemon - when the requesting process is NOT on the
 *     master's node.  On one node prte_plm_base_spawn_response takes the
 *     local shortcut and none of the above is exercised, which is why this
 *     client belongs in this harness rather than in a unit test.
 *
 * The child is this same binary run with --child: it finds its parent through
 * PMIX_PARENT_ID, connects, and disconnects, so each iteration also builds and
 * dissolves a connected assemblage - the shape MPI_Comm_spawn and
 * MPI_Comm_disconnect produce, and the thing mpi4py's spawn tests do over and
 * over.
 *
 *   spawnloop [--iters N] [--kids N] [--apps N] [--self] [--child] [--hold S]
 *
 * --self makes EVERY rank spawn on its own, and connect only to the children
 * it asked for - the shape MPI_Comm_spawn_multiple on COMM_SELF produces, and
 * the one mpi4py's TestSpawnMultipleSelf drives.  It is the concurrent case:
 * several spawn requests are in flight at the master at once, each of which
 * joins its session before it is named, while earlier children are completing.
 * --apps gives each spawn several app contexts, as spawn_multiple does.
 *
 * --hold turns it into a placeholder instead: report where the runtime put
 * this proc, stay alive S seconds, leave.  That is what makes the OTHER
 * cross-job property checkable - a node rank names a proc among everything
 * alive on its node, across every job there, so it takes three overlapping
 * jobs to show whether the runtime is handing out duplicates.
 *
 * Output lines, all prefixed SPWN so the harness can grep:
 *
 *   SPWN PARENT <rank> HOST <hostname> NODERANK <nr>
 *   SPWN CHILD <nspace> <rank> HOST <hostname> NODERANK <nr>
 *   SPWN ITER <i> OK <child nspace> <seconds>
 *   SPWN FAIL <what> <iter> <status string>
 *   SPWN DONE <iterations completed>
 *
 * A hang is a failure too, and the harness times the whole run out; the
 * per-iteration line is what tells you which iteration it hung on.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

/* Where am I, and what node rank did the runtime give me?  Printed rather
 * than checked: two live procs sharing a node rank is a real defect, but it
 * is the harness that can compare across jobs, not a proc inside one. */
static void whereami(const char *role, const char *nspace)
{
    pmix_value_t *val = NULL;
    char host[256];
    unsigned nr = 0;

    if (0 > gethostname(host, sizeof(host))) {
        snprintf(host, sizeof(host), "?");
    }
    host[sizeof(host) - 1] = '\0';
    if (PMIX_SUCCESS == PMIx_Get(&myproc, PMIX_NODE_RANK, NULL, 0, &val) &&
        NULL != val) {
        nr = (unsigned) val->data.uint16;
        PMIX_VALUE_RELEASE(val);
    }
    if (NULL == nspace) {
        printf("SPWN %s %u HOST %s NODERANK %u\n", role, myproc.rank, host, nr);
    } else {
        printf("SPWN %s %s %u HOST %s NODERANK %u\n", role, nspace, myproc.rank,
               host, nr);
    }
    fflush(stdout);
}

/* The child half: attach to the parent named by PMIX_PARENT_ID, then leave
 * cleanly.  A child that cannot find its parent is a failure of the spawn,
 * not of the child - PMIX_PARENT_ID is what the runtime publishes to say who
 * asked for this job. */
static int be_child(void)
{
    pmix_proc_t parent, procs[2];
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        printf("SPWN FAIL child-init 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 1;
    }
    whereami("CHILD", myproc.nspace);

    PMIX_PROC_LOAD(&parent, myproc.nspace, myproc.rank);
    rc = PMIx_Get(&parent, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        printf("SPWN FAIL child-parentid 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 2;
    }
    memcpy(&parent, val->data.proc, sizeof(pmix_proc_t));
    PMIX_VALUE_RELEASE(val);

    /* PMIX_PARENT_ID names a PROC.  Connect to exactly that proc, so a
     * COMM_SELF-style spawn does not demand the spawner's whole job. */
    PMIX_PROC_LOAD(&procs[0], parent.nspace, parent.rank);
    PMIX_PROC_LOAD(&procs[1], myproc.nspace, PMIX_RANK_WILDCARD);

    if (PMIX_SUCCESS != (rc = PMIx_Connect(procs, 2, NULL, 0))) {
        printf("SPWN FAIL child-connect 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 3;
    }
    if (PMIX_SUCCESS != (rc = PMIx_Disconnect(procs, 2, NULL, 0))) {
        printf("SPWN FAIL child-disconnect 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 4;
    }
    PMIx_Finalize(NULL, 0);
    return 0;
}

int main(int argc, char **argv)
{
    pmix_proc_t procs[2];
    pmix_app_t *app;
    pmix_status_t rc;
    char nsp2[PMIX_MAX_NSLEN + 1];
    char selfpath[4096];
    ssize_t len;
    int iters = 25, kids = 2, hold = 0, napps = 1, nfailed = 0, i, n;
    size_t a;
    bool self = false, orphan = false;
    struct timespec t0, t1;

    for (n = 1; n < argc; n++) {
        if (0 == strcmp(argv[n], "--child")) {
            return be_child();
        } else if (0 == strcmp(argv[n], "--iters") && n + 1 < argc) {
            iters = atoi(argv[++n]);
        } else if (0 == strcmp(argv[n], "--kids") && n + 1 < argc) {
            kids = atoi(argv[++n]);
        } else if (0 == strcmp(argv[n], "--apps") && n + 1 < argc) {
            napps = atoi(argv[++n]);
        } else if (0 == strcmp(argv[n], "--self")) {
            self = true;
        } else if (0 == strcmp(argv[n], "--orphan")) {
            /* spawn children that will connect back to us, and then leave
             * without ever joining that connect.  A collective cannot
             * complete without every participant, so the children are stuck
             * unless the runtime ends the fence when we die. */
            orphan = true;
        } else if (0 == strcmp(argv[n], "--hold") && n + 1 < argc) {
            hold = atoi(argv[++n]);
        } else {
            fprintf(stderr, "usage: %s [--iters N] [--kids N] [--apps N] "
                            "[--self] [--child] [--hold S]\n", argv[0]);
            return 1;
        }
    }

    if (0 < hold) {
        if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
            printf("SPWN FAIL hold-init 0 %s\n", PMIx_Error_string(rc));
            fflush(stdout);
            return 1;
        }
        whereami("HOLD", myproc.nspace);
        sleep((unsigned) hold);
        PMIx_Finalize(NULL, 0);
        return 0;
    }

    /* The child is this same binary, and the spawn names it by absolute path
     * because /tmp and the cwd are per-container while the install volume is
     * shared - so the path has to be one that resolves on whatever node the
     * child is mapped onto. */
    len = readlink("/proc/self/exe", selfpath, sizeof(selfpath) - 1);
    if (0 >= len) {
        printf("SPWN FAIL self-path 0 cannot read /proc/self/exe\n");
        fflush(stdout);
        return 1;
    }
    selfpath[len] = '\0';

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        printf("SPWN FAIL parent-init 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 1;
    }
    whereami("PARENT", NULL);

    for (i = 0; i < iters; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);

        PMIX_APP_CREATE(app, (size_t) napps);
        for (a = 0; a < (size_t) napps; a++) {
            app[a].cmd = strdup(selfpath);
            app[a].maxprocs = kids;
            PMIX_ARGV_APPEND(rc, app[a].argv, selfpath);
            PMIX_ARGV_APPEND(rc, app[a].argv, "--child");
        }
        rc = PMIx_Spawn(NULL, 0, app, (size_t) napps, nsp2);
        PMIX_APP_FREE(app, (size_t) napps);
        if (PMIX_SUCCESS != rc) {
            /* Report and carry on, rather than leaving.  A caller that walks
             * out on a failed spawn is not what an application does - mpi4py
             * catches the error and runs the next test - and it changes what
             * is under test: an earlier child of this rank may still be
             * parked in PMIx_Connect waiting for us, and abandoning it turns
             * every later measurement into a study of that. */
            printf("SPWN FAIL spawn %d %s\n", i, PMIx_Error_string(rc));
            fflush(stdout);
            ++nfailed;
            continue;
        }

        /* COMM_SELF spawn connects the ONE spawning rank to its children;
         * a wildcard here would demand every rank of this job join, which
         * they are not doing - each is off spawning children of its own. */
        if (orphan) {
            printf("SPWN ORPHANED %d %s\n", i, nsp2);
            fflush(stdout);
            continue;
        }
        PMIX_PROC_LOAD(&procs[0], myproc.nspace,
                       self ? myproc.rank : PMIX_RANK_WILDCARD);
        PMIX_PROC_LOAD(&procs[1], nsp2, PMIX_RANK_WILDCARD);
        if (PMIX_SUCCESS != (rc = PMIx_Connect(procs, 2, NULL, 0))) {
            printf("SPWN FAIL connect %d %s\n", i, PMIx_Error_string(rc));
            fflush(stdout);
            return 11;
        }
        if (PMIX_SUCCESS != (rc = PMIx_Disconnect(procs, 2, NULL, 0))) {
            printf("SPWN FAIL disconnect %d %s\n", i, PMIx_Error_string(rc));
            fflush(stdout);
            return 12;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("SPWN ITER %d OK %s %.3f\n", i, nsp2,
               (double) (t1.tv_sec - t0.tv_sec) + 1.0e-9 * (double) (t1.tv_nsec - t0.tv_nsec));
        fflush(stdout);
    }

    printf("SPWN DONE %d (%d spawns failed)\n", iters, nfailed);
    fflush(stdout);
    PMIx_Finalize(NULL, 0);
    return 0;
}
