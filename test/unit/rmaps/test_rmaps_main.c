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

#include "src/mca/rmaps/base/base.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"
#include "constants.h"

#include <pmix_server.h>

extern int test_policy_parse(void);
extern int test_job_policy(void);
extern int test_job_qualifiers(void);
extern int test_resolve_options(void);
extern int test_ranking(void);
extern int test_binding(void);
extern int test_check_avail(void);
extern int test_resize(void);
extern int test_dispatch(void);
extern int test_round_robin(void);
extern int test_ppr(void);
extern int test_seq(void);
extern int test_rank_file(void);
extern int test_devices(bool pmix_up);

/* shared with the mapper tests in this directory */
prte_rmaps_base_module_t *test_rmaps_module(const char *name);

/*
 * Hand a mapper test its subject.
 *
 * The tests used to name the component's module symbol directly, which
 * asks two things of the build that are not actually true: that the
 * symbol is visible outside libprrte (it is not, once the library is
 * compiled with -fvisibility=hidden - it belongs to the component, not
 * to the framework), and that the component was built at all.  Ask the
 * framework instead, which is how the mapper is reached in production,
 * and let a caller skip when its component is absent.
 */
prte_rmaps_base_module_t *test_rmaps_module(const char *name)
{
    prte_rmaps_base_selected_module_t *mod;

    PMIX_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules,
                      prte_rmaps_base_selected_module_t)
    {
        if (NULL != mod->component &&
            0 == strcmp(name, mod->component->pmix_mca_component_name)) {
            return mod->module;
        }
    }
    return NULL;
}

int main(void)
{
    int rc, failures = 0;
    bool pmix_up;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* One test publishes a device assignment as a PMIX_DATA_ARRAY
     * attribute, and prte_attr_load() copies it with PMIx_Data_copy, which
     * will not run until PMIx itself is up.  A daemon reaches that state
     * through PMIx_server_init, so do the same - here rather than inside
     * the test, because the matching finalize has to come after the
     * frameworks close (see below) and only main can sequence that. */
    pmix_up = (PMIX_SUCCESS == PMIx_server_init(NULL, NULL, 0));

    /* the mapper tests take their module from the framework, so it has to
     * be open and selected before they run */
    rc = pmix_mca_base_framework_open(&prte_rmaps_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "rmaps framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }
    rc = prte_rmaps_base_select();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "rmaps select failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_policy_parse();
    failures += test_job_policy();
    failures += test_job_qualifiers();
    failures += test_resolve_options();
    failures += test_ranking();
    failures += test_binding();
    failures += test_check_avail();
    failures += test_resize();
    failures += test_dispatch();
    failures += test_round_robin();
    failures += test_ppr();
    failures += test_seq();
    failures += test_rank_file();
    failures += test_devices(pmix_up);

    /* Order matters, and getting it wrong is a segfault rather than a
     * leak.  In an --enable-mca-dso build PRRTE's components are shared
     * objects loaded through PMIx's MCA base, so PMIx_server_finalize
     * DLCLOSES them - and closing a PRRTE framework afterwards calls each
     * selected component's close function at an address that is no longer
     * mapped.  Frameworks first, PMIx second. */
    (void) pmix_mca_base_framework_close(&prte_rmaps_base_framework);
    if (pmix_up) {
        PMIx_server_finalize();
    }
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rmaps unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rmaps unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
