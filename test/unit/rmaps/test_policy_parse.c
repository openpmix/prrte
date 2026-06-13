/*
 * Copyright (c) 2024-2025 Nanook Consulting  All rights reserved.
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

    /* --- ppr:2:core stores PPR count and policy --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "ppr:2:core");
    CHECK("mapby ppr:2:core: rc", PRTE_SUCCESS == rc);
    u16 = get_u16(&app->attributes, PRTE_APP_PPR);
    CHECK("mapby ppr:2:core: ppn=2", 2 == u16);
    u16 = get_u16(&app->attributes, PRTE_APP_MAPBY);
    CHECK("mapby ppr:2:core: policy=PPR", PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(u16));
    PMIX_RELEASE(app);

    /* --- rejected: OVERSUBSCRIBE --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:oversubscribe");
    CHECK("mapby OVERSUBSCRIBE: rejected", PRTE_ERR_BAD_PARAM == rc);
    PMIX_RELEASE(app);

    /* --- rejected: NOOVERSUBSCRIBE --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:nooversubscribe");
    CHECK("mapby NOOVERSUBSCRIBE: rejected", PRTE_ERR_BAD_PARAM == rc);
    PMIX_RELEASE(app);

    /* --- rejected: INHERIT --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:inherit");
    CHECK("mapby INHERIT: rejected", PRTE_ERR_BAD_PARAM == rc);
    PMIX_RELEASE(app);

    /* --- rejected: NOINHERIT --- */
    app = PMIX_NEW(prte_app_context_t);
    rc = prte_rmaps_base_set_app_mapping_policy(app, "core:noinherit");
    CHECK("mapby NOINHERIT: rejected", PRTE_ERR_BAD_PARAM == rc);
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

    return failures;
}
