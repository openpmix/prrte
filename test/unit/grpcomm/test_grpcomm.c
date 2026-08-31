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
 * What *can* be exercised in isolation is the structural contract and the
 * constructor/destructor invariants that the collective code relies on:
 *
 *   1. The release path.  grpcomm hands a completed collective back through
 *      prte_grpcomm_release_bcast, which must default to the real broadcast.
 *      Nothing else establishes that, and a NULL or wrongly-aimed default
 *      would silently drop every release a controller emits -- a DVM-wide
 *      hang with no diagnostic.
 *
 *   3. The globals: prte_grpcomm_globals.context_id must start at
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

#include "src/grpcomm/grpcomm.h"
#include "src/grpcomm/grpcomm_internal.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * grpcomm releases a completed collective through prte_grpcomm_release_bcast,
 * which must default to the real broadcast.  Nothing else establishes that,
 * and a NULL or wrongly-aimed default would silently drop every release a
 * controller emits - a DVM-wide hang with no diagnostic.
 */
static int test_release_bcast_default(void)
{
    int failures = 0;

    CHECK("release path defaults to xcast",
          prte_grpcomm_release_bcast == prte_grpcomm_xcast);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_release_bcast_default\n");
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

    CHECK("context_id pool start", UINT32_MAX == prte_grpcomm_globals.context_id);

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
#if PRTE_TEST_GRPCOMM_INTERNALS
    /* fence signature: empty proc set */
    prte_grpcomm_fence_signature_t *fsig =
        PMIX_NEW(prte_grpcomm_fence_signature_t);
    CHECK("fence sig signature NULL", NULL == fsig->signature);
    CHECK("fence sig sz 0", 0 == fsig->sz);
    /* exercise the destructor's free(signature) path */
    fsig->sz = 2;
    fsig->signature = (pmix_proc_t *) malloc(fsig->sz * sizeof(pmix_proc_t));
    PMIX_RELEASE(fsig);

    /* group signature: NONE op, no members, all flags clear */
    prte_grpcomm_group_signature_t *gsig =
        PMIX_NEW(prte_grpcomm_group_signature_t);
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
    /* a fresh tracker has not been told which collective it is in - and
     * UNKNOWN has to be distinct from "barrier", or the first contribution
     * to arrive would find its operation already decided for it */
    CHECK("fence trk op UNKNOWN", PRTE_GRPCOMM_FENCE_OP_UNKNOWN == fc->op);
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
          !prte_grpcomm_proc_departed(&member));

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
          !prte_grpcomm_proc_departed(&member));
    PMIX_LOAD_PROCID(&member, jdata->nspace, 1);
    CHECK("departed: member on a failed daemon departed",
          prte_grpcomm_proc_departed(&member));

    /* a wildcard names a namespace, not a process, so it is never resolvable
     * to a single daemon and must not be reported as departed */
    PMIX_LOAD_PROCID(&member, jdata->nspace, PMIX_RANK_WILDCARD);
    CHECK("departed: wildcard is not a departed member",
          !prte_grpcomm_proc_departed(&member));

    /* an unresolvable member is treated as departed rather than as an error -
     * losing the mapping is what a node being torn down looks like */
    PMIX_LOAD_PROCID(&member, "ft-test-missing", 0);
    CHECK("departed: unresolvable member counts as departed",
          prte_grpcomm_proc_departed(&member));

    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
    return failures;
}

/*
 * A group operation's directives belong to the PMIx server that handed
 * them to us: it keeps them alive until the operation completes and then
 * frees them, arrays and all.  The signature we build from them has the
 * opposite ownership -- its destructor frees what it holds -- so every
 * array a directive carries has to be copied into it.  A signature that
 * merely pointed at the caller's array would free it out from under PMIx
 * and leave PMIx to free it a second time, which is why this test asserts
 * on the pointers and not just on the contents.
 */
static int test_group_directives(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_INTERNALS
    prte_grpcomm_group_signature_t sig;
    pmix_info_t *dirs;
    pmix_data_array_t darray;
    pmix_proc_t *p, *dirmembers, *dirorder;
    void *grpinfo, *endpts;
    pmix_status_t rc, st = PMIX_SUCCESS;
    int timeout = 0;
    size_t bootstrap = 3;
    int tmo = 42;
    size_t ndirs = 5;

    PMIX_INFO_CREATE(dirs, ndirs);
    PMIX_INFO_LOAD(&dirs[0], PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_TIMEOUT, &tmo, PMIX_INT);
    PMIX_INFO_LOAD(&dirs[2], PMIX_GROUP_BOOTSTRAP, &bootstrap, PMIX_SIZE);

    /* PMIX_INFO_LOAD copies the array into the directive, so the directive
     * ends up owning one of its own - exactly as it does off the wire */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_PROC);
    p = (pmix_proc_t *) darray.array;
    PMIX_LOAD_PROCID(&p[0], "grp-added", 0);
    PMIX_LOAD_PROCID(&p[1], "grp-added", 1);
    PMIX_INFO_LOAD(&dirs[3], PMIX_GROUP_ADD_MEMBERS, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 1, PMIX_PROC);
    p = (pmix_proc_t *) darray.array;
    PMIX_LOAD_PROCID(&p[0], "grp-added", 1);
    PMIX_INFO_LOAD(&dirs[4], PMIX_GROUP_FINAL_MEMBERSHIP_ORDER, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    dirmembers = (pmix_proc_t *) dirs[3].value.data.darray->array;
    dirorder = (pmix_proc_t *) dirs[4].value.data.darray->array;

    PMIX_CONSTRUCT(&sig, prte_grpcomm_group_signature_t);
    grpinfo = PMIx_Info_list_start();
    endpts = PMIx_Info_list_start();
    rc = prte_grpcomm_group_parse_directives(&sig, dirs, ndirs,
                                                    &timeout, &st, grpinfo, endpts);
    CHECK("directives: parsed", PMIX_SUCCESS == rc);
    CHECK("directives: context id requested", sig.assignID);
    CHECK("directives: timeout collected", 42 == timeout);
    CHECK("directives: bootstrap count", 3 == sig.bootstrap);

    CHECK("directives: add-members counted", 2 == sig.naddmembers);
    CHECK("directives: add-members copied, not borrowed",
          NULL != sig.addmembers && sig.addmembers != dirmembers);
    CHECK("directives: add-members content",
          NULL != sig.addmembers &&
          0 == memcmp(sig.addmembers, dirmembers, 2 * sizeof(pmix_proc_t)));

    CHECK("directives: final order counted", 1 == sig.nfinal);
    CHECK("directives: final order copied, not borrowed",
          NULL != sig.final_order && sig.final_order != dirorder);
    CHECK("directives: final order content",
          NULL != sig.final_order &&
          0 == memcmp(sig.final_order, dirorder, sizeof(pmix_proc_t)));

    /* the destructor frees everything the signature owns. The directives
     * must come through it untouched, because their owner frees them next -
     * and would be freeing them a second time if we had borrowed */
    PMIX_DESTRUCT(&sig);
    CHECK("directives: add-member array survives the signature",
          dirmembers == (pmix_proc_t *) dirs[3].value.data.darray->array &&
          2 == dirs[3].value.data.darray->size &&
          0 == strcmp("grp-added", dirmembers[0].nspace));
    CHECK("directives: final-order array survives the signature",
          dirorder == (pmix_proc_t *) dirs[4].value.data.darray->array &&
          1 == dirs[4].value.data.darray->size);
    PMIx_Info_list_release(grpinfo);
    PMIx_Info_list_release(endpts);
    PMIX_INFO_FREE(dirs, ndirs);

    /* a directive whose array is missing describes no members rather than
     * something to reach into - these arrive off the wire too */
    PMIX_INFO_CREATE(dirs, 1);
    PMIX_INFO_LOAD(&dirs[0], PMIX_GROUP_ADD_MEMBERS, NULL, PMIX_BOOL);
    PMIX_CONSTRUCT(&sig, prte_grpcomm_group_signature_t);
    grpinfo = PMIx_Info_list_start();
    endpts = PMIx_Info_list_start();
    rc = prte_grpcomm_group_parse_directives(&sig, dirs, 1,
                                                    &timeout, &st, grpinfo, endpts);
    CHECK("directives: an empty add-members is not an error", PMIX_SUCCESS == rc);
    CHECK("directives: an empty add-members adds nobody",
          NULL == sig.addmembers && 0 == sig.naddmembers);
    PMIX_DESTRUCT(&sig);
    PMIx_Info_list_release(grpinfo);
    PMIx_Info_list_release(endpts);
    PMIX_INFO_FREE(dirs, 1);

    /* an array of the wrong element type is refused rather than copied. The
     * copy is sized in pmix_proc_t, so an array of anything smaller would be
     * read past its end by a factor - and this array is whatever a client
     * chose to send */
    PMIX_INFO_CREATE(dirs, 1);
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 4, PMIX_BYTE);
    PMIX_INFO_LOAD(&dirs[0], PMIX_GROUP_ADD_MEMBERS, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_CONSTRUCT(&sig, prte_grpcomm_group_signature_t);
    grpinfo = PMIx_Info_list_start();
    endpts = PMIx_Info_list_start();
    rc = prte_grpcomm_group_parse_directives(&sig, dirs, 1,
                                                    &timeout, &st, grpinfo, endpts);
    CHECK("directives: an add-members array of the wrong type is refused",
          PMIX_SUCCESS != rc);
    CHECK("directives: ...and nothing was copied out of it",
          NULL == sig.addmembers && 0 == sig.naddmembers);
    PMIX_DESTRUCT(&sig);
    PMIx_Info_list_release(grpinfo);
    PMIx_Info_list_release(endpts);
    PMIX_INFO_FREE(dirs, 1);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_group_directives\n");
    }
    return failures;
}

/*
 * Building a fence tracker is where a fence goes wrong long before any
 * message moves, because the tracker is what says how many contributions
 * the rollup is waiting for.  Three answers have to be right:
 *
 *   - a signature naming the daemon job is "every daemon", which
 *     create_dmns() reports as a count with no array at all.  Both readers
 *     of that pair used to index the array anyway;
 *   - a signature naming nobody -- which is what a truncated message
 *     unpacks to -- resolves to no daemons, not to every daemon;
 *   - a signature that cannot be resolved yields no tracker AND leaves
 *     none behind.  A half-built one left on the list is worse than none,
 *     because the next fence of that signature finds it and answers from a
 *     rollup that expects nothing.
 */
static int test_fence_tracker(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_INTERNALS
    prte_grpcomm_fence_signature_t sig;
    prte_grpcomm_fence_t *coll, *again;
    int32_t save_daemons;
    pmix_nspace_t save_nspace;

    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.failed_dmns, 8);
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    }
    save_daemons = prte_process_info.num_daemons;
    prte_process_info.num_daemons = 4;
    /* "is this the daemon job?" is asked with PMIX_CHECK_NSPACE, which
     * answers yes to anything when either side is empty - so this test says
     * nothing at all unless we have a namespace of our own, as a daemon
     * always does */
    PMIX_LOAD_NSPACE(save_nspace, PRTE_PROC_MY_NAME->nspace);
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-unit-dvm");

    /* a fence over the daemon job itself: every daemon takes part, and we
     * are a leaf here, so our own contribution is the only one */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_fence_signature_t);
    sig.sz = 1;
    PMIX_PROC_CREATE(sig.signature, 1);
    PMIX_LOAD_PROCID(&sig.signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    coll = prte_grpcomm_fence_get_tracker(&sig, true);
    CHECK("tracker: a daemon-job fence resolves", NULL != coll);
    if (NULL != coll) {
        CHECK("tracker: every daemon means all of them, with no array",
              NULL == coll->dmns && 4 == coll->ndmns);
        CHECK("tracker: expects our children plus ourselves",
              (size_t) prte_rml_base.n_children + 1 == coll->nexpected);
    }
    /* the same signature must find that tracker rather than build a second */
    again = prte_grpcomm_fence_get_tracker(&sig, true);
    CHECK("tracker: the same signature returns the same tracker", again == coll);
    CHECK("tracker: and does not add another",
          1 == pmix_list_get_size(&prte_grpcomm_globals.fence_ops));
    PMIX_DESTRUCT(&sig);

    /* a signature naming nobody is refused rather than read as the "all
     * daemons" case above, which is what an empty nspace would otherwise
     * match */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_fence_signature_t);
    coll = prte_grpcomm_fence_get_tracker(&sig, true);
    CHECK("tracker: an empty signature is refused", NULL == coll);
    CHECK("tracker: and builds nothing",
          1 == pmix_list_get_size(&prte_grpcomm_globals.fence_ops));
    PMIX_DESTRUCT(&sig);

    /* a signature we cannot resolve at all - no such job, and nothing has
     * failed, so this is a genuine error rather than a torn-down node */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_fence_signature_t);
    sig.sz = 1;
    PMIX_PROC_CREATE(sig.signature, 1);
    PMIX_LOAD_PROCID(&sig.signature[0], "fence-nosuchjob", 0);
    coll = prte_grpcomm_fence_get_tracker(&sig, true);
    CHECK("tracker: an unresolvable signature yields no tracker", NULL == coll);
    CHECK("tracker: and leaves no wreckage on the list",
          1 == pmix_list_get_size(&prte_grpcomm_globals.fence_ops));
    /* ...so asking again is a fresh attempt, not a hand-back of the wreck */
    coll = prte_grpcomm_fence_get_tracker(&sig, false);
    CHECK("tracker: nothing to find on a second look", NULL == coll);
    PMIX_DESTRUCT(&sig);

    prte_process_info.num_daemons = save_daemons;
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, save_nspace);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.fence_ops);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fence_tracker\n");
    }
    return failures;
}

#if PRTE_TEST_GRPCOMM_INTERNALS
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

    coll->sig = PMIX_NEW(prte_grpcomm_fence_signature_t);
    coll->sig->sz = 1;
    PMIX_PROC_CREATE(coll->sig->signature, 1);
    PMIX_LOAD_PROCID(&coll->sig->signature[0], nspace, PMIX_RANK_WILDCARD);
    coll->nexpected = 2;
    coll->nreported = 1;
    pmix_list_append(&prte_grpcomm_globals.fence_ops, &coll->super);
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
#if PRTE_TEST_GRPCOMM_INTERNALS
    prte_rml_recovery_status_t status;
    prte_grpcomm_fence_t *bystander, *doomed;
    prte_proc_type_t save_type;

    /* init() is what constructs the tracker list, and it takes a selected
     * module and a live RML - stand the list up by hand instead */
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.failed_dmns, 8);
    if (NULL == prte_job_data) {
        prte_job_data = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    }
    prte_grpcomm_release_bcast = stub_xcast;
    fence_xcast_calls = 0;

    /* daemon 1 has failed; one job lives entirely on the survivor, the
     * other straddles both */
    pmix_bitmap_set_bit(&prte_rml_base.failed_dmns, 1);
    build_job("fence-live-nspace", 1);
    build_job("fence-lost-nspace", 2);

    /* nothing in flight: the handler must not reach for anything */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_fence_fault_handler(&status);
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
    prte_grpcomm_fence_fault_handler(&status);
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
    prte_grpcomm_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    prte_process_info.proc_type = save_type;
    CHECK("fault: a non-controller broadcasts nothing", 0 == fence_xcast_calls);
    CHECK("fault: a non-controller ends nothing", !doomed->aborting);

    /* the controller's global pass: exactly the fence that lost a
     * participant is ended, and the bystander is left to re-converge */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_fence_fault_handler(&status);
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
          2 == pmix_list_get_size(&prte_grpcomm_globals.fence_ops));

    /* a second failure must not re-broadcast a release already sent */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    prte_grpcomm_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: an aborting fence is not ended twice", 1 == fence_xcast_calls);

    /* a revival: local scope with nothing newly failed.  It reshapes the
     * tree as a death does, but rides a forward-first broadcast, so it
     * cannot order a restart - everything in flight ends, bystander
     * included */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_LOCAL;
    prte_grpcomm_fence_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("fault: a revival ends the bystander too", 2 == fence_xcast_calls);
    CHECK("fault: revival released the bystander",
          0 == strcmp("fence-live-nspace", fence_xcast_nspace));
    CHECK("fault: bystander now aborting", bystander->aborting);

    prte_grpcomm_release_bcast = prte_grpcomm_xcast;
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.fence_ops);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_ops, pmix_list_t);
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fence_fault_handler\n");
    }
    return failures;
}

/*
 * The recovery epoch tells one round of a restarted collective from the
 * next, and it is issued by the DVM master and carried on the global failure
 * notice as an absolute value.  The alternative - each daemon counting the
 * notices it has seen - makes the number a function of what a daemon
 * received, so a daemon that missed one is a step behind for the rest of the
 * DVM's life and has every contribution it offers dropped as stale.  A
 * daemon launched into a DVM that has already recovered (an elastic grow
 * after a shrink) has missed all of them, which is why the value has to be
 * something it can simply be told.
 */
static int test_recovery_epoch(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_INTERNALS
    prte_rml_recovery_status_t status;
    uint32_t first, second;

    /* the master issues the numbers, and they increase strictly.  They do
     * not wait on the master applying them: its own epoch does not move
     * until its broadcast is relayed back to it, and a second failure
     * detected inside that window must not reissue the number the first
     * notice is already carrying - that would collapse two restarts into
     * one, which is a hang rather than a wrong answer */
    first = prte_grpcomm_issue_epoch();
    second = prte_grpcomm_issue_epoch();
    CHECK("epoch: issued values increase", second > first);
    CHECK("epoch: issuing does not move the applied epoch",
          0 == prte_grpcomm_current_epoch());

    /* the state the fault handler walks.  Nothing is in flight, so every
     * restart below is a walk over empty lists - what is under test is the
     * counter, not what it restarts */
    PMIX_CONSTRUCT(&prte_grpcomm_globals.xcast_ops, prte_grpcomm_xcast_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.group_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.completed_group_ops, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.failed_dmns, pmix_bitmap_t);
    pmix_bitmap_init(&prte_rml_base.failed_dmns, 8);

    /* the local pass never moves the epoch: the restart has to be
     * simultaneous across the DVM, and only the global notice is */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_LOCAL;
    status.epoch = 4;
    prte_grpcomm_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("epoch: the local pass does not move the epoch",
          0 == prte_grpcomm_current_epoch());

    /* the global notice carries the epoch, and it is adopted as given -
     * not incremented.  A daemon seeing its first notice at 7 must land on
     * 7, exactly where a daemon that saw all six before it lands */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    status.epoch = 7;
    prte_grpcomm_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("epoch: a global notice is adopted as given",
          7 == prte_grpcomm_current_epoch());

    /* replaying it changes nothing.  This is what makes the value safe to
     * send by more than one route - the WIREUP broadcast a joining daemon
     * receives carries it too, and the two can arrive in either order */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    status.epoch = 7;
    prte_grpcomm_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("epoch: a replayed notice does not advance it",
          7 == prte_grpcomm_current_epoch());

    /* and an older one cannot walk it back */
    PMIX_CONSTRUCT(&status, prte_rml_recovery_status_t);
    status.scope = PRTE_RML_FAULT_SCOPE_GLOBAL;
    status.epoch = 3;
    prte_grpcomm_fault_handler(&status);
    PMIX_DESTRUCT(&status);
    CHECK("epoch: an older notice does not walk it back",
          7 == prte_grpcomm_current_epoch());

    /* seeding a daemon that joined late - what the WIREUP broadcast does -
     * is the same operation, and lands it where the DVM already is */
    prte_grpcomm_advance_epoch(9);
    CHECK("epoch: a seed carries a late joiner forward",
          9 == prte_grpcomm_current_epoch());

    prte_grpcomm_globals.recovery_epoch = 0;
    PMIX_DESTRUCT(&prte_grpcomm_globals.xcast_ops);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.group_ops);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.group_ops, pmix_list_t);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.completed_group_ops);
    PMIX_DESTRUCT(&prte_rml_base.failed_dmns);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_recovery_epoch\n");
    }
    return failures;
}

/*
 * Which operation a fence runs is named by PMIX_COLLECT_DATA and by nothing
 * else.  Both halves of that are pinned here: the classifier, which reads the
 * directive out of a caller's info array, and the merge, which is how a daemon
 * decides whether an arriving contribution agrees with what it already knows.
 *
 * Note what the classifier's signature does not take: a payload.  A fence's
 * operation cannot be inferred from whether a participant contributed bytes,
 * because the bytes vary from daemon to daemon while the operation must not -
 * a participant with nothing to publish is fully a participant in an
 * allgather, and since PMIx learned to contribute only what changed that is
 * the ordinary case rather than a corner one.  Deriving the operation from the
 * payload would have daemons disagree about which collective they are running,
 * and a fence has no originator to settle it.
 */
static int test_fence_operation(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_INTERNALS
    pmix_info_t info[2];
    prte_grpcomm_fence_t *coll;
    bool flag;
    int tmo = 30;

    /* No directives at all.  The caller asked to synchronize and said nothing
     * else, and that is a barrier - the absence of the flag has to mean the
     * same as the flag being false, because a caller wanting a plain barrier
     * has no reason to pass anything. */
    CHECK("op: no directives is a barrier",
          PRTE_GRPCOMM_FENCE_OP_BARRIER == prte_grpcomm_fence_op_from_info(NULL, 0));

    flag = true;
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    CHECK("op: collect true is an allgather",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == prte_grpcomm_fence_op_from_info(info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);

    flag = false;
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    CHECK("op: collect false is a barrier",
          PRTE_GRPCOMM_FENCE_OP_BARRIER == prte_grpcomm_fence_op_from_info(info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* Present with no value.  PMIx's convention is that the bare presence of
     * an attribute means true, and this has to read it the same way PMIx's own
     * fence does or a caller that set the flag that way would silently be
     * given a barrier and then find its data missing. */
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, NULL, PMIX_BOOL);
    CHECK("op: a valueless collect flag is still true",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == prte_grpcomm_fence_op_from_info(info, 1));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* An unrelated directive names no operation and must not be mistaken for
     * one; and the flag is still found when it is not the first entry. */
    PMIX_INFO_LOAD(&info[0], PMIX_TIMEOUT, &tmo, PMIX_INT);
    CHECK("op: an unrelated directive leaves it a barrier",
          PRTE_GRPCOMM_FENCE_OP_BARRIER == prte_grpcomm_fence_op_from_info(info, 1));
    flag = true;
    PMIX_INFO_LOAD(&info[1], PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    CHECK("op: the flag is found past the first entry",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == prte_grpcomm_fence_op_from_info(info, 2));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* The merge.  A tracker that has heard nothing adopts; one that has heard
     * something requires agreement. */
    coll = PMIX_NEW(prte_grpcomm_fence_t);
    CHECK("merge: an unknown tracker adopts",
          prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_ALLGATHER));
    CHECK("merge: and records what it adopted",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == coll->op);
    CHECK("merge: the same answer again agrees",
          prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_ALLGATHER));
    /* An arrival that names no operation tells us nothing, so it cannot
     * disagree with anything either - it must not be read as a barrier. */
    CHECK("merge: an unknown arrival cannot disagree",
          prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_UNKNOWN));
    CHECK("merge: and does not disturb what was recorded",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == coll->op);
    /* The disagreement this whole mechanism exists to catch.  It has to be
     * caught here because it is otherwise invisible: a barrier now puts
     * nothing on the wire, so PMIx's own per-blob collect-flag comparison
     * never sees the two answers together. */
    CHECK("merge: the opposite operation disagrees",
          !prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_BARRIER));
    CHECK("merge: and a disagreement changes nothing",
          PRTE_GRPCOMM_FENCE_OP_ALLGATHER == coll->op);
    PMIX_RELEASE(coll);

    /* the same, adopting the other way round, so neither operation is
     * privileged by the order the tests happen to run in */
    coll = PMIX_NEW(prte_grpcomm_fence_t);
    CHECK("merge: an unknown tracker adopts a barrier too",
          prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_BARRIER));
    CHECK("merge: barrier recorded",
          PRTE_GRPCOMM_FENCE_OP_BARRIER == coll->op);
    CHECK("merge: an allgather against a barrier disagrees",
          !prte_grpcomm_fence_op_merge(coll, PRTE_GRPCOMM_FENCE_OP_ALLGATHER));
    PMIX_RELEASE(coll);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fence_operation\n");
    }
    return failures;
}

#if PRTE_TEST_GRPCOMM_INTERNALS
/* Build a throwaway signature naming one proc, so the tests below have
 * distinct collectives to talk about without a job to resolve them against. */
static void gen_sig(prte_grpcomm_fence_signature_t *sig, const char *nspace,
                    pmix_rank_t rank)
{
    PMIX_CONSTRUCT(sig, prte_grpcomm_fence_signature_t);
    sig->sz = 1;
    PMIX_PROC_CREATE(sig->signature, 1);
    PMIX_LOAD_PROCID(&sig->signature[0], nspace, rank);
}
#endif

/*
 * Telling one round over a signature from the next.
 *
 * A fence signature is only its participant list, so a contribution that
 * outlived the release ending its fence is otherwise indistinguishable from
 * the first contribution to the next fence over the same procs - and the
 * second one converges early on the first one's data.  What makes that
 * reachable is abort_fence_op(), which ends a fence while contributions are
 * still climbing the tree.
 *
 * The counter is driven by releases, and UNKNOWN is load-bearing: it is the
 * state of a daemon grown into the DVM after the job started, which has
 * released none of the earlier rounds.  It must not be confused with round 0,
 * which every other daemon has long since retired.
 */
static int test_fence_generation(void)
{
    int failures = 0;
#if PRTE_TEST_GRPCOMM_INTERNALS
    prte_grpcomm_fence_signature_t a, b;
    size_t i;

    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_generations, pmix_list_t);
    gen_sig(&a, "gen-nspace-a", PMIX_RANK_WILDCARD);
    gen_sig(&b, "gen-nspace-b", PMIX_RANK_WILDCARD);

    /* Nothing released yet.  The answer is "no claim", not zero - a daemon
     * that has never taken part cannot assert that round 0 is behind it. */
    CHECK("gen: an unseen signature is UNKNOWN",
          PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == prte_grpcomm_fence_gen_next(&a));
    CHECK("gen: and nothing is stale against it",
          !prte_grpcomm_fence_gen_is_stale(&a, 0));
    CHECK("gen: ...including a high generation",
          !prte_grpcomm_fence_gen_is_stale(&a, 7));

    /* An UNKNOWN stamp is silence, never staleness.  This is what a
     * newly-grown daemon sends, and dropping it would hang the fence it is
     * legitimately joining. */
    prte_grpcomm_fence_gen_record(&a, 4);
    CHECK("gen: an UNKNOWN stamp is never stale",
          !prte_grpcomm_fence_gen_is_stale(&a, PRTE_GRPCOMM_FENCE_GEN_UNKNOWN));

    /* Recording a release moves us to the next round. */
    CHECK("gen: releasing 4 puts the next fence at 5",
          5 == prte_grpcomm_fence_gen_next(&a));
    CHECK("gen: round 4 is now stale", prte_grpcomm_fence_gen_is_stale(&a, 4));
    CHECK("gen: as is anything below it",
          prte_grpcomm_fence_gen_is_stale(&a, 0));
    CHECK("gen: but the round we are on is not",
          !prte_grpcomm_fence_gen_is_stale(&a, 5));
    CHECK("gen: nor is one ahead of us",
          !prte_grpcomm_fence_gen_is_stale(&a, 6));

    /* Adopt, do not increment.  A daemon that missed rounds must be able to
     * jump straight to the true number rather than count from its arrival. */
    prte_grpcomm_fence_gen_record(&a, 20);
    CHECK("gen: a release adopts its generation rather than incrementing",
          21 == prte_grpcomm_fence_gen_next(&a));

    /* ...and a release that arrives out of order cannot walk it backwards,
     * which would un-stale contributions already correctly dropped. */
    prte_grpcomm_fence_gen_record(&a, 6);
    CHECK("gen: an out-of-order release does not move it back",
          21 == prte_grpcomm_fence_gen_next(&a));

    /* Signatures are independent - a fence over other procs is another
     * collective entirely, and must not inherit this one's count. */
    CHECK("gen: a different signature is untouched",
          PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == prte_grpcomm_fence_gen_next(&b));

    /* The memo is bounded.  Eviction is a graceful loss - that signature
     * returns to the old behaviour - but the list must not grow without end
     * on a long-lived DVM that fences over many different proc sets. */
    for (i = 0; i < PRTE_GRPCOMM_FENCE_MEMO_MAX + 8; i++) {
        prte_grpcomm_fence_signature_t t;
        char ns[PMIX_MAX_NSLEN + 1];
        snprintf(ns, sizeof(ns), "gen-fill-%zu", i);
        gen_sig(&t, ns, PMIX_RANK_WILDCARD);
        prte_grpcomm_fence_gen_record(&t, 0);
        PMIX_DESTRUCT(&t);
    }
    CHECK("gen: the memo stays bounded",
          PRTE_GRPCOMM_FENCE_MEMO_MAX >=
              pmix_list_get_size(&prte_grpcomm_globals.fence_generations));

    PMIX_DESTRUCT(&a);
    PMIX_DESTRUCT(&b);
    PMIX_LIST_DESTRUCT(&prte_grpcomm_globals.fence_generations);
    PMIX_CONSTRUCT(&prte_grpcomm_globals.fence_generations, pmix_list_t);
#endif

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fence_generation\n");
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

    failures += test_release_bcast_default();
    failures += test_base_context_id();
    failures += test_classes();
#if PRTE_TEST_GRPCOMM_INTERNALS
    failures += test_member_departed();
#endif
    failures += test_group_directives();
    failures += test_fence_operation();
    failures += test_fence_generation();
    failures += test_fence_tracker();
    failures += test_fence_fault_handler();
    failures += test_recovery_epoch();

    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all grpcomm unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d grpcomm unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
