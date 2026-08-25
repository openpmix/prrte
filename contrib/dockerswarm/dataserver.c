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
 *   dataserver publish <key> <value> [range] [seconds] [access]
 *       PMIx_Publish the key, print "PUBLISHED <key>", then stay alive for
 *       <seconds> (the data lives as long as the publisher does, and a
 *       lookup has to find it while it is there).
 *
 *       <access> names an access-permission list to publish with:
 *         self-uid / other-uid   PMIX_ACCESS_USERIDS holding our own
 *                                effective uid, or one that is not ours,
 *                                inside a PMIX_ACCESS_PERMISSIONS array
 *         self-gid / other-gid   PMIX_ACCESS_GRPIDS at the top level,
 *                                holding our own effective gid or another
 *       Absent a list, published data is readable only by its publisher's
 *       own uid and gid, which is the default the data server applies.
 *
 *   dataserver lookup <key> [seconds] [range]
 *       PMIx_Lookup with no wait.  Prints one "FOUND <key> <value>" line per
 *       key returned, then "STATUS <status>".
 *
 *   dataserver lookupwait <key> [seconds] [range]
 *       PMIx_Lookup with PMIX_WAIT, which parks the request in the data
 *       server until somebody publishes the key.  Prints "WAITING" first so
 *       the harness can tell it got that far.  It also carries PMIX_TIMEOUT
 *       = <seconds>, so a key nobody ever publishes ends in
 *       "STATUS PMIX_ERR_TIMEOUT" rather than hanging.
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
 *   dataserver persist <key> <value> <persistence> [seconds] [range]
 *       Publish with an explicit PMIX_PERSISTENCE and exit, so the JOB
 *       ends.  <persistence> is one of first-read, proc, app, session,
 *       indef.  What a later job can still look up is what the data server
 *       kept: "app" must be gone once this job terminates, "session" must
 *       not be.  Persistence means nothing until something removes data on
 *       a lifetime boundary, so this is the test that it does.
 *
 *   dataserver unpublish <key> [seconds] [pubrange] [unpubrange]
 *       Publish, then unpublish, then look up - all from one process, since
 *       only the publisher may unpublish its own data.  Prints
 *       "UNPUBLISHED" and then the lookup outcome.
 *
 *       The two ranges are separate on purpose.  Duplicate keys are allowed
 *       on different ranges, so an unpublish removes only what was published
 *       to the range IT names (PMIX_RANGE_SESSION when it names none) - give
 *       the two arguments different values and the item must survive.
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

/* Load the access-permission directive named by <access> into *info.
 * Returns the number of directives loaded (0 or 1).  The uid form is
 * nested inside PMIX_ACCESS_PERMISSIONS and the gid form is given at the
 * top level, so the two spellings the data server accepts both get
 * exercised. */
static size_t load_access(const char *access, pmix_info_t *info)
{
    pmix_data_array_t *ids, *perms;
    uint32_t *idp;
    pmix_info_t *ip;

    if (NULL == access) {
        return 0;
    }

    PMIX_DATA_ARRAY_CREATE(ids, 1, PMIX_UINT32);
    idp = (uint32_t *) ids->array;

    if (0 == strcmp(access, "self-uid") || 0 == strcmp(access, "other-uid")) {
        idp[0] = (uint32_t) geteuid();
        if ('o' == access[0]) {
            idp[0] += 1;
        }
        /* wrap it in a PMIX_ACCESS_PERMISSIONS array */
        PMIX_DATA_ARRAY_CREATE(perms, 1, PMIX_INFO);
        ip = (pmix_info_t *) perms->array;
        PMIX_INFO_LOAD(&ip[0], PMIX_ACCESS_USERIDS, ids, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(ids);
        PMIX_INFO_LOAD(info, PMIX_ACCESS_PERMISSIONS, perms, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(perms);
        return 1;
    }

    if (0 == strcmp(access, "self-gid") || 0 == strcmp(access, "other-gid")) {
        idp[0] = (uint32_t) getegid();
        if ('o' == access[0]) {
            idp[0] += 1;
        }
        PMIX_INFO_LOAD(info, PMIX_ACCESS_GRPIDS, ids, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(ids);
        return 1;
    }

    PMIX_DATA_ARRAY_FREE(ids);
    fprintf(stderr, "ERROR unknown access spec: %s\n", access);
    return 0;
}

static int do_publish(const char *key, const char *value,
                      const char *rangestr, int seconds, const char *access)
{
    pmix_status_t rc;
    pmix_info_t info[3];
    size_t n, ninfo = 2;
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
    ninfo += load_access(access, &info[2]);

    rc = PMIx_Publish(info, ninfo);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
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

static int do_unpublish(const char *key, int seconds, const char *pubrange,
                        const char *unpubrange)
{
    pmix_status_t rc;
    pmix_info_t info[2];
    pmix_data_range_t range = parse_range(pubrange);
    pmix_data_range_t urange = parse_range(unpubrange);
    /* PMIx_Unpublish takes a NULL-terminated key array */
    char *keys[2] = {NULL, NULL};

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_INFO_LOAD(&info[0], key, "unpublish-me", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_RANGE, &range, PMIX_DATA_RANGE);
    rc = PMIx_Publish(info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
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
    (void) lookup_keys(keys, 1, seconds, false, pubrange);

    keys[0] = (char *) key;
    keys[1] = NULL;
    if (NULL == unpubrange) {
        /* say nothing about the range, which is the common case and the
         * one that leans on the default matching the publish default */
        rc = PMIx_Unpublish(keys, NULL, 0);
    } else {
        PMIX_INFO_LOAD(&info[0], PMIX_RANGE, &urange, PMIX_DATA_RANGE);
        rc = PMIx_Unpublish(keys, info, 1);
        PMIX_INFO_DESTRUCT(&info[0]);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Unpublish: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    printf("UNPUBLISHED %s\n", key);
    fflush(stdout);

    keys[0] = (char *) key;
    (void) lookup_keys(keys, 1, seconds, false, pubrange);

    PMIx_Finalize(NULL, 0);
    return 0;
}

static pmix_persistence_t parse_persist(const char *s)
{
    if (NULL == s) {
        return PMIX_PERSIST_APP;
    }
    if (0 == strcmp(s, "first-read")) {
        return PMIX_PERSIST_FIRST_READ;
    }
    if (0 == strcmp(s, "proc")) {
        return PMIX_PERSIST_PROC;
    }
    if (0 == strcmp(s, "session")) {
        return PMIX_PERSIST_SESSION;
    }
    if (0 == strcmp(s, "indef")) {
        return PMIX_PERSIST_INDEF;
    }
    return PMIX_PERSIST_APP;
}

static int do_persist(const char *key, const char *value, const char *pstr,
                      int seconds, const char *rangestr)
{
    pmix_status_t rc;
    pmix_info_t info[3];
    pmix_data_range_t range = parse_range(rangestr);
    pmix_persistence_t persist = parse_persist(pstr);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_INFO_LOAD(&info[0], key, value, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_RANGE, &range, PMIX_DATA_RANGE);
    PMIX_INFO_LOAD(&info[2], PMIX_PERSISTENCE, &persist, PMIX_PERSIST);
    rc = PMIx_Publish(info, 3);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
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

int main(int argc, char **argv)
{
    char *keys[2];

    if (2 > argc) {
        fprintf(stderr,
                "usage: %s publish <key> <value> [range] [secs] [access]\n"
                "       %s lookup <key> [secs] [range]\n"
                "       %s lookupwait <key> [secs] [range]\n"
                "       %s lookup2 <key1> <key2> [secs]\n"
                "       %s unpublish <key> [secs] [pubrange] [unpubrange]\n"
                "       %s persist <key> <value> <persistence> [secs] [range]\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    if (0 == strcmp(argv[1], "publish")) {
        if (4 > argc) {
            fprintf(stderr, "publish needs <key> <value>\n");
            return 2;
        }
        return do_publish(argv[2], argv[3], (5 > argc) ? NULL : argv[4],
                          (6 > argc) ? 120 : atoi(argv[5]),
                          (7 > argc) ? NULL : argv[6]);
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
        return do_unpublish(argv[2], (4 > argc) ? 20 : atoi(argv[3]),
                            (5 > argc) ? NULL : argv[4],
                            (6 > argc) ? NULL : argv[5]);
    }

    if (0 == strcmp(argv[1], "persist")) {
        if (5 > argc) {
            fprintf(stderr, "persist needs <key> <value> <persistence>\n");
            return 2;
        }
        return do_persist(argv[2], argv[3], argv[4],
                          (6 > argc) ? 0 : atoi(argv[5]),
                          (7 > argc) ? NULL : argv[6]);
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
