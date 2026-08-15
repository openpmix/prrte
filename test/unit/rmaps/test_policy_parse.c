/*
 * Copyright (c) 2024-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/attr.h"

int test_policy_parse(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * prte_get_attribute for PMIX_UINT16 requires pre-allocated storage:
 * uint16_t u16; uint16_t *ptr = &u16; then pass (void **)&ptr.
 * The value is memcpy'd into *ptr (not allocated by the callee).
 */
static uint16_t get_u16(pmix_list_t *attrs, prte_attribute_key_t key)
{
    uint16_t val = 0;
    uint16_t *ptr = &val;
    if (!prte_get_attribute(attrs, key, (void **) &ptr, PMIX_UINT16)) {
        return 0;
    }
    return val;
}

static bool get_bool(pmix_list_t *attrs, prte_attribute_key_t key)
{
    return prte_get_attribute(attrs, key, NULL, PMIX_BOOL);
}

static char *get_str(pmix_list_t *attrs, prte_attribute_key_t key)
{
    char *str = NULL;
    prte_get_attribute(attrs, key, (void **) &str, PMIX_STRING);
    return str;
}

int test_policy_parse(void)
{
    int failures = 0;
    int rc;
    prte_app_context_t *app;
    uint16_t u16;
    char *sval;

    /* --- mapping policy: "slot" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "slot");
    CHECK("mapby slot: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby slot: policy", PRTE_MAPPING_BYSLOT == PRTE_GET_MAPPING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- mapping policy: "node" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "node");
    CHECK("mapby node: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby node: policy", PRTE_MAPPING_BYNODE == PRTE_GET_MAPPING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- mapping policy: "core:pe=2" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:pe=2");
    CHECK("mapby core:pe=2: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_PES_PER_PROC);
    CHECK("mapby core:pe=2: pes", 2 == u16);
    PMIX_RELEASE(app);

    /* --- mapping policy: "core:hwtcpus" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:hwtcpus");
    CHECK("mapby core:hwtcpus: rc", PRTE_SUCCESS == rc);
    CHECK("mapby core:hwtcpus: flag", get_bool(&app->attributes, PRTE_APP_HWT_CPUS));
    PMIX_RELEASE(app);

    /* --- mapping policy: "core:corecpus" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:corecpus");
    CHECK("mapby core:corecpus: rc", PRTE_SUCCESS == rc);
    CHECK("mapby core:corecpus: flag", get_bool(&app->attributes, PRTE_APP_CORE_CPUS));
    PMIX_RELEASE(app);

    /* --- mapping policy: "seq:file=/tmp/myfile" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "seq:file=/tmp/myfile");
    CHECK("mapby seq:file: rc", PRTE_SUCCESS == rc);
    sval = get_str(&app->attributes, PRTE_APP_MAP_FILE);
    CHECK("mapby seq:file: path", NULL != sval && 0 == strcmp(sval, "/tmp/myfile"));
    free(sval);
    PMIX_RELEASE(app);

    /* --- mapping policy: "core:nolocal" sets NOLOCAL directive --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:nolocal");
    CHECK("mapby core:nolocal: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby core:nolocal: directive", PRTE_MAPPING_NO_USE_LOCAL & u16);
    PMIX_RELEASE(app);

    /* --- ppr:2:core stores the whole pattern and the policy ---
     * Both halves: an app that asks for two per core is not asking for two
     * per whatever object the job resolved.  The spelling matches the
     * job-level PRTE_JOB_PPR attribute so one reader serves both. */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "ppr:2:core");
    CHECK("mapby ppr:2:core: rc", PRTE_SUCCESS == rc);
    sval = get_str(&app->attributes, PRTE_APP_PPR);
    CHECK("mapby ppr:2:core: pattern", NULL != sval && 0 == strcmp(sval, "2:core"));
    free(sval);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby ppr:2:core: policy=PPR", PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- a per-app rankfile names the file it is to read --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "rankfile:file=/tmp/myrf");
    CHECK("mapby rankfile:file: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby rankfile:file: policy=BYUSER",
          PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(u16));
    sval = get_str(&app->attributes, PRTE_APP_MAP_FILE);
    CHECK("mapby rankfile:file: path", NULL != sval && 0 == strcmp(sval, "/tmp/myrf"));
    free(sval);
    PMIX_RELEASE(app);

    /* --- ...and is refused when it names none, exactly as at job level.
     * The MCA default file is the other place a name can come from, so
     * clear it for this case rather than depend on the developer not having
     * set one. */
    sval = prte_rmaps_base.file;
    prte_rmaps_base.file = NULL;
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "rankfile");
    CHECK("mapby rankfile without a file: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);
    prte_rmaps_base.file = sval;

    /* --- the qualifiers that span the whole job ---
     * These describe the job, not the app, but they are accepted in a per-app
     * spec and recorded there: on a multi-app command line there is no other
     * place to write them, since it takes two mapping directives to make the
     * mapping per-app at all.  The hoist below is what moves them to the job
     * and holds the apps to agreeing about them.
     */
    /* --- OVERSUBSCRIBE --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:oversubscribe");
    CHECK("mapby OVERSUBSCRIBE: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby OVERSUBSCRIBE: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & u16);
    CHECK("mapby OVERSUBSCRIBE: allowed", !(PRTE_MAPPING_NO_OVERSUBSCRIBE & u16));
    PMIX_RELEASE(app);

    /* --- NOOVERSUBSCRIBE --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:nooversubscribe");
    CHECK("mapby NOOVERSUBSCRIBE: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby NOOVERSUBSCRIBE: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & u16);
    CHECK("mapby NOOVERSUBSCRIBE: denied", PRTE_MAPPING_NO_OVERSUBSCRIBE & u16);
    PMIX_RELEASE(app);

    /* --- a spec that is nothing but a job-level qualifier.  It names no
     * policy, so it used to be discarded without a word --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, ":oversubscribe");
    CHECK("mapby :OVERSUBSCRIBE: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby :OVERSUBSCRIBE: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & u16);
    CHECK("mapby :OVERSUBSCRIBE: no policy", 0 == PRTE_GET_MAPPING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- INHERIT --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:inherit");
    CHECK("mapby INHERIT: rc", PRTE_SUCCESS == rc);
    CHECK("mapby INHERIT: flag", get_bool(&app->attributes, PRTE_JOB_INHERIT));
    PMIX_RELEASE(app);

    /* --- NOINHERIT --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:noinherit");
    CHECK("mapby NOINHERIT: rc", PRTE_SUCCESS == rc);
    CHECK("mapby NOINHERIT: flag", get_bool(&app->attributes, PRTE_JOB_NOINHERIT));
    PMIX_RELEASE(app);

    /* --- contradicting yourself within one spec is still refused.
     * PRTE_ERR_SILENT, not BAD_PARAM, because the parser has already told the
     * user why - BAD_PARAM is reserved for a qualifier it does not recognize,
     * and the caller turns that into a "check for a typo" message this must
     * not attract. --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:oversubscribe:nooversubscribe");
    CHECK("mapby OVERSUB+NOOVERSUB: rejected", PRTE_ERR_SILENT == rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:inherit:noinherit");
    CHECK("mapby INHERIT+NOINHERIT: rejected", PRTE_ERR_SILENT == rc);
    PMIX_RELEASE(app);

    /* --- NULL spec is a no-op --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, NULL);
    CHECK("mapby NULL: rc", PRTE_SUCCESS == rc);
    PMIX_RELEASE(app);

    /* --- ranking policy: "node" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_ranking_policy(app, "node");
    CHECK("rankby node: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_RANKBY);
    CHECK("rankby node: policy", PRTE_RANK_BY_NODE == PRTE_GET_RANKING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- ranking policy: "slot" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_ranking_policy(app, "slot");
    CHECK("rankby slot: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_RANKBY);
    CHECK("rankby slot: policy", PRTE_RANK_BY_SLOT == PRTE_GET_RANKING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- ranking policy: "fill" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_ranking_policy(app, "fill");
    CHECK("rankby fill: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_RANKBY);
    CHECK("rankby fill: policy", PRTE_RANK_BY_FILL == PRTE_GET_RANKING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- an abbreviated qualifier keeps its value.  The matcher accepts any
     * unambiguous prefix, so reading the value at an offset fixed to the full
     * spelling ("PE=", "FILE=", "LIMIT=") silently truncated or zeroed it --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "slot:P=2");
    CHECK("mapby slot:P=2: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_PES_PER_PROC);
    CHECK("mapby slot:P=2: pes", 2 == u16);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "seq:F=/tmp/myfile");
    CHECK("mapby seq:F=path: rc", PRTE_SUCCESS == rc);
    sval = get_str(&app->attributes, PRTE_APP_MAP_FILE);
    CHECK("mapby seq:F=path: whole path", NULL != sval && 0 == strcmp(sval, "/tmp/myfile"));
    free(sval);
    PMIX_RELEASE(app);

    /* --- pe-list is a per-app directive too.  Which cpus one app of an MPMD
     * job may use is as much its own business as which object it maps by,
     * and a multi-app command line has nowhere else to say it --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "pe-list=0-3,6");
    CHECK("mapby pe-list: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby pe-list: policy", PRTE_MAPPING_PELIST == PRTE_GET_MAPPING_POLICY(u16));
    sval = get_str(&app->attributes, PRTE_APP_CPUSET);
    CHECK("mapby pe-list: cpuset", NULL != sval && 0 == strcmp(sval, "0-3,6"));
    free(sval);
    PMIX_RELEASE(app);

    /* --- device= is per-app for the same reason: which devices one app of
     * an MPMD job is placed against is its own business --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu");
    CHECK("mapby device=gpu: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby device=gpu: policy", PRTE_MAPPING_BYDEVICE == PRTE_GET_MAPPING_POLICY(u16));
    sval = get_str(&app->attributes, PRTE_APP_MAP_DEVICE);
    CHECK("mapby device=gpu: spec", NULL != sval && 0 == strcmp(sval, "gpu"));
    free(sval);
    PMIX_RELEASE(app);

    /* the value is read after the "=", not at a fixed offset past the full
     * spelling - the directive may be abbreviated to any unambiguous prefix */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "dev=mlx5_0");
    CHECK("mapby dev=mlx5_0: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby dev=mlx5_0: policy", PRTE_MAPPING_BYDEVICE == PRTE_GET_MAPPING_POLICY(u16));
    sval = get_str(&app->attributes, PRTE_APP_MAP_DEVICE);
    CHECK("mapby dev=mlx5_0: spec read after the =",
          NULL != sval && 0 == strcmp(sval, "mlx5_0"));
    free(sval);
    PMIX_RELEASE(app);

    /* "device" with nothing after the "=" names no device at all */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=");
    CHECK("mapby device= with no value: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* --- the interleave qualifier reorders a device list --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:interleave");
    CHECK("mapby interleave: rc", PRTE_SUCCESS == rc);
    sval = get_str(&app->attributes, PRTE_APP_MAP_INTERLEAVE);
    CHECK("mapby interleave: defaults to package",
          NULL != sval && 0 == strcmp(sval, "package"));
    free(sval);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:interleave=numa");
    CHECK("mapby interleave=numa: rc", PRTE_SUCCESS == rc);
    sval = get_str(&app->attributes, PRTE_APP_MAP_INTERLEAVE);
    CHECK("mapby interleave=numa: level honored",
          NULL != sval && 0 == strcmp(sval, "numa"));
    free(sval);
    PMIX_RELEASE(app);

    /* "node" is refused: interleaving across nodes is what SPAN expresses,
     * and one behavior with two names composes into nonsense */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:interleave=node");
    CHECK("mapby interleave=node: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:interleave=bogus");
    CHECK("mapby interleave=bogus: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* it reorders a DEVICE list, so it means nothing on any other map */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "numa:interleave");
    CHECK("mapby numa:interleave: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* --- shared: a device may be given to more than one proc --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:shared");
    CHECK("mapby shared: rc", PRTE_SUCCESS == rc);
    CHECK("mapby shared: recorded",
          prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED, NULL, PMIX_BOOL));
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:shared=true");
    CHECK("mapby shared=true: recorded",
          PRTE_SUCCESS == rc
          && prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED, NULL, PMIX_BOOL));
    PMIX_RELEASE(app);

    /* false is the default, so it records nothing - and must not be an error */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:shared=false");
    CHECK("mapby shared=false: accepted", PRTE_SUCCESS == rc);
    CHECK("mapby shared=false: not recorded",
          !prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED, NULL, PMIX_BOOL));
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:shared=maybe");
    CHECK("mapby shared=maybe: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* it says devices may be shared, so it means nothing without devices */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "numa:shared");
    CHECK("mapby numa:shared: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* The same prefix hazard as interleave, against a different neighbour:
     * "shared" and "span" both begin with 's', and ":s" has meant SPAN for
     * as long as there has been one.  The shared arm sits after it. */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:s");
    CHECK("mapby ':s' : rc", PRTE_SUCCESS == rc);
    CHECK("mapby ':s' still means SPAN, not shared",
          !prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED, NULL, PMIX_BOOL));
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby ':s' set the SPAN directive",
          0 != (PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(u16)));
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:sh");
    CHECK("mapby ':sh' is shared",
          PRTE_SUCCESS == rc
          && prte_get_attribute(&app->attributes, PRTE_APP_MAP_SHARED, NULL, PMIX_BOOL));
    PMIX_RELEASE(app);

    /* THE regression that matters: "interleave" and "inherit" share a first
     * letter, and the option matcher has no view of the other options - the
     * first arm of the chain that prefix-matches wins. ":i" has meant
     * INHERIT for as long as there has been one, so an interleave arm tested
     * before the inherit arm would silently change what a working command
     * line does. No error, no warning, a different mapping. */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:i");
    CHECK("mapby device=gpu:i : rc", PRTE_SUCCESS == rc);
    CHECK("mapby ':i' still means INHERIT, not interleave",
          !prte_get_attribute(&app->attributes, PRTE_APP_MAP_INTERLEAVE, NULL, PMIX_STRING));
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:in");
    CHECK("mapby device=gpu:in : rc", PRTE_SUCCESS == rc);
    CHECK("mapby ':in' still means INHERIT",
          !prte_get_attribute(&app->attributes, PRTE_APP_MAP_INTERLEAVE, NULL, PMIX_STRING));
    PMIX_RELEASE(app);

    /* ...while ":int" is unambiguously interleave */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "device=gpu:int");
    CHECK("mapby ':int' is interleave",
          PRTE_SUCCESS == rc
          && prte_get_attribute(&app->attributes, PRTE_APP_MAP_INTERLEAVE, NULL, PMIX_STRING));
    PMIX_RELEASE(app);

    /* there is deliberately no bare "gpu" spelling: the class is the
     * directive's VALUE, which is what lets a new class be supported by
     * adding a value rather than a directive */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "gpu");
    CHECK("mapby bare gpu: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* the value is validated here, not left for the mapper to trip over */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "pe-list=0-3-5");
    CHECK("mapby pe-list bad range: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "pe-list=zero");
    CHECK("mapby pe-list non-numeric: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "pe-list");
    CHECK("mapby pe-list with no value: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    /* --- and it survives resolution into the per-app options struct, which
     * is what actually reaches the mapper --- */
    {
        prte_rmaps_options_t opts;
        app = PMIX_NEW(prte_app_context_t);
        prte_rmaps_base_set_app_mapping_policy(app, "pe-list=2-3");
        memset(&opts, 0, sizeof(opts));
        opts.map = PRTE_MAPPING_BYSLOT;
        opts.app_idx = 0;
        rc = prte_rmaps_base_resolve_app_options(NULL, app, &opts);
        CHECK("resolve pe-list: rc", PRTE_SUCCESS == rc);
        CHECK("resolve pe-list: map", PRTE_MAPPING_PELIST == opts.map);
        CHECK("resolve pe-list: maptype", HWLOC_OBJ_MACHINE == opts.maptype);
        CHECK("resolve pe-list: cpuset", NULL != opts.cpuset
              && 0 == strcmp(opts.cpuset, "2-3"));
        free(opts.cpuset);
        PMIX_RELEASE(app);
    }

    /* --- binding policy: "core" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core");
    CHECK("bindto core: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_BINDTO);
    CHECK("bindto core: policy", PRTE_BIND_TO_CORE == PRTE_GET_BINDING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- binding policy: "none" --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "none");
    CHECK("bindto none: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_BINDTO);
    CHECK("bindto none: policy", PRTE_BIND_TO_NONE == PRTE_GET_BINDING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- binding policy: NULL is a no-op --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, NULL);
    CHECK("bindto NULL: rc", PRTE_SUCCESS == rc);
    PMIX_RELEASE(app);

    /* --- binding policy: the LIMIT qualifier ---
     * PRTE_APP_BINDING_LIMIT is a uint16, and the value used to be cast
     * straight out of strtol(), so "limit=70000" quietly became 4464. Zero is
     * refused because rmaps_base_binding.c reads a limit of zero as "no limit
     * at all" (0 < options->limit), which is not what the user wrote. This
     * parser must agree with its job-level twin,
     * prte_hwloc_base_set_binding_policy(). */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=2");
    CHECK("bindto limit: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_BINDING_LIMIT);
    CHECK("bindto limit: value", 2 == u16);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=70000");
    CHECK("bindto limit past uint16 is refused, not truncated", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=foo");
    CHECK("bindto limit non-numeric is refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=2x");
    CHECK("bindto limit with trailing garbage is refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=-1");
    CHECK("bindto limit negative is refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_binding_policy(app, "core:limit=0");
    CHECK("bindto limit zero is refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(app);

    return failures;
}
