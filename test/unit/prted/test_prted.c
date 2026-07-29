/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/prted -- the daemon/HNP body and the PMIx server
 * upcalls that sit under it.
 *
 * Most of src/prted only makes sense with a live DVM: the command dispatcher
 * needs peers to talk to, the spawn path needs a PLM, and the dmodex machinery
 * needs a second daemon that actually holds the data.  Those belong to the
 * integration and dockerswarm harnesses.  What is testable here in isolation
 * is the pure decision-making that the daemon does *before* it touches the
 * network -- and that is exactly where this review found defects:
 *
 *  - prte_strip_trailing_pathsep(), the --prefix/--pmix-prefix normalizer.
 *    Four copies of this loop used "strncpy(param, PRTE_PATH_SEP,
 *    sizeof(param) - 1)" on a char*, so sizeof gave the size of the pointer
 *    and strncpy NUL-padded eight bytes into whatever the prefix had been
 *    allocated as.  "--prefix /" is a two-byte allocation.  The loop also
 *    indexed param[len-1] before checking len, so an empty prefix read off
 *    the front of the allocation.
 *
 *  - prte_parse_singleton_id(), the "--singleton <nspace>.<rank>" splitter.
 *    It did strrchr(ptr, '.') and dereferenced the result unconditionally,
 *    so any value without a '.' was a null dereference.  The value is also
 *    handed down to PMIx_server_init as PMIX_SINGLETON, which faults on a
 *    malformed value, so it has to be rejected at the CLI.
 *
 *  - prte_pmix_xfer_job_info(), the spawn-directive translator.  This is the
 *    one substantial piece of src/prted that runs on plain data structures,
 *    so its policy handling (map-by/rank-by/bind-to conflict rejection, the
 *    boolean directives, the "cache anything unrecognized" default) is
 *    pinned down here rather than being exercised only through a spawn.
 *
 *  - prte_pmix_xfer_app(), which must not release the job object its caller
 *    owns on an error return -- it used to PMIX_RELEASE(jdata) on a getcwd
 *    failure, leaving the caller with a dangling pointer.
 *
 * The tests run without a DVM: prte_init_util() plus the rmaps/schizo/state
 * frameworks is enough for the translation paths.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "constants.h"
#include "src/event/event-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/prted/prted.h"
#include "src/prted/pmix/pmix_server_internal.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/*
 * --prefix normalization.
 *
 * Every case here is run on a heap allocation of exactly strlen+1 bytes, so
 * that any write past the end of the string is a genuine heap overflow that
 * a debug allocator or ASAN will catch -- which is the whole point: the bug
 * this replaces was silent on a large stack buffer and fatal on a short heap
 * one.
 */
static int check_strip(const char *in, const char *expected, int *failures)
{
    char *buf = strdup(in);
    int bad = 0;

    prte_strip_trailing_pathsep(buf);
    if (0 != strcmp(buf, expected)) {
        fprintf(stderr, "FAIL [strip]: \"%s\" -> \"%s\", expected \"%s\"\n",
                in, buf, expected);
        ++(*failures);
        bad = 1;
    }
    free(buf);
    return bad;
}

static int test_prefix_normalization(void)
{
    int failures = 0;

    /* the ordinary cases */
    check_strip("/opt/prrte", "/opt/prrte", &failures);
    check_strip("/opt/prrte/", "/opt/prrte", &failures);
    check_strip("/opt/prrte///", "/opt/prrte", &failures);
    check_strip("relative/path/", "relative/path", &failures);

    /* nothing but separators normalizes to a single separator.  "/" is the
     * two-byte allocation that the old strncpy(..., sizeof(char*) - 1)
     * overran */
    check_strip(PRTE_PATH_SEP, PRTE_PATH_SEP, &failures);
    check_strip(PRTE_PATH_SEP PRTE_PATH_SEP, PRTE_PATH_SEP, &failures);
    check_strip(PRTE_PATH_SEP PRTE_PATH_SEP PRTE_PATH_SEP, PRTE_PATH_SEP, &failures);

    /* an empty prefix has no separator to strip and, critically, no room to
     * write one into - the old loop indexed param[-1] here */
    check_strip("", "", &failures);

    /* must tolerate a NULL rather than faulting */
    prte_strip_trailing_pathsep(NULL);

    return failures;
}

/*
 * --singleton <nspace>.<rank> parsing.
 */
static int test_singleton_id(void)
{
    int failures = 0;
    pmix_nspace_t nspace;
    pmix_rank_t rank;

    /* the normal form */
    memset(nspace, 0, sizeof(nspace));
    rank = PMIX_RANK_INVALID;
    CHECK("singleton/ok", PRTE_SUCCESS == prte_parse_singleton_id("myapp.0", nspace, &rank));
    CHECK("singleton/ok-nspace", 0 == strcmp(nspace, "myapp"));
    CHECK("singleton/ok-rank", 0 == rank);

    /* a namespace may itself contain dots - the LAST one is the separator */
    memset(nspace, 0, sizeof(nspace));
    rank = PMIX_RANK_INVALID;
    CHECK("singleton/dotted",
          PRTE_SUCCESS == prte_parse_singleton_id("my.app.name.17", nspace, &rank));
    CHECK("singleton/dotted-nspace", 0 == strcmp(nspace, "my.app.name"));
    CHECK("singleton/dotted-rank", 17 == rank);

    /* no separator at all - this is the value that used to dereference the
     * NULL returned by strrchr */
    CHECK("singleton/no-dot",
          PRTE_SUCCESS != prte_parse_singleton_id("foo", nspace, &rank));

    /* empty rank, empty nspace, non-numeric rank, trailing garbage */
    CHECK("singleton/empty-rank",
          PRTE_SUCCESS != prte_parse_singleton_id("myapp.", nspace, &rank));
    CHECK("singleton/empty-nspace",
          PRTE_SUCCESS != prte_parse_singleton_id(".0", nspace, &rank));
    CHECK("singleton/non-numeric",
          PRTE_SUCCESS != prte_parse_singleton_id("myapp.abc", nspace, &rank));
    CHECK("singleton/trailing",
          PRTE_SUCCESS != prte_parse_singleton_id("myapp.0x", nspace, &rank));

    /* degenerate inputs */
    CHECK("singleton/null-name",
          PRTE_SUCCESS != prte_parse_singleton_id(NULL, nspace, &rank));
    CHECK("singleton/null-rank",
          PRTE_SUCCESS != prte_parse_singleton_id("myapp.0", nspace, NULL));
    CHECK("singleton/empty",
          PRTE_SUCCESS != prte_parse_singleton_id("", nspace, &rank));

    return failures;
}

static prte_job_t *fresh_job(void)
{
    prte_job_t *jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    return jdata;
}

/*
 * prte_pmix_xfer_job_info() -- the spawn directive translator.
 */
static int test_xfer_job_info(void)
{
    int failures = 0;
    prte_job_t *jdata;
    pmix_info_t info[3];
    char *str;
    uint32_t u32, *u32ptr;

    /* a boolean directive lands as a job attribute */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
    CHECK("xfer/bool-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    CHECK("xfer/bool-set",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_NOTIFY_COMPLETION, NULL, PMIX_BOOL));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* a string directive is copied, not aliased */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_CPU_LIST, "0-3", PMIX_STRING);
    CHECK("xfer/str-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    str = NULL;
    CHECK("xfer/str-set",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_CPUSET, (void **) &str, PMIX_STRING));
    CHECK("xfer/str-value", NULL != str && 0 == strcmp(str, "0-3"));
    if (NULL != str) {
        free(str);
    }
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* a numeric directive that goes through PMIX_VALUE_GET_NUMBER.  Note
     * the get-side convention: prte_get_attribute hands back a freshly
     * allocated value through a pointer-to-pointer, it does not fill a
     * caller-supplied scalar */
    jdata = fresh_job();
    u32 = 42;
    PMIX_INFO_LOAD(&info[0], PMIX_SESSION_ID, &u32, PMIX_UINT32);
    CHECK("xfer/u32-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    u32 = 0;
    u32ptr = &u32;   /* unload copies into caller-supplied storage for scalars */
    CHECK("xfer/u32-set",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_SESSION_ID, (void **) &u32ptr,
                             PMIX_UINT32));
    CHECK("xfer/u32-value", 42 == u32);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* map-by is accepted and records the policy on the map */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "node", PMIX_STRING);
    CHECK("xfer/mapby-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    CHECK("xfer/mapby-set", PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* ...and a second, conflicting mapping policy is refused rather than
     * silently overwriting the first.  PPR and MAPBY are two spellings of
     * the same policy, so giving both must be an error */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "node", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_PPR, "2:node", PMIX_STRING);
    CHECK("xfer/conflict", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 2));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_RELEASE(jdata);

    /* a bad policy string is rejected, not accepted-and-ignored */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "notapolicy", PMIX_STRING);
    CHECK("xfer/bad-mapby", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* an unrecognized key is not an error - it is cached for delivery to
     * the job at nspace registration */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], "prte.test.unknown.key", "value", PMIX_STRING);
    CHECK("xfer/unknown-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    CHECK("xfer/unknown-cached",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, NULL, PMIX_POINTER));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* an empty directive array is a no-op, not a fault */
    jdata = fresh_job();
    CHECK("xfer/empty", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, NULL, 0));
    PMIX_RELEASE(jdata);

    return failures;
}

/*
 * prte_pmix_xfer_app() -- app translation, and the ownership contract that
 * goes with it.
 */
static int test_xfer_app(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_app_context_t *app;
    pmix_app_t papp;

    /* the ordinary case: cmd, argv and maxprocs come across */
    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    papp.cmd = strdup("/bin/hostname");
    PMIx_Argv_append_nosize(&papp.argv, "hostname");
    papp.maxprocs = 3;
    CHECK("app/rc", PRTE_SUCCESS == prte_pmix_xfer_app(jdata, &papp));
    CHECK("app/count", 1 == jdata->num_apps);
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    CHECK("app/present", NULL != app);
    if (NULL != app) {
        CHECK("app/cmd", NULL != app->app && 0 == strcmp(app->app, "/bin/hostname"));
        CHECK("app/argv", NULL != app->argv && 0 == strcmp(app->argv[0], "hostname"));
        CHECK("app/nprocs", 3 == app->num_procs);
        CHECK("app/idx", 0 == app->idx);
        /* the app must point back at the job it was added to */
        CHECK("app/backptr", (struct prte_job_t *) jdata == app->job);
    }
    PMIX_APP_DESTRUCT(&papp);
    PMIX_RELEASE(jdata);

    /* with no cmd, argv[0] becomes the executable */
    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    PMIx_Argv_append_nosize(&papp.argv, "uptime");
    papp.maxprocs = 1;
    CHECK("app/argv0-rc", PRTE_SUCCESS == prte_pmix_xfer_app(jdata, &papp));
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    CHECK("app/argv0", NULL != app && NULL != app->app && 0 == strcmp(app->app, "uptime"));
    PMIX_APP_DESTRUCT(&papp);
    PMIX_RELEASE(jdata);

    /* neither cmd nor argv is a bad-param error */
    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    CHECK("app/no-cmd", PRTE_SUCCESS != prte_pmix_xfer_app(jdata, &papp));
    /* ...and, critically, the caller's job object must still be alive and
     * usable.  This used to PMIX_RELEASE(jdata) on an error path, handing
     * the caller back a dangling pointer it went on to use and release
     * again.  Touching jdata here is the assertion. */
    CHECK("app/job-alive", NULL != jdata->map);
    jdata->num_procs = 7;
    CHECK("app/job-usable", 7 == jdata->num_procs);
    PMIX_APP_DESTRUCT(&papp);
    PMIX_RELEASE(jdata);

    /* two apps land at successive indices, and num_apps tracks them */
    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    papp.cmd = strdup("a");
    papp.maxprocs = 1;
    CHECK("app/multi-1", PRTE_SUCCESS == prte_pmix_xfer_app(jdata, &papp));
    PMIX_APP_DESTRUCT(&papp);
    PMIX_APP_CONSTRUCT(&papp);
    papp.cmd = strdup("b");
    papp.maxprocs = 2;
    CHECK("app/multi-2", PRTE_SUCCESS == prte_pmix_xfer_app(jdata, &papp));
    CHECK("app/multi-count", 2 == jdata->num_apps);
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 1);
    CHECK("app/multi-idx", NULL != app && 1 == app->idx);
    CHECK("app/multi-cmd", NULL != app && 0 == strcmp(app->app, "b"));
    PMIX_APP_DESTRUCT(&papp);
    PMIX_RELEASE(jdata);

    return failures;
}

/*
 * pmix_server_cache_job_info() -- the "I don't recognize this key, hold it
 * for nspace registration" path that prte_pmix_xfer_job_info falls back to.
 * It creates the cache list lazily on first use and appends thereafter; a
 * regression that recreated the list would silently drop every earlier key.
 */
static int test_job_info_cache(void)
{
    int failures = 0;
    prte_job_t *jdata;
    pmix_info_t info;
    pmix_list_t *cache = NULL;

    jdata = fresh_job();
    CHECK("cache/absent",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, NULL, PMIX_POINTER));

    PMIX_INFO_LOAD(&info, "prte.test.first", "1", PMIX_STRING);
    pmix_server_cache_job_info(jdata, &info);
    PMIX_INFO_DESTRUCT(&info);

    PMIX_INFO_LOAD(&info, "prte.test.second", "2", PMIX_STRING);
    pmix_server_cache_job_info(jdata, &info);
    PMIX_INFO_DESTRUCT(&info);

    CHECK("cache/present",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_INFO_CACHE, (void **) &cache,
                             PMIX_POINTER));
    /* the cache is a borrowed pointer - the job owns the list */
    CHECK("cache/both-kept", NULL != cache && 2 == pmix_list_get_size(cache));

    PMIX_RELEASE(jdata);
    return failures;
}

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_event_base_open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }
    /* the directive translator reaches into the rmaps base to parse
     * map-by/rank-by/bind-to, and into the state base for runtime options */
    rc = pmix_mca_base_framework_open(&prte_rmaps_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "rmaps framework open failed: %d\n", rc);
        prte_event_base_close();
        prte_finalize();
        return 1;
    }
    rc = pmix_mca_base_framework_open(&prte_state_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "state framework open failed: %d\n", rc);
        (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
        prte_event_base_close();
        prte_finalize();
        return 1;
    }

    failures += test_prefix_normalization();
    failures += test_singleton_id();
    failures += test_xfer_job_info();
    failures += test_xfer_app();
    failures += test_job_info_cache();

    (void) pmix_mca_base_framework_close(&prte_state_base_framework);
    (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
    prte_event_base_close();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all prted unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d prted unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
