/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests prte_rmaps_base_hoist_job_directives(), the step that takes the
 * qualifiers describing the whole job off the apps that carried them.
 *
 * A user with several apps on the command line has nowhere else to write
 * OVERSUBSCRIBE: it takes two mapping directives to make the mapping per-app
 * at all, and once the line is per-app there is no job-level directive left
 * to hang the qualifier on.  So a per-app spec may carry it, and this is
 * where it becomes the job's - and where the one thing that cannot be
 * honored, two apps asking for opposite things, is refused.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/attr.h"

int test_job_qualifiers(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* a job of napps apps, each app's mapping spec taken from specs[] (NULL
 * entries get no --map-by at all) */
static prte_job_t *build_job(char **specs, int napps)
{
    prte_job_t *jdata;
    prte_app_context_t *app;
    int n;

    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    for (n = 0; n < napps; n++) {
        app = PMIX_NEW(prte_app_context_t);
        app->app = strdup("hostname");
        app->idx = pmix_pointer_array_add(jdata->apps, app);
        jdata->num_apps++;
        if (NULL != specs[n]) {
            (void) prte_rmaps_base_set_app_mapping_policy(app, specs[n]);
        }
    }
    return jdata;
}

static prte_app_context_t *appn(prte_job_t *jdata, int n)
{
    return (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
}

int test_job_qualifiers(void)
{
    int failures = 0;
    int rc;
    prte_job_t *jdata;
    prte_mapping_policy_t oversub;
    bool nolocal;
    uint16_t u16;
    uint16_t *u16ptr = &u16;
    char *specs[2];

    /* === one app of two carries OVERSUBSCRIBE: it becomes the job's === */
    specs[0] = "core";
    specs[1] = "node:oversubscribe";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("oversub on one app: rc", PRTE_SUCCESS == rc);
    CHECK("oversub on one app: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & oversub);
    CHECK("oversub on one app: allowed", !(PRTE_MAPPING_NO_OVERSUBSCRIBE & oversub));
    /* the app keeps its own mapping policy, and only that */
    u16 = 0;
    CHECK("oversub on one app: app keeps map-by",
          prte_get_attribute(&appn(jdata, 1)->attributes, PRTE_APP_MAPBY,
                             (void **) &u16ptr, PMIX_UINT16));
    CHECK("oversub on one app: app policy intact",
          PRTE_MAPPING_BYNODE == PRTE_GET_MAPPING_POLICY(u16));
    CHECK("oversub on one app: app no longer carries it",
          !(PRTE_MAPPING_SUBSCRIBE_GIVEN & u16));
    PMIX_RELEASE(jdata);

    /* === agreeing apps are fine, however many say it === */
    specs[0] = "core:nooversubscribe";
    specs[1] = "node:nooversubscribe";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("agreeing apps: rc", PRTE_SUCCESS == rc);
    CHECK("agreeing apps: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & oversub);
    CHECK("agreeing apps: denied", PRTE_MAPPING_NO_OVERSUBSCRIBE & oversub);
    PMIX_RELEASE(jdata);

    /* === opposite answers cannot both be honored === */
    specs[0] = "core:oversubscribe";
    specs[1] = "node:nooversubscribe";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("conflicting apps: refused", PRTE_ERR_SILENT == rc);
    PMIX_RELEASE(jdata);

    /* === an app must also agree with the job's own directive === */
    specs[0] = NULL;
    specs[1] = "node:oversubscribe";
    jdata = build_job(specs, 2);
    PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
    PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("app vs job: refused", PRTE_ERR_SILENT == rc);
    PMIX_RELEASE(jdata);

    /* === a spec that was nothing but the qualifier leaves no per-app
     * mapping behind, so the job is not dragged onto the per-app path === */
    specs[0] = "core";
    specs[1] = ":oversubscribe";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("qualifier-only spec: rc", PRTE_SUCCESS == rc);
    CHECK("qualifier-only spec: given", PRTE_MAPPING_SUBSCRIBE_GIVEN & oversub);
    CHECK("qualifier-only spec: app has no map-by",
          !prte_get_attribute(&appn(jdata, 1)->attributes, PRTE_APP_MAPBY,
                              (void **) &u16ptr, PMIX_UINT16));
    PMIX_RELEASE(jdata);

    /* === INHERIT moves to the job === */
    specs[0] = "core";
    specs[1] = "node:inherit";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("inherit: rc", PRTE_SUCCESS == rc);
    CHECK("inherit: on the job",
          PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_INHERIT));
    CHECK("inherit: off the app",
          !PRTE_ATTR_IS_TRUE(&appn(jdata, 1)->attributes, PRTE_JOB_INHERIT));
    PMIX_RELEASE(jdata);

    /* === one app inheriting while another refuses to has no meaning === */
    specs[0] = "core:inherit";
    specs[1] = "node:noinherit";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("inherit vs noinherit: refused", PRTE_ERR_SILENT == rc);
    PMIX_RELEASE(jdata);

    /* === a job whose apps say nothing is left alone === */
    specs[0] = "core";
    specs[1] = "node";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("silent apps: rc", PRTE_SUCCESS == rc);
    CHECK("silent apps: nothing hoisted", 0 == oversub);
    CHECK("silent apps: no inherit",
          !PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_INHERIT));
    CHECK("silent apps: no nolocal", !nolocal);
    PMIX_RELEASE(jdata);

    /* === NOLOCAL is the job's answer too, and for a blunter reason than the
     *     others: the directive reaches the mappers only through the JOB's
     *     policy word - get_target_nodes() is handed that whichever app it is
     *     placing - so a nolocal left on an app was read by nothing at all
     *     and the app ran on the head node anyway === */
    specs[0] = "core";
    specs[1] = "node:nolocal";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("nolocal on one app: rc", PRTE_SUCCESS == rc);
    CHECK("nolocal on one app: hoisted", nolocal);
    u16 = 0;
    CHECK("nolocal on one app: app keeps map-by",
          prte_get_attribute(&appn(jdata, 1)->attributes, PRTE_APP_MAPBY,
                             (void **) &u16ptr, PMIX_UINT16));
    CHECK("nolocal on one app: app policy intact",
          PRTE_MAPPING_BYNODE == PRTE_GET_MAPPING_POLICY(u16));
    CHECK("nolocal on one app: app no longer carries it",
          !(PRTE_MAPPING_NO_USE_LOCAL & u16));
    PMIX_RELEASE(jdata);

    /* === an app whose spec held nothing but job-wide qualifiers named no
     *     mapping policy of its own. Leaving the attribute behind drags the
     *     whole job onto the per-app dispatch path to be placed by a policy
     *     of zero, so it goes === */
    specs[0] = "core";
    specs[1] = ":nolocal";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("qualifier-only app: rc", PRTE_SUCCESS == rc);
    CHECK("qualifier-only app: hoisted", nolocal);
    CHECK("qualifier-only app: attribute removed",
          !prte_get_attribute(&appn(jdata, 1)->attributes, PRTE_APP_MAPBY,
                              (void **) &u16ptr, PMIX_UINT16));
    PMIX_RELEASE(jdata);

    /* === but a qualifier the app really did ask for keeps the attribute === */
    specs[0] = "core";
    specs[1] = "package:span:nolocal";
    jdata = build_job(specs, 2);
    rc = prte_rmaps_base_hoist_job_directives(jdata, &oversub, &nolocal);
    CHECK("span survives hoist: rc", PRTE_SUCCESS == rc);
    CHECK("span survives hoist: hoisted", nolocal);
    u16 = 0;
    CHECK("span survives hoist: attribute kept",
          prte_get_attribute(&appn(jdata, 1)->attributes, PRTE_APP_MAPBY,
                             (void **) &u16ptr, PMIX_UINT16));
    CHECK("span survives hoist: span intact", PRTE_MAPPING_SPAN & u16);
    PMIX_RELEASE(jdata);

    return failures;
}
