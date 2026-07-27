/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the ras (Resource Allocation Subsystem) framework.
 *
 * Most of what ras *does* -- talking to SLURM/PBS/LSF/Flux, growing and
 * shrinking a live DVM -- needs a real resource manager and a running set
 * of daemons, so it is covered by the integration and dockerswarm
 * harnesses. What can be pinned down in isolation is the machinery the
 * whole framework funnels through, which is also where the accounting
 * bugs hide:
 *
 *   1. prte_ras_base_node_insert. Every component's allocate() ends here:
 *      it drains the caller's working list into the global prte_node_pool,
 *      dedups against what is already there, normalizes FQDNs, honors
 *      PRTE_NODE_ADD_SLOTS, and maintains prte_ras_base.total_slots_alloc.
 *      The dedup/accounting path in particular is exercised on every
 *      elastic re-grow of a node that was previously shrunk out, where a
 *      double count silently inflates the DVM's idea of its own size.
 *
 *   2. The module contract. The driver walks selected_modules calling
 *      mod->module->allocate, skipping NULL slots; a component that
 *      forgets to fill in allocate contributes nothing and is invisible.
 *      Check every statically-built component's vtable.
 *
 *   3. prte_ras_base_select. Unlike the usual single-winner MCA pattern,
 *      ras keeps ALL modules that answer and stores them priority-sorted;
 *      the driver depends on that ordering, and on `hosts` (priority 1)
 *      always being present and last so the local-host fallback works.
 *
 *   4. prte_ras_base_flag_string, which renders the node flag bitmask for
 *      --display-allocation.
 *
 *   5. The SLURM taint validators. ras/slurm shells out to
 *      scancel/scontrol/sbatch, so validate_jobid / validate_hostname are
 *      the boundary between a scheduler-supplied string and a command
 *      line. Their bounds and allowlists are checked here, along with the
 *      bounded command-output reader they share.
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "src/class/pmix_pointer_array.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/attr.h"
#include "src/util/pmix_argv.h"
#include "src/util/prte_bootstrap.h"
#include "src/util/proc_info.h"

#include "src/mca/ras/base/base.h"
#include "src/mca/ras/ras.h"
#if PRTE_TEST_HAVE_RAS_SLURM
#    include "src/mca/ras/slurm/ras_slurm.h"
#endif

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* Only the components with NO configure.m4 are guaranteed to be built, and
 * therefore guaranteed to have symbols to link against. pbs, gridengine, lsf
 * and flux are all gated on detecting their resource manager, and slurm is
 * gated on the platform (and removable with --without-slurm) - referencing
 * those unconditionally makes `make check` fail to link on an ordinary
 * machine. The gated components are covered structurally by test_select(),
 * which walks whatever the framework actually built. */
extern prte_ras_base_module_t prte_ras_hosts_module;
extern prte_ras_base_module_t prte_ras_pmix_module;
extern prte_ras_base_module_t prte_ras_sim_module;
extern prte_ras_base_module_t prte_ras_testrm_module;
extern prte_ras_base_module_t prte_ras_bootstrap_module;
/* prte_ras_slurm_module, the validators and the bounded output drain come
 * from ras_slurm.h, only when that component is built */

/*
 * prte_init_util() stops short of building the global job/node/session
 * arrays -- that happens in prte_init(), which also wants a live ESS,
 * session directories and a network stack. Stand up just the pieces
 * node_insert touches, exactly as prte_init does.
 */
static int setup_globals(void)
{
    prte_job_t *djob;
    prte_node_t *hnp;

    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_node_pool, 8, INT_MAX, 8);
    prte_node_topologies = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_node_topologies, 8, INT_MAX, 8);
    prte_sessions = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_sessions, 8, INT_MAX, 8);

    /* the daemon job -- node_insert looks it up to test DO_NOT_LAUNCH */
    djob = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(djob->nspace, "prte-unit-test-dvm");
    PMIX_LOAD_PROCID(PRTE_PROC_MY_NAME, "prte-unit-test-dvm", 0);
    prte_set_job_data_object(djob);

    /* the HNP's own node always occupies pool index 0 */
    hnp = PMIX_NEW(prte_node_t);
    hnp->name = strdup("prte-unit-test-hnp");
    hnp->state = PRTE_NODE_STATE_UP;
    hnp->slots = 1;
    hnp->index = pmix_pointer_array_add(prte_node_pool, hnp);

    return (0 == hnp->index) ? PRTE_SUCCESS : PRTE_ERROR;
}

/* append a fresh node to a working list */
static prte_node_t *mknode(pmix_list_t *list, const char *name, int slots)
{
    prte_node_t *nd = PMIX_NEW(prte_node_t);

    nd->name = strdup(name);
    nd->state = PRTE_NODE_STATE_UP;
    nd->slots = slots;
    nd->slots_max = 0;
    nd->slots_inuse = 0;
    pmix_list_append(list, &nd->super);
    return nd;
}

/* how many non-NULL entries the pool holds */
static int pool_count(void)
{
    int i, n = 0;

    for (i = 0; i < prte_node_pool->size; i++) {
        if (NULL != pmix_pointer_array_get_item(prte_node_pool, i)) {
            n++;
        }
    }
    return n;
}

static int test_node_insert(void)
{
    int failures = 0;
    pmix_list_t nodes;
    prte_node_t *nd, *found;
    int rc, before;

    /* --- a plain insert of two new nodes --- */
    prte_ras_base.total_slots_alloc = 0;
    before = pool_count();
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    mknode(&nodes, "unittest-n01", 4);
    mknode(&nodes, "unittest-n02", 6);
    rc = prte_ras_base_node_insert(&nodes, NULL);
    CHECK("insert: rc", PRTE_SUCCESS == rc);
    /* node_insert documents that it removes EVERY item from the list --
     * callers destruct the list afterwards and would double-free if not */
    CHECK("insert: list drained", pmix_list_is_empty(&nodes));
    CHECK("insert: pool grew by 2", pool_count() == before + 2);
    CHECK("insert: slots totalled", 10 == prte_ras_base.total_slots_alloc);
    PMIX_DESTRUCT(&nodes);

    found = prte_node_match(NULL, "unittest-n01");
    CHECK("insert: n01 findable", NULL != found);
    if (NULL != found) {
        CHECK("insert: n01 slots", 4 == found->slots);
        CHECK("insert: n01 indexed", 0 < found->index);
    }

    /* --- re-inserting a node the pool already holds ---
     * This is the elastic re-grow case: a shrink removes the daemon but
     * the pool entry survives, so the regrant arrives as a duplicate. The
     * pool entry must be reused, its slots must NOT be counted a second
     * time, and the incoming PRTE_NODE_STATE_ADDED marking must carry
     * across or the DVM extension will skip the node entirely. */
    before = pool_count();
    prte_ras_base.total_slots_alloc = 0;
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = mknode(&nodes, "unittest-n01", 4);
    nd->state = PRTE_NODE_STATE_ADDED;
    rc = prte_ras_base_node_insert(&nodes, NULL);
    CHECK("dedup: rc", PRTE_SUCCESS == rc);
    CHECK("dedup: list drained", pmix_list_is_empty(&nodes));
    CHECK("dedup: no duplicate entry", pool_count() == before);
    CHECK("dedup: slots not double counted", 0 == prte_ras_base.total_slots_alloc);
    PMIX_DESTRUCT(&nodes);

    found = prte_node_match(NULL, "unittest-n01");
    CHECK("dedup: still findable", NULL != found);
    if (NULL != found) {
        CHECK("dedup: ADDED carried across", PRTE_NODE_STATE_ADDED == found->state);
        CHECK("dedup: slots unchanged", 4 == found->slots);
    }

    /* --- PRTE_NODE_ADD_SLOTS adjusts rather than replaces --- */
    found = prte_node_match(NULL, "unittest-n02");
    CHECK("addslots: n02 present", NULL != found);
    if (NULL != found) {
        found->slots_max = 8;
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        nd = mknode(&nodes, "unittest-n02", 3);
        prte_set_attribute(&nd->attributes, PRTE_NODE_ADD_SLOTS, PRTE_ATTR_LOCAL,
                           NULL, PMIX_BOOL);
        rc = prte_ras_base_node_insert(&nodes, NULL);
        PMIX_DESTRUCT(&nodes);
        CHECK("addslots: rc", PRTE_SUCCESS == rc);
        /* 6 + 3 = 9, clamped to slots_max of 8 */
        CHECK("addslots: clamped to slots_max", 8 == found->slots);
    }

    /* --- FQDN normalization: short name kept, full name preserved --- */
    before = pool_count();
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    mknode(&nodes, "unittest-n03.example.com", 2);
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("fqdn: rc", PRTE_SUCCESS == rc);
    CHECK("fqdn: one node added", pool_count() == before + 1);
    found = prte_node_match(NULL, "unittest-n03");
    CHECK("fqdn: short name resolves", NULL != found);
    if (NULL != found) {
        CHECK("fqdn: name truncated", 0 == strcmp(found->name, "unittest-n03"));
        CHECK("fqdn: rawname kept", NULL != found->rawname &&
              0 == strcmp(found->rawname, "unittest-n03.example.com"));
        /* both spellings must resolve, or a later --host using the FQDN
         * would fabricate a second entry for the same machine */
        CHECK("fqdn: long name resolves too",
              found == prte_node_match(NULL, "unittest-n03.example.com"));
    }

    /* --- an empty list is a no-op, not an error --- */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    before = pool_count();
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("empty: rc", PRTE_SUCCESS == rc);
    CHECK("empty: pool untouched", pool_count() == before);

    return failures;
}

/*
 * The bootstrap rank contract, on which both ras/bootstrap's pool layout
 * and plm's daemon-vpid assignment depend.
 *
 * A bootstrapped daemon computes its OWN vpid from prte.conf, via
 * prte_bootstrap_my_identity(), before it ever contacts the HNP - and
 * prted_report_launch() then looks it up in daemons->procs by the rank it
 * claims. So the HNP has to reach the same number independently, from the
 * same authority. ras/bootstrap records it as the node's pool index and
 * plm reads it back out as the vpid; what makes that round trip safe is
 * that ranks over a well-formed DVMNodes list are exactly 1..N with no
 * gaps, so the pool slots are contiguous and rank_of/host_of_rank are
 * inverses.
 *
 * A host listed twice breaks precisely that property, which is why
 * ras/bootstrap rejects it rather than letting a permanently unclaimable
 * rank stall DVM formation.
 */
static int test_bootstrap_ranks(void)
{
    int failures = 0;
    prte_bootstrap_config_t cfg;
    pmix_rank_t rank;
    const char *host;
    int rc;

    /* --- controller separate from the node list --- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cluster = strdup("test");
    cfg.ctrlhost = strdup("ctrl");
    cfg.keep_fqdn = false;
    PMIx_Argv_append_nosize(&cfg.nodes, "bn01");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn02");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn03");

    CHECK("bootranks: controller is rank 0",
          PRTE_SUCCESS == prte_bootstrap_rank_of(&cfg, "ctrl", &rank) && 0 == rank);
    /* contiguous 1..N is what lets the pool slot double as the rank */
    rc = prte_bootstrap_rank_of(&cfg, "bn01", &rank);
    CHECK("bootranks: first entry is rank 1", PRTE_SUCCESS == rc && 1 == rank);
    rc = prte_bootstrap_rank_of(&cfg, "bn02", &rank);
    CHECK("bootranks: second entry is rank 2", PRTE_SUCCESS == rc && 2 == rank);
    rc = prte_bootstrap_rank_of(&cfg, "bn03", &rank);
    CHECK("bootranks: third entry is rank 3", PRTE_SUCCESS == rc && 3 == rank);
    CHECK("bootranks: a stranger is not a member",
          PRTE_SUCCESS != prte_bootstrap_rank_of(&cfg, "bn99", &rank));
    /* controller not in the list => N+1 daemons */
    CHECK("bootranks: daemon count", 4 == prte_bootstrap_num_daemons(&cfg));

    /* rank_of and host_of_rank must be inverses, or the HNP and the daemons
     * disagree about who holds which vpid */
    for (rank = 1; rank <= 3; rank++) {
        pmix_rank_t back;

        host = NULL;
        rc = prte_bootstrap_host_of_rank(&cfg, rank, &host);
        CHECK("bootranks: rank resolves to a host", PRTE_SUCCESS == rc && NULL != host);
        if (PRTE_SUCCESS != rc || NULL == host) {
            continue;
        }
        rc = prte_bootstrap_rank_of(&cfg, host, &back);
        CHECK("bootranks: round trip", PRTE_SUCCESS == rc && back == rank);
    }
    prte_bootstrap_config_free(&cfg);

    /* --- controller listed among the nodes: its entry consumes no rank --- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cluster = strdup("test");
    cfg.ctrlhost = strdup("bn02");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn01");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn02");   /* the controller, mid-list */
    PMIx_Argv_append_nosize(&cfg.nodes, "bn03");

    rc = prte_bootstrap_rank_of(&cfg, "bn02", &rank);
    CHECK("bootranks(ctrl-in-list): controller still rank 0",
          PRTE_SUCCESS == rc && 0 == rank);
    rc = prte_bootstrap_rank_of(&cfg, "bn01", &rank);
    CHECK("bootranks(ctrl-in-list): rank 1", PRTE_SUCCESS == rc && 1 == rank);
    /* the skipped controller entry must NOT leave a gap */
    rc = prte_bootstrap_rank_of(&cfg, "bn03", &rank);
    CHECK("bootranks(ctrl-in-list): still contiguous", PRTE_SUCCESS == rc && 2 == rank);
    CHECK("bootranks(ctrl-in-list): daemon count", 3 == prte_bootstrap_num_daemons(&cfg));
    prte_bootstrap_config_free(&cfg);

    /* --- a duplicated host is what ras/bootstrap must reject --- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.cluster = strdup("test");
    cfg.ctrlhost = strdup("ctrl");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn01");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn02");
    PMIx_Argv_append_nosize(&cfg.nodes, "bn01");   /* duplicate */
    PMIx_Argv_append_nosize(&cfg.nodes, "bn03");

    /* both occurrences resolve to the FIRST one's rank... */
    rc = prte_bootstrap_rank_of(&cfg, "bn01", &rank);
    CHECK("bootranks(dup): duplicate collapses to one rank",
          PRTE_SUCCESS == rc && 1 == rank);
    /* ...so the position it would have held is never handed to anybody, and
     * the last entry's rank runs past the number of distinct hosts */
    rc = prte_bootstrap_rank_of(&cfg, "bn03", &rank);
    CHECK("bootranks(dup): later ranks are pushed out", PRTE_SUCCESS == rc && 4 == rank);
    host = NULL;
    rc = prte_bootstrap_host_of_rank(&cfg, 3, &host);
    CHECK("bootranks(dup): rank 3 maps back to a host that is not rank 3",
          PRTE_SUCCESS == rc && NULL != host &&
          PRTE_SUCCESS == prte_bootstrap_rank_of(&cfg, host, &rank) && 3 != rank);
    prte_bootstrap_config_free(&cfg);

    return failures;
}

/*
 * A component may pre-assign a node's pool slot by setting node->index
 * before handing it over. ras/bootstrap depends on this: a bootstrapped
 * daemon computes its own vpid from the config file, so the HNP has to
 * land the node at the slot matching that canonical rank rather than at
 * whatever slot the append order happens to produce.
 */
static int test_preassigned_index(void)
{
    int failures = 0;
    pmix_list_t nodes;
    prte_node_t *nd, *found;
    int rc, slot;

    /* pick a slot well clear of anything already in the pool */
    slot = prte_node_pool->size + 16;

    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = mknode(&nodes, "unittest-boot01", 1);
    nd->index = slot;
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("preindex: rc", PRTE_SUCCESS == rc);

    found = prte_node_match(NULL, "unittest-boot01");
    CHECK("preindex: findable", NULL != found);
    if (NULL != found) {
        /* the whole point: the assignment survives the insert */
        CHECK("preindex: landed at the requested slot", slot == found->index);
        CHECK("preindex: pool agrees",
              found == (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, slot));
    }

    /* a slot that is already occupied is a malformed request: fall back to
     * an append rather than clobbering the node that is already there */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = mknode(&nodes, "unittest-boot02", 1);
    nd->index = slot;                       /* same slot as boot01 */
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("preindex: collision rc", PRTE_SUCCESS == rc);
    CHECK("preindex: occupant untouched",
          found == (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, slot));
    nd = prte_node_match(NULL, "unittest-boot02");
    CHECK("preindex: collider still inserted", NULL != nd);
    if (NULL != nd) {
        CHECK("preindex: collider moved elsewhere", slot != nd->index);
    }

    /* the default (-1) still means "append to the lowest free slot" */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    mknode(&nodes, "unittest-boot03", 1);
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("preindex: default rc", PRTE_SUCCESS == rc);
    nd = prte_node_match(NULL, "unittest-boot03");
    CHECK("preindex: appended node indexed", NULL != nd && 0 < nd->index);

    return failures;
}

/*
 * The HNP's node is pre-entered at pool index 0 before any component runs,
 * so an incoming entry for the local host must update that entry in place
 * rather than adding a second one -- otherwise the DVM would try to launch
 * a daemon on itself.
 */
static int test_hnp_dedup(void)
{
    int failures = 0;
    pmix_list_t nodes;
    prte_node_t *hnp;
    int before, rc;

    hnp = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, 0);
    CHECK("hnp: node at index 0", NULL != hnp);
    if (NULL == hnp) {
        return failures;
    }

    before = pool_count();
    prte_ras_base.total_slots_alloc = 0;
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    mknode(&nodes, prte_process_info.nodename, 12);
    rc = prte_ras_base_node_insert(&nodes, NULL);
    PMIX_DESTRUCT(&nodes);
    CHECK("hnp: rc", PRTE_SUCCESS == rc);
    CHECK("hnp: no duplicate local node", pool_count() == before);
    CHECK("hnp: slots adopted", 12 == hnp->slots);
    CHECK("hnp: counted once", 12 == prte_ras_base.total_slots_alloc);
    CHECK("hnp: marked allocated", prte_hnp_is_allocated);

    return failures;
}

/*
 * Every component fills in the same vtable and the driver dereferences the
 * slots it uses without further checks beyond a NULL test on allocate.
 */
static int test_module_contract(void)
{
    int failures = 0;
    size_t i;
    struct {
        const char *name;
        prte_ras_base_module_t *mod;
        bool needs_allocate;
    } mods[] = {
        {"hosts",      &prte_ras_hosts_module,      true},
        {"simulator",  &prte_ras_sim_module,        true},
        {"testrm",     &prte_ras_testrm_module,     true},
        {"bootstrap",  &prte_ras_bootstrap_module,  true},
#if PRTE_TEST_HAVE_RAS_SLURM
        {"slurm",      &prte_ras_slurm_module,      true},
#endif
        /* ras/pmix contributes nothing to initial discovery -- its
         * allocate is a deliberate TAKE_NEXT_OPTION stub -- but the slot
         * must still be filled so the driver has something to call */
        {"pmix",       &prte_ras_pmix_module,       true},
    };

    for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        if (mods[i].needs_allocate && NULL == mods[i].mod->allocate) {
            fprintf(stderr, "FAIL [module contract]: %s has no allocate\n", mods[i].name);
            failures++;
        }
    }

#if PRTE_TEST_HAVE_RAS_SLURM
    /* only slurm implements the elastic completion hooks today; if another
     * component grows them, the base cycles every module for both */
    CHECK("contract: slurm shrink_complete", NULL != prte_ras_slurm_module.shrink_complete);
    CHECK("contract: slurm release_allocation",
          NULL != prte_ras_slurm_module.release_allocation);
    CHECK("contract: slurm init", NULL != prte_ras_slurm_module.init);
    CHECK("contract: slurm modify", NULL != prte_ras_slurm_module.modify);
#endif

    /* the modify path is what serves PMIx_Allocation_request */
    CHECK("contract: hosts modify", NULL != prte_ras_hosts_module.modify);
    CHECK("contract: pmix modify", NULL != prte_ras_pmix_module.modify);

    return failures;
}

/*
 * ras keeps every module that answers, priority-sorted -- it is not a
 * single-winner selection. The driver walks that list in order, and the
 * always-available `hosts` component must sort last so the RM components
 * get first refusal and the local-host fallback still runs.
 */
static int test_select(void)
{
    int failures = 0;
    prte_ras_base_selected_module_t *mod;
    int last_pri = INT_MAX;
    bool ordered = true, found_hosts = false;
    const char *tail = NULL;
    int rc;

    /* keep a developer's own allocation out of the result */
    unsetenv("SLURM_JOBID");
    unsetenv("PBS_ENVIRONMENT");
    unsetenv("PBS_JOBID");
    unsetenv("COBALT_JOBID");
    unsetenv("PE_HOSTFILE");
    unsetenv("JOB_ID");
    unsetenv("SGE_ROOT");

    rc = pmix_mca_base_framework_open(&prte_ras_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    CHECK("select: framework opened", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        return failures;
    }

    rc = prte_ras_base_select();
    CHECK("select: rc", PRTE_SUCCESS == rc);
    CHECK("select: something selected",
          0 < pmix_list_get_size(&prte_ras_base.selected_modules));

    PMIX_LIST_FOREACH(mod, &prte_ras_base.selected_modules,
                      prte_ras_base_selected_module_t) {
        if (mod->pri > last_pri) {
            ordered = false;
        }
        last_pri = mod->pri;
        CHECK("select: module present", NULL != mod->module);
        CHECK("select: component present", NULL != mod->component);
        if (NULL != mod->component) {
            tail = mod->component->pmix_mca_component_name;
            if (0 == strcmp(tail, "hosts")) {
                found_hosts = true;
                CHECK("select: hosts is priority 1", 1 == mod->pri);
            }
        }
    }
    CHECK("select: descending priority order", ordered);
    /* hosts is the unconditional catch-all: without it there is no
     * --host/--hostfile handling and no local-host fallback */
    CHECK("select: hosts selected", found_hosts);
    CHECK("select: hosts sorts last", NULL != tail && 0 == strcmp(tail, "hosts"));

    /* select() latches -- a second call must be a harmless no-op rather
     * than appending every module a second time */
    rc = prte_ras_base_select();
    CHECK("select: idempotent rc", PRTE_SUCCESS == rc);

    return failures;
}

static int test_flag_string(void)
{
    int failures = 0;
    prte_node_t *nd;
    char *s;

    nd = PMIX_NEW(prte_node_t);

    s = prte_ras_base_flag_string(nd);
    CHECK("flags: none", NULL != s && NULL != strstr(s, "NONE"));
    free(s);

    PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_DAEMON_LAUNCHED);
    s = prte_ras_base_flag_string(nd);
    CHECK("flags: daemon launched", NULL != s && NULL != strstr(s, "DAEMON_LAUNCHED"));
    free(s);

    PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_SLOTS_GIVEN);
    PRTE_FLAG_SET(nd, PRTE_NODE_FLAG_OVERSUBSCRIBED);
    s = prte_ras_base_flag_string(nd);
    CHECK("flags: multiple rendered", NULL != s &&
          NULL != strstr(s, "DAEMON_LAUNCHED") &&
          NULL != strstr(s, "SLOTS_GIVEN") &&
          NULL != strstr(s, "OVERSUBSCRIBED"));
    free(s);

    PMIX_RELEASE(nd);
    return failures;
}

#if PRTE_TEST_HAVE_RAS_SLURM
/*
 * ras/slurm builds scancel/scontrol/sbatch command lines out of strings
 * that came from the scheduler or from a PMIx client, so these validators
 * are a security boundary, not a convenience.
 */
static int test_slurm_validators(void)
{
    int failures = 0;
    char toolong[PRTE_SLURM_JOB_ID_MAX_LEN + 8];
    char longhost[PRTE_SLURM_HOSTNAME_MAX_LEN + 8];

    CHECK("jobid: plain", PRTE_SUCCESS == prte_ras_slurm_validate_jobid("12345"));
    CHECK("jobid: zero ok", PRTE_SUCCESS == prte_ras_slurm_validate_jobid("0"));
    CHECK("jobid: NULL", PRTE_SUCCESS != prte_ras_slurm_validate_jobid(NULL));
    CHECK("jobid: empty", PRTE_SUCCESS != prte_ras_slurm_validate_jobid(""));
    CHECK("jobid: non-digit", PRTE_SUCCESS != prte_ras_slurm_validate_jobid("12a45"));
    CHECK("jobid: negative", PRTE_SUCCESS != prte_ras_slurm_validate_jobid("-1"));
    /* the whole point: nothing that could reach a shell */
    CHECK("jobid: shell metachar",
          PRTE_SUCCESS != prte_ras_slurm_validate_jobid("1;rm -rf /"));
    CHECK("jobid: substitution",
          PRTE_SUCCESS != prte_ras_slurm_validate_jobid("$(id)"));
    CHECK("jobid: trailing space",
          PRTE_SUCCESS != prte_ras_slurm_validate_jobid("123 "));

    memset(toolong, '9', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK("jobid: over length", PRTE_SUCCESS != prte_ras_slurm_validate_jobid(toolong));

    CHECK("host: plain", PRTE_SUCCESS == prte_ras_slurm_validate_hostname("node01"));
    CHECK("host: fqdn",
          PRTE_SUCCESS == prte_ras_slurm_validate_hostname("node01.example.com"));
    CHECK("host: dashes", PRTE_SUCCESS == prte_ras_slurm_validate_hostname("nid-00-01"));
    CHECK("host: NULL", PRTE_SUCCESS != prte_ras_slurm_validate_hostname(NULL));
    CHECK("host: empty", PRTE_SUCCESS != prte_ras_slurm_validate_hostname(""));
    CHECK("host: space", PRTE_SUCCESS != prte_ras_slurm_validate_hostname("node 01"));
    CHECK("host: semicolon",
          PRTE_SUCCESS != prte_ras_slurm_validate_hostname("node01;reboot"));
    CHECK("host: backtick",
          PRTE_SUCCESS != prte_ras_slurm_validate_hostname("`whoami`"));
    CHECK("host: comma rejected (lists must be split first)",
          PRTE_SUCCESS != prte_ras_slurm_validate_hostname("n01,n02"));
    CHECK("host: newline", PRTE_SUCCESS != prte_ras_slurm_validate_hostname("n01\n"));

    memset(longhost, 'a', sizeof(longhost) - 1);
    longhost[sizeof(longhost) - 1] = '\0';
    CHECK("host: over length", PRTE_SUCCESS != prte_ras_slurm_validate_hostname(longhost));

    return failures;
}

/*
 * The command-output drain is shared by scancel and scontrol and copies
 * scheduler error text into a fixed buffer -- an off-by-one here writes
 * past a stack array on the HNP.
 */
static int test_slurm_drain_output(void)
{
    int failures = 0;
    char buf[32];
    char guard[64];
    FILE *fp;
    int rc;

    fp = popen("printf 'hello world\\n'", "r");
    CHECK("drain: popen", NULL != fp);
    if (NULL == fp) {
        return failures;
    }
    rc = prte_ras_slurm_drain_cmd_output(fp, buf, sizeof(buf));
    pclose(fp);
    CHECK("drain: rc", PRTE_SUCCESS == rc);
    CHECK("drain: captured", 0 == strcmp(buf, "hello world\n"));

    /* more output than the buffer holds must truncate, stay
     * NUL-terminated, and be marked with the "..." suffix */
    fp = popen("printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n'", "r");
    CHECK("drain: popen 2", NULL != fp);
    if (NULL != fp) {
        memset(guard, 'X', sizeof(guard));
        rc = prte_ras_slurm_drain_cmd_output(fp, guard, 16);
        pclose(fp);
        CHECK("drain: truncate rc", PRTE_SUCCESS == rc);
        CHECK("drain: NUL terminated within bounds", 15 >= strlen(guard));
        CHECK("drain: truncation marked", NULL != strstr(guard, "..."));
        CHECK("drain: did not write past the buffer", 'X' == guard[16]);
    }

    /* a NULL output buffer means "drain and discard", not an error */
    fp = popen("printf 'ignored\\n'", "r");
    if (NULL != fp) {
        rc = prte_ras_slurm_drain_cmd_output(fp, NULL, 0);
        pclose(fp);
        CHECK("drain: discard mode", PRTE_SUCCESS == rc);
    }

    CHECK("drain: NULL stream rejected",
          PRTE_SUCCESS != prte_ras_slurm_drain_cmd_output(NULL, buf, sizeof(buf)));

    return failures;
}
#endif /* PRTE_TEST_HAVE_RAS_SLURM */

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    rc = setup_globals();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "test globals setup failed: %d\n", rc);
        return 1;
    }

    failures += test_module_contract();
    failures += test_select();
    failures += test_node_insert();
    failures += test_bootstrap_ranks();
    failures += test_preassigned_index();
    failures += test_hnp_dedup();
    failures += test_flag_string();
#if PRTE_TEST_HAVE_RAS_SLURM
    failures += test_slurm_validators();
    failures += test_slurm_drain_output();
#endif

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all ras unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d ras unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
