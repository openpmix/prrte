/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * slotinfo -- ask the daemon we are connected to about the allocation.
 *
 * Prints one line per key, tagged with the node the asking rank is on:
 *
 *   SLOTS <host> <n>          PMIX_NUM_SLOTS, no qualifier
 *   AVAIL <host> <n>          PMIX_QUERY_AVAILABLE_SLOTS
 *   ERROR <host> <key> <why>  the query failed
 *
 * Why this exists: the answers are properties of the DVM, so every rank must
 * get the same ones no matter which daemon it happens to be talking to.  A
 * prted holds only the identity half of the node pool - the nidmap ships node
 * names, aliases, daemon vpids and pool slots, never slot counts or node
 * state - so a daemon answering out of its own copy reports every node as
 * having zero slots, and reports it as success.  The rank that lands on the
 * master gets the true count, every other rank gets 0, and nothing anywhere
 * says a word about it.  That is invisible on one node, which is why this
 * test is here rather than in test/unit: it takes a DVM whose ranks are on
 * daemons other than the master to see it at all.
 *
 * The daemon now relays such a query to the master (PRTE_RML_TAG_QUERY) and
 * merges the answer with whatever it could answer itself, so the assertion
 * the harness makes is simply that every rank agrees, and that the number is
 * not zero.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_query_t *query;
    pmix_info_t *results = NULL;
    size_t nresults = 0, n;
    char host[256];
    uint32_t val;
    int ret = 0;

    (void) argc;
    (void) argv;

    if (0 != gethostname(host, sizeof(host))) {
        strncpy(host, "unknown", sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* Both keys in ONE query, deliberately: they are answered by different
     * arms, and a request that mixes keys is exactly what the relay has to
     * keep whole.  Anything the daemon can answer itself must still come
     * back alongside what only the master can answer. */
    PMIX_QUERY_CREATE(query, 1);
    PMIx_Argv_append_nosize(&query[0].keys, PMIX_NUM_SLOTS);
    PMIx_Argv_append_nosize(&query[0].keys, PMIX_QUERY_AVAILABLE_SLOTS);

    rc = PMIx_Query_info(query, 1, &results, &nresults);
    if (PMIX_SUCCESS != rc && PMIX_QUERY_PARTIAL_SUCCESS != rc) {
        printf("ERROR %s query %s\n", host, PMIx_Error_string(rc));
        fflush(stdout);
        ret = 1;
        goto done;
    }

    for (n = 0; n < nresults; n++) {
        if (PMIX_CHECK_KEY(&results[n], PMIX_NUM_SLOTS)) {
            PMIX_VALUE_GET_NUMBER(rc, &results[n].value, val, uint32_t);
            if (PMIX_SUCCESS != rc) {
                printf("ERROR %s %s bad-type\n", host, PMIX_NUM_SLOTS);
                ret = 1;
                continue;
            }
            printf("SLOTS %s %u\n", host, val);
        } else if (PMIX_CHECK_KEY(&results[n], PMIX_QUERY_AVAILABLE_SLOTS)) {
            PMIX_VALUE_GET_NUMBER(rc, &results[n].value, val, uint32_t);
            if (PMIX_SUCCESS != rc) {
                printf("ERROR %s %s bad-type\n", host, PMIX_QUERY_AVAILABLE_SLOTS);
                ret = 1;
                continue;
            }
            printf("AVAIL %s %u\n", host, val);
        }
    }
    fflush(stdout);

done:
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_QUERY_FREE(query, 1);
    PMIx_Finalize(NULL, 0);
    return ret;
}
