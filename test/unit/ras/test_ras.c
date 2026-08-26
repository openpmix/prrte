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
 *   5. prte_ras_base_activate_hosts, which resolves a --activate
 *      specification against the node pool, and
 *      prte_ras_base_activate_nodes beneath it, which is the same resolver
 *      as a PMIX_ALLOC_ACTIVATE request reaches - with the hostfile as its
 *      own argument rather than folded into the host list. It is the one
 *      operation that may extend the DVM under an allocation PRRTE does not
 *      own, so what it will and will not accept is exactly the safety
 *      property.
 *
 *   6. ras/slurm's "detect and report an allocation" capability, driven
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
#include "src/util/pmix_printf.h"
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

    if (NULL != module) {
        prte_ras_base_module_t *mod = (prte_ras_base_module_t *) module;
        pmix_list_t nodes;

        /* it discovers nothing: this component forwards runtime requests to
         * a scheduler and never contributes a node to initial discovery.
         * Worth pinning because the query gate above now makes it the sole
         * selected module whenever anyone points it at a scheduler, so this
         * return is what sends the base to its local-node fallback rather
         * than to another allocator. */
        CHECK("pmix: has an allocate", NULL != mod->allocate);
        if (NULL != mod->allocate) {
            PMIX_CONSTRUCT(&nodes, pmix_list_t);
            CHECK("pmix: allocate contributes nothing",
                  PRTE_ERR_TAKE_NEXT_OPTION == mod->allocate(NULL, &nodes));
            CHECK("pmix: allocate leaves the list empty",
                  0 == pmix_list_get_size(&nodes));
            PMIX_LIST_DESTRUCT(&nodes);
        }

        /* With no scheduler reachable, modify() must say UNREACH.
         *
         * It used to say PMIX_ERR_TAKE_NEXT_OPTION, from a time when the
         * framework kept every module that answered and ras/hosts could
         * serve the request locally further down the list.  Selection keeps
         * exactly one module now, so there is no next option: the driver's
         * loop simply ends and req->pstatus is left holding the
         * PMIX_ERR_NOT_SUPPORTED it was seeded with, telling the requester
         * the operation is unsupported when the truth is that the scheduler
         * is out of touch - "give up" where "try again" was meant.
         *
         * scheduler_lookup_done is what makes this deterministic and cheap:
         * prte_pmix_set_scheduler() looks for a scheduler exactly once, and
         * with the search already marked done it answers UNREACH without
         * going near PMIx_tool_attach_to_server (a blocking rendezvous scan
         * this test has no business performing). */
        CHECK("pmix: has a modify", NULL != mod->modify);
        if (NULL != mod->modify) {
            prte_pmix_server_req_t *req;
            bool saved = prte_pmix_server_globals.scheduler_lookup_done;

            prte_pmix_server_globals.scheduler_lookup_done = true;
            req = PMIX_NEW(prte_pmix_server_req_t);
            req->allocdir = PMIX_ALLOC_EXTEND;
            CHECK("pmix: modify reports UNREACH with no scheduler",
                  PMIX_ERR_UNREACH == mod->modify(req));
            PMIX_RELEASE(req);
            prte_pmix_server_globals.scheduler_lookup_done = saved;
        }
    }

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

/*
 * --activate: resolve a --host-syntax specification against the node pool and
 * mark the nodes a grow should launch on.
 *
 * The grow itself is not exercised here -- prte_ras_base_activate_dvm_grow()
 * activates a job state, which needs a live state machine and event base --
 * so every case below either refuses (returning before any grow) or takes the
 * "--add-host is also present, so ITS grow is the one that launches" branch,
 * which marks the nodes and returns without activating anything. That branch
 * is worth pinning for its own sake: the ordering it encodes is what keeps a
 * second grow from racing ahead of add-host's asynchronous node insertion.
 *
 * What this covers is the whole decision surface: which pool entries a
 * specification names, which it is allowed to name, and the rule that a
 * specification failing anywhere marks nothing.
 */
static prte_job_t *mkactivate_job(const char *spec, bool with_addhost)
{
    prte_job_t *jdata;
    prte_app_context_t *app;

    jdata = PMIX_NEW(prte_job_t);
    app = PMIX_NEW(prte_app_context_t);
    app->idx = pmix_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;
    if (NULL != spec) {
        prte_set_attribute(&app->attributes, PRTE_APP_ACTIVATE_HOSTS,
                           PRTE_ATTR_GLOBAL, (void *) spec, PMIX_STRING);
    }
    if (with_addhost) {
        prte_set_attribute(&app->attributes, PRTE_APP_ADD_HOST,
                           PRTE_ATTR_GLOBAL, (void *) "somewhere", PMIX_STRING);
    }
    return jdata;
}

/* run one specification and report the rc; nodes are marked in place */
static int activate(const char *spec, bool with_addhost)
{
    prte_job_t *jdata = mkactivate_job(spec, with_addhost);
    int rc = prte_ras_base_activate_hosts(jdata);

    PMIX_RELEASE(jdata);
    return rc;
}

/* write `content` to `path` and run "<prefix>file=<path>" through activate */
static int mkhostfile_and_activate2(const char *path, const char *content,
                                    const char *prefix, bool with_addhost)
{
    FILE *fp;
    char *spec;
    int rc;

    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "could not write test hostfile %s\n", path);
        return PRTE_ERROR;
    }
    fputs(content, fp);
    fclose(fp);

    pmix_asprintf(&spec, "%s%s", prefix, path);
    rc = activate(spec, with_addhost);
    free(spec);
    unlink(path);
    return rc;
}

static int mkhostfile_and_activate(const char *path, const char *content,
                                   bool with_addhost)
{
    return mkhostfile_and_activate2(path, content, "file=", with_addhost);
}

static int test_activate_hosts(void)
{
    int failures = 0;
    prte_node_t *idle1, *idle2, *busy, *gone, *nptr;
    prte_proc_t *dmn;
    bool saved_alloc;
    prte_node_state_t *parked;
    char *relative;
    const char *hf = "prte_test_activate_hosts.txt";
    int i, npool;

    /* The "+e" forms select across the whole pool, so this test needs to own
     * it. The earlier tests have left their nodes there - up, with no daemon,
     * which is exactly what +e looks for - so park them out of the way and
     * put them back on the way out. */
    npool = prte_node_pool->size;
    parked = (prte_node_state_t *) malloc(npool * sizeof(prte_node_state_t));
    for (i = 0; i < npool; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
        if (NULL == nptr) {
            continue;
        }
        parked[i] = nptr->state;
        nptr->state = PRTE_NODE_STATE_NOT_INCLUDED;
    }

    /* Four pool entries beyond the HNP: two allocated nodes with no daemon
     * (exactly what --activate exists to reach), one already in the DVM, and
     * one the scheduler has taken back. */
    idle1 = PMIX_NEW(prte_node_t);
    idle1->name = strdup("act-idle1");
    idle1->state = PRTE_NODE_STATE_UP;
    idle1->slots = 4;
    idle1->index = pmix_pointer_array_add(prte_node_pool, idle1);

    idle2 = PMIX_NEW(prte_node_t);
    idle2->name = strdup("act-idle2");
    idle2->state = PRTE_NODE_STATE_UP;
    idle2->slots = 4;
    idle2->index = pmix_pointer_array_add(prte_node_pool, idle2);

    busy = PMIX_NEW(prte_node_t);
    busy->name = strdup("act-busy");
    busy->state = PRTE_NODE_STATE_UP;
    busy->slots = 4;
    busy->index = pmix_pointer_array_add(prte_node_pool, busy);
    dmn = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&dmn->name, PRTE_PROC_MY_NAME->nspace, 7);
    busy->daemon = dmn;

    gone = PMIX_NEW(prte_node_t);
    gone->name = strdup("act-gone");
    gone->state = PRTE_NODE_STATE_NOT_INCLUDED;
    gone->slots = 4;
    gone->index = pmix_pointer_array_add(prte_node_pool, gone);

    /* the relative "+nK" index counts from the first allocated entry, and
     * pool entry 0 is the HNP's - which the earlier tests have established
     * as allocated */
    saved_alloc = prte_hnp_is_allocated;
    prte_hnp_is_allocated = true;

    /* a job with no directive at all is not this function's business */
    CHECK("activate: no directive is a no-op",
          PRTE_SUCCESS == activate(NULL, false));

    /* a name the allocation does not contain is refused - this is the whole
     * difference from --add-host, which would have inserted it */
    CHECK("activate: unknown host refused",
          PRTE_ERR_SILENT == activate("act-nosuch", false));
    CHECK("activate: nothing marked by a refused spec",
          PRTE_NODE_STATE_UP == idle1->state);

    /* a slot count is refused rather than silently dropped: activate has no
     * authority to change what the allocation grants */
    CHECK("activate: slot modifier refused",
          PRTE_ERR_SILENT == activate("act-idle1:8", false));
    CHECK("activate: slots unchanged by the refusal", 4 == idle1->slots);
    CHECK("activate: nothing marked by the refusal",
          PRTE_NODE_STATE_UP == idle1->state);

    /* a node the scheduler took back is not available: launching a daemon on
     * it fails, and a daemon that cannot start takes the DVM down */
    CHECK("activate: excluded node refused",
          PRTE_ERR_SILENT == activate("act-gone", false));

    /* naming a node already in the DVM is satisfied, not refused - and
     * activates no grow, since there is nothing to launch */
    CHECK("activate: node already in the DVM is a no-op",
          PRTE_SUCCESS == activate("act-busy", false));

    /* a spec that fails on its LAST token must not leave the earlier ones
     * marked: those marks would be swept into some later, unrelated grow */
    CHECK("activate: partial spec refused",
          PRTE_ERR_SILENT == activate("act-idle1,act-nosuch", false));
    CHECK("activate: first token not marked by a later failure",
          PRTE_NODE_STATE_UP == idle1->state);

    /* "+e" is valid --host syntax and is deliberately NOT accepted here: it
     * selects nodes with no process on them, which is mostly nodes already
     * in the DVM, so the request would start nothing and report success */
    CHECK("activate: +e refused",
          PRTE_ERR_SILENT == activate("+e", false));
    CHECK("activate: +e:2 refused",
          PRTE_ERR_SILENT == activate("+e:2", false));
    CHECK("activate: nothing marked by the refused +e",
          PRTE_NODE_STATE_UP == idle1->state &&
          PRTE_NODE_STATE_UP == idle2->state);

    /* an out-of-range relative index is refused */
    CHECK("activate: +n99 refused",
          PRTE_ERR_SILENT == activate("+n99", false));
    /* as is a malformed relative token */
    CHECK("activate: +x refused",
          PRTE_ERR_SILENT == activate("+x", false));
    /* ...and a count that is not a number. --host's own parser takes
     * strtol's answer unchecked, so "+nabc" means "+n0" there; here it must
     * not quietly resolve to a node nobody named. */
    CHECK("activate: +nabc refused",
          PRTE_ERR_SILENT == activate("+nabc", false));

    /* file= reads a hostfile in --hostfile's own format, and resolves what
     * it finds against the pool exactly as a typed name is resolved */
    CHECK("activate: bare file= refused",
          PRTE_ERR_SILENT == activate("file=", false));
    CHECK("activate: a file that does not exist is refused",
          PRTE_ERR_SILENT == activate("file=/nonexistent/activate-hosts", false));
    CHECK("activate: a file naming an unallocated host is refused",
          PRTE_ERR_SILENT == mkhostfile_and_activate(hf, "act-nosuch\n", false));
    CHECK("activate: nothing marked by the refused file",
          PRTE_NODE_STATE_UP == idle1->state);

    /* Now the accepting path. --add-host is present, so this marks the nodes
     * and leaves the grow to add-host's request - which is exactly the
     * ordering contract, and lets the marking be observed here. */
    CHECK("activate: named node accepted",
          PRTE_SUCCESS == activate("act-idle1", true));
    CHECK("activate: named node marked ADDED",
          PRTE_NODE_STATE_ADDED == idle1->state);
    CHECK("activate: other node untouched",
          PRTE_NODE_STATE_UP == idle2->state);

    /* a hostfile naming the rest of them.  It carries a "slots=" that must
     * NOT be applied - a hostfile handed to a launcher selects nodes, it
     * does not resize them - and an entry for a node already in the DVM,
     * which is satisfied rather than refused. */
    CHECK("activate: hostfile accepted",
          PRTE_SUCCESS == mkhostfile_and_activate(hf,
                              "act-idle2 slots=99\nact-busy\n", true));
    CHECK("activate: node named in the file marked ADDED",
          PRTE_NODE_STATE_ADDED == idle2->state);
    CHECK("activate: the file's slot count was not applied", 4 == idle2->slots);
    CHECK("activate: excluded node still excluded",
          PRTE_NODE_STATE_NOT_INCLUDED == gone->state);
    CHECK("activate: node in the DVM never marked",
          PRTE_NODE_STATE_UP == busy->state);

    /* the forms mix in one list */
    idle1->state = PRTE_NODE_STATE_UP;
    idle2->state = PRTE_NODE_STATE_UP;
    CHECK("activate: a name and a file in one spec",
          PRTE_SUCCESS == mkhostfile_and_activate2(hf, "act-idle2\n",
                              "act-idle1,file=", true));
    CHECK("activate: the named node was marked",
          PRTE_NODE_STATE_ADDED == idle1->state);
    CHECK("activate: the file's node was marked too",
          PRTE_NODE_STATE_ADDED == idle2->state);

    /* "+all" takes every allocated node that is not in the DVM - and only
     * those: the one carrying a daemon and the one the scheduler took back
     * are not candidates however the request is spelled */
    idle1->state = PRTE_NODE_STATE_UP;
    idle2->state = PRTE_NODE_STATE_UP;
    CHECK("activate: +all accepted",
          PRTE_SUCCESS == activate("+all", true));
    CHECK("activate: +all marked both idle nodes",
          PRTE_NODE_STATE_ADDED == idle1->state &&
          PRTE_NODE_STATE_ADDED == idle2->state);
    CHECK("activate: +all did not touch the node in the DVM",
          PRTE_NODE_STATE_UP == busy->state);
    CHECK("activate: +all did not touch the excluded node",
          PRTE_NODE_STATE_NOT_INCLUDED == gone->state);
    CHECK("activate: +ALL is the same token",
          PRTE_SUCCESS == activate("+ALL", true));

    /* with every candidate already claimed by the pending grow, "+all" is
     * satisfied rather than refused, and starts no second grow */
    CHECK("activate: +all is satisfied when every candidate is already claimed",
          PRTE_SUCCESS == activate("+all", false));

    /* ...but "+all" is a whole token: "+allende" is not it */
    CHECK("activate: +allende refused",
          PRTE_ERR_SILENT == activate("+allende", false));

    /* the relative index names a pool entry directly. With the HNP's node in
     * the allocation the index is the pool index, as it is for --host. */
    idle1->state = PRTE_NODE_STATE_UP;
    pmix_asprintf(&relative, "+n%d", idle1->index);
    CHECK("activate: relative index accepted",
          PRTE_SUCCESS == activate(relative, true));
    CHECK("activate: relative index marked its pool entry",
          PRTE_NODE_STATE_ADDED == idle1->state);
    free(relative);

    prte_hnp_is_allocated = saved_alloc;
    for (i = 0; i < npool; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
        if (NULL == nptr) {
            continue;
        }
        nptr->state = parked[i];
    }
    free(parked);
    busy->daemon = NULL;
    PMIX_RELEASE(dmn);
    pmix_pointer_array_set_item(prte_node_pool, idle1->index, NULL);
    pmix_pointer_array_set_item(prte_node_pool, idle2->index, NULL);
    pmix_pointer_array_set_item(prte_node_pool, busy->index, NULL);
    pmix_pointer_array_set_item(prte_node_pool, gone->index, NULL);
    PMIX_RELEASE(idle1);
    PMIX_RELEASE(idle2);
    PMIX_RELEASE(busy);
    PMIX_RELEASE(gone);

    return failures;
}


/*
 * prte_ras_base_activate_nodes: the entry point both requesters share.
 *
 * The command line reaches it through prte_ras_base_activate_hosts() above,
 * folding its hostfile into the host list as "file=<path>"; a
 * PMIX_ALLOC_ACTIVATE request reaches it directly, carrying the hostfile as a
 * separate attribute.  What is tested here is the part only the second caller
 * can reach - the hostfile as its own argument - plus the activation count,
 * which is what tells that caller whether anything will be launched and so
 * whether a completion event is coming.
 *
 * No grow is activated by this function at all, which is why every case can
 * be run without a state machine behind it.
 */
static int mkactfile(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");

    if (NULL == fp) {
        fprintf(stderr, "could not write test hostfile %s\n", path);
        return PRTE_ERROR;
    }
    fputs(content, fp);
    fclose(fp);
    return PRTE_SUCCESS;
}

static int test_activate_nodes(void)
{
    int failures = 0;
    prte_node_t *idle1, *idle2, *nptr;
    prte_node_state_t *parked;
    const char *hf1 = "prte_test_activate_nodes1.txt";
    const char *hf2 = "prte_test_activate_nodes2.txt";
    char *both;
    int i, npool, n = -1;

    /* own the pool, as the +e forms above require - the same reason applies
     * to "+all" here */
    npool = prte_node_pool->size;
    parked = (prte_node_state_t *) malloc(npool * sizeof(prte_node_state_t));
    for (i = 0; i < npool; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
        if (NULL == nptr) {
            continue;
        }
        parked[i] = nptr->state;
        nptr->state = PRTE_NODE_STATE_NOT_INCLUDED;
    }

    idle1 = PMIX_NEW(prte_node_t);
    idle1->name = strdup("actn-idle1");
    idle1->state = PRTE_NODE_STATE_UP;
    idle1->slots = 4;
    idle1->index = pmix_pointer_array_add(prte_node_pool, idle1);

    idle2 = PMIX_NEW(prte_node_t);
    idle2->name = strdup("actn-idle2");
    idle2->state = PRTE_NODE_STATE_UP;
    idle2->slots = 4;
    idle2->index = pmix_pointer_array_add(prte_node_pool, idle2);

    /* naming nothing at all is a bad request, not an empty success: a
     * requester that meant "everything" says so with "+all" */
    CHECK("activate_nodes: nothing named is refused",
          PRTE_ERR_BAD_PARAM == prte_ras_base_activate_nodes(NULL, NULL, &n));
    CHECK("activate_nodes: refusal reports no activation", 0 == n);
    CHECK("activate_nodes: an empty spec is the same as none",
          PRTE_ERR_BAD_PARAM == prte_ras_base_activate_nodes("", "", &n));

    /* the host specification alone */
    CHECK("activate_nodes: host spec accepted",
          PRTE_SUCCESS == prte_ras_base_activate_nodes("actn-idle1", NULL, &n));
    CHECK("activate_nodes: it marked the node", PRTE_NODE_STATE_ADDED == idle1->state);
    CHECK("activate_nodes: and counted it", 1 == n);

    /* a node already marked is met, and counts for nothing: there is no
     * second daemon to launch, so no completion to wait for */
    CHECK("activate_nodes: re-naming a marked node succeeds",
          PRTE_SUCCESS == prte_ras_base_activate_nodes("actn-idle1", NULL, &n));
    CHECK("activate_nodes: with nothing left to activate", 0 == n);

    /* the hostfile as its own argument - the form only the PMIx directive
     * produces, since the command line spells it "file=<path>" */
    idle1->state = PRTE_NODE_STATE_UP;
    if (PRTE_SUCCESS != mkactfile(hf1, "actn-idle1\n")) {
        return failures + 1;
    }
    CHECK("activate_nodes: hostfile argument accepted",
          PRTE_SUCCESS == prte_ras_base_activate_nodes(NULL, hf1, &n));
    CHECK("activate_nodes: the file's node was marked",
          PRTE_NODE_STATE_ADDED == idle1->state);
    CHECK("activate_nodes: and counted", 1 == n);

    /* both arguments together, each naming a different node */
    idle1->state = PRTE_NODE_STATE_UP;
    CHECK("activate_nodes: host spec and hostfile together",
          PRTE_SUCCESS == prte_ras_base_activate_nodes("actn-idle2", hf1, &n));
    CHECK("activate_nodes: both nodes marked",
          PRTE_NODE_STATE_ADDED == idle1->state &&
          PRTE_NODE_STATE_ADDED == idle2->state);
    CHECK("activate_nodes: both counted", 2 == n);

    /* several hostfiles, comma-delimited, as every other hostfile attribute
     * accepts */
    idle1->state = PRTE_NODE_STATE_UP;
    idle2->state = PRTE_NODE_STATE_UP;
    if (PRTE_SUCCESS != mkactfile(hf2, "actn-idle2\n")) {
        return failures + 1;
    }
    pmix_asprintf(&both, "%s,%s", hf1, hf2);
    CHECK("activate_nodes: a comma-delimited hostfile list",
          PRTE_SUCCESS == prte_ras_base_activate_nodes(NULL, both, &n));
    CHECK("activate_nodes: every file's node marked",
          PRTE_NODE_STATE_ADDED == idle1->state &&
          PRTE_NODE_STATE_ADDED == idle2->state);
    CHECK("activate_nodes: all of them counted", 2 == n);
    free(both);

    /* a refusal anywhere marks nothing and says so */
    idle1->state = PRTE_NODE_STATE_UP;
    idle2->state = PRTE_NODE_STATE_UP;
    CHECK("activate_nodes: an unknown host is refused",
          PRTE_ERR_SILENT == prte_ras_base_activate_nodes("actn-idle1,actn-nosuch",
                                                          NULL, &n));
    CHECK("activate_nodes: nothing was marked by the refused spec",
          PRTE_NODE_STATE_UP == idle1->state);
    CHECK("activate_nodes: and nothing counted", 0 == n);

    /* the refusal covers the hostfile argument too */
    if (PRTE_SUCCESS != mkactfile(hf2, "actn-nosuch\n")) {
        return failures + 1;
    }
    CHECK("activate_nodes: an unknown host in the hostfile is refused",
          PRTE_ERR_SILENT == prte_ras_base_activate_nodes("actn-idle1", hf2, &n));
    CHECK("activate_nodes: the host spec's node was not marked either",
          PRTE_NODE_STATE_UP == idle1->state);

    unlink(hf1);
    unlink(hf2);
    for (i = 0; i < npool; i++) {
        nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, i);
        if (NULL == nptr) {
            continue;
        }
        nptr->state = parked[i];
    }
    free(parked);
    pmix_pointer_array_set_item(prte_node_pool, idle1->index, NULL);
    pmix_pointer_array_set_item(prte_node_pool, idle2->index, NULL);
    PMIX_RELEASE(idle1);
    PMIX_RELEASE(idle2);

    return failures;
}

/*
 * prte_ras_base_spawn_alloc: the allocation request a spawn can carry.
 *
 * What is testable without a live DVM is the front half - whether the
 * request is well enough formed to be posted at all - and that is exactly
 * the half that decides between "this spawn is refused outright" and "the
 * job is now held while an allocator answers". The posting itself needs the
 * event base and an allocator to answer, so the cases here all stop before
 * it: no request at all, and the two ways a request can be malformed.
 */
static prte_job_t *mkspawnalloc_job(pmix_info_t *req, size_t nreq)
{
    prte_job_t *jdata;
    pmix_data_array_t darray;

    jdata = PMIX_NEW(prte_job_t);
    if (NULL != req) {
        darray.type = PMIX_INFO;
        darray.size = nreq;
        darray.array = req;
        prte_set_attribute(&jdata->attributes, PRTE_JOB_SPAWN_ALLOC,
                           PRTE_ATTR_GLOBAL, &darray, PMIX_DATA_ARRAY);
    }
    return jdata;
}

static int test_spawn_alloc(void)
{
    int failures = 0;
    prte_job_t *jdata;
    pmix_info_t req[2];
    pmix_alloc_directive_t directive = PMIX_ALLOC_NEW;
    bool posted = true;
    int rc;

    /* a spawn that asks for no allocation is not this function's business,
     * and must not be reported as having posted anything */
    jdata = mkspawnalloc_job(NULL, 0);
    rc = prte_ras_base_spawn_alloc(jdata, &posted);
    CHECK("spawn_alloc: no request is a no-op", PRTE_SUCCESS == rc);
    CHECK("spawn_alloc: and posts nothing", !posted);
    PMIX_RELEASE(jdata);

    /* a request that names no directive cannot be served: the allocation API
     * takes the directive as a parameter, and carried as data it has to be
     * stated rather than guessed */
    PMIX_INFO_LOAD(&req[0], PMIX_ALLOC_NODE_LIST, "node01", PMIX_STRING);
    jdata = mkspawnalloc_job(req, 1);
    posted = true;
    rc = prte_ras_base_spawn_alloc(jdata, &posted);
    CHECK("spawn_alloc: a request with no directive is refused",
          PRTE_ERR_BAD_PARAM == rc);
    CHECK("spawn_alloc: nothing was posted for it", !posted);
    CHECK("spawn_alloc: and the request is not left on the job to be retried",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_ALLOC,
                              NULL, PMIX_DATA_ARRAY));
    PMIX_RELEASE(jdata);
    PMIX_INFO_DESTRUCT(&req[0]);

    /* a directive of the wrong type is refused rather than reinterpreted -
     * the value arrived from a client, and every other reading of it would
     * be an allocation nobody asked for */
    PMIX_INFO_LOAD(&req[0], PMIX_ALLOC_REQ_DIRECTIVE, "new", PMIX_STRING);
    PMIX_INFO_LOAD(&req[1], PMIX_ALLOC_NODE_LIST, "node01", PMIX_STRING);
    jdata = mkspawnalloc_job(req, 2);
    posted = true;
    rc = prte_ras_base_spawn_alloc(jdata, &posted);
    CHECK("spawn_alloc: a mistyped directive is refused",
          PRTE_ERR_BAD_PARAM == rc);
    CHECK("spawn_alloc: nothing posted for the mistyped directive", !posted);
    PMIX_RELEASE(jdata);
    PMIX_INFO_DESTRUCT(&req[0]);
    PMIX_INFO_DESTRUCT(&req[1]);

    /* the attribute is consumed by the attempt, whatever its outcome: the
     * launch is re-driven once the DVM is ready, and a second pass must not
     * ask for a second allocation. Shown here on the refusal path, which is
     * the one that returns while the job still exists to inspect. */
    PMIX_INFO_LOAD(&req[0], PMIX_ALLOC_REQ_DIRECTIVE, &directive, PMIX_ALLOC_DIRECTIVE);
    jdata = mkspawnalloc_job(req, 1);
    CHECK("spawn_alloc: the request is carried on the job",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_SPAWN_ALLOC,
                             NULL, PMIX_DATA_ARRAY));
    PMIX_RELEASE(jdata);
    PMIX_INFO_DESTRUCT(&req[0]);

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

    rc = setup_globals();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "test globals setup failed: %d\n", rc);
        return 1;
    }

    /* An allocation request carried on a spawn is a pmix_data_array_t, and
     * copying one is a PMIx operation - PMIx_Data_copy refuses to run until
     * PMIx itself is up, which is how prte_set_attribute would fail to store
     * such an attribute here while working perfectly in a daemon.  A daemon
     * reaches that state through PMIx_server_init, so do the same. */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        prte_finalize();
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
    failures += test_activate_hosts();
    failures += test_activate_nodes();
    failures += test_spawn_alloc();
    /* after test_select(), which opens the framework and latches a
     * selection made with no SLURM allocation in the environment -- so
     * nothing has called slurm's init() before this does */
    failures += test_pmix_gate();
    failures += test_slurm_allocation();

    /* PMIx last, and after the frameworks are closed: finalizing it unloads
     * the components PRRTE has open in a DSO build */
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all ras unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d ras unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
