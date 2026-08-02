/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
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
 *
 *   5. The daemon-failure decisions.  Recovering a collective needs the
 *      routing tree, but *deciding* what to recover does not: it reads
 *      only the failed-daemon set and the job map, both of which can be
 *      stood up by hand here.  So the departed-member predicate and the
 *      fence fault handler's choice -- re-converge a fence that merely
 *      lost a message path, end one that lost a participant -- are pinned
 *      down away from a live DVM, with the release broadcast stubbed.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "constants.h"
#include "src/mca/base/pmix_base.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/class/pmix_bitmap.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"

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
/*
 * Ask the framework for a component by name, and for the module that
 * component hands *this* process.
 *
 * These are two different questions.  A component is present whenever the
 * framework opened it; whether it yields a module is up to its query,
 * which is free to decline - iof/prted, for one, answers only a daemon.
 * Naming the component's module symbol instead would assume it was linked
 * into libprrte, which is false with --enable-mca-dso: there the component
 * is a separate DSO and the symbol is not there to link against.
 */
static bool grpcomm_component_present(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;

    PMIX_LIST_FOREACH(cli, &prte_grpcomm_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 == strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            return true;
        }
    }
    return false;
}

static prte_grpcomm_base_module_t *grpcomm_module(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    int pri = 0;

    PMIX_LIST_FOREACH(cli, &prte_grpcomm_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 != strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL == cli->cli_component->pmix_mca_query_component) {
            return NULL;
        }
        if (PRTE_SUCCESS != cli->cli_component->pmix_mca_query_component(&mod, &pri)) {
            return NULL;
        }
        return (prte_grpcomm_base_module_t *) mod;
    }
    return NULL;
}

static int test_module_contract(void)
{
    int failures = 0;
    prte_grpcomm_base_module_t *m = grpcomm_module("direct");

    if (NULL == m) {
        fprintf(stdout, "SKIPPED test_module_contract (grpcomm/direct absent)\n");
        return 0;
    }

    CHECK("init set", NULL != m->init);
    CHECK("finalize set", NULL != m->finalize);
    CHECK("fault_handler set", NULL != m->fault_handler);
    CHECK("xcast set", NULL != m->xcast);
    CHECK("xcast_nb set", NULL != m->xcast_nb);
    CHECK("fence set", NULL != m->fence);
    CHECK("group set", NULL != m->group);

    /* The identity of each entry point can only be checked where the
     * component's own symbols are linkable - see PRTE_TEST_GRPCOMM_DIRECT
     * in this directory's Makefile.am. */
#if PRTE_TEST_GRPCOMM_DIRECT
    CHECK("xcast identity", m->xcast == prte_grpcomm_direct_xcast);
    CHECK("xcast_nb identity", m->xcast_nb == prte_grpcomm_direct_xcast_nb);
    CHECK("fence identity", m->fence == prte_grpcomm_direct_fence);
    CHECK("group identity", m->group == prte_grpcomm_direct_group);
#endif

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
    /* the framework must have opened a component by this name - asking it
     * for one is the assertion */
    if (!grpcomm_component_present("direct")) {
        fprintf(stdout, "SKIPPED test_component_identity"
                        " (grpcomm/direct not loadable from the build tree)\n");
        return 0;
    }
    CHECK("component present", grpcomm_component_present("direct"));

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

    /* Everything from here to the group caddy constructs a class the
     * component defines.  Those classes are reachable only where the
     * component's symbols are linkable - not from a DSO build, and not
     * once the library hides its internals - so they are compiled in
     * only when this directory's Makefile.am says so. */
#if PRTE_TEST_GRPCOMM_DIRECT
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
    /* a group is not fault tolerant unless somebody asks for it */
    CHECK("grp sig ft_collective false", !gsig->ft_collective);
    /* exercise the destructor's free of groupID/members/addmembers and the
     * final order, which the destructor used to leak */
    gsig->groupID = strdup("test-group");
    gsig->nmembers = 1;
    gsig->members = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    gsig->naddmembers = 1;
    gsig->addmembers = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    gsig->nfinal = 1;
    PMIX_PROC_CREATE(gsig->final_order, gsig->nfinal);
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
    CHECK("grp trk grpinfo opened", NULL != gc->grpinfo);
    CHECK("grp trk endpts opened", NULL != gc->endpts);
    /* the rollup must start with nothing reported from anywhere: a set slot
     * bit or a raised self_reported would silently subtract a contribution
     * from what the collective waits for */
    CHECK("grp trk no slots reported",
          0 == pmix_bitmap_num_set_bits(&gc->reported_slots,
                                        pmix_bitmap_size(&gc->reported_slots)));
    CHECK("grp trk self_reported false", !gc->self_reported);
    CHECK("grp trk converged false", !gc->converged);
    CHECK("grp trk aborting false", !gc->aborting);
    CHECK("grp trk timeout 0", 0 == gc->timeout);
    CHECK("grp trk tev inactive", !gc->tev_active);
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

#endif

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

/* The departed-member predicate decides whether a construct that lost a
 * daemon may complete on the survivors, so it is worth pinning down away from
 * a live DVM.  It reads only the failed-daemon set and the job data, both of
 * which can be stood up by hand here. */
static int test_member_departed(void)
{
    int failures = 0;
    pmix_proc_t member;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_node_t *node;
    prte_proc_t *dmn;

    /* prte_init_util() opens neither the RML nor the job pool, so the two
     * globals this predicate reads are still raw static storage - stand them
     * up by hand, as the ras and runtime unit tests do */
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.failed_dmns, 8);
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    }

    /* with nothing failed, nobody has departed - and this is the path every
     * fault-free collective takes, so it must not even consult the job map */
    PMIX_LOAD_PROCID(&member, "no-such-nspace", 0);
    CHECK("departed: clean failed set says no",
          !prte_grpcomm_direct_proc_departed(&member));

    /* mark daemon 1 as failed and hang a job off daemons 0 and 1 */
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 1);

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "ft-test-nspace");
    prte_set_job_data_object(jdata);

    for (pmix_rank_t r = 0; r < 2; r++) {
        node = PMIX_NEW(prte_node_t);
        dmn = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&dmn->name, PRTE_PROC_MY_NAME->nspace, r);
        node->daemon = dmn;
        proc = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&proc->name, jdata->nspace, r);
        proc->node = node;
        pmix_pointer_array_set_item(jdata->procs, r, proc);
    }
    jdata->num_procs = 2;

    /* rank 0 lives on the surviving daemon, rank 1 on the failed one */
    PMIX_LOAD_PROCID(&member, jdata->nspace, 0);
    CHECK("departed: member on a live daemon survives",
          !prte_grpcomm_direct_proc_departed(&member));
    PMIX_LOAD_PROCID(&member, jdata->nspace, 1);
    CHECK("departed: member on a failed daemon departed",
          prte_grpcomm_direct_proc_departed(&member));

    /* a wildcard names a namespace, not a process, so it is never resolvable
     * to a single daemon and must not be reported as departed */
    PMIX_LOAD_PROCID(&member, jdata->nspace, PMIX_RANK_WILDCARD);
    CHECK("departed: wildcard is not a departed member",
          !prte_grpcomm_direct_proc_departed(&member));

    /* an unresolvable member is treated as departed rather than as an error -
     * losing the mapping is what a node being torn down looks like */
    PMIX_LOAD_PROCID(&member, "ft-test-missing", 0);
    CHECK("departed: unresolvable member counts as departed",
          prte_grpcomm_direct_proc_departed(&member));

    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
    return failures;
}

#if PRTE_TEST_GRPCOMM_DIRECT
/* Stands in for the down-tree release the controller broadcasts to end a
 * fence.  There is no routing tree here to broadcast over, and the abort is
 * defined by what it puts on the wire anyway: the signature of the fence
 * being ended, and the status its participants are to be completed with. */
static int fence_xcast_calls;
static prte_rml_tag_t fence_xcast_tag;
static pmix_status_t fence_xcast_status;
static char fence_xcast_nspace[PMIX_MAX_NSLEN + 1];

static int stub_xcast(prte_rml_tag_t tag, pmix_data_buffer_t *msg)
{
    size_t sz = 0;
    pmix_proc_t *procs = NULL;
    pmix_status_t st = PMIX_SUCCESS;
    int32_t cnt;

    fence_xcast_calls++;
    fence_xcast_tag = tag;
    fence_xcast_status = PMIX_SUCCESS;
    fence_xcast_nspace[0] = '\0';

    /* read it back the way fence_release() does - signature, then status */
    cnt = 1;
    if (PMIX_SUCCESS != PMIx_Data_unpack(NULL, msg, &sz, &cnt, PMIX_SIZE)) {
        return PRTE_SUCCESS;
    }
    if (0 < sz) {
        PMIX_PROC_CREATE(procs, sz);
        cnt = (int32_t) sz;
        if (PMIX_SUCCESS == PMIx_Data_unpack(NULL, msg, procs, &cnt, PMIX_PROC)) {
            PMIX_LOAD_NSPACE(fence_xcast_nspace, procs[0].nspace);
        }
        PMIX_PROC_FREE(procs, sz);
    }
    cnt = 1;
    if (PMIX_SUCCESS == PMIx_Data_unpack(NULL, msg, &st, &cnt, PMIX_INT32)) {
        fence_xcast_status = st;
    }
    return PRTE_SUCCESS;
}

/* Hang a job of nprocs off daemons 0..nprocs-1, one rank apiece, so a
 * signature naming this namespace resolves through the job map. */
static void build_job(const char *nspace, pmix_rank_t nprocs)
{
    prte_job_t *jdata;
    prte_node_t *node;
    prte_proc_t *proc, *dmn;
    pmix_rank_t r;

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, nspace);
    prte_set_job_data_object(jdata);

    for (r = 0; r < nprocs; r++) {
        node = PMIX_NEW(prte_node_t);
        dmn = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&dmn->name, PRTE_PROC_MY_NAME->nspace, r);
        node->daemon = dmn;
        proc = PMIX_NEW(prte_proc_t);
        PMIX_LOAD_PROCID(&proc->name, jdata->nspace, r);
        proc->node = node;
        pmix_pointer_array_set_item(jdata->procs, r, proc);
    }
    jdata->num_procs = nprocs;
}

/* Append a tracker standing in for a fence rolling up right now over the
 * whole of nspace -- the wildcard is how a fence signature is usually
 * spelled, and expanding it is part of what is under test here. */
static prte_grpcomm_fence_t *add_fence_op(const char *nspace)
{
    prte_grpcomm_fence_t *coll = PMIX_NEW(prte_grpcomm_fence_t);

    coll->sig = PMIX_NEW(prte_grpcomm_direct_fence_signature_t);
    coll->sig->sz = 1;
    PMIX_PROC_CREATE(coll->sig->signature, 1);
    PMIX_LOAD_PROCID(&coll->sig->signature[0], nspace, PMIX_RANK_WILDCARD);
    coll->nexpected = 2;
    coll->nreported = 1;
    pmix_list_append(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);
    return coll;
}
#endif

/*
 * A daemon failure used to end every fence in flight, and a fence ends its
 * participants -- so the loss of a daemon anywhere was fatal to whatever
 * else happened to be fencing at the time, and fences run constantly.  The
 * handler must now ask whether the two had anything to do with each other:
 * a fence whose participants all survive lost only a message path and
 * re-converges over the repaired tree at the new recovery epoch, while one
 * that genuinely lost a participant cannot produce its answer and ends --
 * itself, with PMIX_ERR_LOST_CONNECTION, leaving the DVM standing.
 *
 * The decision is the controller's, made once on the global-scope pass; a
 * revival is the exception, ending everything in flight because its
 * broadcast is forward-first and so cannot order a restart.
 */
static int test_fence_fault_handler(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_DIRECT
    prte_rml_recovery_status_t status;
    prte_grpcomm_fence_t *bystander, *doomed;
    prte_proc_type_t save_type;

    /* init() is what constructs the tracker list, and it takes a selected
     * module and a live RML - stand the list up by hand instead */
    PMIX_CONSTRUCT(&prte_mca_grpcomm_direct_component.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.failed_dmns, 8);
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    }
    prte_grpcomm.xcast = stub_xcast;
    fence_xcast_calls = 0;

    /* daemon 1 has failed; one job lives entirely on the survivor, the
     * other straddles both */
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 1);
    build_job("fence-live-nspace", 1);
    build_job("fence-lost-nspace", 2);

    /* nothing in flight: the handler must not reach for anything */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: no ops, nothing broadcast", 0 == fence_xcast_calls);

    bystander = add_fence_op("fence-live-nspace");
    doomed = add_fence_op("fence-lost-nspace");

    /* the local pass of a death belongs to the RML's own tree repair - the
     * collectives act on the global notice, which reaches every daemon in
     * one order, so acting here would end a fence on the daemons nearest
     * the failure while their peers waited out a re-convergence */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_LOCAL;
    PMIx_Data_array_construct(&status.failed_ranks, 1, PMIX_PROC_RANK);
    ((pmix_rank_t *) status.failed_ranks.array)[0] = 1;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: local pass of a death broadcasts nothing", 0 == fence_xcast_calls);
    CHECK("fault: local pass leaves the doomed fence alone", !doomed->aborting);

    /* only the controller decides: a fence's participants are completed by
     * the release it broadcasts, so a daemon acting on its own would end
     * its own clients and leave everyone else's blocked */
    save_type = prte_process_info.proc_type;
    prte_process_info.proc_type = PRTE_PROC_DAEMON;
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    prte_process_info.proc_type = save_type;
    CHECK("fault: a non-controller broadcasts nothing", 0 == fence_xcast_calls);
    CHECK("fault: a non-controller ends nothing", !doomed->aborting);

    /* the controller's global pass: exactly the fence that lost a
     * participant is ended, and the bystander is left to re-converge */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: exactly one release broadcast", 1 == fence_xcast_calls);
    CHECK("fault: released on the fence-release tag",
          PRTE_RML_TAG_FENCE_RELEASE == fence_xcast_tag);
    CHECK("fault: the fence that lost a participant is the one ended",
          0 == strcmp("fence-lost-nspace", fence_xcast_nspace));
    CHECK("fault: ended with a lost connection",
          PMIX_ERR_LOST_CONNECTION == fence_xcast_status);
    CHECK("fault: the doomed fence is marked aborting", doomed->aborting);
    CHECK("fault: the bystander fence is untouched", !bystander->aborting);
    /* the tracker is not dropped here - it goes when its own release comes
     * back around, which is also what completes the local participants */
    CHECK("fault: both trackers still in flight",
          2 == pmix_list_get_size(&prte_mca_grpcomm_direct_component.fence_ops));

    /* a second failure must not re-broadcast a release already sent */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: an aborting fence is not ended twice", 1 == fence_xcast_calls);

    /* a revival: local scope with nothing newly failed.  It reshapes the
     * tree as a death does, but rides a forward-first broadcast, so it
     * cannot order a restart - everything in flight ends, bystander
     * included */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_LOCAL;
    prte_grpcomm_direct_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: a revival ends the bystander too", 2 == fence_xcast_calls);
    CHECK("fault: revival released the bystander",
          0 == strcmp("fence-live-nspace", fence_xcast_nspace));
    CHECK("fault: bystander now aborting", bystander->aborting);

    prte_grpcomm.xcast = NULL;
    PMIX_LIST_DESTRUCT(&prte_mca_grpcomm_direct_component.fence_ops);
    PMIX_CONSTRUCT(&prte_mca_grpcomm_direct_component.fence_ops, pmix_list_t);
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fence_fault_handler\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;
    pmix_status_t prc;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    /* the fence abort under test packs a release buffer, and PMIx_Data_pack
     * refuses to run until PMIx itself is up.  A daemon reaches that state
     * through PMIx_server_init, so do the same */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        prte_finalize();
        return 1;
    }

    /* the module and component tests ask the framework for their subject,
     * so it has to be open before they run */
    rc = pmix_mca_base_framework_open(&prte_grpcomm_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "grpcomm framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_base_context_id();
    failures += test_classes();
#if PRTE_TEST_GRPCOMM_DIRECT
    failures += test_member_departed();
#endif
    failures += test_fence_fault_handler();

    (void) pmix_mca_base_framework_close(&prte_grpcomm_base_framework);
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all grpcomm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d grpcomm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
