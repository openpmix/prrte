/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * dataserver -- a minimal PMIx client for exercising PRRTE's publish/lookup
 * data server (src/runtime/data_server/) across nodes.
 *
 *   dataserver publish <key> <value> [range] [seconds]
 *       PMIx_Publish the key, print "PUBLISHED <key>", then stay alive for
 *       <seconds> (the data lives as long as the publisher does, and a
 *       lookup has to find it while it is there).
 *
 *   dataserver lookup <key> [seconds] [range]
 *       PMIx_Lookup with no wait.  Prints one "FOUND <key> <value>" line per
 *       key returned, then "STATUS <status>".
 *
 *   dataserver lookupwait <key> [seconds] [range]
 *       PMIx_Lookup with PMIX_WAIT, which parks the request in the data
 *       server until somebody publishes the key.  Prints "WAITING" first so
 *       the harness can tell it got that far.
 *
 *   dataserver lookup2 <key1> <key2> [seconds]
 *       Look both keys up in one call.  With only one of them published
 *       this is the PARTIAL_SUCCESS path.
 *
 * The lookup range matters and defaults to whatever PMIx picks.  A
 * PMIX_RANGE_LOCAL publish is routed by the daemon to its OWN data server
 * instance rather than to the HNP (see pmix_server_pub.c), so it is only
 * reachable by a lookup carrying the same range.
 *
 *   dataserver unpublish <key> [seconds]
 *       Publish, then unpublish, then look up - all from one process, since
 *       only the publisher may unpublish its own data.  Prints
 *       "UNPUBLISHED" and then the lookup outcome.
 *
 * <range> is one of session (default), namespace, local, proc-local, global.
 *
 * Why this exists: the store is a SINGLE array on the HNP, and every client
 * reaches it over the RML through its own daemon.  So the publisher's
 * "proxy" (the daemon that relayed its request) and the requestor's proxy
 * are only different objects when the two processes are on different nodes -
 * which is exactly what PMIX_RANGE_LOCAL discriminates on, and what a
 * single-host run can never distinguish.  Same for the parked-request path:
 * the publish that satisfies a waiting lookup arrives from one daemon and
 * the reply goes back out through another.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;

static pmix_data_range_t parse_range(const char *s)
{
    if (NULL == s) {
        return PMIX_RANGE_SESSION;
    }
    if (0 == strcmp(s, "namespace")) {
        return PMIX_RANGE_NAMESPACE;
    }
    if (0 == strcmp(s, "local")) {
        return PMIX_RANGE_LOCAL;
    }
    if (0 == strcmp(s, "proc-local")) {
        return PMIX_RANGE_PROC_LOCAL;
    }
    if (0 == strcmp(s, "global")) {
        return PMIX_RANGE_GLOBAL;
    }
    return PMIX_RANGE_SESSION;
}

static int do_publish(const char *key, const char *value,
                      const char *rangestr, int seconds)
{
    pmix_status_t rc;
    pmix_info_t info[2];
    pmix_data_range_t range = parse_range(rangestr);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    printf("NSPACE %s\n", myproc.nspace);
    fflush(stdout);

    PMIX_INFO_LOAD(&info[0], key, value, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_RANGE, &range, PMIX_DATA_RANGE);

    rc = PMIx_Publish(info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Publish(%s): %s\n", key, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    printf("PUBLISHED %s\n", key);
    fflush(stdout);

    sleep(seconds);

    PMIx_Finalize(NULL, 0);
    return 0;
}

/* Look up 1..2 keys and report what came back. Returns 0 if the lookup
 * itself succeeded (fully or partially). */
static int lookup_keys(char **keys, size_t nkeys, int seconds, bool wait,
                       const char *rangestr)
{
    pmix_status_t rc;
    pmix_pdata_t *pdata;
    pmix_info_t info[3];
    pmix_data_range_t range;
    size_t n, ninfo = 1;

    PMIX_PDATA_CREATE(pdata, nkeys);
    for (n = 0; n < nkeys; n++) {
        PMIX_LOAD_KEY(pdata[n].key, keys[n]);
    }

    PMIX_INFO_LOAD(&info[0], PMIX_TIMEOUT, &seconds, PMIX_INT);
    if (wait) {
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_WAIT, NULL, PMIX_BOOL);
        printf("WAITING\n");
        fflush(stdout);
    }
    if (NULL != rangestr) {
        range = parse_range(rangestr);
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_RANGE, &range, PMIX_DATA_RANGE);
    }

    rc = PMIx_Lookup(pdata, nkeys, info, ninfo);

    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }

    for (n = 0; n < nkeys; n++) {
        if (0 == strlen(pdata[n].key) || PMIX_UNDEF == pdata[n].value.type) {
            continue;
        }
        printf("FOUND %s %s (from %s:%u)\n", pdata[n].key,
               (PMIX_STRING == pdata[n].value.type && NULL != pdata[n].value.data.string)
                   ? pdata[n].value.data.string : "<non-string>",
               pdata[n].proc.nspace, (unsigned) pdata[n].proc.rank);
    }
    printf("STATUS %s\n", PMIx_Error_string(rc));
    fflush(stdout);

    PMIX_PDATA_FREE(pdata, nkeys);
    /* A partial result is a legitimate outcome we are deliberately probing,
     * not a failure of this process.  Exiting non-zero would make PRRTE tear
     * the job down and bury the FOUND lines under an abort banner. */
    return (PMIX_SUCCESS == rc || PMIX_ERR_PARTIAL_SUCCESS == rc
            || PMIX_ERR_NOT_FOUND == rc) ? 0 : 1;
}

static int do_lookup(char **keys, size_t nkeys, int seconds, bool wait,
                     const char *rangestr)
{
    pmix_status_t rc;
    int ret;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    ret = lookup_keys(keys, nkeys, seconds, wait, rangestr);
    PMIx_Finalize(NULL, 0);
    return ret;
}

static int do_unpublish(const char *key, int seconds)
{
    pmix_status_t rc;
    pmix_info_t info;
    /* PMIx_Unpublish takes a NULL-terminated key array */
    char *keys[2] = {NULL, NULL};

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_INFO_LOAD(&info, key, "unpublish-me", PMIX_STRING);
    rc = PMIx_Publish(&info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Publish: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    printf("PUBLISHED %s\n", key);
    fflush(stdout);

    /* confirm it is really there before we take it away, so a later
     * NOTFOUND cannot be blamed on the publish */
    keys[0] = (char *) key;
    (void) lookup_keys(keys, 1, seconds, false, NULL);

    keys[0] = (char *) key;
    keys[1] = NULL;
    rc = PMIx_Unpublish(keys, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Unpublish: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    printf("UNPUBLISHED %s\n", key);
    fflush(stdout);

    keys[0] = (char *) key;
    (void) lookup_keys(keys, 1, seconds, false, NULL);

    PMIx_Finalize(NULL, 0);
    return 0;
}

int main(int argc, char **argv)
{
    char *keys[2];

    if (2 > argc) {
        fprintf(stderr,
                "usage: %s publish <key> <value> [range] [secs]\n"
                "       %s lookup <key> [secs] [range]\n"
                "       %s lookupwait <key> [secs] [range]\n"
                "       %s lookup2 <key1> <key2> [secs]\n"
                "       %s unpublish <key> [secs]\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    if (0 == strcmp(argv[1], "publish")) {
        if (4 > argc) {
            fprintf(stderr, "publish needs <key> <value>\n");
            return 2;
        }
        return do_publish(argv[2], argv[3], (5 > argc) ? NULL : argv[4],
                          (6 > argc) ? 120 : atoi(argv[5]));
    }
    if (0 == strcmp(argv[1], "lookup") || 0 == strcmp(argv[1], "lookupwait")) {
        if (3 > argc) {
            fprintf(stderr, "%s needs a key\n", argv[1]);
            return 2;
        }
        keys[0] = argv[2];
        return do_lookup(keys, 1, (4 > argc) ? 20 : atoi(argv[3]),
                         (0 == strcmp(argv[1], "lookupwait")),
                         (5 > argc) ? NULL : argv[4]);
    }
    if (0 == strcmp(argv[1], "lookup2")) {
        if (4 > argc) {
            fprintf(stderr, "lookup2 needs two keys\n");
            return 2;
        }
        keys[0] = argv[2];
        keys[1] = argv[3];
        return do_lookup(keys, 2, (5 > argc) ? 20 : atoi(argv[4]), false, NULL);
    }
    if (0 == strcmp(argv[1], "unpublish")) {
        if (3 > argc) {
            fprintf(stderr, "unpublish needs a key\n");
            return 2;
        }
        return do_unpublish(argv[2], (4 > argc) ? 20 : atoi(argv[3]));
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
