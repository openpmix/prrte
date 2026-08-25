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
 *  - create_app()'s environment directives, reached through
 *    prte_parse_locals().  --set-env/--prepend-env/--append-env/--unset-env/-x
 *    edit each other, so the order the user gave them in is the value the
 *    process ends up with; these check that a command line comes back out in
 *    the order it went in, case by case and then over every ordering of a
 *    pool of directives.
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
#include "src/prted/pmix/pmix_server.h"
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
    pmix_envar_t envt;
    prte_attribute_t *attr;
    size_t nenvars;
    bool found_app;

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

    /* envar directives are multi-valued: each must land on the list, and none
     * may displace one already there under the same key - prte_pmix_xfer_app()
     * runs first and puts app_idx 0's directives on this same list */
    jdata = fresh_job();
    PMIX_ENVAR_CONSTRUCT(&envt);
    envt.envar = (char *) "APP_LEVEL";
    envt.value = (char *) "1";
    prte_append_attribute(&jdata->attributes, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                          &envt, PMIX_ENVAR);
    envt.envar = (char *) "JOB_LEVEL";
    envt.value = (char *) "2";
    PMIX_INFO_LOAD(&info[0], PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
    envt.envar = (char *) "JOB_LEVEL_2";
    envt.value = (char *) "3";
    PMIX_INFO_LOAD(&info[1], PMIX_SET_ENVAR, &envt, PMIX_ENVAR);
    CHECK("xfer/envar-rc", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 2));
    nenvars = 0;
    found_app = false;
    PMIX_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t) {
        if (PRTE_JOB_SET_ENVAR == attr->key) {
            ++nenvars;
            if (0 == strcmp(attr->data.data.envar.envar, "APP_LEVEL")) {
                found_app = true;
            }
        }
    }
    CHECK("xfer/envar-count", 3 == nenvars);
    CHECK("xfer/envar-preexisting-kept", found_app);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_RELEASE(jdata);

    /* an empty directive array is a no-op, not a fault */
    jdata = fresh_job();
    CHECK("xfer/empty", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, NULL, 0));
    PMIX_RELEASE(jdata);

    /* A directive whose value is a PMIX_STRING carrying no string is what
     * PMIX_INFO_LOAD produces from a NULL - an ordinary thing for an
     * application to hand PMIx_Spawn, and something PMIx itself treats as
     * legal.  The branches that parse such a value rather than pass it on
     * to a NULL-tolerant callee used to dereference it, so a client could
     * segfault the daemon it was attached to with one spawn request.  Each
     * of these must come back as a refusal, and must come back at all. */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_STDIN_TGT, NULL, PMIX_STRING);
    CHECK("xfer/null-stdin-tgt", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_SPAWN_TIMEOUT, NULL, PMIX_STRING);
    CHECK("xfer/null-spawn-timeout", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_TIMEOUT, NULL, PMIX_STRING);
    CHECK("xfer/null-job-timeout", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_PARENT_ID, NULL, PMIX_PROC);
    CHECK("xfer/null-parent-id", PRTE_SUCCESS != prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_RELEASE(jdata);

    /* ...and a key whose value we only ever hand onward stays a no-op: the
     * guard above must not have turned every empty value into a refusal */
    jdata = fresh_job();
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_ID, NULL, PMIX_STRING);
    CHECK("xfer/null-passthrough", PRTE_SUCCESS == prte_pmix_xfer_job_info(jdata, info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
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

    /* PMIX_WDIR overrides whatever pmix_app_t.cwd already gave us, so the
     * earlier value has to be let go rather than stranded - and a WDIR
     * carrying no string at all must be refused rather than dereferenced */
    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    papp.cmd = strdup("c");
    papp.cwd = strdup("/tmp");
    papp.maxprocs = 1;
    PMIX_INFO_CREATE(papp.info, 1);
    papp.ninfo = 1;
    PMIX_INFO_LOAD(&papp.info[0], PMIX_WDIR, "/usr", PMIX_STRING);
    CHECK("app/wdir-rc", PRTE_SUCCESS == prte_pmix_xfer_app(jdata, &papp));
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, 0);
    CHECK("app/wdir-override", NULL != app && NULL != app->cwd
                               && 0 == strcmp(app->cwd, "/usr"));
    PMIX_APP_DESTRUCT(&papp);
    PMIX_RELEASE(jdata);

    jdata = fresh_job();
    PMIX_APP_CONSTRUCT(&papp);
    papp.cmd = strdup("d");
    papp.maxprocs = 1;
    PMIX_INFO_CREATE(papp.info, 1);
    papp.ninfo = 1;
    PMIX_INFO_LOAD(&papp.info[0], PMIX_WDIR, NULL, PMIX_STRING);
    CHECK("app/wdir-null", PRTE_SUCCESS != prte_pmix_xfer_app(jdata, &papp));
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

/*
 * Environment directives, and the order they are applied in.
 *
 * --set-env replaces a variable's value outright; --prepend-env and
 * --append-env edit the value already there; --unset-env removes it; -x
 * forwards one.  So the order the user gave them in IS the result:
 *
 *   --prepend-env "FOO[:]" x --set-env FOO=1     must leave  FOO=1
 *   --set-env FOO=1 --prepend-env "FOO[:]" x     must leave  FOO=x:1
 *
 * Both are what the user asked for; neither is a merge policy the runtime
 * gets to choose.  create_app() therefore has to emit the directives onto
 * the app's info list in command-line order, because that list is applied
 * in order (see the PRTE_JOB_*_ENVAR loop in odls_base_default_fns.c).
 *
 * The hard case is an option repeated after another has intervened:
 *
 *   --set-env FOO=1 --prepend-env "FOO[:]" x --set-env FOO=2
 *
 * The parse result groups every occurrence of an option onto that option's
 * single instance, so walking those instances applies BOTH sets and then
 * the prepend, arriving at FOO=x:2 where the user asked for FOO=2.  The
 * ordering has to come from the position each value was given at, which is
 * what pmix_cmd_line_get_ordered() reports.
 *
 * Each case below is checked twice: the sequence of directives emitted,
 * and the value a process would actually end up with after they are
 * applied the way the odls applies them.  The second is the one that
 * matters to a user; the first says where it went wrong when it does.
 *
 * Built against a PMIx that predates PMIX_CAP_CLI_ORDER there are no
 * position stamps to take the order from, and create_app() falls back to
 * walking the parse result's instances -- which emits every occurrence of
 * an option at the position of that option's FIRST occurrence.  That is
 * the documented, correct-as-far-as-it-goes fallback, so the cases it
 * cannot place are skipped there rather than failed: a build against an
 * older PMIx still checks everything the fallback does claim to get
 * right.  What went unchecked is counted and reported, so the reduced
 * coverage is visible rather than passing silently.
 */

/* The environment directive options, and how many tokens of value each
 * one carries after it. */
static const struct {
    const char *opt;
    int nvals;
} envar_opts[] = {
    {"--set-env", 1},
    {"--unset-env", 1},
    {"-x", 1},
    {"--prepend-env", 2},   /* the variable name, then the value */
    {"--append-env", 2},
    {NULL, 0}
};

/*
 * Collect the directive options of one app segment -- argv from "start"
 * up to a ':' or the end -- and return the index the scan stopped at, so
 * an MPMD command line can be walked segment by segment.  An option's
 * values are consumed with it: they are values, not options, even when
 * one happens to be spelled like one.
 */
static size_t envar_segment_opts(const char *const *argv, size_t start,
                                 const char **opts, size_t maxopts,
                                 size_t *nopts)
{
    size_t n, i;
    int v;

    *nopts = 0;
    for (n = start; NULL != argv[n];) {
        if (0 == strcmp(":", argv[n])) {
            return n + 1;
        }
        for (i = 0; NULL != envar_opts[i].opt; i++) {
            if (0 == strcmp(envar_opts[i].opt, argv[n])) {
                break;
            }
        }
        if (NULL == envar_opts[i].opt) {
            /* not a directive - nothing to place */
            ++n;
            continue;
        }
        if (*nopts < maxopts) {
            opts[(*nopts)++] = envar_opts[i].opt;
        }
        for (v = 0, ++n; v < envar_opts[i].nvals && NULL != argv[n]; v++, n++) {
            continue;
        }
    }
    return n;
}

/* Do the occurrences of each option in this sequence already sit
 * together?  If they do, grouping them onto each option's first
 * occurrence changes nothing and the instances walk reproduces the
 * command line; if they do not, only the stamps can place them. */
static bool envar_opts_contiguous(const char *const *opts, size_t nopts)
{
    size_t n, i;

    for (n = 1; n < nopts; n++) {
        if (0 == strcmp(opts[n], opts[n - 1])) {
            continue;
        }
        /* a different option than the one before it - so this is the
         * start of a run, and it must be this option's FIRST run */
        for (i = 0; i < n; i++) {
            if (0 == strcmp(opts[n], opts[i])) {
                return false;
            }
        }
    }
    return true;
}

/* True when this command line can only be ordered from the stamps: some
 * directive option recurs after a different one intervened.  Checked per
 * app segment, because each segment is parsed on its own. */
static bool envar_needs_cli_order(const char *const *argv)
{
    const char *opts[32];
    size_t nopts, n = 0;

    while (NULL != argv[n]) {
        n = envar_segment_opts(argv, n, opts, 32, &nopts);
        if (!envar_opts_contiguous(opts, nopts)) {
            return true;
        }
    }
    return false;
}

/* Find "name" in an env array and return a pointer to its value, or NULL.
 * Mirrors envar_value() in odls_base_default_fns.c. */
static char *test_envar_value(char *envstr, const char *name)
{
    size_t len = strlen(name);

    if (0 != strncmp(envstr, name, len) || '=' != envstr[len]) {
        return NULL;
    }
    return envstr + len + 1;
}

/* Apply one directive to an environment exactly as the odls does. */
static void test_apply_envar(pmix_info_t *info, char ***env)
{
    pmix_envar_t *envar;
    char *ptr, *tmp;
    bool found;
    int n, m;

    if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
        for (n = 0; NULL != (*env) && NULL != (*env)[n]; n++) {
            if (NULL != test_envar_value((*env)[n], info->value.data.string)) {
                free((*env)[n]);
                for (m = n; NULL != (*env)[m + 1]; m++) {
                    (*env)[m] = (*env)[m + 1];
                }
                (*env)[m] = NULL;
                break;
            }
        }
        return;
    }
    if (PMIX_ENVAR != info->value.type) {
        return;
    }
    envar = &info->value.data.envar;

    if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR) ||
        PMIX_CHECK_KEY(info, PMIX_ADD_ENVAR)) {
        PMIx_Setenv(envar->envar, envar->value, true, env);
        return;
    }

    found = false;
    for (n = 0; NULL != (*env) && NULL != (*env)[n]; n++) {
        ptr = test_envar_value((*env)[n], envar->envar);
        if (NULL == ptr) {
            continue;
        }
        if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
            pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, envar->value,
                          envar->separator, ptr);
        } else {
            pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, ptr,
                          envar->separator, envar->value);
        }
        free((*env)[n]);
        (*env)[n] = tmp;
        found = true;
        break;
    }
    if (!found) {
        // nothing to edit - the directive sets the variable
        PMIx_Setenv(envar->envar, envar->value, true, env);
    }
    return;
}

/* Render one directive as "KEY NAME[sep]=value", so a mismatch reports
 * what was emitted rather than just that something was. */
static char *test_render_envar(pmix_info_t *info)
{
    pmix_envar_t *envar;
    char *out = NULL;

    if (PMIX_CHECK_KEY(info, PMIX_UNSET_ENVAR)) {
        pmix_asprintf(&out, "unset %s", info->value.data.string);
        return out;
    }
    if (PMIX_ENVAR != info->value.type) {
        return NULL;
    }
    envar = &info->value.data.envar;
    if (PMIX_CHECK_KEY(info, PMIX_SET_ENVAR)) {
        pmix_asprintf(&out, "set %s=%s", envar->envar, envar->value);
    } else if (PMIX_CHECK_KEY(info, PMIX_PREPEND_ENVAR)) {
        pmix_asprintf(&out, "prepend %s[%c]=%s", envar->envar,
                      envar->separator, envar->value);
    } else if (PMIX_CHECK_KEY(info, PMIX_APPEND_ENVAR)) {
        pmix_asprintf(&out, "append %s[%c]=%s", envar->envar,
                      envar->separator, envar->value);
    }
    return out;
}

/* Reach the "prte" personality through the framework: its module belongs
 * to a component that may be a DSO, and is not visible outside libprrte in
 * a hidden-visibility build. */
static prte_schizo_base_module_t *envar_test_schizo(void)
{
    prte_schizo_base_active_module_t *mod;

    PMIX_LIST_FOREACH(mod, &prte_schizo_base.active_modules,
                      prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->name &&
            0 == strcmp("prte", mod->module->name)) {
            return mod->module;
        }
    }
    fprintf(stderr, "FAIL [envar]: no prte schizo module\n");
    return NULL;
}

typedef struct {
    const char *desc;
    const char *argv[24];
    const char *seq;    /* '|'-joined directives, in the order emitted */
    const char *foo;    /* value of FOO afterwards, NULL if it is not set */
    const char *bar;    /* value of BAR afterwards, NULL if it is not set */
} envar_case_t;

static envar_case_t envar_cases[] = {

    /* ---- the two orderings that must not be confused ---------------- */
    {"set then prepend",
     {"prterun", "--set-env", "FOO=1", "--prepend-env", "FOO[:]", "x",
      "myapp", NULL},
     "set FOO=1|prepend FOO[:]=x", "x:1", NULL},

    {"prepend then set",
     {"prterun", "--prepend-env", "FOO[:]", "x", "--set-env", "FOO=1",
      "myapp", NULL},
     "prepend FOO[:]=x|set FOO=1", "1", NULL},

    /* ---- THE case: an option repeated after another intervened ------ */
    {"set, prepend, set",
     {"prterun", "--set-env", "FOO=1", "--prepend-env", "FOO[:]", "x",
      "--set-env", "FOO=2", "myapp", NULL},
     "set FOO=1|prepend FOO[:]=x|set FOO=2", "2", NULL},

    {"prepend, set, prepend",
     {"prterun", "--prepend-env", "FOO[:]", "a", "--set-env", "FOO=1",
      "--prepend-env", "FOO[:]", "b", "myapp", NULL},
     "prepend FOO[:]=a|set FOO=1|prepend FOO[:]=b", "b:1", NULL},

    /* ---- all four directives interleaved ---------------------------- */
    {"set, append, set, prepend",
     {"prterun", "--set-env", "FOO=1", "--append-env", "FOO[:]", "z",
      "--set-env", "FOO=2", "--prepend-env", "FOO[:]", "y", "myapp", NULL},
     "set FOO=1|append FOO[:]=z|set FOO=2|prepend FOO[:]=y", "y:2", NULL},

    {"append, prepend, append",
     {"prterun", "--append-env", "FOO[,]", "a", "--prepend-env", "FOO[,]",
      "b", "--append-env", "FOO[,]", "c", "myapp", NULL},
     "append FOO[,]=a|prepend FOO[,]=b|append FOO[,]=c", "b,a,c", NULL},

    /* ---- unset in the middle ---------------------------------------- */
    {"set, unset, set",
     {"prterun", "--set-env", "FOO=1", "--unset-env", "FOO",
      "--set-env", "FOO=2", "myapp", NULL},
     "set FOO=1|unset FOO|set FOO=2", "2", NULL},

    {"set, unset",
     {"prterun", "--set-env", "FOO=1", "--unset-env", "FOO", "myapp", NULL},
     "set FOO=1|unset FOO", NULL, NULL},

    /* a prepend onto a variable that was just unset has nothing to edit,
     * so it sets it - the ordering has to survive to get that right */
    {"set, unset, prepend",
     {"prterun", "--set-env", "FOO=1", "--unset-env", "FOO",
      "--prepend-env", "FOO[:]", "x", "myapp", NULL},
     "set FOO=1|unset FOO|prepend FOO[:]=x", "x", NULL},

    /* ---- -x, which forwards a variable and can also set one --------- */
    {"-x, prepend, -x",
     {"prterun", "-x", "FOO=1", "--prepend-env", "FOO[:]", "x",
      "-x", "FOO=2", "myapp", NULL},
     "set FOO=1|prepend FOO[:]=x|set FOO=2", "2", NULL},

    /* ---- two variables interleaved: no cross-talk -------------------- */
    {"two variables interleaved",
     {"prterun", "--set-env", "FOO=1", "--set-env", "BAR=1",
      "--prepend-env", "FOO[:]", "x", "--append-env", "BAR[:]", "y",
      "myapp", NULL},
     "set FOO=1|set BAR=1|prepend FOO[:]=x|append BAR[:]=y", "x:1", "1:y"},

    /* ---- the plain repeats, which the grouped walk got right and must
     * still get right --------------------------------------------------*/
    {"set repeated with nothing between",
     {"prterun", "--set-env", "FOO=1", "--set-env", "FOO=2", "myapp", NULL},
     "set FOO=1|set FOO=2", "2", NULL},

    {"prepend repeated with nothing between",
     {"prterun", "--prepend-env", "FOO[:]", "a", "--prepend-env", "FOO[:]",
      "b", "myapp", NULL},
     "prepend FOO[:]=a|prepend FOO[:]=b", "b:a", NULL},

    /* ---- a long alternation, to be sure nothing drifts --------------- */
    {"six alternating directives",
     {"prterun", "--set-env", "FOO=1", "--prepend-env", "FOO[:]", "a",
      "--set-env", "FOO=2", "--prepend-env", "FOO[:]", "b",
      "--set-env", "FOO=3", "--prepend-env", "FOO[:]", "c",
      "myapp", NULL},
     "set FOO=1|prepend FOO[:]=a|set FOO=2|prepend FOO[:]=b|"
     "set FOO=3|prepend FOO[:]=c", "c:3", NULL},

    /* ---- an unrelated option in between must not disturb the order --- */
    {"an unrelated option between the two sets",
     {"prterun", "--set-env", "FOO=1", "--prepend-env", "FOO[:]", "x",
      "--np", "2", "--set-env", "FOO=2", "myapp", NULL},
     "set FOO=1|prepend FOO[:]=x|set FOO=2", "2", NULL},

    {NULL, {NULL}, NULL, NULL, NULL}
};

/* Collect the envar directives an app carries, in list order, and the
 * environment they produce. */
static int check_envar_app(const char *desc, prte_pmix_app_t *app,
                           const envar_case_t *c)
{
    pmix_data_array_t darray;
    pmix_info_t *infos;
    char **rendered = NULL, **env = NULL;
    char *got = NULL, *entry, *val;
    int failures = 0, rc;
    size_t n;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 0, PMIX_INFO);
    rc = PMIx_Info_list_convert(app->info, &darray);
    if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
        fprintf(stderr, "FAIL [%s]: info list convert: %s\n", desc,
                PMIx_Error_string(rc));
        return 1;
    }
    infos = (pmix_info_t *) darray.array;
    for (n = 0; n < darray.size; n++) {
        entry = test_render_envar(&infos[n]);
        if (NULL == entry) {
            continue;
        }
        PMIx_Argv_append_nosize(&rendered, entry);
        free(entry);
        test_apply_envar(&infos[n], &env);
    }
    got = (NULL == rendered) ? strdup("") : PMIx_Argv_join(rendered, '|');

    if (0 != strcmp(got, c->seq)) {
        fprintf(stderr, "FAIL [%s]: directives\n   wanted %s\n   got    %s\n",
                desc, c->seq, got);
        ++failures;
    }

    val = NULL;
    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (NULL != (entry = test_envar_value(env[n], "FOO"))) {
            val = entry;
            break;
        }
    }
    if ((NULL == c->foo) != (NULL == val) ||
        (NULL != c->foo && NULL != val && 0 != strcmp(c->foo, val))) {
        fprintf(stderr, "FAIL [%s]: FOO wanted \"%s\", got \"%s\"\n", desc,
                (NULL == c->foo) ? "(unset)" : c->foo,
                (NULL == val) ? "(unset)" : val);
        ++failures;
    }

    if (NULL != c->bar) {
        val = NULL;
        for (n = 0; NULL != env && NULL != env[n]; n++) {
            if (NULL != (entry = test_envar_value(env[n], "BAR"))) {
                val = entry;
                break;
            }
        }
        if (NULL == val || 0 != strcmp(c->bar, val)) {
            fprintf(stderr, "FAIL [%s]: BAR wanted \"%s\", got \"%s\"\n", desc,
                    c->bar, (NULL == val) ? "(unset)" : val);
            ++failures;
        }
    }

    free(got);
    PMIx_Argv_free(rendered);
    PMIx_Argv_free(env);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    return failures;
}

static int test_envar_order(int *nskipped)
{
    prte_schizo_base_module_t *schizo;
    envar_case_t *c;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    int failures = 0, rc;

    schizo = envar_test_schizo();
    if (NULL == schizo) {
        return 1;
    }

    for (c = envar_cases; NULL != c->desc; c++) {
        if (!PRTE_PMIX_CLI_ORDER && envar_needs_cli_order(c->argv)) {
            ++(*nskipped);
            continue;
        }
        PMIX_CONSTRUCT(&apps, pmix_list_t);
        rc = prte_parse_locals(schizo, &apps, (char **) c->argv,
                               NULL, NULL, NULL, NULL);
        if (PRTE_SUCCESS != rc) {
            fprintf(stderr, "FAIL [%s]: parse failed: %s\n", c->desc,
                    prte_strerror(rc));
            ++failures;
            PMIX_LIST_DESTRUCT(&apps);
            continue;
        }
        if (1 != pmix_list_get_size(&apps)) {
            fprintf(stderr, "FAIL [%s]: %d apps, wanted 1\n", c->desc,
                    (int) pmix_list_get_size(&apps));
            ++failures;
            PMIX_LIST_DESTRUCT(&apps);
            continue;
        }
        app = (prte_pmix_app_t *) pmix_list_get_first(&apps);
        failures += check_envar_app(c->desc, app, c);
        PMIX_LIST_DESTRUCT(&apps);
    }

    return failures;
}

/*
 * The same claim, made mechanically instead of case by case.
 *
 * Every ordering of a pool of directives is generated, run through the
 * parser, and checked against the order it was given in - an expectation
 * derived from the input rather than written out by hand, so it cannot be
 * quietly fitted to whatever the code happens to do.  The pool mixes all
 * five spellings, and repeats one of them, because a repeat is what the
 * grouped view cannot place: with six entries, 720 orderings.
 */
typedef struct {
    const char *tok1;     /* the option */
    const char *tok2;     /* its first value */
    const char *tok3;     /* its second value, or NULL */
    const char *rendered; /* the directive it must produce */
} envar_directive_t;

static envar_directive_t envar_pool[] = {
    {"--set-env", "FOO=1", NULL, "set FOO=1"},
    {"--set-env", "FOO=2", NULL, "set FOO=2"},
    {"--prepend-env", "FOO[:]", "a", "prepend FOO[:]=a"},
    {"--append-env", "FOO[:]", "b", "append FOO[:]=b"},
    {"--unset-env", "FOO", NULL, "unset FOO"},
    {"-x", "FOO=3", NULL, "set FOO=3"},
};
#define ENVAR_POOL_SIZE ((int) (sizeof(envar_pool) / sizeof(envar_pool[0])))

static int check_envar_permutation(prte_schizo_base_module_t *schizo,
                                   const int *order, int *nskipped)
{
    char **argv = NULL, **wanted = NULL, **rendered = NULL;
    char *want = NULL, *got = NULL, *desc = NULL, *entry;
    const char *opts[ENVAR_POOL_SIZE];
    pmix_data_array_t darray;
    pmix_info_t *infos;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    int failures = 0, rc, i;
    size_t n;

    for (i = 0; i < ENVAR_POOL_SIZE; i++) {
        opts[i] = envar_pool[order[i]].tok1;
    }
    if (!PRTE_PMIX_CLI_ORDER && !envar_opts_contiguous(opts, ENVAR_POOL_SIZE)) {
        ++(*nskipped);
        return 0;
    }

    PMIx_Argv_append_nosize(&argv, "prterun");
    for (i = 0; i < ENVAR_POOL_SIZE; i++) {
        PMIx_Argv_append_nosize(&argv, envar_pool[order[i]].tok1);
        PMIx_Argv_append_nosize(&argv, envar_pool[order[i]].tok2);
        if (NULL != envar_pool[order[i]].tok3) {
            PMIx_Argv_append_nosize(&argv, envar_pool[order[i]].tok3);
        }
        PMIx_Argv_append_nosize(&wanted, envar_pool[order[i]].rendered);
    }
    PMIx_Argv_append_nosize(&argv, "myapp");
    want = PMIx_Argv_join(wanted, '|');
    desc = PMIx_Argv_join(argv, ' ');

    PMIX_CONSTRUCT(&apps, pmix_list_t);
    rc = prte_parse_locals(schizo, &apps, argv, NULL, NULL, NULL, NULL);
    if (PRTE_SUCCESS != rc || 1 != pmix_list_get_size(&apps)) {
        fprintf(stderr, "FAIL [permute]: %s: parse failed (%s)\n", desc,
                prte_strerror(rc));
        ++failures;
        goto cleanup;
    }
    app = (prte_pmix_app_t *) pmix_list_get_first(&apps);

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 0, PMIX_INFO);
    rc = PMIx_Info_list_convert(app->info, &darray);
    if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
        fprintf(stderr, "FAIL [permute]: %s: convert failed\n", desc);
        ++failures;
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        goto cleanup;
    }
    infos = (pmix_info_t *) darray.array;
    for (n = 0; n < darray.size; n++) {
        entry = test_render_envar(&infos[n]);
        if (NULL == entry) {
            continue;
        }
        PMIx_Argv_append_nosize(&rendered, entry);
        free(entry);
    }
    got = (NULL == rendered) ? strdup("") : PMIx_Argv_join(rendered, '|');
    if (0 != strcmp(want, got)) {
        fprintf(stderr, "FAIL [permute]: %s\n   wanted %s\n   got    %s\n",
                desc, want, got);
        ++failures;
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

cleanup:
    PMIX_LIST_DESTRUCT(&apps);
    PMIx_Argv_free(argv);
    PMIx_Argv_free(wanted);
    PMIx_Argv_free(rendered);
    free(want);
    free(got);
    free(desc);
    return failures;
}

/* Heap's algorithm - every ordering of the pool, each exactly once */
static int permute_envar(prte_schizo_base_module_t *schizo, int *order,
                         int k, int *ncases, int *nskipped)
{
    int failures = 0, i, tmp;

    if (1 == k) {
        ++(*ncases);
        return check_envar_permutation(schizo, order, nskipped);
    }
    for (i = 0; i < k; i++) {
        failures += permute_envar(schizo, order, k - 1, ncases, nskipped);
        if (0 == (k % 2)) {
            tmp = order[i];
            order[i] = order[k - 1];
            order[k - 1] = tmp;
        } else {
            tmp = order[0];
            order[0] = order[k - 1];
            order[k - 1] = tmp;
        }
    }
    return failures;
}

static int test_envar_order_permutations(int *nskipped)
{
    prte_schizo_base_module_t *schizo;
    int order[ENVAR_POOL_SIZE];
    int failures, ncases = 0, skipped = 0, i;

    schizo = envar_test_schizo();
    if (NULL == schizo) {
        return 1;
    }
    for (i = 0; i < ENVAR_POOL_SIZE; i++) {
        order[i] = i;
    }
    failures = permute_envar(schizo, order, ENVAR_POOL_SIZE, &ncases, &skipped);
    fprintf(stdout, "envar order: %d orderings generated, %d checked, %d skipped, %d failed\n",
            ncases, ncases - skipped, skipped, failures);
    *nskipped += skipped;
    return failures;
}

/*
 * Each app segment of an MPMD command line carries its own directives, and
 * they are parsed into separate results - so the ordering has to hold
 * per segment, not just for the first one.
 */
static int test_envar_order_mpmd(int *nskipped)
{
    static envar_case_t first = {
        "mpmd/app1", {NULL},
        "set FOO=1|prepend FOO[:]=x|set FOO=2", "2", NULL};
    static envar_case_t second = {
        "mpmd/app2", {NULL},
        "prepend FOO[:]=y|set FOO=9|prepend FOO[:]=z", "z:9", NULL};
    char *argv[] = {"prterun",
                    "--set-env", "FOO=1", "--prepend-env", "FOO[:]", "x",
                    "--set-env", "FOO=2", "myapp",
                    ":",
                    "--prepend-env", "FOO[:]", "y", "--set-env", "FOO=9",
                    "--prepend-env", "FOO[:]", "z", "myotherapp", NULL};
    prte_schizo_base_module_t *schizo;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    int failures = 0, rc;

    schizo = envar_test_schizo();
    if (NULL == schizo) {
        return 1;
    }
    /* both segments repeat an option after another intervened */
    if (!PRTE_PMIX_CLI_ORDER && envar_needs_cli_order((const char *const *) argv)) {
        ++(*nskipped);
        return 0;
    }

    PMIX_CONSTRUCT(&apps, pmix_list_t);
    rc = prte_parse_locals(schizo, &apps, argv, NULL, NULL, NULL, NULL);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "FAIL [mpmd]: parse failed: %s\n", prte_strerror(rc));
        PMIX_LIST_DESTRUCT(&apps);
        return 1;
    }
    if (2 != pmix_list_get_size(&apps)) {
        fprintf(stderr, "FAIL [mpmd]: %d apps, wanted 2\n",
                (int) pmix_list_get_size(&apps));
        PMIX_LIST_DESTRUCT(&apps);
        return 1;
    }
    app = (prte_pmix_app_t *) pmix_list_get_first(&apps);
    failures += check_envar_app(first.desc, app, &first);
    app = (prte_pmix_app_t *) pmix_list_get_next(&app->super);
    failures += check_envar_app(second.desc, app, &second);

    PMIX_LIST_DESTRUCT(&apps);
    return failures;
}

/*
 * Where a placement directive lands: on the job, or on the app that carried
 * it.
 *
 * The first app segment is where a command line speaks for the job, so a
 * directive written there and nowhere else is the job's, however many apps
 * follow.  Written on any later app it is that app's alone, and its silent
 * siblings fall back to the defaults - they said nothing, which is not the
 * same as agreeing.  Reading a lone directive on a later app as the job's
 * meant asking for one app to be placed differently placed them all that
 * way, with no way to say what was plainly meant.
 */
static bool app_has_key(prte_pmix_app_t *app, const char *key)
{
    pmix_data_array_t darray;
    pmix_info_t *infos;
    bool found = false;
    size_t n;
    int rc;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 0, PMIX_INFO);
    rc = PMIx_Info_list_convert(app->info, &darray);
    if (PMIX_SUCCESS != rc) {
        /* PMIX_ERR_EMPTY: the app carried no directives at all */
        return false;
    }
    infos = (pmix_info_t *) darray.array;
    for (n = 0; n < darray.size; n++) {
        if (PMIx_Check_key(infos[n].key, key)) {
            found = true;
            break;
        }
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    return found;
}

static bool list_has_key(pmix_list_t *list, const char *key)
{
    prte_info_item_t *item;

    PMIX_LIST_FOREACH(item, list, prte_info_item_t) {
        if (PMIx_Check_key(item->info.key, key)) {
            return true;
        }
    }
    return false;
}

/* The spelling here is "--mapby", the canonical one the option tables
 * carry: a real command line reaches create_app() only after
 * prte_schizo_base_normalize_argv() has rewritten the deprecated hyphenated
 * "--map-by" into it. */
typedef struct {
    const char *desc;
    const char *argv[20];
    bool on_job;        /* the directive belongs to the whole job */
    bool on_app0;       /* app 0 carries one of its own */
    bool on_app1;       /* app 1 carries one of its own */
} distrib_case_t;

static distrib_case_t distrib_cases[] = {
    {"first app only -> the job",
     {"prterun", "--mapby", "core", "-n", "2", "app1", ":", "-n", "2", "app2", NULL},
     true, false, false},
    {"second app only -> that app alone",
     {"prterun", "-n", "2", "app1", ":", "--mapby", "core", "-n", "2", "app2", NULL},
     false, false, true},
    {"both apps -> each its own",
     {"prterun", "--mapby", "slot", "-n", "2", "app1", ":",
      "--mapby", "core", "-n", "2", "app2", NULL},
     false, true, true},
    {"no directive at all",
     {"prterun", "-n", "2", "app1", ":", "-n", "2", "app2", NULL},
     false, false, false},
    {NULL, {NULL}, false, false, false},
};

static int test_directive_distribution(void)
{
    prte_schizo_base_module_t *schizo;
    distrib_case_t *c;
    pmix_list_t apps, jobdata;
    prte_pmix_app_t *app0, *app1;
    int failures = 0, rc;

    schizo = envar_test_schizo();
    if (NULL == schizo) {
        return 1;
    }

    for (c = distrib_cases; NULL != c->desc; c++) {
        PMIX_CONSTRUCT(&apps, pmix_list_t);
        PMIX_CONSTRUCT(&jobdata, pmix_list_t);
        rc = prte_parse_locals(schizo, &apps, (char **) c->argv, NULL, NULL,
                               &jobdata, NULL);
        if (PRTE_SUCCESS != rc) {
            fprintf(stderr, "FAIL [%s]: parse failed: %s\n", c->desc, prte_strerror(rc));
            ++failures;
            goto next;
        }
        if (2 != pmix_list_get_size(&apps)) {
            fprintf(stderr, "FAIL [%s]: %d apps, wanted 2\n", c->desc,
                    (int) pmix_list_get_size(&apps));
            ++failures;
            goto next;
        }
        app0 = (prte_pmix_app_t *) pmix_list_get_first(&apps);
        app1 = (prte_pmix_app_t *) pmix_list_get_next(&app0->super);
        CHECK(c->desc, c->on_job == list_has_key(&jobdata, PMIX_MAPBY));
        CHECK(c->desc, c->on_app0 == app_has_key(app0, PMIX_MAPBY));
        CHECK(c->desc, c->on_app1 == app_has_key(app1, PMIX_MAPBY));

    next:
        PMIX_LIST_DESTRUCT(&apps);
        PMIX_LIST_DESTRUCT(&jobdata);
    }

    if (0 == failures) {
        fprintf(stdout, "  PASS directive distribution\n");
    }
    return failures;
}


/*
 * A scheduler's session time limit (PMIX_ALLOC_TIME) is the one directive
 * whose whole meaning is a string parse, and getting it wrong terminates a
 * user's session early or never. The format is scanned from the RIGHT, so a
 * bare "2" is two seconds - not two months.
 */
static const struct {
    const char *spec;
    long expected;      /* -1 == must be rejected */
} session_time_cases[] = {
    /* right-to-left: seconds, minutes, hours, days, months */
    {"0",                       0},
    {"2",                       2},
    {"90",                     90},
    {"1:30",                   90},
    {"0:01",                    1},
    {"2:00:00",              7200},
    {"1:00:00:00",          86400},
    {"1:00:00:00:00",     2592000},
    {"1:2:3:4:5",         2592000 + 2*86400 + 3*3600 + 4*60 + 5},
    /* malformed */
    {NULL,                     -1},
    {"",                       -1},
    {"abc",                    -1},
    {"1:",                     -1},
    {":1",                     -1},
    {"1::2",                   -1},
    {"-5",                     -1},
    {"5s",                     -1},
    {"1:2:3:4:5:6",            -1},   /* one field too many */
};

static int test_session_time(void)
{
    size_t n;
    long got;
    int failures = 0;
    char desc[128];

    for (n = 0; n < sizeof(session_time_cases) / sizeof(session_time_cases[0]); n++) {
        got = prte_pmix_server_parse_session_time(session_time_cases[n].spec);
        snprintf(desc, sizeof(desc), "sessiontime/%s",
                 (NULL == session_time_cases[n].spec) ? "NULL"
                                                      : session_time_cases[n].spec);
        CHECK(desc, got == session_time_cases[n].expected);
    }
    return failures;
}

/*
 * The departed-job registry.  A lookup that cannot find a job has to tell
 * "we have not heard of it YET" from "it has been and gone": the first is a
 * race worth waiting out, the second is a request that would wait forever,
 * because nothing drains those on a timer.  This list is the only thing that
 * can tell them apart, so the two properties that matter are that it
 * remembers what it was told and that it stays bounded - a persistent DVM
 * runs jobs without end.
 */
/*
 * prte_pmix_server_req_t's constructor.
 *
 * PMIX_NEW mallocs and does not zero, so a field the constructor forgets
 * holds whatever the previous occupant of that memory left behind.  The
 * field that matters most is tproc: the loops that decide which request an
 * arriving answer belongs to match on it, and an operation that names no
 * target proc at all - monitor, publish/lookup, spawn, tool connection -
 * never writes it.  Those loops screen such a request by testing its
 * nspace for PMIx's "invalid", which only works because the constructor
 * puts the sentinel there.
 *
 * The recycling half of this test is the part that catches the bug: it
 * releases a request that DID name a target before allocating another,
 * which the allocator satisfies from the same block - so a constructor
 * that leaves tproc alone hands the new request the dead one's identity.
 */
static int test_request_tracker(void)
{
    int failures = 0;
    prte_pmix_server_req_t *req;
    pmix_proc_t real, wild;

    PMIX_LOAD_PROCID(&real, "unit-test-tracker", 3);
    PMIX_LOAD_PROCID(&wild, "unit-test-tracker", PMIX_RANK_WILDCARD);

    req = PMIX_NEW(prte_pmix_server_req_t);
    CHECK("a new request names no target proc", PMIX_NSPACE_INVALID(req->tproc.nspace));
    CHECK("...with an invalid rank", PMIX_RANK_INVALID == req->tproc.rank);
    CHECK("...so it matches no real proc", !PMIX_CHECK_PROCID(&req->tproc, &real));
    CHECK("...it is on neither request array",
          -1 == req->local_index && -1 == req->remote_index);
    CHECK("...and it owns no info array", !req->copy && NULL == req->info);
    /* the sentinel is not sufficient on its own, which is why the matching
     * loops screen for it: an empty nspace matches every namespace, so a
     * job-level fetch (rank=WILDCARD) does pair with an untargeted request */
    CHECK("a job-level fetch would match it", PMIX_CHECK_PROCID(&req->tproc, &wild));

    /* mdxcbfunc is the other field the matching loops key on, and it carries
     * more weight than tproc does: pmix_server_dmdx_resp() uses it to tell a
     * direct-modex request from everything else sharing local_reqs - the
     * scheduler relays record a REAL requestor in tproc, so the sentinel
     * check above does not screen them - and having decided, it CALLS it.
     * A stale one left in a recycled block is therefore not a mismatch but a
     * jump through a dangling function pointer. */
    CHECK("a new request carries no modex callback", NULL == req->mdxcbfunc);

    /* leave a real identity in the allocator, then take the block back */
    PMIX_XFER_PROCID(&req->tproc, &real);
    req->mdxcbfunc = (pmix_modex_cbfunc_t) 0x1;
    PMIX_RELEASE(req);
    req = PMIX_NEW(prte_pmix_server_req_t);
    CHECK("a recycled request does not inherit the last one's target",
          PMIX_NSPACE_INVALID(req->tproc.nspace) && PMIX_RANK_INVALID == req->tproc.rank);
    CHECK("...nor the last one's modex callback", NULL == req->mdxcbfunc);
    PMIX_RELEASE(req);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_request_tracker\n");
    }
    return failures;
}

/* The monitor collective's accounting for a daemon that dies mid-flight.
 *
 * The request fans out with an xcast and then counts direct replies, so
 * nothing in it is keyed on the routing tree and nothing repairs it when the
 * tree changes - a daemon that dies simply never answers.  What closes that
 * is prte_pmix_server_fault_handler(), and the two things it has to get right
 * are both invisible in a single-shot test: the routing tree calls it TWICE
 * for one death (LOCAL scope, then GLOBAL), and it must not account a rank
 * this particular request was never waiting on.
 *
 * The recovery status is filled in by hand rather than constructed, because
 * its constructor reads the live routing tree, which no unit test has. */
static pmix_status_t mon_status;
static int mon_calls;

static void mon_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                       void *cbdata, pmix_release_cbfunc_t release_fn,
                       void *release_cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(info, ninfo, cbdata, release_fn, release_cbdata);
    /* deliberately does NOT invoke release_fn: that thread-shifts onto an
     * event base nothing is driving here.  The test owns the request. */
    mon_status = status;
    ++mon_calls;
}

static prte_pmix_server_req_t *mon_req(pmix_info_t *monitor, pmix_rank_t nexpect)
{
    prte_pmix_server_req_t *req;
    pmix_rank_t r;

    req = PMIX_NEW(prte_pmix_server_req_t);
    req->monitor = monitor;      /* moncopy stays false - we own it */
    req->infocbfunc = mon_cbfunc;
    for (r = 1; r <= nexpect; r++) {
        pmix_bitmap_set_bit(&req->expected_dmns, (int) r);
    }
    req->ndaemons = nexpect;
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);
    return req;
}

static void mon_reported(prte_pmix_server_req_t *req, pmix_rank_t r, bool ok)
{
    pmix_bitmap_set_bit(&req->reported_dmns, (int) r);
    ++req->nreported;
    if (ok) {
        ++req->nsuccess;
    }
}

static void mon_drop(prte_pmix_server_req_t *req)
{
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);
    PMIX_RELEASE(req);
}

static int test_monitor_accounting(void)
{
    int failures = 0;
    prte_pmix_server_req_t *req;
    prte_rml_recovery_status_t st;
    pmix_rank_t failed[2];
    pmix_info_t monitor;
    uint32_t nrep;

    PMIX_CONSTRUCT(&prte_pmix_server_globals.local_reqs, pmix_pointer_array_t);
    pmix_pointer_array_init(&prte_pmix_server_globals.local_reqs, 8, INT32_MAX, 8);
    PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_HEARTBEAT, NULL, PMIX_BOOL);

    memset(&st, 0, sizeof(st));
    st.failed_ranks.array = failed;
    st.failed_ranks.type = PMIX_PROC_RANK;

    /* one of three answered, the other two died: the caller is holding a
     * sample of part of the DVM and has to be told so */
    req = mon_req(&monitor, 3);
    mon_reported(req, 1, true);
    mon_calls = 0;
    mon_status = PMIX_SUCCESS;
    failed[0] = 2;
    failed[1] = 3;
    st.failed_ranks.size = 2;
    prte_pmix_server_fault_handler(&st);
    CHECK("losing the last outstanding daemons completes the request", 1 == mon_calls);
    CHECK("...with partial success", PMIX_ERR_PARTIAL_SUCCESS == mon_status);

    /* the tree reports every death twice - LOCAL scope then GLOBAL - and the
     * request is still on the array when the second call arrives, because the
     * release it was handed thread-shifts.  A second completion here would
     * answer the client twice and release the request under it. */
    nrep = req->nreported;
    prte_pmix_server_fault_handler(&st);
    CHECK("the second notice for the same death is a no-op", 1 == mon_calls);
    /* assert the accounting directly, not just that it did not complete
     * twice: an over-count sails past the equality test in the completion
     * check and would leave this looking clean while the request could
     * never again be completed by anything */
    CHECK("...and does not count the same death twice", nrep == req->nreported);
    mon_drop(req);

    /* nobody answered at all - "partial" would be a lie, so the status says
     * why instead */
    req = mon_req(&monitor, 2);
    mon_calls = 0;
    failed[0] = 1;
    failed[1] = 2;
    st.failed_ranks.size = 2;
    prte_pmix_server_fault_handler(&st);
    CHECK("losing every daemon completes the request", 1 == mon_calls);
    CHECK("...and does not call that a partial success",
          PMIX_SUCCESS != mon_status && PMIX_ERR_PARTIAL_SUCCESS != mon_status);
    mon_drop(req);

    /* a death this request was never waiting on must not be counted: doing so
     * completes it early, before the daemons it IS waiting on have answered */
    req = mon_req(&monitor, 1);
    mon_calls = 0;
    failed[0] = 7;
    st.failed_ranks.size = 1;
    prte_pmix_server_fault_handler(&st);
    CHECK("an unrelated daemon's death is not accounted", 0 == mon_calls);
    CHECK("...and leaves the request waiting", 0 == req->nreported);
    mon_drop(req);

    /* a request that never fanned out has no expected set to consult */
    req = mon_req(&monitor, 0);
    mon_calls = 0;
    failed[0] = 1;
    st.failed_ranks.size = 1;
    prte_pmix_server_fault_handler(&st);
    CHECK("a request that never fanned out is left alone", 0 == mon_calls);
    mon_drop(req);

    PMIX_INFO_DESTRUCT(&monitor);
    PMIX_DESTRUCT(&prte_pmix_server_globals.local_reqs);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_monitor_accounting\n");
    }
    return failures;
}

static int test_departed_jobs(void)
{
    int failures = 0;
    /* the helpers take a pmix_nspace_t - a fixed-size array - so hand them
     * one rather than a string literal the compiler can see is shorter */
    pmix_nspace_t ns, first, last;
    int i;

    /* the registry lives in the server globals, which a full
     * pmix_server_init() would have constructed for us */
    PMIX_CONSTRUCT(&prte_pmix_server_globals.departed_jobs, pmix_list_t);

    PMIX_LOAD_NSPACE(ns, "unit-test-never-existed");
    CHECK("an unknown job has not departed", !prte_pmix_server_job_has_departed(ns));

    PMIX_LOAD_NSPACE(first, "unit-test-dep@1");
    prte_pmix_server_job_departed(first);
    CHECK("a departed job is remembered", prte_pmix_server_job_has_departed(first));
    PMIX_LOAD_NSPACE(ns, "unit-test-dep@2");
    CHECK("...and its neighbours are not", !prte_pmix_server_job_has_departed(ns));

    /* recording the same one twice must not consume a second slot, or a job
     * whose departure is reported more than once would push out the history
     * of jobs that really did depart */
    prte_pmix_server_job_departed(first);
    CHECK("a repeat does not grow the list",
          1 == pmix_list_get_size(&prte_pmix_server_globals.departed_jobs));

    /* the bound holds, and it is the OLDEST that is dropped */
    for (i = 2; i <= 40; i++) {
        snprintf(ns, sizeof(ns), "unit-test-dep@%d", i);
        prte_pmix_server_job_departed(ns);
    }
    CHECK("the list stays bounded",
          32 >= pmix_list_get_size(&prte_pmix_server_globals.departed_jobs));
    PMIX_LOAD_NSPACE(last, "unit-test-dep@40");
    CHECK("the most recent departure is still known",
          prte_pmix_server_job_has_departed(last));
    CHECK("the oldest has aged out", !prte_pmix_server_job_has_departed(first));

    /* LIST_DESTRUCT, not DESTRUCT: the latter only re-initializes the list
     * and would leak every entry still on it */
    PMIX_LIST_DESTRUCT(&prte_pmix_server_globals.departed_jobs);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_departed_jobs\n");
    }
    return failures;
}

/* The registry of connected assemblages.
 *
 * PMIx_Connect asks the host to treat a set of processes as one application
 * for fault purposes, and the one obligation that creates is narrow: a member
 * that terminates without first calling PMIx_Disconnect owes the rest of the
 * assemblage a PMIX_ERR_PROC_TERM_WO_SYNC event.  Nothing recorded the
 * membership, so there was nothing to consult when a member died.
 *
 * The membership is held on the DVM master, and what is testable without a
 * DVM is the bookkeeping itself: that the same set is recognized however it
 * is ordered, that a set is matched literally rather than loosely, who a
 * wildcard member covers, and that a record is neither leaked nor dropped
 * while somebody named in it still exists.  Sending the event needs daemons.
 */
/*
 * PMIX_GROUP_LEFT arrives as an ordinary event notification, which means its
 * info array is whatever the generating client passed to PMIx_Notify_event:
 * PMIx forwards it to the host without inspecting what any entry holds, and
 * the host then xcasts it to every daemon in the DVM.  So every value has to
 * have its type checked before the matching member of its union is read, and
 * the departing proc has to name a concrete identity - PMIX_CHECK_PROCID
 * counts an empty nspace and a wildcard rank as matching anything, so a
 * departure that names neither would drop whichever member sits first.
 *
 * Both mistakes were live, and both are reachable by any process attached to
 * any daemon: a PMIX_GROUP_ID carrying a PMIX_SIZE handed strcmp() eight
 * bytes of the caller's choosing, on every daemon at once.
 */
static int test_group_left(void)
{
    int failures = 0;
    prte_pmix_server_pset_t *grp;
    pmix_info_t info[2];
    pmix_proc_t source, affected;
    size_t bogus = 0x41;    /* a value that is a fatal pointer and a fine size */

    PMIX_CONSTRUCT(&prte_pmix_server_globals.groups, pmix_list_t);
    grp = PMIX_NEW(prte_pmix_server_pset_t);
    grp->name = strdup("unit-test-group");
    grp->num_members = 3;
    PMIX_PROC_CREATE(grp->members, grp->num_members);
    PMIX_LOAD_PROCID(&grp->members[0], "unit-test-grp@1", 0);
    PMIX_LOAD_PROCID(&grp->members[1], "unit-test-grp@1", 1);
    PMIX_LOAD_PROCID(&grp->members[2], "unit-test-grp@1", 2);
    pmix_list_append(&prte_pmix_server_globals.groups, &grp->super);

    /* a proc that is in no group, so a fall back to the event source cannot
     * itself remove anything and confuse the cases below */
    PMIX_LOAD_PROCID(&source, "unit-test-grp@9", 0);

    /* a group id of the wrong type is not a group id */
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, &bogus, PMIX_SIZE);
    PMIX_LOAD_PROCID(&affected, "unit-test-grp@1", 1);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a mistyped group id is ignored", 3 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* ...and neither is a PMIX_STRING that carries no string, which is what
     * a string packed with a zero length comes back as */
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, NULL, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a group id with no string is ignored", 3 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* an affected proc of the wrong type is not a proc: the departure falls
     * back to the event source, which belongs to no group here */
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, "unit-test-group", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &bogus, PMIX_SIZE);
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a mistyped affected proc is ignored", 3 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* a departure that names no concrete identity must not stand for the
     * first member it is compared against */
    PMIX_LOAD_PROCID(&affected, NULL, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a wildcard departure drops nobody", 3 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* an event that is not a departure leaves the registry alone */
    PMIX_LOAD_PROCID(&affected, "unit-test-grp@1", 1);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    prte_pmix_server_group_member_left(PMIX_ERR_LOST_CONNECTION, &source, info, 2);
    CHECK("only PMIX_GROUP_LEFT is acted on", 3 == grp->num_members);

    /* and the case all of that guards: rank 1 leaves, rank 2 slides down,
     * and the members that stay keep their order */
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a departing member is dropped", 2 == grp->num_members);
    CHECK("the members that stay keep their order",
          0 == grp->members[0].rank && 2 == grp->members[1].rank);

    /* a second notice for a member already gone is a no-op, not an underflow */
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("a repeated departure is harmless", 2 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* a departure from a group we do not hold */
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, "unit-test-other", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    prte_pmix_server_group_member_left(PMIX_GROUP_LEFT, &source, info, 2);
    CHECK("an unknown group is left alone", 2 == grp->num_members);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    PMIX_LIST_DESTRUCT(&prte_pmix_server_globals.groups);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_group_left\n");
    }
    return failures;
}

/*
 * A PMIx_Query_info's qualifiers reach the host exactly as the requesting
 * process wrote them: PMIx forwards the array without inspecting what any
 * entry holds.  Six of the ones _query() acts on are strings it goes on to
 * hand to strlen(), strcmp() or PMIx_Check_nspace(), so a qualifier carrying
 * a value of some other type gave those functions whatever eight bytes the
 * caller chose as a pointer - which any process or tool attached to any
 * daemon could send.
 *
 * The query is driven through the real upcall rather than through the shifted
 * handler, because the upcall is what a client reaches and because the answer
 * this pins is the one the client is given: a malformed qualifier is
 * PMIX_ERR_BAD_PARAM, not a fault and not the PMIX_ERR_NOT_FOUND that the
 * status decision at the end of the handler used to overwrite it with.
 */
static pmix_status_t qstatus;
static bool qdone;

static void query_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                         void *cbdata, pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(info, ninfo, cbdata);
    qstatus = status;
    qdone = true;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

static pmix_status_t run_query(pmix_query_t *q)
{
    pmix_proc_t requestor;
    pmix_status_t rc;
    int spins = 0;

    PMIX_LOAD_PROCID(&requestor, "unit-test-query", 0);
    qstatus = PMIX_SUCCESS;
    qdone = false;
    rc = pmix_server_query_fn(&requestor, q, 1, query_cbfunc, NULL);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the upcall posts to the event base and this thread is the one that
     * drives it, so turn it until the handler has answered */
    while (!qdone && spins < 1000) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE | PRTE_EVLOOP_NONBLOCK);
        ++spins;
    }
    if (!qdone) {
        return PMIX_ERR_TIMEOUT;
    }
    return qstatus;
}

static int test_query_qualifiers(void)
{
    int failures = 0;
    pmix_query_t q;
    size_t bogus = 0x41;    /* a fatal pointer and a perfectly good size */
    bool made_array = false;
    prte_job_t *jdata = NULL;

    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT32_MAX, 8);
        made_array = true;
    }
    /* the namespace qualifier is checked against the jobs we know about */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-query@1");
    if (PRTE_SUCCESS != prte_set_job_data_object(jdata)) {
        fprintf(stderr, "FAIL [test_query_qualifiers]: could not register a job\n");
        PMIX_RELEASE(jdata);
        return 1;
    }

    /* a mistyped qualifier is refused, not read */
    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, PMIX_QUERY_NAMESPACES);
    PMIX_INFO_CREATE(q.qualifiers, 1);
    q.nqual = 1;
    PMIX_INFO_LOAD(&q.qualifiers[0], PMIX_HOSTNAME, &bogus, PMIX_SIZE);
    CHECK("a mistyped hostname qualifier is refused", PMIX_ERR_BAD_PARAM == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, PMIX_QUERY_NAMESPACES);
    PMIX_INFO_CREATE(q.qualifiers, 1);
    q.nqual = 1;
    PMIX_INFO_LOAD(&q.qualifiers[0], PMIX_NSPACE, &bogus, PMIX_SIZE);
    CHECK("a mistyped nspace qualifier is refused", PMIX_ERR_BAD_PARAM == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, PMIX_QUERY_NAMESPACES);
    PMIX_INFO_CREATE(q.qualifiers, 1);
    q.nqual = 1;
    PMIX_INFO_LOAD(&q.qualifiers[0], PMIX_ALLOC_ID, &bogus, PMIX_SIZE);
    CHECK("a mistyped allocation id qualifier is refused", PMIX_ERR_BAD_PARAM == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    /* a numeric qualifier that is not a number is refused too - it used to
     * leave the id at its "not given" sentinel and be silently ignored */
    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, PMIX_QUERY_NAMESPACES);
    PMIX_INFO_CREATE(q.qualifiers, 1);
    q.nqual = 1;
    PMIX_INFO_LOAD(&q.qualifiers[0], PMIX_NODEID, "not-a-number", PMIX_STRING);
    CHECK("a non-numeric nodeid qualifier is refused", PMIX_ERR_BAD_PARAM == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    /* an unknown namespace is a bad parameter and stays one: the status
     * decision at the end used to replace any error but NOT_SUPPORTED with
     * NOT_FOUND, because a failed arm leaves an empty result list behind */
    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, PMIX_QUERY_NAMESPACES);
    PMIX_INFO_CREATE(q.qualifiers, 1);
    q.nqual = 1;
    PMIX_INFO_LOAD(&q.qualifiers[0], PMIX_NSPACE, "unit-test-query@nope", PMIX_STRING);
    CHECK("an unknown namespace is reported as such", PMIX_ERR_BAD_PARAM == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    /* ...and a key this DVM does not implement still says so */
    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, "prte.unit.test.no.such.key");
    CHECK("an unrecognized key is not supported", PMIX_ERR_NOT_SUPPORTED == run_query(&q));
    PMIX_QUERY_DESTRUCT(&q);

    pmix_pointer_array_set_item(prte_job_data, jdata->index, NULL);
    PMIX_RELEASE(jdata);
    if (made_array) {
        PMIX_RELEASE(prte_job_data);
        prte_job_data = NULL;
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_query_qualifiers\n");
    }
    return failures;
}

static int test_connections(void)
{
    int failures = 0;
    pmix_proc_t members[2], other[2], proc;
    prte_proc_t pdata;
    prte_job_t *jdata;
    bool made_array = false;

    /* the registry lives in the server globals, which a full
     * pmix_server_init() would have constructed - and it declines to record
     * anything until that has happened, which is what keeps every other test
     * in this binary out of it */
    PMIX_CONSTRUCT(&prte_pmix_server_globals.connections, pmix_list_t);
    prte_pmix_server_globals.initialized = true;
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT32_MAX, 8);
        made_array = true;
    }

    PMIX_LOAD_PROCID(&members[0], "unit-test-conn@1", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&members[1], "unit-test-conn@2", PMIX_RANK_WILDCARD);

    prte_pmix_server_connection_record(members, 2);
    CHECK("a connect is recorded",
          1 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* the PMIx definition says the order of the participants carries no
     * meaning, so the same set given the other way round is the same set */
    other[0] = members[1];
    other[1] = members[0];
    prte_pmix_server_connection_record(other, 2);
    CHECK("the same set in another order is the same connection",
          1 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* a wildcard member stands for every proc of that namespace - which is
     * how a connect between two jobs is nearly always expressed, so getting
     * this wrong means never notifying anybody */
    PMIX_LOAD_PROCID(&proc, "unit-test-conn@1", 3);
    CHECK("a wildcard member covers a rank of that job",
          prte_pmix_server_is_connected(&proc));
    PMIX_LOAD_PROCID(&proc, "unit-test-conn@9", 0);
    CHECK("and covers nothing outside it", !prte_pmix_server_is_connected(&proc));

    /* Matching a set is literal.  PMIx will not pair a connect expressed with
     * wildcards with one that named the ranks, so a disconnect naming a set
     * we never recorded must leave the record we do hold alone rather than
     * drop it in somebody else's name. */
    PMIX_LOAD_PROCID(&other[0], "unit-test-conn@1", 0);
    PMIX_LOAD_PROCID(&other[1], "unit-test-conn@2", 0);
    prte_pmix_server_connection_drop(other, 2);
    CHECK("a different set does not drop this one",
          1 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* a job of the assemblage ends, but the other is still running: the
     * survivors are still connected to each other and still owed an event
     * apiece, so the record has to stay */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-conn@2");
    CHECK("the surviving job registers", PRTE_SUCCESS == prte_set_job_data_object(jdata));
    prte_pmix_server_connection_purge(members[0].nspace);
    CHECK("a connection outlives one member's job",
          1 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* ...and once nothing named in it is left, it has nobody to serve */
    pmix_pointer_array_set_item(prte_job_data, jdata->index, NULL);
    PMIX_RELEASE(jdata);
    prte_pmix_server_connection_purge(members[1].nspace);
    CHECK("and is forgotten when nothing named in it is left",
          0 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* a disconnect of the set that was recorded does drop it */
    prte_pmix_server_connection_record(members, 2);
    prte_pmix_server_connection_drop(members, 2);
    CHECK("a disconnect drops the connection",
          0 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /*
     * Dissolving is deliberately more generous than recording: a request
     * that names every member of an assemblage dissolves it, whether or not
     * it names them the same way.  The case that needs it is the one the
     * runtime creates itself - a spawn connects the child to the parent
     * PROCESS, while an application that wants out disconnects the two JOBS,
     * which is the shape MPI_Comm_disconnect has.  Under an exact-set rule
     * that request matches nothing and the implicit assemblage can never be
     * left, so a child's later failure would take a parent with it that had
     * said in as many words that it was done.
     */
    PMIX_LOAD_PROCID(&other[0], "unit-test-conn@1", 0);
    other[1] = members[1];
    prte_pmix_server_connection_record(other, 2);
    prte_pmix_server_connection_drop(members, 2);
    CHECK("a disconnect of the jobs dissolves a connection to one rank of one",
          0 == pmix_list_get_size(&prte_pmix_server_globals.connections));

    /* ...and the generosity only goes that way: naming one rank does not
     * dissolve an assemblage that named the whole job */
    prte_pmix_server_connection_record(members, 2);
    prte_pmix_server_connection_drop(other, 2);
    CHECK("a disconnect of one rank leaves a whole-job connection alone",
          1 == pmix_list_get_size(&prte_pmix_server_globals.connections));
    prte_pmix_server_connection_drop(members, 2);

    /* a proc that belongs to nothing is not connected, and reporting its
     * termination is a no-op rather than a walk off the end of an empty list */
    PMIX_CONSTRUCT(&pdata, prte_proc_t);
    PMIX_LOAD_PROCID(&pdata.name, "unit-test-conn@1", 0);
    pdata.exit_code = 0;
    CHECK("an unconnected proc is not connected", !prte_pmix_server_is_connected(&pdata.name));
    prte_pmix_server_connection_terminated(&pdata);
    PMIX_DESTRUCT(&pdata);

    prte_pmix_server_globals.initialized = false;
    PMIX_LIST_DESTRUCT(&prte_pmix_server_globals.connections);
    if (made_array) {
        PMIX_RELEASE(prte_job_data);
        prte_job_data = NULL;
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_connections\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0, skipped = 0;

    /* the schizo modules pick their option table off prte_tool_actual and
     * put prte_tool_basename in their error text - a tool sets both in
     * main() long before schizo is opened */
    prte_tool_actual = "prterun";
    prte_tool_basename = "prterun";

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

    /* create_app() runs the cmd line through a schizo personality */
    rc = pmix_mca_base_framework_open(&prte_schizo_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "schizo framework open failed: %d\n", rc);
        (void) pmix_mca_base_framework_close(&prte_state_base_framework);
        (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
        prte_event_base_close();
        prte_finalize();
        return 1;
    }
    rc = prte_schizo_base_select();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "schizo select failed: %d\n", rc);
        (void) pmix_mca_base_framework_close(&prte_schizo_base_framework);
        (void) pmix_mca_base_framework_close(&prte_state_base_framework);
        (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
        prte_event_base_close();
        prte_finalize();
        return 1;
    }

    failures += test_request_tracker();
    failures += test_departed_jobs();
    failures += test_monitor_accounting();
    failures += test_connections();
    failures += test_query_qualifiers();
    failures += test_group_left();
    failures += test_prefix_normalization();
    failures += test_singleton_id();
    failures += test_xfer_job_info();
    failures += test_xfer_app();
    failures += test_job_info_cache();
    failures += test_session_time();
    failures += test_directive_distribution();
    failures += test_envar_order(&skipped);
    failures += test_envar_order_permutations(&skipped);
    failures += test_envar_order_mpmd(&skipped);

    (void) pmix_mca_base_framework_close(&prte_schizo_base_framework);
    (void) pmix_mca_base_framework_close(&prte_state_base_framework);
    (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
    prte_event_base_close();
    prte_finalize();

    if (0 < skipped) {
        fprintf(stdout,
                "NOTE: %d envar-ordering case(s) skipped - this PMIx predates "
                "PMIX_CAP_CLI_ORDER,\n      so create_app() cannot order a "
                "directive repeated after another intervened.\n"
                "      Build against a PMIx that has the capability to check "
                "those cases.\n",
                skipped);
    }
    if (0 == failures) {
        fprintf(stdout, "PASSED all prted unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d prted unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
