/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_schizo_base_setup_fork(): an app given a PMIx prefix has the
 * prefix's library directory put at the head of its LD_LIBRARY_PATH,
 * which means walking the app's environment by NAME=value. An entry
 * that is not an assignment is passed over, never split.
 */

#include "test_schizo.h"

#include "src/mca/schizo/base/base.h"
#include "src/util/attr.h"

static const char *find(char **env, const char *name)
{
    size_t len = strlen(name);
    int i;

    for (i = 0; NULL != env && NULL != env[i]; i++) {
        if (0 == strncmp(env[i], name, len) && '=' == env[i][len]) {
            return env[i] + len + 1;
        }
    }
    return NULL;
}

int test_setup_fork(void)
{
    int failures = 0;
    prte_job_t *jdata;
    prte_app_context_t *app;
    const char *v;
    char prefix[] = "/opt/prte-test-prefix";
    int rc;

    jdata = PMIX_NEW(prte_job_t);
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup("/bin/true");
    PMIx_Argv_append_nosize(&app->env, "FIRST=1");
    PMIx_Argv_append_nosize(&app->env, "NOT_AN_ASSIGNMENT");
    PMIx_Argv_append_nosize(&app->env, "LD_LIBRARY_PATH=/usr/lib");
    prte_set_attribute(&app->attributes, PRTE_APP_PMIX_PREFIX, PRTE_ATTR_GLOBAL, prefix,
                       PMIX_STRING);

    rc = prte_schizo_base_setup_fork(jdata, app);
    CHECK("setup_fork:rc", PRTE_SUCCESS == rc);
    v = find(app->env, "PMIX_PREFIX");
    CHECK("setup_fork:pmix-prefix", NULL != v && 0 == strcmp(v, prefix));
    v = find(app->env, "LD_LIBRARY_PATH");
    CHECK("setup_fork:ldlp-prepended",
          NULL != v && 0 == strncmp(v, prefix, strlen(prefix)) && NULL != strstr(v, ":/usr/lib"));
    CHECK("setup_fork:entry-kept", NULL != find(app->env, "FIRST"));
    {
        int i;
        bool kept = false;

        for (i = 0; NULL != app->env[i]; i++) {
            kept = kept || (0 == strcmp(app->env[i], "NOT_AN_ASSIGNMENT"));
        }
        CHECK("setup_fork:non-assignment-untouched", kept);
    }

    PMIX_RELEASE(app);
    PMIX_RELEASE(jdata);
    return failures;
}
