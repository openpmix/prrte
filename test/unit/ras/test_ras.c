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
 *   3. prte_ras_base_select. Exactly one module is selected - an allocation
 *      has one owner - and with no RM in the environment that is `hosts`
 *      (priority 1), so the local-host fallback still works.
 *
 *   4. prte_ras_base_flag_string, which renders the node flag bitmask for
 *      --display-allocation.
 *
 *   5. ras/slurm's "detect and report an allocation" capability, driven
 *      through the framework rather than by calling into the component.
 *      That is everything the component can do when jansson is absent --
 *      every modify entry point declines without it -- and it is the half
 *      that needs no scheduler to exercise. The modify surface belongs to
 *      the multi-node harness in contrib/dockerswarm, which builds
 *      --with-jansson deliberately.
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

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/ras/ras.h"

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
 * machine. Being *built* is not enough either: a component in the default
 * --enable-mca-dso list is a run-time loadable plugin, which puts nothing in
 * libprrte to reference at all. The gated components are covered
 * structurally by test_select(), which walks whatever the framework
 * actually built, and ras/slurm is driven through that same public
 * interface by test_slurm_allocation() below - no component symbol is
 * named anywhere in this file. */

/* A component built into libprrte can have its module named directly, and
 * that is the only way to check the vtable of one that declines to answer
 * a query in this environment -- ras/simulator, ras/testrm and
 * ras/bootstrap all serve situations this test is not running in.  Built
 * as a DSO the same component is a separate object with no symbol to link
 * against, so those checks compile out and the framework lookup below is
 * all that is left.  See this directory's Makefile.am. */
#if PRTE_TEST_RAS_HOSTS
extern prte_ras_base_module_t prte_ras_hosts_module;
#endif
#if PRTE_TEST_RAS_SIMULATOR
extern prte_ras_base_module_t prte_ras_sim_module;
#endif
#if PRTE_TEST_RAS_TESTRM
extern prte_ras_base_module_t prte_ras_testrm_module;
#endif
#if PRTE_TEST_RAS_BOOTSTRAP
extern prte_ras_base_module_t prte_ras_bootstrap_module;
#endif
#if PRTE_TEST_RAS_PMIX
extern prte_ras_base_module_t prte_ras_pmix_module;
#endif

/* the module symbol for a component, where this build has one */
static prte_ras_base_module_t *ras_module_symbol(const char *name)
{
#if PRTE_TEST_RAS_HOSTS
    if (0 == strcmp("hosts", name)) {
        return &prte_ras_hosts_module;
    }
#endif
#if PRTE_TEST_RAS_SIMULATOR
    if (0 == strcmp("simulator", name)) {
        return &prte_ras_sim_module;
    }
#endif
#if PRTE_TEST_RAS_TESTRM
    if (0 == strcmp("testrm", name)) {
        return &prte_ras_testrm_module;
    }
#endif
#if PRTE_TEST_RAS_BOOTSTRAP
    if (0 == strcmp("bootstrap", name)) {
        return &prte_ras_bootstrap_module;
    }
#endif
#if PRTE_TEST_RAS_PMIX
    if (0 == strcmp("pmix", name)) {
        return &prte_ras_pmix_module;
    }
#endif
    (void) name;
    return NULL;
}

/* defined below, next to the ras/slurm case that also uses it */
static pmix_mca_base_component_t *find_ras_component(const char *name);

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

    /* the elastic campaign lists -- constructed by prte_init(), which this
     * test does not reach; prte_finalize() destructs them on the way out */
    PMIX_CONSTRUCT(&prte_shrink_campaigns, pmix_list_t);
    PMIX_CONSTRUCT(&prte_grow_campaigns, pmix_list_t);

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
        bool needs_allocate;
    } mods[] = {
        {"hosts",      true},
        {"simulator",  true},
        {"testrm",     true},
        {"bootstrap",  true},
        /* ras/slurm is covered the same way by test_slurm_allocation() */
        /* ras/pmix contributes nothing to initial discovery -- its
         * allocate is a deliberate TAKE_NEXT_OPTION stub -- but the slot
         * must still be filled so the driver has something to call */
        {"pmix",       true},
    };

    /* Ask each component for its module rather than naming the module
     * symbol.  Those symbols live in libprrte only while the component is
     * built into it; with --enable-mca-dso the component is a separate
     * object and the symbol is not there to link against, which is what
     * this file's own find_ras_component() was already written for. */
    for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        pmix_mca_base_component_t *comp = find_ras_component(mods[i].name);
        pmix_mca_base_module_t *module = NULL;
        prte_ras_base_module_t *mod;
        int pri = 0;

        if (NULL != comp && NULL != comp->pmix_mca_query_component &&
            PRTE_SUCCESS == comp->pmix_mca_query_component(&module, &pri) &&
            NULL != module) {
            mod = (prte_ras_base_module_t *) module;
        } else {
            /* the component is absent, or declines to serve this
             * environment; its module is still worth checking if this
             * build put it somewhere we can see */
            mod = ras_module_symbol(mods[i].name);
        }
        if (NULL == mod) {
            fprintf(stdout, "  SKIP module contract for %s (%s)\n", mods[i].name,
                    NULL == comp ? "not built" : "component declined, module not linkable");
            continue;
        }
        if (mods[i].needs_allocate && NULL == mod->allocate) {
            fprintf(stderr, "FAIL [module contract]: %s has no allocate\n", mods[i].name);
            failures++;
        }
    }

    /* the modify path is what serves PMIx_Allocation_request */
    for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        pmix_mca_base_component_t *comp;
        pmix_mca_base_module_t *module = NULL;
        int pri = 0;

        if (0 != strcmp("hosts", mods[i].name) && 0 != strcmp("pmix", mods[i].name)) {
            continue;
        }
        prte_ras_base_module_t *m;

        comp = find_ras_component(mods[i].name);
        if (NULL != comp && NULL != comp->pmix_mca_query_component &&
            PRTE_SUCCESS == comp->pmix_mca_query_component(&module, &pri) &&
            NULL != module) {
            m = (prte_ras_base_module_t *) module;
        } else {
            m = ras_module_symbol(mods[i].name);
        }
        if (NULL == m) {
            continue;
        }
        if (NULL == m->modify) {
            fprintf(stderr, "FAIL [module contract]: %s has no modify\n", mods[i].name);
            failures++;
        }
    }

    return failures;
}

/*
 * An allocation has exactly one owner: ras selects a single module, the
 * highest-priority candidate whose init() succeeds.
 *
 * It used to keep every module that answered and walk the list, and that is
 * what made a PMIX_ALLOC_NEW the real allocator had declined fall through to
 * ras/hosts, which added nodes the scheduler had never granted.  So "exactly
 * one" is the property under test, not an implementation detail.
 *
 * This runs with every RM environment variable unset, so the only component
 * that can answer is `hosts` -- which is also the assertion that ras/pmix
 * stays out of the way: it is priority 20 against hosts' 1 and would win if
 * it still answered unconditionally, and since its allocate() only ever
 * returns TAKE_NEXT_OPTION there would then be nothing left to read a
 * hostfile.
 */
static int test_select(void)
{
    int failures = 0;
    prte_ras_base_selected_module_t *mod;
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
    CHECK("select: exactly one module selected",
          1 == pmix_list_get_size(&prte_ras_base.selected_modules));

    mod = (prte_ras_base_selected_module_t *)
              pmix_list_get_first(&prte_ras_base.selected_modules);
    if (NULL == mod) {
        fprintf(stderr, "FAIL [select]: nothing selected\n");
        return failures + 1;
    }
    CHECK("select: module present", NULL != mod->module);
    CHECK("select: component present", NULL != mod->component);
    /* hosts is the catch-all: without it there is no --host/--hostfile
     * handling and no local-host fallback */
    CHECK("select: hosts is the owner with no RM in the environment",
          NULL != mod->component &&
          0 == strcmp("hosts", mod->component->pmix_mca_component_name));
    CHECK("select: hosts is priority 1", 1 == mod->pri);

    /* ...and PRRTE is its own authority over a hostfile allocation, so
     * add-host may grow the pool */
    CHECK("select: a hostfile allocation is not scheduler-owned",
          !prte_ras_base.scheduler_owned);
    CHECK("select: the flag came from the module",
          NULL != mod->module &&
          mod->module->scheduler_owned == prte_ras_base.scheduler_owned);

    /* select() latches -- a second call must be a harmless no-op rather
     * than selecting a second time */
    rc = prte_ras_base_select();
    CHECK("select: idempotent rc", PRTE_SUCCESS == rc);
    CHECK("select: still exactly one module",
          1 == pmix_list_get_size(&prte_ras_base.selected_modules));

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

/* locate a component by name in whatever the framework opened, static or
 * dlopened -- the same list prte_ras_base_select() walks */
static pmix_mca_base_component_t *find_ras_component(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;

    PMIX_LIST_FOREACH(cli, &prte_ras_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t) {
        if (NULL != cli->cli_component &&
            0 == strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            return (pmix_mca_base_component_t *) cli->cli_component;
        }
    }
    return NULL;
}

/*
 * ras/pmix must not answer a query unless someone has pointed it at a
 * scheduler.
 *
 * It used to answer unconditionally, "in case the system includes a scheduler
 * that supports PMIx operations".  That was survivable only while the
 * framework kept every module that answered, because this component's
 * allocate() always returns TAKE_NEXT_OPTION - it forwards requests to a
 * scheduler, it never discovers nodes - so the next module down did the
 * allocating.  With one module selected, an unconditional answer at priority
 * 20 beats ras/hosts at 1 and there is nothing left to read a hostfile.
 *
 * Driven through the framework rather than by naming the component's symbols:
 * ras-pmix may be a plugin, in which case it has none in libprrte, and naming
 * prte_mca_ras_pmix_component here fails the --enable-mca-dso link outright.
 * The switch is reached the same way anything else reaches another
 * component's parameter - look the MCA variable up by name, and write through
 * the pointer to the registering code's own storage that get_value returns.
 */
static int test_pmix_gate(void)
{
    int failures = 0;
    pmix_mca_base_component_t *comp;
    pmix_mca_base_module_t *module = NULL;
    bool *sched_switch = NULL;
    int pri = -1, rc, vari;

    comp = find_ras_component("pmix");
    if (NULL == comp) {
        fprintf(stdout, "SKIP test_pmix_gate: ras/pmix component not found\n");
        return 0;
    }
    vari = pmix_mca_base_var_find("prte", "ras", "pmix", "system_scheduler");
    if (0 > vari ||
        PMIX_SUCCESS != pmix_mca_base_var_get_value(vari, &sched_switch, NULL, NULL) ||
        NULL == sched_switch) {
        fprintf(stdout, "SKIP test_pmix_gate: ras_pmix_system_scheduler not registered\n");
        return 0;
    }
    CHECK("pmix: has a query function", NULL != comp->pmix_mca_query_component);
    if (NULL == comp->pmix_mca_query_component) {
        return failures;
    }

    /* nothing configured: no scheduler to forward to, so no module */
    rc = comp->pmix_mca_query_component(&module, &pri);
    CHECK("pmix: refuses when not pointed at a scheduler",
          PRTE_SUCCESS != rc || NULL == module);

    /* point it at one - any one of the connection parameters is the
     * statement of intent, and the system-scheduler switch is the one with
     * no string to free afterwards */
    *sched_switch = true;
    module = NULL;
    pri = -1;
    rc = comp->pmix_mca_query_component(&module, &pri);
    *sched_switch = false;

    CHECK("pmix: answers once pointed at a scheduler", PRTE_SUCCESS == rc);
    CHECK("pmix: returns a module", NULL != module);
    CHECK("pmix: at priority 20", 20 == pri);
    /* and what it allocates belongs to that scheduler, so add-host may not
     * grow it */
    CHECK("pmix: is scheduler-owned",
          NULL == module ||
          ((prte_ras_base_module_t *) module)->scheduler_owned);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_pmix_gate\n");
    }
    return failures;
}

/*
 * ras/slurm, exercised the way the base exercises it: query the component
 * for a module, then drive that module's allocate().
 *
 * This is the whole of what the component can do without jansson --
 * serve_extend_req, serve_release_req and serve_cancel_req each return
 * PRTE_ERR_NOT_AVAILABLE when prte_ras_slurm_have_jansson() is false -- and
 * it is the half that needs no live scheduler. Going through the vtable
 * rather than calling in directly is not a workaround: it is the only way
 * the DVM ever reaches this code, and it keeps the test working whether the
 * component was linked statically or loaded as a plugin.
 *
 * The elastic modify surface is deliberately not here. It shells out to
 * sbatch/scontrol and only means anything across multiple nodes, so it
 * belongs to contrib/dockerswarm -- which is also the only automated build
 * that configures --with-jansson and therefore compiles the JSON parser.
 *
 * The rejection cases below make the component log the refusals it is
 * supposed to log, so a passing run of this test still prints SLURM error
 * text. That output is the point, not a symptom.
 */
static int test_slurm_allocation(void)
{
    int failures = 0;
    pmix_mca_base_component_t *comp;
    pmix_mca_base_module_t *module;
    prte_ras_base_module_t *mod;
    prte_job_t *jdata;
    pmix_list_t nodes;
    prte_node_t *nd;
    char *toolong;
    /* SLURM_NODELIST is a compressed regex; "node[01-04]" must come back as
     * four zero-padded names, and "4(x4)" as four slots on each of them */
    static const char *expect[] = {"node01", "node02", "node03", "node04"};
    size_t i;
    int rc, pri;

    comp = find_ras_component("slurm");
    if (NULL == comp) {
#if PRTE_TEST_HAVE_RAS_SLURM
        fprintf(stderr,
                "SKIP [slurm]: component was built but the framework does not "
                "have it. It is a plugin, so a tree that has not been installed "
                "-- or one built with --enable-testbuild-launchers, where its "
                "JSON parser resolves to nothing -- has nothing to load.\n");
#else
        fprintf(stderr, "SKIP [slurm]: component not built on this platform\n");
#endif
        return 0;
    }
    CHECK("slurm: has a query function", NULL != comp->pmix_mca_query_component);
    if (NULL == comp->pmix_mca_query_component) {
        return failures;
    }

    /* "detect": no SLURM in the environment means the job is not ours */
    unsetenv("SLURM_JOBID");
    module = NULL;
    pri = -1;
    rc = comp->pmix_mca_query_component(&module, &pri);
    CHECK("slurm query: no module without SLURM_JOBID", NULL == module);
    CHECK("slurm query: declines without SLURM_JOBID", PRTE_SUCCESS != rc);

    setenv("SLURM_JOBID", "12345", 1);
    module = NULL;
    pri = -1;
    rc = comp->pmix_mca_query_component(&module, &pri);
    CHECK("slurm query: rc", PRTE_SUCCESS == rc);
    CHECK("slurm query: offers a module", NULL != module);
    /* the framework guide documents this ordering: slurm sits below the
     * nodefile-driven RMs and above the catch-all */
    CHECK("slurm query: priority 50", 50 == pri);
    if (NULL == module) {
        return failures;
    }

    mod = (prte_ras_base_module_t *) module;
    /* the vtable contract, on the module the component actually hands out.
     * slurm is the only component implementing shrink_complete today; if
     * another grows it, the base cycles every module for it. release_allocation
     * is unset - the session stack now retains its own reference and drains
     * itself at finalize, so the reactive hook is no longer needed. */
    CHECK("slurm contract: allocate", NULL != mod->allocate);
    CHECK("slurm contract: init", NULL != mod->init);
    CHECK("slurm contract: modify", NULL != mod->modify);
    CHECK("slurm contract: shrink_complete", NULL != mod->shrink_complete);
    CHECK("slurm contract: release_allocation unset", NULL == mod->release_allocation);
    if (NULL == mod->allocate) {
        return failures;
    }
    /* init() builds the session stack that allocate() pushes onto. The
     * framework does this at selection; nothing has selected slurm here
     * because test_select() ran with SLURM_JOBID unset. */
    if (NULL != mod->init) {
        CHECK("slurm init: rc", PRTE_SUCCESS == mod->init());
    }

    jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* "report": the compressed nodelist expands into real nodes */
    setenv("SLURM_NODELIST", "node[01-04]", 1);
    setenv("SLURM_TASKS_PER_NODE", "4(x4)", 1);
    unsetenv("SLURM_CPUS_PER_TASK");
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = mod->allocate(jdata, &nodes);
    CHECK("slurm allocate: rc", PRTE_SUCCESS == rc);
    CHECK("slurm allocate: node count", 4 == pmix_list_get_size(&nodes));
    i = 0;
    PMIX_LIST_FOREACH(nd, &nodes, prte_node_t) {
        if (i < sizeof(expect) / sizeof(expect[0])) {
            CHECK("slurm allocate: node name",
                  NULL != nd->name && 0 == strcmp(expect[i], nd->name));
            CHECK("slurm allocate: slots per node", 4 == nd->slots);
            CHECK("slurm allocate: node is up", PRTE_NODE_STATE_UP == nd->state);
        }
        i++;
    }
    PMIX_LIST_DESTRUCT(&nodes);

    /* SLURM_NODELIST is per-job, not per-step, so a second step re-reads the
     * same list. Returning SUCCESS here would insert the whole allocation a
     * second time; the contract is PRTE_EXISTS with nothing added. */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = mod->allocate(jdata, &nodes);
    CHECK("slurm allocate: re-discovery reports EXISTS", PRTE_EXISTS == rc);
    CHECK("slurm allocate: re-discovery adds nothing", 0 == pmix_list_get_size(&nodes));
    PMIX_LIST_DESTRUCT(&nodes);

    /* The jobid reaches scontrol/sbatch command lines, so it is a taint
     * boundary: tag_node_allocation and assign_new_session both run it
     * through validate_jobid, and allocate() must fail rather than carry a
     * shell metacharacter forward. */
    setenv("SLURM_JOBID", "1;rm -rf /", 1);
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = mod->allocate(jdata, &nodes);
    CHECK("slurm allocate: rejects a jobid with a shell metacharacter",
          PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);

    setenv("SLURM_JOBID", "$(id)", 1);
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = mod->allocate(jdata, &nodes);
    CHECK("slurm allocate: rejects a jobid with a substitution", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);

    setenv("SLURM_JOBID", "12a45", 1);
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = mod->allocate(jdata, &nodes);
    CHECK("slurm allocate: rejects a non-numeric jobid", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);

    /* check_taint bounds SLURM_NODELIST by ras_slurm_max_envar_length
     * (default 32000) before anything tries to parse it */
    toolong = malloc(64 * 1024);
    CHECK("slurm allocate: test allocation", NULL != toolong);
    if (NULL != toolong) {
        memset(toolong, 'a', 64 * 1024 - 1);
        toolong[64 * 1024 - 1] = '\0';
        setenv("SLURM_JOBID", "12346", 1);
        setenv("SLURM_NODELIST", toolong, 1);
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        rc = mod->allocate(jdata, &nodes);
        CHECK("slurm allocate: rejects an over-length nodelist",
              PRTE_ERR_BAD_PARAM == rc);
        PMIX_LIST_DESTRUCT(&nodes);
        free(toolong);
    }

    if (NULL != mod->finalize) {
        mod->finalize();
    }
    unsetenv("SLURM_JOBID");
    unsetenv("SLURM_NODELIST");
    unsetenv("SLURM_TASKS_PER_NODE");

    return failures;
}

/*
 * prte_ras_base_dvm_is_growing() -- the precondition that decides whether a
 * PMIX_ALLOC_RELEASE may start a shrink campaign now or has to be parked until
 * the DVM settles (#2617). It is a pure function of the daemon job and the grow
 * campaign list, so it is exactly the sort of decision this test can pin; what
 * it guards -- an unreachable daemon stalling the shrink broadcast -- needs the
 * dockerswarm harness.
 *
 * The three states of a growing DVM each get a case, and so does the one that
 * must NOT read as growing: a daemon that failed to start never reports a URI,
 * and if that counted it would park every later release for the life of the
 * DVM.
 */
static int test_dvm_growing(void)
{
    int failures = 0;
    prte_job_t *djob;
    prte_proc_t *d1;
    prte_grow_campaign_t *camp;

    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* a settled DVM: one daemon besides us, and it has reported home */
    d1 = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&d1->name, PRTE_PROC_MY_NAME->nspace, 1);
    d1->rml_uri = strdup("1;tcp://127.0.0.1:1234");
    PRTE_FLAG_SET(d1, PRTE_PROC_FLAG_ALIVE);
    pmix_pointer_array_set_item(djob->procs, 1, d1);
    CHECK("dvm_is_growing: a settled DVM is not growing",
          !prte_ras_base_dvm_is_growing());

    /* state 1: the grow has been requested but setup_virtual_machine has not
     * run yet -- no campaign exists and the new daemons have no procs */
    prte_set_attribute(&djob->attributes, PRTE_JOB_EXTEND_DVM,
                       PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);
    CHECK("dvm_is_growing: a requested-but-unstarted grow is growing",
          prte_ras_base_dvm_is_growing());
    prte_remove_attribute(&djob->attributes, PRTE_JOB_EXTEND_DVM);
    CHECK("dvm_is_growing: settled again once the attribute is consumed",
          !prte_ras_base_dvm_is_growing());

    /* state 2: launch in progress -- a campaign is recorded */
    camp = PMIX_NEW(prte_grow_campaign_t);
    camp->ntargets = 1;
    camp->targets = (pmix_rank_t *) malloc(sizeof(pmix_rank_t));
    camp->targets[0] = 2;
    pmix_list_append(&prte_grow_campaigns, &camp->super);
    CHECK("dvm_is_growing: a recorded grow campaign is growing",
          prte_ras_base_dvm_is_growing());
    pmix_list_remove_item(&prte_grow_campaigns, &camp->super);
    PMIX_RELEASE(camp);
    CHECK("dvm_is_growing: settled again once the campaign drains",
          !prte_ras_base_dvm_is_growing());

    /* state 3: a daemon recorded as launched that has never reported in.
     * rml_uri is the test, and only rml_uri: ALIVE and RUNNING are both set
     * when the launch is merely recorded. */
    free(d1->rml_uri);
    d1->rml_uri = NULL;
    d1->state = PRTE_PROC_STATE_RUNNING;
    CHECK("dvm_is_growing: a daemon that has not reported home is growing",
          prte_ras_base_dvm_is_growing());

    /* ...but a daemon that failed to start is not still joining, and must not
     * park releases forever */
    PRTE_FLAG_UNSET(d1, PRTE_PROC_FLAG_ALIVE);
    d1->state = PRTE_PROC_STATE_FAILED_TO_START;
    CHECK("dvm_is_growing: a daemon that failed to start is not growing",
          !prte_ras_base_dvm_is_growing());

    pmix_pointer_array_set_item(djob->procs, 1, NULL);
    PMIX_RELEASE(d1);

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

    rc = setup_globals();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "test globals setup failed: %d\n", rc);
        return 1;
    }

    /* the module contract asks each component for its module, so the
     * framework has to be open before it runs; test_select() opens it
     * again, which the refcount makes a no-op */
    rc = pmix_mca_base_framework_open(&prte_ras_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "ras framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_module_contract();
    failures += test_select();
    failures += test_node_insert();
    failures += test_bootstrap_ranks();
    failures += test_preassigned_index();
    failures += test_hnp_dedup();
    failures += test_flag_string();
    failures += test_dvm_growing();
    /* after test_select(), which opens the framework and latches a
     * selection made with no SLURM allocation in the environment -- so
     * nothing has called slurm's init() before this does */
    failures += test_pmix_gate();
    failures += test_slurm_allocation();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all ras unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d ras unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
