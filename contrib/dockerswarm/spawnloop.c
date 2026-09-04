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
 *   spawnloop [--iters N] [--kids N] [--child] [--hold S]
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

    PMIX_PROC_LOAD(&procs[0], parent.nspace, PMIX_RANK_WILDCARD);
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
    char self[4096];
    ssize_t len;
    int iters = 25, kids = 2, hold = 0, i, n;
    struct timespec t0, t1;

    for (n = 1; n < argc; n++) {
        if (0 == strcmp(argv[n], "--child")) {
            return be_child();
        } else if (0 == strcmp(argv[n], "--iters") && n + 1 < argc) {
            iters = atoi(argv[++n]);
        } else if (0 == strcmp(argv[n], "--kids") && n + 1 < argc) {
            kids = atoi(argv[++n]);
        } else if (0 == strcmp(argv[n], "--hold") && n + 1 < argc) {
            hold = atoi(argv[++n]);
        } else {
            fprintf(stderr, "usage: %s [--iters N] [--kids N] [--child] [--hold S]\n",
                    argv[0]);
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
    len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (0 >= len) {
        printf("SPWN FAIL self-path 0 cannot read /proc/self/exe\n");
        fflush(stdout);
        return 1;
    }
    self[len] = '\0';

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        printf("SPWN FAIL parent-init 0 %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        return 1;
    }
    whereami("PARENT", NULL);

    for (i = 0; i < iters; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);

        PMIX_APP_CREATE(app, 1);
        app->cmd = strdup(self);
        app->maxprocs = kids;
        PMIX_ARGV_APPEND(rc, app->argv, self);
        PMIX_ARGV_APPEND(rc, app->argv, "--child");
        rc = PMIx_Spawn(NULL, 0, app, 1, nsp2);
        PMIX_APP_FREE(app, 1);
        if (PMIX_SUCCESS != rc) {
            printf("SPWN FAIL spawn %d %s\n", i, PMIx_Error_string(rc));
            fflush(stdout);
            return 10;
        }

        PMIX_PROC_LOAD(&procs[0], myproc.nspace, PMIX_RANK_WILDCARD);
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

    printf("SPWN DONE %d\n", iters);
    fflush(stdout);
    PMIx_Finalize(NULL, 0);
    return 0;
}
