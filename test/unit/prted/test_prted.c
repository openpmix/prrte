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
 */

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

static int test_envar_order(void)
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
        PMIX_CONSTRUCT(&apps, pmix_list_t);
        rc = prte_parse_locals(schizo, &apps, (char **) c->argv,
                               NULL, NULL, NULL);
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
                                   const int *order)
{
    char **argv = NULL, **wanted = NULL, **rendered = NULL;
    char *want = NULL, *got = NULL, *desc = NULL, *entry;
    pmix_data_array_t darray;
    pmix_info_t *infos;
    pmix_list_t apps;
    prte_pmix_app_t *app;
    int failures = 0, rc, i;
    size_t n;

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
    rc = prte_parse_locals(schizo, &apps, argv, NULL, NULL, NULL);
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
                         int k, int *ncases)
{
    int failures = 0, i, tmp;

    if (1 == k) {
        ++(*ncases);
        return check_envar_permutation(schizo, order);
    }
    for (i = 0; i < k; i++) {
        failures += permute_envar(schizo, order, k - 1, ncases);
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

static int test_envar_order_permutations(void)
{
    prte_schizo_base_module_t *schizo;
    int order[ENVAR_POOL_SIZE];
    int failures, ncases = 0, i;

    schizo = envar_test_schizo();
    if (NULL == schizo) {
        return 1;
    }
    for (i = 0; i < ENVAR_POOL_SIZE; i++) {
        order[i] = i;
    }
    failures = permute_envar(schizo, order, ENVAR_POOL_SIZE, &ncases);
    fprintf(stdout, "envar order: %d orderings checked, %d failed\n",
            ncases, failures);
    return failures;
}

/*
 * Each app segment of an MPMD command line carries its own directives, and
 * they are parsed into separate results - so the ordering has to hold
 * per segment, not just for the first one.
 */
static int test_envar_order_mpmd(void)
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

    PMIX_CONSTRUCT(&apps, pmix_list_t);
    rc = prte_parse_locals(schizo, &apps, argv, NULL, NULL, NULL);
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

int main(void)
{
    int rc, failures = 0;

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

    failures += test_prefix_normalization();
    failures += test_singleton_id();
    failures += test_xfer_job_info();
    failures += test_xfer_app();
    failures += test_job_info_cache();
    failures += test_envar_order();
    failures += test_envar_order_permutations();
    failures += test_envar_order_mpmd();

    (void) pmix_mca_base_framework_close(&prte_schizo_base_framework);
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
