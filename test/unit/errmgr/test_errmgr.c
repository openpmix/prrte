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
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/errmgr/base/base.h"
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
/*
 * Ask the framework for the module a component hands *this* process.
 *
 * Naming the component's module symbol assumes it was linked into
 * libprrte; with --enable-mca-dso the component is a separate DSO and the
 * symbol is not there to link against.  Each errmgr component also gates
 * its query on the process type, so the caller says which type it is
 * asking as.
 */
static prte_errmgr_base_module_t *errmgr_module(const char *name, prte_proc_type_t as)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    prte_proc_type_t save;
    int pri = 0;

    PMIX_LIST_FOREACH(cli, &prte_errmgr_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 != strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL == cli->cli_component->pmix_mca_query_component) {
            return NULL;
        }
        save = prte_process_info.proc_type;
        prte_process_info.proc_type = as;
        if (PRTE_SUCCESS != cli->cli_component->pmix_mca_query_component(&mod, &pri)) {
            mod = NULL;
        }
        prte_process_info.proc_type = save;
        return (prte_errmgr_base_module_t *) mod;
    }
    return NULL;
}

static int test_errmgr_modules(void)
{
    int failures = 0;
    prte_errmgr_base_module_t *dvm, *prted;

    /* the log-only fallback: logfn set, no state-machine wiring */
    CHECK("default logfn", NULL != prte_errmgr_default_fns.logfn);
    CHECK("default init", NULL == prte_errmgr_default_fns.init);
    CHECK("default finalize", NULL == prte_errmgr_default_fns.finalize);

    /* the live global must never present a NULL logfn, regardless of
     * whether a component has been selected yet */
    CHECK("live logfn", NULL != prte_errmgr.logfn);

    /* the HNP component, asked as the master it serves */
    dvm = errmgr_module("dvm", PRTE_PROC_MASTER);
    if (NULL == dvm) {
        fprintf(stdout, "  (skipping dvm module checks: component absent)\n");
    } else {
        CHECK("dvm logfn", NULL != dvm->logfn);
        CHECK("dvm init", NULL != dvm->init);
        CHECK("dvm finalize", NULL != dvm->finalize);
    }

    /* the daemon component, asked as the daemon it serves */
    prted = errmgr_module("prted", PRTE_PROC_DAEMON);
    if (NULL == prted) {
        fprintf(stdout, "  (skipping prted module checks: component absent)\n");
    } else {
        CHECK("prted logfn", NULL != prted->logfn);
        CHECK("prted init", NULL != prted->init);
        CHECK("prted finalize", NULL != prted->finalize);
    }

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

/*
 * Stand up a synthetic prte_local_children array.  The errmgr handlers all
 * consult this global to decide whether this node still has work on it, so a
 * test can pose that question by populating it directly.
 */
static prte_proc_t *add_child(const char *nspace, pmix_rank_t rank, bool alive)
{
    prte_proc_t *p = PMIX_NEW(prte_proc_t);

    PMIX_LOAD_PROCID(&p->name, nspace, rank);
    p->pid = 1000 + rank;
    p->state = alive ? PRTE_PROC_STATE_RUNNING : PRTE_PROC_STATE_TERMINATED;
    p->exit_code = alive ? 0 : (int32_t) rank;
    if (alive) {
        PRTE_FLAG_SET(p, PRTE_PROC_FLAG_ALIVE);
    }
    pmix_pointer_array_add(prte_local_children, p);
    return p;
}

static void reset_children(void)
{
    int i;
    prte_proc_t *p;

    if (NULL == prte_local_children) {
        prte_local_children = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_local_children, 8, INT32_MAX, 8);
        return;
    }
    for (i = 0; i < prte_local_children->size; i++) {
        p = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL != p) {
            pmix_pointer_array_set_item(prte_local_children, i, NULL);
            PMIX_RELEASE(p);
        }
    }
}

/*
 * prte_errmgr_base_any_live_children is how every handler answers "is this
 * node empty yet".  It used to be spelled out inline at each call site, and
 * one of those inline scans reused the variable holding the proc whose error
 * was being handled - so a surviving sibling was reported to the HNP as the
 * failed proc.  Now there is one implementation, and this pins its answers.
 */
static int test_any_live_children(void)
{
    int failures = 0;

    reset_children();

    /* nobody home */
    CHECK("empty: any job", !prte_errmgr_base_any_live_children(NULL));
    CHECK("empty: named job", !prte_errmgr_base_any_live_children("jobA"));

    /* one dead child of jobA */
    add_child("jobA", 0, false);
    CHECK("dead only: any job", !prte_errmgr_base_any_live_children(NULL));
    CHECK("dead only: jobA", !prte_errmgr_base_any_live_children("jobA"));

    /* a live child of jobB - visible to the wildcard and to jobB, but the
     * question "does jobA still have anyone" must stay false */
    add_child("jobB", 0, true);
    CHECK("live jobB: any job", prte_errmgr_base_any_live_children(NULL));
    CHECK("live jobB: jobB", prte_errmgr_base_any_live_children("jobB"));
    CHECK("live jobB: jobA unaffected", !prte_errmgr_base_any_live_children("jobA"));

    /* a live child of jobA, added after the dead one, must still be found -
     * the scan has to cover the whole array, not stop at the first entry */
    add_child("jobA", 1, true);
    CHECK("live jobA", prte_errmgr_base_any_live_children("jobA"));

    /* a hole in the middle of the array must not end the scan */
    reset_children();
    add_child("jobA", 0, false);
    pmix_pointer_array_set_item(prte_local_children, 1, NULL);
    add_child("jobA", 2, true);
    CHECK("live after hole", prte_errmgr_base_any_live_children("jobA"));

    reset_children();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_any_live_children\n");
    }
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

    /* the module checks ask the framework for their subject */
    rc = pmix_mca_base_framework_open(&prte_errmgr_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "errmgr framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_errmgr_modules();
    failures += test_errmgr_base_log();
    failures += test_any_live_children();

    (void) pmix_mca_base_framework_close(&prte_errmgr_base_framework);
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all errmgr unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d errmgr unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
