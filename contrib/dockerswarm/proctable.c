/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * proctable -- a minimal PMIx client for exercising the translators in
 * src/pmix/pmix.c across a real, multi-node DVM.
 *
 *   proctable procs [seconds]
 *       PMIx_Query(PMIX_QUERY_PROC_TABLE) for this process's OWN namespace.
 *       Prints one "PROC <rank> <host> <state> <statename>" line per entry,
 *       then "COUNT <n>".
 *
 *   proctable localprocs [seconds]
 *       Same, but PMIX_QUERY_LOCAL_PROC_TABLE -- only the procs the
 *       answering daemon hosts.
 *
 * Why this exists, and why it cannot be a unit test:
 *
 * prte_pmix_convert_state() is the only thing standing between a
 * prte_proc_t's state and what a tool or an application is told about that
 * proc, and PMIX_QUERY_PROC_TABLE is the one path that calls it (twice, in
 * pmix_server_queries.c). It was written with bare integer cases, and the two
 * state spaces do not number alike, so several PRRTE states fell through its
 * switch to PMIX_PROC_STATE_UNDEF. UNDEF is a legal answer, so nothing
 * anywhere reported an error -- a queried proc simply had no state.
 *
 * The unit test pins the mapping down as a table. What it cannot show is that
 * the mapping is reached with a *real* proc state on a *real* daemon: the
 * proc table is assembled from prte_proc_t objects the answering daemon
 * holds, and which of those it holds is exactly what differs between one node
 * and ten. The local-vs-global proc table split has no meaning at all on a
 * single host.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

static const char *statename(pmix_proc_state_t s)
{
    switch (s) {
    case PMIX_PROC_STATE_UNDEF:
        return "UNDEF";
    case PMIX_PROC_STATE_PREPPED:
        return "PREPPED";
    case PMIX_PROC_STATE_LAUNCH_UNDERWAY:
        return "LAUNCH_UNDERWAY";
    case PMIX_PROC_STATE_RESTART:
        return "RESTART";
    case PMIX_PROC_STATE_TERMINATE:
        return "TERMINATE";
    case PMIX_PROC_STATE_RUNNING:
        return "RUNNING";
    case PMIX_PROC_STATE_CONNECTED:
        return "CONNECTED";
    case PMIX_PROC_STATE_UNTERMINATED:
        return "UNTERMINATED";
    case PMIX_PROC_STATE_TERMINATED:
        return "TERMINATED";
    case PMIX_PROC_STATE_KILLED_BY_CMD:
        return "KILLED_BY_CMD";
    case PMIX_PROC_STATE_ABORTED:
        return "ABORTED";
    case PMIX_PROC_STATE_FAILED_TO_START:
        return "FAILED_TO_START";
    case PMIX_PROC_STATE_ABORTED_BY_SIG:
        return "ABORTED_BY_SIG";
    case PMIX_PROC_STATE_TERM_WO_SYNC:
        return "TERM_WO_SYNC";
    case PMIX_PROC_STATE_COMM_FAILED:
        return "COMM_FAILED";
    case PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED:
        return "SENSOR_BOUND_EXCEEDED";
    case PMIX_PROC_STATE_CALLED_ABORT:
        return "CALLED_ABORT";
    case PMIX_PROC_STATE_HEARTBEAT_FAILED:
        return "HEARTBEAT_FAILED";
    case PMIX_PROC_STATE_MIGRATING:
        return "MIGRATING";
    case PMIX_PROC_STATE_CANNOT_RESTART:
        return "CANNOT_RESTART";
    case PMIX_PROC_STATE_TERM_NON_ZERO:
        return "TERM_NON_ZERO";
    case PMIX_PROC_STATE_FAILED_TO_LAUNCH:
        return "FAILED_TO_LAUNCH";
    default:
        return "UNRECOGNIZED";
    }
}

/* Walk whatever the query returned looking for a PMIX_PROC_INFO array, and
 * print each entry. The daemon returns the table as a data array inside the
 * results, so this has to descend one level. */
static int print_proc_table(pmix_info_t *results, size_t nresults)
{
    size_t n, m;
    int found = 0;

    for (n = 0; n < nresults; n++) {
        pmix_data_array_t *da;
        pmix_proc_info_t *pi;

        if (PMIX_DATA_ARRAY != results[n].value.type) {
            continue;
        }
        da = results[n].value.data.darray;
        if (NULL == da || PMIX_PROC_INFO != da->type || NULL == da->array) {
            continue;
        }
        pi = (pmix_proc_info_t *) da->array;
        for (m = 0; m < da->size; m++) {
            printf("PROC %u %s %d %s\n", pi[m].proc.rank,
                   (NULL == pi[m].hostname) ? "-" : pi[m].hostname, (int) pi[m].state,
                   statename(pi[m].state));
            found++;
        }
    }
    printf("COUNT %d\n", found);
    return found;
}

static int do_proc_table(const char *key, int seconds)
{
    pmix_query_t query;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    PMIX_QUERY_CONSTRUCT(&query);
    PMIX_ARGV_APPEND(rc, query.keys, key);
    /* qualify with our own namespace - without it the daemon answers for
     * whatever it feels like, and we want OUR job's table */
    PMIX_QUERY_QUALIFIERS_CREATE(&query, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_NSPACE, myproc.nspace, PMIX_STRING);

    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    if (PMIX_SUCCESS != rc) {
        printf("ERR %d %s\n", (int) rc, PMIx_Error_string(rc));
        PMIX_QUERY_DESTRUCT(&query);
        return 1;
    }
    print_proc_table(results, nresults);
    PMIX_INFO_FREE(results, nresults);
    PMIX_QUERY_DESTRUCT(&query);

    if (0 < seconds) {
        sleep(seconds);
    }
    return 0;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    int ret = 1;

    if (2 > argc) {
        fprintf(stderr, "usage: %s procs|localprocs [args]\n", argv[0]);
        return 2;
    }

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (0 == strcmp(argv[1], "procs")) {
        ret = do_proc_table(PMIX_QUERY_PROC_TABLE, (3 <= argc) ? atoi(argv[2]) : 0);
    } else if (0 == strcmp(argv[1], "localprocs")) {
        ret = do_proc_table(PMIX_QUERY_LOCAL_PROC_TABLE, (3 <= argc) ? atoi(argv[2]) : 0);
    } else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        ret = 2;
    }

    PMIx_Finalize(NULL, 0);
    return ret;
}
