/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * tm-probe: ask the PBS TM API directly what a TM-based launcher relies on.
 *
 *     tm-probe <shell command>
 *
 * Run inside a PBS job. Spawns "/bin/sh -c <shell command>" on every node of
 * the job but this one, with an environment of its own - PRTE_TM_PROBE set,
 * a minimal PATH, nothing inherited - waits for each spawn to be accepted,
 * and exits WITHOUT waiting for the tasks. The suite then checks, from the
 * job's output, that the tasks saw PRTE_TM_PROBE (tm_spawn delivers the
 * environment it is given, which pbs_tmrsh does not let a caller choose),
 * that they ran to the end after we had gone, and that PBS charged the job
 * for them.
 *
 * Prints "nnodes=<n>", then "spawned=<n>" and exits 0 when every spawn was
 * accepted; anything else exits 1 after saying what failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tm.h>

int main(int argc, char **argv)
{
    struct tm_roots roots;
    tm_node_id *nodes = NULL;
    tm_task_id tid;
    tm_event_t ev, got;
    int nnodes = 0, rc, err, n, spawned = 0;
    char *env[] = {"PRTE_TM_PROBE=delivered-by-tm_spawn", "PATH=/usr/bin:/bin", NULL};
    char *cmd[] = {"/bin/sh", "-c", NULL, NULL};

    if (2 != argc) {
        fprintf(stderr, "usage: tm-probe <shell command>\n");
        return 1;
    }
    cmd[2] = argv[1];

    if (TM_SUCCESS != (rc = tm_init(NULL, &roots))) {
        printf("tm_init failed: %d\n", rc);
        return 1;
    }
    if (TM_SUCCESS != (rc = tm_nodeinfo(&nodes, &nnodes))) {
        printf("tm_nodeinfo failed: %d\n", rc);
        return 1;
    }
    printf("nnodes=%d\n", nnodes);

    /* node 0 is this one; spawn on the rest, all at once */
    for (n = 1; n < nnodes; n++) {
        rc = tm_spawn(3, cmd, env, nodes[n], &tid, &ev);
        if (TM_SUCCESS != rc) {
            printf("tm_spawn on node %d failed: %d\n", n, rc);
            return 1;
        }
    }
    /* each spawn completes with one event; the tasks' exits we do not
     * wait for - that they outlive us is the point */
    for (n = 1; n < nnodes; n++) {
        rc = tm_poll(TM_NULL_EVENT, &got, 1, &err);
        if (TM_SUCCESS != rc || TM_SUCCESS != err) {
            printf("tm_poll: rc=%d err=%d\n", rc, err);
            return 1;
        }
        spawned++;
    }
    printf("spawned=%d\n", spawned);
    fflush(stdout);
    (void) tm_finalize();
    return 0;
}
