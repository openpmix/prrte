/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the errmgr framework's base contract.
 *
 * The errmgr's real work lives in state-machine handlers that mutate
 * global DVM state (prte_local_children, num_daemons, the abort/term
 * flags, ...) and drive termination through prte_plm / prte_grpcomm.
 * Those paths cannot be exercised without a live DVM and are covered by
 * the integration/dockerswarm harnesses instead.  What *can* be checked
 * in isolation is the framework's structural contract, and in particular
 * the load-bearing invariant the framework guide calls out: every errmgr
 * module -- the two role components and the log-only default -- must carry
 * a non-NULL logfn, because prte_errmgr.logfn can be invoked during a very
 * early failure before the framework is even opened.  A regression that
 * zero-initializes one of these structs would turn that early-failure
 * report into a crash; these tests guard against exactly that.
 */

#include "prte_config.h"
#include <stdio.h>

#include "constants.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/dvm/errmgr_dvm.h"
#include "src/mca/errmgr/prted/errmgr_prted.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * The default (log-only) module used by tools and during early errors,
 * and the two role modules, must all satisfy the module contract:
 *  - logfn is ALWAYS non-NULL (the early-failure invariant);
 *  - the default module leaves init/finalize NULL (it only logs);
 *  - each real role module wires up both init and finalize.
 */
static int test_errmgr_modules(void)
{
    int failures = 0;

    /* the log-only fallback: logfn set, no state-machine wiring */
    CHECK("default logfn", NULL != prte_errmgr_default_fns.logfn);
    CHECK("default init", NULL == prte_errmgr_default_fns.init);
    CHECK("default finalize", NULL == prte_errmgr_default_fns.finalize);

    /* the live global must never present a NULL logfn, regardless of
     * whether a component has been selected yet */
    CHECK("live logfn", NULL != prte_errmgr.logfn);

    /* the HNP component */
    CHECK("dvm logfn", NULL != prte_errmgr_dvm_module.logfn);
    CHECK("dvm init", NULL != prte_errmgr_dvm_module.init);
    CHECK("dvm finalize", NULL != prte_errmgr_dvm_module.finalize);

    /* the daemon component */
    CHECK("prted logfn", NULL != prte_errmgr_prted_module.logfn);
    CHECK("prted init", NULL != prte_errmgr_prted_module.init);
    CHECK("prted finalize", NULL != prte_errmgr_prted_module.finalize);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_errmgr_modules\n");
    }
    return failures;
}

/*
 * prte_errmgr_base_log is the default logfn.  It turns an error code into
 * a string via prte_strerror and prints it; a "silent" code (one that maps
 * to NULL) must be handled by printing nothing rather than dereferencing a
 * NULL string.  We can't easily capture stdout here, but we can drive both
 * the normal and the silent paths and confirm neither crashes.
 */
static int test_errmgr_base_log(void)
{
    int failures = 0;

    /* a normal, named error code -> prints a message, must not crash */
    prte_errmgr_base_log(PRTE_ERR_OUT_OF_RESOURCE, __FILE__, __LINE__);

    /* PRTE_SUCCESS is not an error; prte_strerror still yields a string,
     * so this simply exercises the common path with a boundary value */
    prte_errmgr_base_log(PRTE_SUCCESS, __FILE__, __LINE__);

    /* an out-of-range code exercises the "silent" (NULL string) guard -
     * the function must return without dereferencing the NULL */
    prte_errmgr_base_log(-99999, __FILE__, __LINE__);

    /* reaching here means none of the calls crashed */
    fprintf(stdout, "PASSED test_errmgr_base_log\n");
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

    failures += test_errmgr_modules();
    failures += test_errmgr_base_log();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all errmgr unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d errmgr unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
