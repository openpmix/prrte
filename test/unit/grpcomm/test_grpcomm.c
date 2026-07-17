/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the grpcomm framework.
 *
 * The substantive work of grpcomm -- the reliable xcast broadcast, the
 * up-tree fence allgather, and the PMIx group construct/destruct/cancel
 * collectives -- all rides the RML radix routing tree across a live DVM
 * and cannot run without one; it is covered by the integration and
 * dockerswarm harnesses.
 *
 * What *can* be exercised in isolation is the framework's structural
 * contract and the constructor/destructor invariants that its collective
 * code relies on:
 *
 *   1. The module contract.  grpcomm.h states that every function pointer
 *      in prte_grpcomm_base_module_t "MUST be provided"; the sole
 *      component, direct, is what the DVM always selects, so a regression
 *      that left one of its seven slots NULL would crash at first use
 *      (exactly the failure the errmgr logfn test guards against).  We
 *      confirm all seven are wired and that the public entry points are
 *      the ones the module advertises.
 *
 *   2. The component identity: name "direct", grpcomm v4 component.
 *
 *   3. The base globals: prte_grpcomm_base.context_id must start at
 *      UINT32_MAX -- group construct hands it out and *decrements*, so a
 *      wrong initial value would collide with bottom-up context ids.
 *
 *   4. The reference-counted signature/tracker/caddy classes: their
 *      constructors must establish the documented defaults (the group
 *      tracker in particular must open its grpinfo/endpts info-lists, and
 *      every counter must start at zero so a rollup cannot complete
 *      early), and their destructors must free their owned members --
 *      including the NULL case -- without crashing.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "constants.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/grpcomm/direct/grpcomm_direct.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * Every slot of the direct module's vtable must be non-NULL, and the
 * collective entry points must be the public functions the rest of the
 * tree calls.  init/finalize/fault_handler are file-static in the
 * component, so we can only assert they are wired, not their identity.
 */
static int test_module_contract(void)
{
    int failures = 0;
    prte_grpcomm_base_module_t *m = &prte_grpcomm_direct_module;

    CHECK("init set", NULL != m->init);
    CHECK("finalize set", NULL != m->finalize);
    CHECK("fault_handler set", NULL != m->fault_handler);
    CHECK("xcast set", NULL != m->xcast);
    CHECK("xcast_nb set", NULL != m->xcast_nb);
    CHECK("fence set", NULL != m->fence);
    CHECK("group set", NULL != m->group);

    /* the public entry points must be exactly what the module advertises */
    CHECK("xcast identity", m->xcast == prte_grpcomm_direct_xcast);
    CHECK("xcast_nb identity", m->xcast_nb == prte_grpcomm_direct_xcast_nb);
    CHECK("fence identity", m->fence == prte_grpcomm_direct_fence);
    CHECK("group identity", m->group == prte_grpcomm_direct_group);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_module_contract\n");
    }
    return failures;
}

/*
 * The direct component identifies itself as the grpcomm component named
 * "direct".  Selection depends on that name, so guard it.
 */
static int test_component_identity(void)
{
    int failures = 0;
    prte_grpcomm_base_component_t *c = &prte_mca_grpcomm_direct_component.super;

    CHECK("component name", 0 == strcmp(c->pmix_mca_component_name, "direct"));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_component_identity\n");
    }
    return failures;
}

/*
 * The group context-id pool counts DOWN from UINT32_MAX so DVM-assigned
 * ids never collide with ids handed out from the bottom of the range.
 * The static initializer in grpcomm_base_frame.c must establish that.
 */
static int test_base_context_id(void)
{
    int failures = 0;

    CHECK("context_id pool start", UINT32_MAX == prte_grpcomm_base.context_id);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_base_context_id\n");
    }
    return failures;
}

/*
 * Constructor defaults and destructor safety for every reference-counted
 * class the component defines.  The defaults are load-bearing: a rollup
 * completes when nreported == nexpected, so a non-zero counter default
 * would let a collective finish before anyone reported.
 */
static int test_classes(void)
{
    int failures = 0;

    /* fence signature: empty proc set */
    prte_grpcomm_direct_fence_signature_t *fsig =
        PMIX_NEW(prte_grpcomm_direct_fence_signature_t);
    CHECK("fence sig signature NULL", NULL == fsig->signature);
    CHECK("fence sig sz 0", 0 == fsig->sz);
    /* exercise the destructor's free(signature) path */
    fsig->sz = 2;
    fsig->signature = (pmix_proc_t *) malloc(fsig->sz * sizeof(pmix_proc_t));
    PMIX_RELEASE(fsig);

    /* group signature: NONE op, no members, all flags clear */
    prte_grpcomm_direct_group_signature_t *gsig =
        PMIX_NEW(prte_grpcomm_direct_group_signature_t);
    CHECK("grp sig op NONE", PMIX_GROUP_NONE == gsig->op);
    CHECK("grp sig groupID NULL", NULL == gsig->groupID);
    CHECK("grp sig assignID false", !gsig->assignID);
    CHECK("grp sig ctxid_assigned false", !gsig->ctxid_assigned);
    CHECK("grp sig members NULL", NULL == gsig->members);
    CHECK("grp sig nmembers 0", 0 == gsig->nmembers);
    CHECK("grp sig bootstrap 0", 0 == gsig->bootstrap);
    CHECK("grp sig follower false", !gsig->follower);
    CHECK("grp sig addmembers NULL", NULL == gsig->addmembers);
    CHECK("grp sig naddmembers 0", 0 == gsig->naddmembers);
    CHECK("grp sig final_order NULL", NULL == gsig->final_order);
    CHECK("grp sig nfinal 0", 0 == gsig->nfinal);
    /* exercise the destructor's free(groupID)/free(members)/free(addmembers) */
    gsig->groupID = strdup("test-group");
    gsig->nmembers = 1;
    gsig->members = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    gsig->naddmembers = 1;
    gsig->addmembers = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    PMIX_RELEASE(gsig);

    /* fence tracker: clean rollup counters, constructed bucket */
    prte_grpcomm_fence_t *fc = PMIX_NEW(prte_grpcomm_fence_t);
    CHECK("fence trk sig NULL", NULL == fc->sig);
    CHECK("fence trk status SUCCESS", PMIX_SUCCESS == fc->status);
    CHECK("fence trk dmns NULL", NULL == fc->dmns);
    CHECK("fence trk ndmns 0", 0 == fc->ndmns);
    CHECK("fence trk nexpected 0", 0 == fc->nexpected);
    CHECK("fence trk nreported 0", 0 == fc->nreported);
    CHECK("fence trk timeout 0", 0 == fc->timeout);
    CHECK("fence trk cbfunc NULL", NULL == fc->cbfunc);
    PMIX_RELEASE(fc);

    /* group tracker: every counter zero, both info-lists opened */
    prte_grpcomm_group_t *gc = PMIX_NEW(prte_grpcomm_group_t);
    CHECK("grp trk sig NULL", NULL == gc->sig);
    CHECK("grp trk status SUCCESS", PMIX_SUCCESS == gc->status);
    CHECK("grp trk dmns NULL", NULL == gc->dmns);
    CHECK("grp trk ndmns 0", 0 == gc->ndmns);
    CHECK("grp trk bootstrap false", !gc->bootstrap);
    CHECK("grp trk nexpected 0", 0 == gc->nexpected);
    CHECK("grp trk nreported 0", 0 == gc->nreported);
    CHECK("grp trk nleaders 0", 0 == gc->nleaders);
    CHECK("grp trk nleaders_reported 0", 0 == gc->nleaders_reported);
    CHECK("grp trk nfollowers 0", 0 == gc->nfollowers);
    CHECK("grp trk nfollowers_reported 0", 0 == gc->nfollowers_reported);
    CHECK("grp trk assignID false", !gc->assignID);
    CHECK("grp trk grpinfo opened", NULL != gc->grpinfo);
    CHECK("grp trk endpts opened", NULL != gc->endpts);
    PMIX_RELEASE(gc);

    /* fence caddy: all borrowed pointers NULL, sizes zero */
    prte_pmix_fence_caddy_t *fcd = PMIX_NEW(prte_pmix_fence_caddy_t);
    CHECK("fence caddy sig NULL", NULL == fcd->sig);
    CHECK("fence caddy buf NULL", NULL == fcd->buf);
    CHECK("fence caddy procs NULL", NULL == fcd->procs);
    CHECK("fence caddy nprocs 0", 0 == fcd->nprocs);
    CHECK("fence caddy data NULL", NULL == fcd->data);
    CHECK("fence caddy cbfunc NULL", NULL == fcd->cbfunc);
    PMIX_RELEASE(fcd);

    /* group caddy (lives in the framework header): NONE op, empty */
    prte_pmix_grp_caddy_t *gcd = PMIX_NEW(prte_pmix_grp_caddy_t);
    CHECK("grp caddy op NONE", PMIX_GROUP_NONE == gcd->op);
    CHECK("grp caddy grpid NULL", NULL == gcd->grpid);
    CHECK("grp caddy procs NULL", NULL == gcd->procs);
    CHECK("grp caddy nprocs 0", 0 == gcd->nprocs);
    CHECK("grp caddy directives NULL", NULL == gcd->directives);
    CHECK("grp caddy cbfunc NULL", NULL == gcd->cbfunc);
    /* exercise the destructor's free(grpid) path */
    gcd->grpid = strdup("test-group");
    PMIX_RELEASE(gcd);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_classes\n");
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

    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_base_context_id();
    failures += test_classes();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all grpcomm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d grpcomm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
