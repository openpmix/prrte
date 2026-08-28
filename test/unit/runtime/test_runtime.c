/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/runtime.
 *
 * This directory is the runtime's foundation: the three central data
 * structures (prte_job_t, prte_node_t, prte_proc_t) with their constructors
 * and destructors, the global registries every other framework looks things
 * up in, the wire encoding used to ship a job to a daemon, the session
 * (reservation) bookkeeping, and the publish/lookup data server.  Nothing
 * here needs a live DVM, which is exactly why it is worth pinning down: a
 * defect at this layer shows up somewhere else entirely.
 *
 * The cases below are built around the defects the review turned up:
 *
 *  - prte_get_session_object_from_refid() compared against
 *    session->user_refid without checking it for NULL, so a single
 *    reservation created without a user reference id (they are optional -
 *    the sibling lookup on alloc_refid does check) crashed every subsequent
 *    lookup by refid;
 *  - prte_node_match() dereferenced prte_node_pool unconditionally when
 *    asked to search the global pool, which is a NULL pointer until
 *    prte_init has laid the global arrays out;
 *  - prte_map_copy() blitted the source pointer-array's size/max_size over
 *    the destination's and then copied addr[] element by element into an
 *    allocation the destination constructor had sized at
 *    PRTE_GLOBAL_ARRAY_BLOCK_SIZE.  A map spanning more nodes than that
 *    wrote off the end of the heap block.  It also took no reference on the
 *    nodes it copied, while prte_job_map_destruct releases every node it
 *    holds - so the two maps between them dropped one refcount too many;
 *  - prte_app_copy() walked app->attributes, a list of prte_attribute_t, as
 *    a list of prte_value_t: it read the value from the wrong offset and
 *    dropped the attribute key entirely, leaving every copied attribute
 *    unfindable;
 *  - prte_node_copy() carried neither the node's aliases nor its attributes
 *    nor its session, so a duplicated node answered to only one of its
 *    names and silently joined the default pool;
 *  - the data server's object constructors left prte_data_object_t::proxy
 *    and prte_data_req_t::proxy uninitialized (PMIX_NEW does not zero its
 *    allocation), and those are precisely what the PMIX_RANGE_LOCAL access
 *    check compares;
 *  - the progress thread's cpu-range parser tested the strtoul end pointer
 *    for NULL - which it never is - so a bare "3" took the range branch and
 *    stepped past the terminating NUL, and ranges excluded their upper
 *    bound.
 *
 * Everything is driven through the exported entry points, so the tests keep
 * working if the internals are rearranged.
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "types.h"

#include "src/class/pmix_pointer_array.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/pmix/pmix-internal.h"
#include "src/rml/oob/oob.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/runtime/prte_worker_pool.h"
#include "src/runtime/runtime.h"
#include "src/util/attr.h"
#include "src/util/proc_info.h"

#include "src/runtime/data_server/ds.h"
#include "src/runtime/data_server/prte_data_server.h"

/* pmix_nspace_t is a char[256]; passing a string literal where one is
 * expected trips -Wstringop-overread, so route literals through a buffer */
static pmix_nspace_t ns_scratch;
static const char *NS(const char *s)
{
    PMIX_LOAD_NSPACE(ns_scratch, s);
    return (const char *) ns_scratch;
}

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* harness                                                            */
/* ------------------------------------------------------------------ */

/* prte_init_util() stops short of building the global job/node/session
 * arrays - prte_init() does that, and it needs a selected ess module and a
 * live event base.  Build them by hand so the lookup tests have registries
 * to work against. */
static void setup_globals(void)
{
    prte_job_data = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_job_data, 8, INT_MAX, 8);
    prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_node_pool, 8, INT_MAX, 8);
    prte_node_topologies = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_node_topologies, 8, INT_MAX, 8);
    prte_sessions = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_sessions, 8, INT_MAX, 8);
}

static void empty_array(pmix_pointer_array_t *array)
{
    int i;
    void *item;

    if (NULL == array) {
        return;
    }
    for (i = 0; i < array->size; i++) {
        item = pmix_pointer_array_get_item(array, i);
        if (NULL != item) {
            pmix_pointer_array_set_item(array, i, NULL);
            PMIX_RELEASE(item);
        }
    }
}

/* leave the registries empty between tests so one test's leftovers cannot
 * satisfy another's lookup */
static void reset_globals(void)
{
    empty_array(prte_job_data);
    empty_array(prte_node_pool);
    /* sessions null their own slot on destruct, so just walk and release */
    empty_array(prte_sessions);
}

static prte_job_t *make_job(const char *nspace)
{
    prte_job_t *jdata = PMIX_NEW(prte_job_t);

    PMIX_LOAD_NSPACE(jdata->nspace, nspace);
    return jdata;
}

static prte_node_t *make_node(const char *name)
{
    prte_node_t *node = PMIX_NEW(prte_node_t);

    node->name = strdup(name);
    node->slots = 4;
    node->slots_available = 4;
    node->state = PRTE_NODE_STATE_UP;
    return node;
}

/* ------------------------------------------------------------------ */
/* the global job registry                                            */
/* ------------------------------------------------------------------ */

static int test_job_registry(void)
{
    int failures = 0;
    prte_job_t *j1, *j2, *dup;
    int rc, saved_index;
    pmix_nspace_t bogus;

    reset_globals();

    /* an unknown nspace is a miss, not a crash */
    CHECK("job: unknown nspace misses", NULL == prte_get_job_data_object(NS("nosuchjob")));

    j1 = make_job("job.one");
    rc = prte_set_job_data_object(j1);
    CHECK("job: first insert succeeds", PRTE_SUCCESS == rc);
    CHECK("job: insert records an index", 0 <= j1->index);
    CHECK("job: lookup finds it", j1 == prte_get_job_data_object(NS("job.one")));

    j2 = make_job("job.two");
    CHECK("job: second insert succeeds", PRTE_SUCCESS == prte_set_job_data_object(j2));
    CHECK("job: both are findable",
          j1 == prte_get_job_data_object(NS("job.one"))
              && j2 == prte_get_job_data_object(NS("job.two")));

    /* the registry is keyed on nspace, so re-registering one has to be
     * refused rather than silently shadowing the live object */
    dup = make_job("job.one");
    CHECK("job: duplicate nspace is refused", PRTE_EXISTS == prte_set_job_data_object(dup));
    CHECK("job: the original is still the one on file",
          j1 == prte_get_job_data_object(NS("job.one")));
    PMIX_RELEASE(dup);

    /* an invalid nspace is rejected on both sides */
    PMIX_LOAD_NSPACE(bogus, NULL);
    CHECK("job: invalid nspace does not resolve", NULL == prte_get_job_data_object(bogus));
    dup = PMIX_NEW(prte_job_t);
    CHECK("job: invalid nspace cannot be registered", PRTE_ERROR == prte_set_job_data_object(dup));
    PMIX_RELEASE(dup);

    /* a freed job vacates its slot, and the next insert reuses it - the
     * array is a slot map, not an append-only log */
    saved_index = j1->index;
    PMIX_RELEASE(j1);
    CHECK("job: destruct vacates the global slot",
          NULL == pmix_pointer_array_get_item(prte_job_data, saved_index));
    CHECK("job: and the nspace no longer resolves", NULL == prte_get_job_data_object(NS("job.one")));

    j1 = make_job("job.three");
    CHECK("job: the vacated slot is reused", PRTE_SUCCESS == prte_set_job_data_object(j1));
    CHECK("job: reuse lands in the freed slot", saved_index == j1->index);

    reset_globals();
    return failures;
}

/* ------------------------------------------------------------------ */
/* sessions (reservations)                                            */
/* ------------------------------------------------------------------ */

static int test_session_registry(void)
{
    int failures = 0;
    prte_session_t *s1, *s2, *dup;

    reset_globals();

    s1 = PMIX_NEW(prte_session_t);
    s1->session_id = 100;
    s1->alloc_refid = strdup("alloc-100");
    s1->user_refid = strdup("user-100");
    CHECK("session: insert succeeds", PRTE_SUCCESS == prte_set_session_object(s1));
    CHECK("session: lookup by id", s1 == prte_get_session_object(100));
    CHECK("session: lookup by alloc refid", s1 == prte_get_session_object_from_id("alloc-100"));
    CHECK("session: lookup by user refid", s1 == prte_get_session_object_from_refid("user-100"));

    /* A reservation may carry an alloc id without a user reference id -
     * PMIX_ALLOC_REQ_ID is the requester's own optional label.  The refid
     * lookup used to strcasecmp against that NULL, so one such session made
     * every later lookup-by-refid a segfault.  The alloc-id lookup has
     * always guarded; both must. */
    s2 = PMIX_NEW(prte_session_t);
    s2->session_id = 200;
    s2->alloc_refid = strdup("alloc-200");
    /* user_refid deliberately left NULL */
    CHECK("session: second insert succeeds", PRTE_SUCCESS == prte_set_session_object(s2));
    CHECK("session: refid lookup survives a session with no user refid",
          s1 == prte_get_session_object_from_refid("user-100"));
    CHECK("session: refid lookup misses cleanly",
          NULL == prte_get_session_object_from_refid("user-999"));
    CHECK("session: alloc-id lookup still works alongside it",
          s2 == prte_get_session_object_from_id("alloc-200"));
    /* symmetrically, a session with no alloc_refid must not break that one */
    CHECK("session: id lookup misses cleanly", NULL == prte_get_session_object_from_id("nope"));

    /* NULL keys are a miss, not a crash */
    CHECK("session: NULL alloc id misses", NULL == prte_get_session_object_from_id(NULL));
    CHECK("session: NULL user refid misses", NULL == prte_get_session_object_from_refid(NULL));
    CHECK("session: unknown id misses", NULL == prte_get_session_object(4242));

    /* session ids are unique */
    dup = PMIX_NEW(prte_session_t);
    dup->session_id = 100;
    CHECK("session: duplicate id refused", PRTE_EXISTS == prte_set_session_object(dup));
    PMIX_RELEASE(dup);

    reset_globals();
    return failures;
}

static int test_session_ownership(void)
{
    int failures = 0;
    prte_session_t *s, *s2;
    prte_session_t *saved_default;

    reset_globals();
    saved_default = prte_default_session;

    s = PMIX_NEW(prte_session_t);
    s->session_id = 7;
    CHECK("owner: insert succeeds", PRTE_SUCCESS == prte_set_session_object(s));

    /* a fresh reservation has no owners, so nobody may spawn into it */
    CHECK("owner: an unowned reservation admits nobody", !prte_session_is_owned_by(s, NS("nsA")));

    prte_session_add_owner(s, NS("nsA"));
    CHECK("owner: the added namespace is admitted", prte_session_is_owned_by(s, NS("nsA")));
    CHECK("owner: others are still refused", !prte_session_is_owned_by(s, NS("nsB")));

    /* adding twice must not grow the list - owners accumulates one entry per
     * namespace spawned into the session, and a job can be spawned into the
     * same session repeatedly */
    prte_session_add_owner(s, NS("nsA"));
    CHECK("owner: re-adding is a no-op", 1 == PMIx_Argv_count(s->owners));
    prte_session_add_owner(s, NS("nsB"));
    CHECK("owner: a second namespace appends", 2 == PMIx_Argv_count(s->owners));
    CHECK("owner: and is admitted", prte_session_is_owned_by(s, NS("nsB")));

    /* the default session belongs to everyone and records no owners */
    prte_default_session = s;
    CHECK("owner: the default session admits anyone", prte_session_is_owned_by(s, NS("nsZ")));
    prte_session_add_owner(s, NS("nsZ"));
    CHECK("owner: the default session records nothing", 2 == PMIx_Argv_count(s->owners));
    prte_default_session = saved_default;

    /* a NULL session is the default pool */
    CHECK("owner: NULL session admits anyone", prte_session_is_owned_by(NULL, NS("nsZ")));

    /* An EMPTY namespace must never be recorded as an owner, and the case
     * that matters is a reservation whose owner list is still empty - with
     * entries present, the wildcard below makes the empty one look like a
     * duplicate and it is dropped by accident rather than on purpose.
     *
     * This is not hypothetical: a spawn request arrives with no namespace at
     * all (the HNP names the job later, in prte_plm_base_setup_job), so the
     * code that granted the spawned job ownership at request-vetting time
     * handed in exactly this.  An empty entry does not merely fail to
     * identify anybody - PMIx_Check_nspace treats an empty side as a
     * wildcard, so it matches EVERY namespace and retires the reservation's
     * ownership gate for good. */
    s2 = PMIX_NEW(prte_session_t);
    s2->session_id = 8;
    CHECK("owner: second insert succeeds", PRTE_SUCCESS == prte_set_session_object(s2));
    prte_session_add_owner(s2, NS(""));
    CHECK("owner: an empty namespace is not recorded", 0 == PMIx_Argv_count(s2->owners));
    CHECK("owner: an empty owner does not admit everybody",
          !prte_session_is_owned_by(s2, NS("nsC")));

    reset_globals();
    return failures;
}

/* ------------------------------------------------------------------ */
/* proc lookups                                                       */
/* ------------------------------------------------------------------ */

static int test_proc_lookups(void)
{
    int failures = 0;
    prte_job_t *jdata, *djob;
    prte_proc_t *proc, *daemon;
    prte_node_t *node;
    pmix_proc_t name, unknown;

    reset_globals();

    /* build a daemon on a node, and an application proc mapped to it */
    djob = make_job("prte-daemons");
    prte_set_job_data_object(djob);
    node = make_node("nodeA");
    node->index = pmix_pointer_array_add(prte_node_pool, node);

    daemon = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&daemon->name, "prte-daemons", 3);
    daemon->node = node; /* borrowed backpointer */
    PMIX_RETAIN(daemon); /* the node holds a real reference; the job holds one too */
    node->daemon = daemon;
    pmix_pointer_array_set_item(djob->procs, 3, daemon);

    jdata = make_job("app.job");
    prte_set_job_data_object(jdata);
    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, "app.job", 1);
    proc->node = node;
    proc->node_rank = 2;
    pmix_pointer_array_set_item(jdata->procs, 1, proc);

    PMIX_LOAD_PROCID(&name, "app.job", 1);
    CHECK("proc: lookup finds the proc", proc == prte_get_proc_object(&name));
    CHECK("proc: hostname resolves through the node",
          NULL != prte_get_proc_hostname(&name)
              && 0 == strcmp("nodeA", prte_get_proc_hostname(&name)));
    CHECK("proc: node rank is reported", 2 == prte_get_proc_node_rank(&name));
    CHECK("proc: the hosting daemon's vpid is reported",
          3 == prte_get_proc_daemon_vpid(&name));

    /* a rank the job does not have */
    PMIX_LOAD_PROCID(&unknown, "app.job", 99);
    CHECK("proc: unknown rank misses", NULL == prte_get_proc_object(&unknown));
    CHECK("proc: unknown rank has no hostname", NULL == prte_get_proc_hostname(&unknown));
    CHECK("proc: unknown rank has no daemon",
          PMIX_RANK_INVALID == prte_get_proc_daemon_vpid(&unknown));

    /* a job that does not exist */
    PMIX_LOAD_PROCID(&unknown, "no.such.job", 0);
    CHECK("proc: unknown job misses", NULL == prte_get_proc_object(&unknown));
    CHECK("proc: unknown job has no daemon",
          PMIX_RANK_INVALID == prte_get_proc_daemon_vpid(&unknown));

    /* an unmapped proc has no node, so neither hostname nor daemon resolve */
    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, "app.job", 2);
    pmix_pointer_array_set_item(jdata->procs, 2, proc);
    PMIX_LOAD_PROCID(&name, "app.job", 2);
    CHECK("proc: an unmapped proc has no hostname", NULL == prte_get_proc_hostname(&name));
    CHECK("proc: an unmapped proc has no daemon",
          PMIX_RANK_INVALID == prte_get_proc_daemon_vpid(&name));

    reset_globals();
    return failures;
}

/* ------------------------------------------------------------------ */
/* node matching                                                      */
/* ------------------------------------------------------------------ */

static int test_node_matching(void)
{
    int failures = 0;
    prte_node_t *n1, *n2, *found;
    pmix_list_t nodes;
    pmix_pointer_array_t *saved_pool;

    reset_globals();

    n1 = make_node("compute-01");
    PMIx_Argv_append_nosize(&n1->aliases, "c01");
    PMIx_Argv_append_nosize(&n1->aliases, "compute-01.cluster.example");
    n2 = make_node("compute-02");

    /* search a caller-supplied list */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    pmix_list_append(&nodes, &n1->super);
    pmix_list_append(&nodes, &n2->super);

    CHECK("match: by primary name", n1 == prte_node_match(&nodes, "compute-01"));
    CHECK("match: by short alias", n1 == prte_node_match(&nodes, "c01"));
    CHECK("match: by fqdn alias",
          n1 == prte_node_match(&nodes, "compute-01.cluster.example"));
    CHECK("match: the other node still resolves", n2 == prte_node_match(&nodes, "compute-02"));
    CHECK("match: an unknown name misses", NULL == prte_node_match(&nodes, "compute-99"));
    CHECK("match: a NULL name misses", NULL == prte_node_match(&nodes, NULL));

    /* the same node object answers to all of its names */
    CHECK("match: nptr_match on identical names", prte_nptr_match(n1, n1));
    CHECK("match: nptr_match rejects different nodes", !prte_nptr_match(n1, n2));
    CHECK("match: quickmatch on the primary name", prte_quickmatch(n1, "compute-01"));
    CHECK("match: quickmatch on an alias", prte_quickmatch(n1, "c01"));
    CHECK("match: quickmatch rejects a stranger", !prte_quickmatch(n1, "compute-42"));

    /* an alias on one side matches the other's primary name */
    {
        prte_node_t *n3 = make_node("c01");
        CHECK("match: nptr_match through an alias", prte_nptr_match(n1, n3));
        PMIX_RELEASE(n3);
    }

    pmix_list_remove_first(&nodes);
    pmix_list_remove_first(&nodes);
    PMIX_DESTRUCT(&nodes);

    /* now search the global pool */
    n1->index = pmix_pointer_array_add(prte_node_pool, n1);
    n2->index = pmix_pointer_array_add(prte_node_pool, n2);
    found = prte_node_match(NULL, "compute-02");
    CHECK("match: global pool by name", n2 == found);
    CHECK("match: global pool by alias", n1 == prte_node_match(NULL, "c01"));
    CHECK("match: global pool miss", NULL == prte_node_match(NULL, "compute-99"));

    /* prte_node_match(NULL, ...) is reachable from parsing paths that run
     * before prte_init has built the global arrays.  It used to walk
     * prte_node_pool->size on a NULL pool. */
    saved_pool = prte_node_pool;
    prte_node_pool = NULL;
    CHECK("match: an unbuilt node pool misses instead of crashing",
          NULL == prte_node_match(NULL, "compute-01"));
    prte_node_pool = saved_pool;

    reset_globals();
    return failures;
}

/* ------------------------------------------------------------------ */
/* copy functions                                                     */
/* ------------------------------------------------------------------ */

static int test_node_copy(void)
{
    int failures = 0;
    prte_node_t *src, *cpy = NULL;
    prte_session_t *sess;
    char *user = NULL;

    reset_globals();

    sess = PMIX_NEW(prte_session_t);
    sess->session_id = 55;

    src = make_node("bigmem-07");
    PMIx_Argv_append_nosize(&src->aliases, "bm07");
    src->rawname = strdup("bigmem-07.raw");
    src->slots = 64;
    src->slots_max = 128;
    src->slots_available = 60;
    src->session = sess;
    prte_set_attribute(&src->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_LOCAL,
                       "operator", PMIX_STRING);

    CHECK("nodecopy: copy succeeds", PRTE_SUCCESS == prte_node_copy(&cpy, src));
    if (NULL == cpy) {
        PMIX_RELEASE(src);
        PMIX_RELEASE(sess);
        return failures + 1;
    }
    CHECK("nodecopy: name carried", NULL != cpy->name && 0 == strcmp("bigmem-07", cpy->name));
    CHECK("nodecopy: slots carried", 64 == cpy->slots && 128 == cpy->slots_max);
    CHECK("nodecopy: slots_available carried", 60 == cpy->slots_available);
    /* the three that were being dropped */
    CHECK("nodecopy: rawname carried",
          NULL != cpy->rawname && 0 == strcmp("bigmem-07.raw", cpy->rawname));
    CHECK("nodecopy: aliases carried",
          NULL != cpy->aliases && 1 == PMIx_Argv_count(cpy->aliases)
              && 0 == strcmp("bm07", cpy->aliases[0]));
    CHECK("nodecopy: the copy answers to the alias too", prte_quickmatch(cpy, "bm07"));
    CHECK("nodecopy: session carried", sess == cpy->session);
    CHECK("nodecopy: attributes carried",
          prte_get_attribute(&cpy->attributes, PRTE_NODE_USERNAME, (void **) &user, PMIX_STRING)
              && NULL != user && 0 == strcmp("operator", user));
    free(user);

    /* the copy owns its own strings */
    CHECK("nodecopy: name is not shared", cpy->name != src->name);
    CHECK("nodecopy: the copy has no daemon of its own", NULL == cpy->daemon);

    PMIX_RELEASE(cpy);
    PMIX_RELEASE(src);
    PMIX_RELEASE(sess);
    reset_globals();
    return failures;
}

static int test_app_copy(void)
{
    int failures = 0;
    prte_app_context_t *src, *cpy = NULL;
    char *ppr = NULL;
    bool bval;

    src = PMIX_NEW(prte_app_context_t);
    src->idx = 2;
    src->app = strdup("/bin/true");
    src->num_procs = 8;
    src->first_rank = 4;
    src->cwd = strdup("/tmp/work");
    PMIx_Argv_append_nosize(&src->argv, "true");
    PMIx_Argv_append_nosize(&src->argv, "--flag");
    PMIx_Argv_append_nosize(&src->env, "FOO=bar");
    PRTE_FLAG_SET(src, PRTE_APP_FLAG_USED_ON_NODE);
    prte_set_attribute(&src->attributes, PRTE_APP_PMIX_PREFIX, PRTE_ATTR_GLOBAL,
                       "/opt/prte", PMIX_STRING);
    prte_set_attribute(&src->attributes, PRTE_APP_PRELOAD_BIN, PRTE_ATTR_GLOBAL,
                       NULL, PMIX_BOOL);

    CHECK("appcopy: copy succeeds", PRTE_SUCCESS == prte_app_copy(&cpy, src));
    if (NULL == cpy) {
        PMIX_RELEASE(src);
        return failures + 1;
    }
    CHECK("appcopy: idx carried", 2 == cpy->idx);
    CHECK("appcopy: app carried", NULL != cpy->app && 0 == strcmp("/bin/true", cpy->app));
    CHECK("appcopy: num_procs carried", 8 == cpy->num_procs);
    CHECK("appcopy: first_rank carried", 4 == cpy->first_rank);
    CHECK("appcopy: flags carried", PRTE_FLAG_TEST(cpy, PRTE_APP_FLAG_USED_ON_NODE));
    CHECK("appcopy: argv carried",
          2 == PMIx_Argv_count(cpy->argv) && 0 == strcmp("--flag", cpy->argv[1]));
    CHECK("appcopy: env carried", 1 == PMIx_Argv_count(cpy->env));
    CHECK("appcopy: cwd carried", NULL != cpy->cwd && 0 == strcmp("/tmp/work", cpy->cwd));

    /* Attributes were being copied through the wrong struct type, which
     * dropped the key: the copied list had the right length but nothing in
     * it could be looked up. */
    CHECK("appcopy: attribute count matches",
          pmix_list_get_size(&src->attributes) == pmix_list_get_size(&cpy->attributes));
    CHECK("appcopy: a string attribute is findable BY KEY",
          prte_get_attribute(&cpy->attributes, PRTE_APP_PMIX_PREFIX, (void **) &ppr, PMIX_STRING));
    CHECK("appcopy: ...with its value intact", NULL != ppr && 0 == strcmp("/opt/prte", ppr));
    free(ppr);
    bval = prte_get_attribute(&cpy->attributes, PRTE_APP_PRELOAD_BIN, NULL, PMIX_BOOL);
    CHECK("appcopy: a bool attribute is findable by key", bval);
    /* and nothing invented itself */
    CHECK("appcopy: an unset attribute is still unset",
          !prte_get_attribute(&cpy->attributes, PRTE_APP_NO_CACHEDIR, NULL, PMIX_BOOL));

    PMIX_RELEASE(cpy);
    PMIX_RELEASE(src);
    return failures;
}

static int test_map_copy(void)
{
    int failures = 0;
    prte_job_map_t *src, *cpy = NULL;
    prte_node_t *nodes[PRTE_GLOBAL_ARRAY_BLOCK_SIZE * 2];
    const int nnodes = PRTE_GLOBAL_ARRAY_BLOCK_SIZE * 2;
    int i, live;
    char name[64];

    src = PMIX_NEW(prte_job_map_t);
    src->mapping = 3;
    src->ranking = 5;
    src->binding = 7;
    src->num_new_daemons = 11;
    src->daemon_vpid_start = 13;
    src->num_nodes = nnodes;

    /* Deliberately more nodes than PRTE_GLOBAL_ARRAY_BLOCK_SIZE, which is
     * what the destination map's array is constructed with.  The old copy
     * wrote src->nodes->size entries into that fixed allocation. */
    for (i = 0; i < nnodes; i++) {
        snprintf(name, sizeof(name), "n%03d", i);
        nodes[i] = make_node(name);
        pmix_pointer_array_set_item(src->nodes, i, nodes[i]);
        /* the map holds a reference of its own; keep ours as well so we can
         * check the refcount arithmetic below */
        PMIX_RETAIN(nodes[i]);
    }

    CHECK("mapcopy: copy succeeds", PRTE_SUCCESS == prte_map_copy((struct prte_job_map_t **) &cpy,
                                                                  (struct prte_job_map_t *) src));
    if (NULL == cpy) {
        for (i = 0; i < nnodes; i++) {
            PMIX_RELEASE(nodes[i]);
        }
        PMIX_RELEASE(src);
        return failures + 1;
    }

    CHECK("mapcopy: policies carried",
          3 == cpy->mapping && 5 == cpy->ranking && 7 == cpy->binding);
    CHECK("mapcopy: daemon counts carried",
          11 == cpy->num_new_daemons && 13 == cpy->daemon_vpid_start);
    CHECK("mapcopy: num_nodes carried", nnodes == (int) cpy->num_nodes);
    /* the mapper names were not being copied at all */

    /* every node came across, including the ones past the initial block */
    live = 0;
    for (i = 0; i < nnodes; i++) {
        if (nodes[i] == (prte_node_t *) pmix_pointer_array_get_item(cpy->nodes, i)) {
            live++;
        }
    }
    CHECK("mapcopy: every node crossed, including past the first block",
          nnodes == live);
    CHECK("mapcopy: the destination array grew to fit", cpy->nodes->size >= nnodes);
    CHECK("mapcopy: the source array was not disturbed",
          nodes[nnodes - 1]
              == (prte_node_t *) pmix_pointer_array_get_item(src->nodes, nnodes - 1));

    /* Releasing both maps must leave our own references intact.  With no
     * retain in the copy, the second map's destructor dropped a reference it
     * never took and the nodes went away underneath us. */
    PMIX_RELEASE(cpy);
    PMIX_RELEASE(src);
    live = 0;
    for (i = 0; i < nnodes; i++) {
        /* if the refcounts were balanced these are all still ours */
        if (NULL != nodes[i]->name && 'n' == nodes[i]->name[0]) {
            live++;
        }
        PMIX_RELEASE(nodes[i]);
    }
    CHECK("mapcopy: both maps released without dropping the caller's refs",
          nnodes == live);

    /* a NULL map copies to NULL rather than allocating */
    cpy = (prte_job_map_t *) 0x1;
    CHECK("mapcopy: NULL source yields NULL",
          PRTE_SUCCESS == prte_map_copy((struct prte_job_map_t **) &cpy, NULL) && NULL == cpy);

    return failures;
}

/* ------------------------------------------------------------------ */
/* pack / unpack round trips                                          */
/* ------------------------------------------------------------------ */

static int test_pack_roundtrip(void)
{
    int failures = 0;
    pmix_data_buffer_t buf;
    prte_node_t *wnode;
    prte_proc_t *wdmn;
    prte_job_t *src, *dst = NULL;
    prte_app_context_t *app, *app2;
    prte_proc_t *proc;
    prte_job_map_t *saved_map;
    prte_job_pack_mode_t mode = PRTE_JOB_PACK_NO_CPUSETS;
    char *sval = NULL;
    int rc;

    reset_globals();

    src = make_job("wire.job");
    PMIX_LOAD_NSPACE(src->launcher, "the.launcher");
    src->num_procs = 2;
    src->offset = 16;
    src->stdin_target = 1;
    src->total_slots_alloc = 48;
    src->state = PRTE_JOB_STATE_RUNNING;
    PMIx_Argv_append_nosize(&src->personality, "prte");
    PMIx_Argv_append_nosize(&src->personality, "ompi");
    /* only PRTE_ATTR_GLOBAL attributes go on the wire - that asymmetry is
     * load-bearing (the mapper reads an unpacked copy of the job), so test
     * both dispositions */
    prte_set_attribute(&src->attributes, PRTE_JOB_PPR, PRTE_ATTR_GLOBAL, "2:node", PMIX_STRING);
    prte_set_attribute(&src->attributes, PRTE_JOB_DO_NOT_LAUNCH, PRTE_ATTR_LOCAL, NULL, PMIX_BOOL);

    app = PMIX_NEW(prte_app_context_t);
    app->idx = 0;
    app->app = strdup("/bin/hostname");
    app->num_procs = 2;
    app->first_rank = 0;
    app->cwd = strdup("/home/user");
    PMIx_Argv_append_nosize(&app->argv, "hostname");
    PMIx_Argv_append_nosize(&app->argv, "-f");
    PMIx_Argv_append_nosize(&app->env, "PATH=/bin");
    prte_set_attribute(&app->attributes, PRTE_APP_PMIX_PREFIX, PRTE_ATTR_GLOBAL,
                       "/opt/prte", PMIX_STRING);
    pmix_pointer_array_set_item(src->apps, 0, app);
    src->num_apps = 1;

    /* The placement no longer travels as a per-proc array: it travels as a
     * node map plus a proc map per app, and the receiver rebuilds each
     * proc's rank, hosting daemon, app, app rank and local rank from those.
     * So this needs a real map - nodes that are in the global pool (that is
     * what the far end resolves names against) and that carry a daemon (that
     * is where "parent" comes from) - rather than a bare num_nodes. */
    wnode = make_node("wire.node1");
    wnode->index = pmix_pointer_array_add(prte_node_pool, wnode);
    wdmn = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&wdmn->name, "wire.dvm", 1);
    wnode->daemon = wdmn;   /* the node holds a counted reference */

    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, "wire.job", 0);
    proc->parent = 1;
    proc->local_rank = 0;
    proc->node_rank = 0;
    proc->app_rank = 0;
    proc->app_idx = 0;
    proc->state = PRTE_PROC_STATE_RUNNING;
    proc->cpuset = strdup("0-3");
    pmix_pointer_array_set_item(src->procs, 0, proc);
    PMIX_RETAIN(proc);
    pmix_pointer_array_add(wnode->procs, proc);

    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, "wire.job", 1);
    proc->parent = 1;
    proc->local_rank = 1;
    proc->node_rank = 1;
    proc->app_rank = 1;
    proc->app_idx = 0;
    proc->state = PRTE_PROC_STATE_RUNNING;
    pmix_pointer_array_set_item(src->procs, 1, proc);
    PMIX_RETAIN(proc);
    pmix_pointer_array_add(wnode->procs, proc);

    src->map = PMIX_NEW(prte_job_map_t);
    src->map->mapping = 9;
    src->map->ranking = 4;
    src->map->binding = 6;
    PMIX_RETAIN(wnode);
    pmix_pointer_array_add(src->map->nodes, wnode);
    src->map->num_nodes = 1;

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = prte_job_pack(&buf, src, PRTE_JOB_PACK_ALL);
    CHECK("wire: job packs", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        rc = prte_job_unpack(&buf, &dst, &mode);
        CHECK("wire: job unpacks", PRTE_SUCCESS == rc && NULL != dst);
        CHECK("wire: the pack mode round-trips", PRTE_JOB_PACK_ALL == mode);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    if (NULL != dst) {
        CHECK("wire: nspace round-trips", PMIX_CHECK_NSPACE(dst->nspace, "wire.job"));
        CHECK("wire: launcher round-trips", PMIX_CHECK_NSPACE(dst->launcher, "the.launcher"));
        CHECK("wire: num_procs round-trips", 2 == dst->num_procs);
        CHECK("wire: offset round-trips", 16 == dst->offset);
        CHECK("wire: stdin_target round-trips", 1 == dst->stdin_target);
        CHECK("wire: total_slots round-trips", 48 == dst->total_slots_alloc);
        CHECK("wire: state round-trips", PRTE_JOB_STATE_RUNNING == dst->state);
        CHECK("wire: personality round-trips",
              2 == PMIx_Argv_count(dst->personality)
                  && 0 == strcmp("ompi", dst->personality[1]));
        CHECK("wire: a GLOBAL attribute crosses",
              prte_get_attribute(&dst->attributes, PRTE_JOB_PPR, (void **) &sval, PMIX_STRING)
                  && NULL != sval && 0 == strcmp("2:node", sval));
        free(sval);
        sval = NULL;
        /* a LOCAL attribute deliberately does not */
        CHECK("wire: a LOCAL attribute does not cross",
              !prte_get_attribute(&dst->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL));

        CHECK("wire: num_apps round-trips", 1 == dst->num_apps);
        app2 = (prte_app_context_t *) pmix_pointer_array_get_item(dst->apps, 0);
        CHECK("wire: the app crossed", NULL != app2);
        if (NULL != app2) {
            CHECK("wire: app name round-trips",
                  NULL != app2->app && 0 == strcmp("/bin/hostname", app2->app));
            CHECK("wire: app num_procs round-trips", 2 == app2->num_procs);
            CHECK("wire: app argv round-trips",
                  2 == PMIx_Argv_count(app2->argv) && 0 == strcmp("-f", app2->argv[1]));
            CHECK("wire: app env round-trips", 1 == PMIx_Argv_count(app2->env));
            CHECK("wire: app cwd round-trips",
                  NULL != app2->cwd && 0 == strcmp("/home/user", app2->cwd));
            CHECK("wire: app attribute round-trips",
                  prte_get_attribute(&app2->attributes, PRTE_APP_PMIX_PREFIX,
                                     (void **) &sval, PMIX_STRING)
                      && NULL != sval && 0 == strcmp("/opt/prte", sval));
            free(sval);
            sval = NULL;
        }

        proc = (prte_proc_t *) pmix_pointer_array_get_item(dst->procs, 0);
        CHECK("wire: proc 0 crossed", NULL != proc);
        if (NULL != proc) {
            CHECK("wire: proc name round-trips",
                  PMIX_CHECK_NSPACE(proc->name.nspace, "wire.job") && 0 == proc->name.rank);
            CHECK("wire: proc parent round-trips", 1 == proc->parent);
            CHECK("wire: proc cpuset round-trips",
                  NULL != proc->cpuset && 0 == strcmp("0-3", proc->cpuset));
            CHECK("wire: proc state round-trips", PRTE_PROC_STATE_RUNNING == proc->state);
        }
        proc = (prte_proc_t *) pmix_pointer_array_get_item(dst->procs, 1);
        CHECK("wire: proc 1 crossed and has no cpuset",
              NULL != proc && NULL == proc->cpuset && 1 == proc->local_rank);
        /* Nothing of proc 1's identity or placement was on the wire - all of
         * it is rebuilt from the node map and the app's proc map. */
        CHECK("wire: proc 1 takes the job's nspace",
              NULL != proc && PMIX_CHECK_NSPACE(proc->name.nspace, "wire.job")
                  && 1 == proc->name.rank);
        CHECK("wire: proc 1's daemon is derived from the node map",
              NULL != proc && 1 == proc->parent);
        CHECK("wire: proc 1's app and app rank are derived",
              NULL != proc && 0 == proc->app_idx && 1 == proc->app_rank);
        CHECK("wire: proc 1's node rank still travels",
              NULL != proc && 1 == proc->node_rank);

        CHECK("wire: the map crossed", NULL != dst->map);
        if (NULL != dst->map) {
            CHECK("wire: map policies round-trip",
                  9 == dst->map->mapping && 4 == dst->map->ranking && 6 == dst->map->binding);
            CHECK("wire: map num_nodes round-trips", 1 == dst->map->num_nodes);
        }
        PMIX_RELEASE(dst);
    }

    /* a job with no map packs a flag instead, and unpacks with map == NULL */
    saved_map = src->map;
    PMIX_RETAIN(saved_map);
    PMIX_RELEASE(src->map);
    src->map = NULL;
    dst = NULL;
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    if (PRTE_SUCCESS == prte_job_pack(&buf, src, PRTE_JOB_PACK_ALL)) {
        rc = prte_job_unpack(&buf, &dst, NULL);
        CHECK("wire: a mapless job round-trips", PRTE_SUCCESS == rc && NULL != dst);
        if (NULL != dst) {
            CHECK("wire: ...and still has no map", NULL == dst->map);
            PMIX_RELEASE(dst);
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* The launch path's shape: the same job with the cpusets left out for
     * the scatter.  Everything else has to arrive exactly as before, and
     * the mode has to say so - a decoder that guessed wrong here would
     * read the next proc's node rank as this one's cpuset. */
    PMIX_RETAIN(saved_map);
    src->map = saved_map;
    dst = NULL;
    mode = PRTE_JOB_PACK_ALL;
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    if (PRTE_SUCCESS == prte_job_pack(&buf, src, PRTE_JOB_PACK_NO_CPUSETS)) {
        rc = prte_job_unpack(&buf, &dst, &mode);
        CHECK("wire: a cpuset-less job round-trips", PRTE_SUCCESS == rc && NULL != dst);
        CHECK("wire: ...and says so", PRTE_JOB_PACK_NO_CPUSETS == mode);
        if (NULL != dst) {
            proc = (prte_proc_t *) pmix_pointer_array_get_item(dst->procs, 0);
            CHECK("wire: ...the bound proc arrives with no cpuset",
                  NULL != proc && NULL == proc->cpuset);
            CHECK("wire: ...and the fields after it are still intact",
                  NULL != proc && PRTE_PROC_STATE_RUNNING == proc->state
                      && 2 == dst->num_procs && 1 == dst->stdin_target
                      && PRTE_JOB_STATE_RUNNING == dst->state
                      && PMIX_CHECK_NSPACE(dst->launcher, "the.launcher"));
            PMIX_RELEASE(dst);
        }
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIX_RELEASE(saved_map);

    PMIX_RELEASE(src);
    reset_globals();
    return failures;
}

static int test_node_pack_roundtrip(void)
{
    int failures = 0;
    pmix_data_buffer_t buf;
    prte_node_t *src, *dst = NULL;
    char *sval = NULL;
    int rc;

    src = make_node("wire-node");
    src->num_procs = 6;
    src->state = PRTE_NODE_STATE_UP;
    PRTE_FLAG_SET(src, PRTE_NODE_FLAG_OVERSUBSCRIBED);
    prte_set_attribute(&src->attributes, PRTE_NODE_USERNAME, PRTE_ATTR_GLOBAL,
                       "svc", PMIX_STRING);
    prte_set_attribute(&src->attributes, PRTE_NODE_SERIAL_NUMBER, PRTE_ATTR_LOCAL, "wn", PMIX_STRING);

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = prte_node_pack(&buf, src);
    CHECK("wire: node packs", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        rc = prte_node_unpack(&buf, &dst);
        CHECK("wire: node unpacks", PRTE_SUCCESS == rc && NULL != dst);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    if (NULL != dst) {
        CHECK("wire: node name round-trips",
              NULL != dst->name && 0 == strcmp("wire-node", dst->name));
        CHECK("wire: node num_procs round-trips", 6 == dst->num_procs);
        CHECK("wire: node state round-trips", PRTE_NODE_STATE_UP == dst->state);
        CHECK("wire: the oversubscribed flag round-trips",
              PRTE_FLAG_TEST(dst, PRTE_NODE_FLAG_OVERSUBSCRIBED));
        CHECK("wire: a GLOBAL node attribute crosses",
              prte_get_attribute(&dst->attributes, PRTE_NODE_USERNAME, (void **) &sval,
                                 PMIX_STRING)
                  && NULL != sval && 0 == strcmp("svc", sval));
        free(sval);
        CHECK("wire: a LOCAL node attribute does not cross",
              !prte_get_attribute(&dst->attributes, PRTE_NODE_SERIAL_NUMBER, NULL, PMIX_STRING));
        PMIX_RELEASE(dst);
    }

    PMIX_RELEASE(src);
    return failures;
}

/* ------------------------------------------------------------------ */
/* object lifetimes                                                   */
/* ------------------------------------------------------------------ */

static int test_object_lifetimes(void)
{
    int failures = 0;
    prte_node_t *node;
    prte_proc_t *proc, *daemon;

    reset_globals();

    /* A proc's node backpointer is BORROWED - retaining it would close a
     * cycle with node->procs / node->daemon and nothing would ever be freed.
     * The node clears the backpointer on the procs it knows about before it
     * goes away, so a proc that outlives its node is left pointing at NULL
     * rather than at freed memory. */
    node = make_node("lifetime-node");
    proc = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&proc->name, "life.job", 0);
    proc->node = node;
    pmix_pointer_array_set_item(node->procs, 0, proc);
    PMIX_RETAIN(proc); /* our own reference, so the proc outlives the node */

    daemon = PMIX_NEW(prte_proc_t);
    PMIX_LOAD_PROCID(&daemon->name, "prte-daemons", 1);
    daemon->node = node;
    node->daemon = daemon;

    PMIX_RELEASE(node);
    CHECK("lifetime: a surviving proc's node backpointer was cleared", NULL == proc->node);
    PMIX_RELEASE(proc);

    /* the launch-fence campaign objects zero their own pointer fields:
     * PMIX_NEW does not, and the destructor free()s them */
    {
        prte_shrink_campaign_t *sc = PMIX_NEW(prte_shrink_campaign_t);
        prte_grow_campaign_t *gc = PMIX_NEW(prte_grow_campaign_t);

        CHECK("lifetime: shrink campaign starts empty",
              NULL == sc->targets && NULL == sc->alloc_id && NULL == sc->req_id
                  && 0 == sc->ntargets && !sc->have_requester);
        CHECK("lifetime: grow campaign starts empty",
              NULL == gc->targets && NULL == gc->requesters
                  && 0 == gc->ntargets && 0 == gc->nrequesters);
        /* destructing an untouched campaign must not free garbage */
        PMIX_RELEASE(sc);
        PMIX_RELEASE(gc);
    }

    /* a fresh session starts with an invalid id and no owners */
    {
        prte_session_t *s = PMIX_NEW(prte_session_t);

        CHECK("lifetime: a fresh session has no owners", NULL == s->owners);
        CHECK("lifetime: a fresh session has no index", -1 == s->index);
        CHECK("lifetime: a fresh session has the default inheritance",
              PRTE_INHERIT_DEFAULT_VALUE == s->inheritance);
        PMIX_RELEASE(s);
    }

    reset_globals();
    return failures;
}

/* The teardown ordering prte_finalize depends on.  A reservation holds a
 * COUNTED reference on each of its nodes while the global pool holds its own,
 * so releasing the reservation must hand back exactly one reference and leave
 * the node standing.  Getting this wrong is a double free at every DVM exit,
 * which is why it is pinned here rather than left to the swarm. */
static int test_session_teardown(void)
{
    int failures = 0;
    prte_session_t *sess;
    prte_node_t *n1, *n2;
    int i, live;

    reset_globals();

    n1 = make_node("res-01");
    n2 = make_node("res-02");
    n1->index = pmix_pointer_array_add(prte_node_pool, n1);
    n2->index = pmix_pointer_array_add(prte_node_pool, n2);

    sess = PMIX_NEW(prte_session_t);
    sess->session_id = 900;
    CHECK("teardown: session registered", PRTE_SUCCESS == prte_set_session_object(sess));

    /* this is what create_reservation does: retain, then hand to the session */
    PMIX_RETAIN(n1);
    pmix_pointer_array_add(sess->nodes, n1);
    n1->session = sess;
    PMIX_RETAIN(n2);
    pmix_pointer_array_add(sess->nodes, n2);
    n2->session = sess;
    CHECK("teardown: the nodes point back at their reservation",
          sess == n1->session && sess == n2->session);

    /* release the reservation - the pool still holds both nodes */
    pmix_pointer_array_set_item(prte_sessions, sess->index, NULL);
    PMIX_RELEASE(sess);

    live = 0;
    for (i = 0; i < prte_node_pool->size; i++) {
        if (NULL != pmix_pointer_array_get_item(prte_node_pool, i)) {
            live++;
        }
    }
    CHECK("teardown: releasing a reservation leaves its nodes in the pool", 2 == live);
    /* ...and the borrowed backpointer was cleared rather than left dangling */
    CHECK("teardown: the nodes' session backpointer was cleared",
          NULL == n1->session && NULL == n2->session);
    /* the nodes are still usable, which they would not be after a double free */
    CHECK("teardown: the surviving nodes are intact",
          NULL != n1->name && 0 == strcmp("res-01", n1->name)
              && NULL != n2->name && 0 == strcmp("res-02", n2->name));

    reset_globals();

    /* The default session is the other half of the ordering: its node array
     * IS prte_node_pool, so releasing it performs the pool teardown.  Model
     * that exactly as prte_init builds it. */
    {
        pmix_pointer_array_t *pool;
        prte_session_t *dflt;
        prte_node_t *nd;

        pool = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(pool, 8, INT_MAX, 8);
        nd = make_node("pool-01");
        nd->index = pmix_pointer_array_add(pool, nd);

        dflt = PMIX_NEW(prte_session_t);
        dflt->session_id = UINT32_MAX;
        PMIX_RELEASE(dflt->nodes);
        dflt->nodes = pool;
        CHECK("teardown: the default session registered",
              PRTE_SUCCESS == prte_set_session_object(dflt));
        CHECK("teardown: the default session carries the pool", pool == dflt->nodes);

        /* Releasing it must take the pool and its nodes with it, and must not
         * trip over the substituted array.  There is nothing to inspect
         * afterwards - both the array and the node are gone - so the
         * assertion is that we reach the next statement at all: a double
         * free or a destructor run against the wrong parent class aborts
         * here instead. */
        pmix_pointer_array_set_item(prte_sessions, dflt->index, NULL);
        PMIX_RELEASE(dflt);
        CHECK("teardown: the default session took the node pool with it, intact",
              NULL == pmix_pointer_array_get_item(prte_sessions, 0));
    }

    reset_globals();
    return failures;
}

static int test_exit_status(void)
{
    int failures = 0;
    int saved = prte_exit_status;

    PRTE_RESET_EXIT_STATUS();
    CHECK("exit: reset clears the status", 0 == prte_exit_status);

    /* the first non-zero status wins: subsequent processes are killed as a
     * consequence of the first failure, and reporting their status instead
     * would hide the cause */
    PRTE_UPDATE_EXIT_STATUS(17);
    CHECK("exit: the first failure is recorded", 17 == prte_exit_status);
    PRTE_UPDATE_EXIT_STATUS(9);
    CHECK("exit: a later failure does not overwrite it", 17 == prte_exit_status);
    PRTE_UPDATE_EXIT_STATUS(0);
    CHECK("exit: a success does not clear it", 17 == prte_exit_status);
    PRTE_RESET_EXIT_STATUS();
    CHECK("exit: reset clears it again", 0 == prte_exit_status);
    /* ...and a zero into a zero stays zero */
    PRTE_UPDATE_EXIT_STATUS(0);
    CHECK("exit: zero over zero is zero", 0 == prte_exit_status);

    prte_exit_status = saved;
    return failures;
}

/* ------------------------------------------------------------------ */
/* data server                                                        */
/* ------------------------------------------------------------------ */

static int test_data_server_objects(void)
{
    int failures = 0;
    prte_data_object_t *obj;
    prte_data_req_t *req;

    /* PMIX_NEW does not zero what it allocates, so anything the access
     * checks read has to be set by the constructor.  proxy on both objects
     * is exactly that: it is what PMIX_RANGE_LOCAL compares. */
    obj = PMIX_NEW(prte_data_object_t);
    CHECK("ds: a new data object has an initialized proxy",
          PMIX_RANK_UNDEF == obj->proxy.rank && 0 == strlen(obj->proxy.nspace));
    CHECK("ds: a new data object has an initialized owner",
          PMIX_RANK_UNDEF == obj->owner.rank && 0 == strlen(obj->owner.nspace));
    CHECK("ds: a new data object defaults to session range",
          PMIX_RANGE_SESSION == obj->range);
    /* PMIx adds no persistence of its own before handing a publish to the
     * host, so the constructor's value is the one that governs.  It is
     * deliberately NOT the Standard's default of PMIX_PERSIST_APP: APP
     * means the publishing process's APPLICATION, and an MPMD job's
     * applications need not end together, so applying that literally would
     * shorten the retention every unmarked publish has been getting.
     * NSPACE is that same lifetime, said out loud. */
    CHECK("ds: a new data object defaults to nspace persistence",
          PMIX_PERSIST_NSPACE == obj->persistence);
    CHECK("ds: a new data object has no uid", UINT32_MAX == obj->uid);
    /* neither of the two lifetimes a proc name cannot express is known
     * until a publisher is resolved against its own job */
    CHECK("ds: a new data object has no application", UINT32_MAX == obj->app_idx);
    CHECK("ds: a new data object has no session", UINT32_MAX == obj->session_id);
    /* the retention timeout reads this, and PMIX_NEW does not zero what it
     * hands back - an unstamped item would be arbitrarily old or arbitrarily
     * young depending on the heap */
    CHECK("ds: a new data object has a stamped access time", 0 != obj->last_access);
    PMIX_RELEASE(obj);

    req = PMIX_NEW(prte_data_req_t);
    CHECK("ds: a new request has an initialized proxy",
          PMIX_RANK_UNDEF == req->proxy.rank && 0 == strlen(req->proxy.nspace));
    CHECK("ds: a new request has an initialized requestor",
          PMIX_RANK_UNDEF == req->requestor.rank && 0 == strlen(req->requestor.nspace));
    CHECK("ds: a new request has no keys", NULL == req->keys);
    /* the destructor free()s keys - it must not free garbage */
    PMIX_RELEASE(req);

    return failures;
}

/* The persistence ordering ds_purge applies when a lifetime ends.  Worth
 * pinning down because the values are NOT a usable numeric ladder:
 * PMIX_PERSIST_INDEF is 0 and outlives every other value, so any attempt to
 * decide this with a comparison gets it exactly backwards. */
static int test_data_server_persistence(void)
{
    int failures = 0;

    /* no horizon: an explicit PMIx_Unpublish(NULL, ...) takes everything
     * the caller published, however long it asked for it to be kept */
    CHECK("persist: no horizon takes INDEF",
          prte_data_server_expires_by(PMIX_PERSIST_INDEF, PMIX_PERSIST_INVALID));
    CHECK("persist: no horizon takes SESSION",
          prte_data_server_expires_by(PMIX_PERSIST_SESSION, PMIX_PERSIST_INVALID));

    CHECK("persist: no horizon takes FIRST_READ",
          prte_data_server_expires_by(PMIX_PERSIST_FIRST_READ, PMIX_PERSIST_INVALID));

    /* a process ended */
    CHECK("persist: PROC goes at the PROC horizon",
          prte_data_server_expires_by(PMIX_PERSIST_PROC, PMIX_PERSIST_PROC));
    CHECK("persist: APP stays at the PROC horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_APP, PMIX_PERSIST_PROC));
    CHECK("persist: SESSION stays at the PROC horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_SESSION, PMIX_PERSIST_PROC));

    /* an application ended - what a terminating job reports */
    CHECK("persist: PROC goes at the APP horizon",
          prte_data_server_expires_by(PMIX_PERSIST_PROC, PMIX_PERSIST_APP));
    CHECK("persist: APP goes at the APP horizon",
          prte_data_server_expires_by(PMIX_PERSIST_APP, PMIX_PERSIST_APP));
    CHECK("persist: SESSION stays at the APP horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_SESSION, PMIX_PERSIST_APP));
    CHECK("persist: INDEF stays at the APP horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_INDEF, PMIX_PERSIST_APP));

    /* a namespace ended - every application of one job */
    CHECK("persist: PROC goes at the NSPACE horizon",
          prte_data_server_expires_by(PMIX_PERSIST_PROC, PMIX_PERSIST_NSPACE));
    CHECK("persist: APP goes at the NSPACE horizon",
          prte_data_server_expires_by(PMIX_PERSIST_APP, PMIX_PERSIST_NSPACE));
    CHECK("persist: NSPACE goes at the NSPACE horizon",
          prte_data_server_expires_by(PMIX_PERSIST_NSPACE, PMIX_PERSIST_NSPACE));
    CHECK("persist: SESSION stays at the NSPACE horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_SESSION, PMIX_PERSIST_NSPACE));
    CHECK("persist: INDEF stays at the NSPACE horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_INDEF, PMIX_PERSIST_NSPACE));

    /* ...and one application of it ending does NOT take the job's own data.
     * This is the distinction the NSPACE policy exists for: an MPMD job's
     * apps share a namespace and need not end together. */
    CHECK("persist: NSPACE stays at the APP horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_NSPACE, PMIX_PERSIST_APP));
    CHECK("persist: NSPACE stays at the PROC horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_NSPACE, PMIX_PERSIST_PROC));
    CHECK("persist: no horizon takes NSPACE",
          prte_data_server_expires_by(PMIX_PERSIST_NSPACE, PMIX_PERSIST_INVALID));

    /* the session ended */
    CHECK("persist: NSPACE goes at the SESSION horizon",
          prte_data_server_expires_by(PMIX_PERSIST_NSPACE, PMIX_PERSIST_SESSION));
    CHECK("persist: SESSION goes at the SESSION horizon",
          prte_data_server_expires_by(PMIX_PERSIST_SESSION, PMIX_PERSIST_SESSION));
    CHECK("persist: APP goes at the SESSION horizon",
          prte_data_server_expires_by(PMIX_PERSIST_APP, PMIX_PERSIST_SESSION));
    /* the one value no lifetime reclaims */
    CHECK("persist: INDEF stays at the SESSION horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_INDEF, PMIX_PERSIST_SESSION));

    /* ...and the other one.  FIRST_READ's criterion is the first access,
     * which no lifetime ending satisfies: an item published for a reader
     * that has not started yet must survive its publisher, or a handover
     * from one generation of a job to the next cannot work at all.  A
     * publisher wanting its data gone when it goes away says PROC or APP.
     * This is issue #2733, and every horizon used to take it. */
    CHECK("persist: FIRST_READ stays at the PROC horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_FIRST_READ, PMIX_PERSIST_PROC));
    CHECK("persist: FIRST_READ stays at the APP horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_FIRST_READ, PMIX_PERSIST_APP));
    CHECK("persist: FIRST_READ stays at the SESSION horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_FIRST_READ, PMIX_PERSIST_SESSION));
    CHECK("persist: FIRST_READ stays at the NSPACE horizon",
          !prte_data_server_expires_by(PMIX_PERSIST_FIRST_READ, PMIX_PERSIST_NSPACE));

    return failures;
}

static int test_data_server_range(void)
{
    int failures = 0;
    prte_data_object_t *data;
    prte_data_req_t *req;

    data = PMIX_NEW(prte_data_object_t);
    PMIX_LOAD_PROCID(&data->owner, "publisher.ns", 2);
    PMIX_LOAD_PROCID(&data->proxy, "prte-daemons", 5);

    req = PMIX_NEW(prte_data_req_t);
    PMIX_LOAD_PROCID(&req->requestor, "other.ns", 0);
    PMIX_LOAD_PROCID(&req->proxy, "prte-daemons", 9);

    /* session/global/undef are open to everyone */
    data->range = PMIX_RANGE_SESSION;
    CHECK("range: SESSION admits a stranger", PMIX_SUCCESS == prte_data_server_check_range(req, data));
    data->range = PMIX_RANGE_GLOBAL;
    CHECK("range: GLOBAL admits a stranger", PMIX_SUCCESS == prte_data_server_check_range(req, data));
    data->range = PMIX_RANGE_UNDEF;
    CHECK("range: UNDEF admits a stranger", PMIX_SUCCESS == prte_data_server_check_range(req, data));

    /* NAMESPACE admits only the publisher's own namespace */
    data->range = PMIX_RANGE_NAMESPACE;
    CHECK("range: NAMESPACE refuses another namespace",
          PMIX_SUCCESS != prte_data_server_check_range(req, data));
    PMIX_LOAD_PROCID(&req->requestor, "publisher.ns", 7);
    CHECK("range: NAMESPACE admits any rank of the publisher's namespace",
          PMIX_SUCCESS == prte_data_server_check_range(req, data));

    /* LOCAL admits only requestors behind the same daemon.  This is the
     * check that was reading an uninitialized data->proxy. */
    data->range = PMIX_RANGE_LOCAL;
    CHECK("range: LOCAL refuses a different daemon",
          PMIX_SUCCESS != prte_data_server_check_range(req, data));
    PMIX_LOAD_PROCID(&req->proxy, "prte-daemons", 5);
    CHECK("range: LOCAL admits the same daemon",
          PMIX_SUCCESS == prte_data_server_check_range(req, data));

    /* PROC_LOCAL admits only the publisher itself */
    data->range = PMIX_RANGE_PROC_LOCAL;
    CHECK("range: PROC_LOCAL refuses a peer in the same namespace",
          PMIX_SUCCESS != prte_data_server_check_range(req, data));
    PMIX_LOAD_PROCID(&req->requestor, "publisher.ns", 2);
    CHECK("range: PROC_LOCAL admits the publisher",
          PMIX_SUCCESS == prte_data_server_check_range(req, data));

    /* RM admits only our own (the host server's) namespace */
    data->range = PMIX_RANGE_RM;
    CHECK("range: RM refuses an application namespace",
          PMIX_SUCCESS != prte_data_server_check_range(req, data));
    PMIX_LOAD_NSPACE(req->requestor.nspace, PRTE_PROC_MY_NAME->nspace);
    CHECK("range: RM admits the host's own namespace",
          PMIX_SUCCESS == prte_data_server_check_range(req, data));

    /* For CUSTOM the accessor list IS the range, so a publisher that named
     * nobody admits nobody - including itself */
    data->range = PMIX_RANGE_CUSTOM;
    PMIX_LOAD_PROCID(&req->requestor, "publisher.ns", 2);
    CHECK("range: CUSTOM denies when the publisher named no accessors",
          PMIX_SUCCESS != prte_data_server_check_range(req, data));
    /* ...and defers to the list when there is one, which the access check
     * is what evaluates */
    data->nauids = 1;
    data->auids = (uint32_t *) malloc(sizeof(uint32_t));
    data->auids[0] = 4242;
    CHECK("range: CUSTOM admits when an accessor list was given",
          PMIX_SUCCESS == prte_data_server_check_range(req, data));

    PMIX_RELEASE(req);
    PMIX_RELEASE(data);
    return failures;
}

/* Access permissions are the FIRST gate on retrieval - a requestor must
 * satisfy them before the range constraint is even asked.  Absent a list,
 * published data belongs to its publisher: only the publisher's own uid and
 * gid may read it.  Each list the publisher does give is a requirement. */
static int test_data_server_access(void)
{
    int failures = 0;
    prte_data_object_t *data;
    prte_data_req_t *req;

    data = PMIX_NEW(prte_data_object_t);
    CHECK("access: a new object has no owner uid/gid",
          UINT32_MAX == data->uid && UINT32_MAX == data->gid);
    CHECK("access: a new object names no accessors",
          NULL == data->auids && 0 == data->nauids &&
          NULL == data->agids && 0 == data->nagids);
    data->uid = 1000;
    data->gid = 100;

    req = PMIX_NEW(prte_data_req_t);
    CHECK("access: a new request has no uid/gid",
          UINT32_MAX == req->uid && UINT32_MAX == req->gid);

    /* no list: the publisher's own identity is the rule */
    req->uid = 1000;
    req->gid = 100;
    CHECK("access: the publisher's own uid and gid are admitted",
          PMIX_SUCCESS == prte_data_server_check_access(req, data));
    req->uid = 1001;
    CHECK("access: another uid is refused",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));
    req->uid = 1000;
    req->gid = 101;
    CHECK("access: another gid is refused",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));

    /* a uid list is a requirement, and it is the ONLY one when no gid list
     * was given - the whole point being to admit somebody else */
    data->nauids = 2;
    data->auids = (uint32_t *) malloc(2 * sizeof(uint32_t));
    data->auids[0] = 1001;
    data->auids[1] = 1002;
    req->uid = 1001;
    req->gid = 999;
    CHECK("access: a uid on the list is admitted whatever its gid",
          PMIX_SUCCESS == prte_data_server_check_access(req, data));
    req->uid = 1003;
    CHECK("access: a uid not on the list is refused",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));
    req->uid = 1000;
    CHECK("access: the publisher's own uid is refused by a list that omits it",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));

    /* both lists given: both are requirements */
    data->nagids = 1;
    data->agids = (uint32_t *) malloc(sizeof(uint32_t));
    data->agids[0] = 50;
    req->uid = 1001;
    req->gid = 50;
    CHECK("access: a requestor meeting both lists is admitted",
          PMIX_SUCCESS == prte_data_server_check_access(req, data));
    req->gid = 51;
    CHECK("access: meeting the uid list but not the gid list is refused",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));
    req->uid = 1003;
    req->gid = 50;
    CHECK("access: meeting the gid list but not the uid list is refused",
          PMIX_ERR_NO_PERMISSIONS == prte_data_server_check_access(req, data));

    /* a gid list on its own */
    free(data->auids);
    data->auids = NULL;
    data->nauids = 0;
    req->uid = 1003;
    req->gid = 50;
    CHECK("access: a gid on the list is admitted whatever its uid",
          PMIX_SUCCESS == prte_data_server_check_access(req, data));

    PMIX_RELEASE(req);
    PMIX_RELEASE(data);
    return failures;
}

/* The requester's own range is the OTHER half of the rule: the PMIx
 * retrieval rules constrain a lookup to data whose publisher falls within
 * the range the requester asked for, which is what keeps duplicate keys
 * published on different ranges apart.  It is the same predicate as
 * above with the two processes swapped, and it used to be unpacked and
 * then never applied to anything. */
static int test_data_server_search_range(void)
{
    int failures = 0;
    prte_data_object_t *data;
    prte_data_req_t *req;

    data = PMIX_NEW(prte_data_object_t);
    PMIX_LOAD_PROCID(&data->owner, "publisher.ns", 2);
    PMIX_LOAD_PROCID(&data->proxy, "prte-daemons", 5);
    /* the publisher's own range admits everyone throughout, so what is
     * under test is only the requester's */
    data->range = PMIX_RANGE_GLOBAL;

    req = PMIX_NEW(prte_data_req_t);
    PMIX_LOAD_PROCID(&req->requestor, "other.ns", 0);
    PMIX_LOAD_PROCID(&req->proxy, "prte-daemons", 9);

    CHECK("search: a new request defaults to the SESSION range",
          PMIX_RANGE_SESSION == req->range);

    /* session/global/undef search everything */
    CHECK("search: SESSION admits any publisher",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));
    req->range = PMIX_RANGE_GLOBAL;
    CHECK("search: GLOBAL admits any publisher",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));
    req->range = PMIX_RANGE_UNDEF;
    CHECK("search: UNDEF admits any publisher",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));

    /* NAMESPACE searches only publishers in the requester's namespace */
    req->range = PMIX_RANGE_NAMESPACE;
    CHECK("search: NAMESPACE refuses a publisher in another namespace",
          PMIX_SUCCESS != prte_data_server_check_search_range(req, data));
    PMIX_LOAD_PROCID(&req->requestor, "publisher.ns", 7);
    CHECK("search: NAMESPACE admits a publisher in the requester's namespace",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));

    /* LOCAL searches only publishers behind the requester's own daemon */
    req->range = PMIX_RANGE_LOCAL;
    CHECK("search: LOCAL refuses a publisher behind another daemon",
          PMIX_SUCCESS != prte_data_server_check_search_range(req, data));
    PMIX_LOAD_PROCID(&req->proxy, "prte-daemons", 5);
    CHECK("search: LOCAL admits a publisher behind the same daemon",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));

    /* PROC_LOCAL searches only what the requester published itself */
    req->range = PMIX_RANGE_PROC_LOCAL;
    CHECK("search: PROC_LOCAL refuses a peer's data",
          PMIX_SUCCESS != prte_data_server_check_search_range(req, data));
    PMIX_LOAD_PROCID(&req->requestor, "publisher.ns", 2);
    CHECK("search: PROC_LOCAL admits the requester's own data",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));

    /* RM searches only data published by the host environment */
    req->range = PMIX_RANGE_RM;
    CHECK("search: RM refuses data published by an application",
          PMIX_SUCCESS != prte_data_server_check_search_range(req, data));
    PMIX_LOAD_NSPACE(data->owner.nspace, PRTE_PROC_MY_NAME->nspace);
    CHECK("search: RM admits data published by the host",
          PMIX_SUCCESS == prte_data_server_check_search_range(req, data));

    /* and CUSTOM denies here too - a requester has no accessor list to be
     * on any more than a publisher has one to name */
    req->range = PMIX_RANGE_CUSTOM;
    CHECK("search: CUSTOM denies (no accessor list is implemented)",
          PMIX_SUCCESS != prte_data_server_check_search_range(req, data));

    PMIX_RELEASE(req);
    PMIX_RELEASE(data);
    return failures;
}

/* A relayed request names the process it is acting for in PMIX_REQUESTOR,
 * and only a TOOL is allowed to make that claim.  The relay is a daemon of
 * another DVM attached to us as a tool (see ds_relay.c); an application
 * process making the same claim is trying to publish - or unpublish - under
 * a peer's identity, so the claim has to be dropped rather than honored. */
/* Who may REMOVE an item.  Ownership is the publishing user, which is a
 * different question from access - see the two directions asserted below. */
static int test_data_server_ownership(void)
{
    int failures = 0;
    prte_data_object_t *data;

    data = PMIX_NEW(prte_data_object_t);
    PMIX_LOAD_PROCID(&data->owner, "publisher.job", 2);
    data->uid = 500;
    data->gid = 20;

    CHECK("owns: the publishing uid and gid may remove",
          prte_data_server_owns(500, 20, data));

    /* The point of keying on the user rather than the process: the
     * publisher itself is gone, and a later job of the same user - a
     * different namespace entirely - has to be able to take the name back.
     * There is no process identity in this call at all. */
    CHECK("owns: another process of the same user may remove",
          prte_data_server_owns(500, 20, data));

    CHECK("owns: a different uid may not remove",
          !prte_data_server_owns(501, 20, data));
    CHECK("owns: a different gid may not remove",
          !prte_data_server_owns(500, 21, data));

    /* The gid took an openpmix change to be handed over at all, and where
     * it is missing both sides read UINT32_MAX.  Degrade to uid alone
     * rather than locking the owner out of its own data. */
    CHECK("owns: an unknown requestor gid degrades to uid alone",
          prte_data_server_owns(500, UINT32_MAX, data));
    data->gid = UINT32_MAX;
    CHECK("owns: an unknown stored gid degrades to uid alone",
          prte_data_server_owns(500, 20, data));
    CHECK("owns: ...and the uid still has to match",
          !prte_data_server_owns(501, 20, data));

    /* An identity we do not have is not an identity that matches: two
     * unknowns comparing equal would let anybody remove anybody's data. */
    data->uid = UINT32_MAX;
    CHECK("owns: an unknown stored uid matches nobody",
          !prte_data_server_owns(UINT32_MAX, UINT32_MAX, data));
    CHECK("owns: ...not even a real uid",
          !prte_data_server_owns(500, 20, data));
    data->uid = 500;
    CHECK("owns: an unknown requestor uid owns nothing",
          !prte_data_server_owns(UINT32_MAX, 20, data));

    /* Ownership and access are different questions.  An accessor list
     * widens who may READ and confers no removal; and the sharper case,
     * an owner whose own list excludes it may still remove what it cannot
     * read.  Both are asserted here by the absence of any accessor list
     * from this predicate at all: it reads data->uid and data->gid, never
     * data->auids or data->agids. */
    data->auids = (uint32_t *) malloc(sizeof(uint32_t));
    data->auids[0] = 501;
    data->nauids = 1;
    CHECK("owns: a uid on the accessor list still may not remove",
          !prte_data_server_owns(501, 20, data));
    CHECK("owns: an owner its own accessor list excludes may still remove",
          prte_data_server_owns(500, 20, data));

    PMIX_RELEASE(data);
    return failures;
}

static int test_data_server_requestor(void)
{
    int failures = 0;
    prte_job_t *toolj, *appj;
    pmix_proc_t owner, behalf;
    pmix_info_t info[3];
    uint32_t uid, gid, claim;

    reset_globals();

    toolj = make_job("relay.tool");
    PRTE_FLAG_SET(toolj, PRTE_JOB_FLAG_TOOL);
    CHECK("requestor: tool job registered", PRTE_SUCCESS == prte_set_job_data_object(toolj));
    appj = make_job("some.app");
    CHECK("requestor: app job registered", PRTE_SUCCESS == prte_set_job_data_object(appj));

    PMIX_LOAD_PROCID(&behalf, "far.away.job", 3);

    /* a tool may act for somebody else */
    PMIX_LOAD_PROCID(&owner, "relay.tool", 0);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR, &behalf, PMIX_PROC);
    prte_ds_check_requestor(&owner, NULL, NULL, info, 1);
    CHECK("requestor: a tool's claim is honored", PMIX_CHECK_PROCID(&owner, &behalf));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* an application process may not */
    PMIX_LOAD_PROCID(&owner, "some.app", 1);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR, &behalf, PMIX_PROC);
    prte_ds_check_requestor(&owner, NULL, NULL, info, 1);
    CHECK("requestor: an application's claim is refused",
          PMIX_CHECK_NSPACE(owner.nspace, "some.app") && 1 == owner.rank);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* neither may a namespace we have never heard of - a job object is what
     * says what the caller is, and without one there is nothing to trust */
    PMIX_LOAD_PROCID(&owner, "unknown.job", 4);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR, &behalf, PMIX_PROC);
    prte_ds_check_requestor(&owner, NULL, NULL, info, 1);
    CHECK("requestor: an unknown namespace's claim is refused",
          PMIX_CHECK_NSPACE(owner.nspace, "unknown.job"));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* a directive of the wrong type is ignored rather than dereferenced */
    PMIX_LOAD_PROCID(&owner, "relay.tool", 0);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR, "not-a-procid", PMIX_STRING);
    prte_ds_check_requestor(&owner, NULL, NULL, info, 1);
    CHECK("requestor: a mistyped directive is ignored",
          PMIX_CHECK_NSPACE(owner.nspace, "relay.tool"));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* The identity a relay claims includes the uid and gid, and has to:
     * ownership is decided by the publishing user, and what PMIx appends to
     * a relayed request is the RELAYING daemon's identity, not the
     * originating process's. */
    claim = 4242;
    PMIX_LOAD_PROCID(&owner, "relay.tool", 0);
    uid = 7; gid = 9;
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR, &behalf, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PRTE_PUBLISH_REQ_UID, &claim, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PRTE_PUBLISH_REQ_GID, &claim, PMIX_UINT32);
    prte_ds_check_requestor(&owner, &uid, &gid, info, 3);
    CHECK("requestor: a tool's claimed uid is honored", 4242 == uid);
    CHECK("requestor: a tool's claimed gid is honored", 4242 == gid);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);

    /* ...and an application process claiming a uid is refused it, which is
     * the half that matters: a uid it could assert is a uid whose published
     * data it could remove */
    PMIX_LOAD_PROCID(&owner, "some.app", 1);
    uid = 7; gid = 9;
    PMIX_INFO_LOAD(&info[0], PRTE_PUBLISH_REQ_UID, &claim, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PRTE_PUBLISH_REQ_GID, &claim, PMIX_UINT32);
    prte_ds_check_requestor(&owner, &uid, &gid, info, 2);
    CHECK("requestor: an application's claimed uid is refused", 7 == uid);
    CHECK("requestor: an application's claimed gid is refused", 9 == gid);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    reset_globals();
    return failures;
}

/* ------------------------------------------------------------------ */
/* progress threads                                                   */
/* ------------------------------------------------------------------ */

static int test_progress_thread_cpus(void)
{
    int failures = 0;
    int cpus[16];
    int n;

    /* A bare cpu number.  strtoul always returns a non-NULL end pointer, so
     * the old "if (NULL == dash)" test never fired: "3" went down the range
     * branch, which stepped the pointer past the terminating NUL and read
     * whatever followed the string. */
    n = prte_progress_thread_parse_cpus("3", cpus, 16);
    CHECK("cpus: a bare number yields exactly one cpu", 1 == n);
    CHECK("cpus: ...and it is the number given", 1 == n && 3 == cpus[0]);

    n = prte_progress_thread_parse_cpus("0", cpus, 16);
    CHECK("cpus: cpu 0 parses", 1 == n && 0 == cpus[0]);

    /* A range is inclusive of its upper bound: "0-3" is four cpus.  The old
     * loop was "k < end", so the last cpu of every range was dropped. */
    n = prte_progress_thread_parse_cpus("0-3", cpus, 16);
    CHECK("cpus: a range names all of its cpus", 4 == n);
    CHECK("cpus: ...in order",
          4 == n && 0 == cpus[0] && 1 == cpus[1] && 2 == cpus[2] && 3 == cpus[3]);

    n = prte_progress_thread_parse_cpus("5-5", cpus, 16);
    CHECK("cpus: a degenerate range is one cpu", 1 == n && 5 == cpus[0]);

    /* mixed list */
    n = prte_progress_thread_parse_cpus("1,4-6,9", cpus, 16);
    CHECK("cpus: a mixed list expands fully", 5 == n);
    CHECK("cpus: ...with the right members",
          5 == n && 1 == cpus[0] && 4 == cpus[1] && 5 == cpus[2] && 6 == cpus[3]
              && 9 == cpus[4]);

    /* garbage is rejected instead of silently binding to cpu 0 */
    CHECK("cpus: non-numeric input is rejected",
          0 > prte_progress_thread_parse_cpus("abc", cpus, 16));
    CHECK("cpus: a trailing dash is rejected",
          0 > prte_progress_thread_parse_cpus("2-", cpus, 16));
    CHECK("cpus: an inverted range is rejected",
          0 > prte_progress_thread_parse_cpus("7-2", cpus, 16));
    CHECK("cpus: trailing junk is rejected",
          0 > prte_progress_thread_parse_cpus("3x", cpus, 16));
    CHECK("cpus: NULL input is rejected",
          0 > prte_progress_thread_parse_cpus(NULL, cpus, 16));
    /* ...and the bound is honored rather than overrun */
    CHECK("cpus: a list larger than the buffer is refused",
          0 > prte_progress_thread_parse_cpus("0-99", cpus, 16));

    return failures;
}

/* ------------------------------------------------------------------ */
/* MCA parameter files                                                */
/* ------------------------------------------------------------------ */

/* The parameter files are applied by publishing their contents into the
 * environment, and an MCA variable reads its environment exactly once, when
 * it is first registered.  So the registration of everything that is not
 * itself needed to *find* the files has to happen after they are read.  It
 * did not: prte_init_minimum() registered every prte_* parameter - plus all
 * of RML, OOB and grpcomm - before preloading the files, so a value set in
 * mca-params.conf was read, published, and then ignored by the variable it
 * named.  The same line in the same file was honored or not depending only
 * on whether the variable behind it happened to be registered here or later,
 * at framework open.
 *
 * setup_paramfile() below runs before prte_init_util(), so the whole
 * ordering is what is under test.
 */
static const char *paramfile = "test_runtime_mca_params.conf";
static bool paramfile_written = false;

static void setup_paramfile(void)
{
    FILE *fp;

    fp = fopen(paramfile, "w");
    if (NULL == fp) {
        /* nothing to test against, and not worth failing the suite over -
         * test_paramfile_ordering reports the skip */
        return;
    }
    /* one core prte_* parameter, and one from each of the two subsystems
     * prte_register_params() registers on its way out */
    fprintf(fp, "# written by test_runtime\n");
    fprintf(fp, "prte_max_msg_size = 55\n");
    fprintf(fp, "rml_base_radix = 3\n");
    fprintf(fp, "prte_uniform_nodes = 1\n");
    fclose(fp);
    paramfile_written = true;

    /* replace the default list (the developer's own ~/.prte/mca-params.conf
     * must not be able to influence this) */
    setenv("PRTE_MCA_prte_param_files", paramfile, 1);
}

static int test_paramfile_ordering(void)
{
    int failures = 0;

    if (!paramfile_written) {
        fprintf(stderr, "SKIP [paramfile]: could not write %s\n", paramfile);
        return 0;
    }

    /* registered in prte_register_params() itself */
    CHECK("paramfile: a core prte_* param takes the file's value",
          55 == prte_oob_base.max_msg_size);
    CHECK("paramfile: a bool param takes the file's value", prte_homo_nodes);
    /* registered by prte_rml_register(), on the way out of the same call */
    CHECK("paramfile: an rml param takes the file's value", 3 == prte_rml_base.radix);

    return failures;
}

static int test_progress_thread_lifecycle(void)
{
    int failures = 0;
    prte_event_base_t *b1, *b2;

    b1 = prte_progress_thread_init("unit-test-thread");
    CHECK("thread: init returns an event base", NULL != b1);

    /* asking for the same name again hands back the same base rather than
     * starting a second thread */
    b2 = prte_progress_thread_init("unit-test-thread");
    CHECK("thread: the same name reuses the same base", b1 == b2);

    /* a different name is a different thread */
    b2 = prte_progress_thread_init("unit-test-other");
    CHECK("thread: a different name gets its own base", NULL != b2 && b1 != b2);

    CHECK("thread: pause succeeds", PRTE_SUCCESS == prte_progress_thread_pause("unit-test-thread"));
    /* resuming a running thread is refused; resuming a paused one works */
    CHECK("thread: resume restarts it",
          PRTE_SUCCESS == prte_progress_thread_resume("unit-test-thread"));
    CHECK("thread: resuming a running thread is refused",
          PRTE_ERR_RESOURCE_BUSY == prte_progress_thread_resume("unit-test-thread"));

    /* unknown names are reported, not silently accepted */
    CHECK("thread: pausing an unknown name is reported",
          PRTE_ERR_NOT_FOUND == prte_progress_thread_pause("no-such-thread"));
    CHECK("thread: resuming an unknown name is reported",
          PRTE_ERR_NOT_FOUND == prte_progress_thread_resume("no-such-thread"));
    CHECK("thread: finalizing an unknown name is reported",
          PRTE_ERR_NOT_FOUND == prte_progress_thread_finalize("no-such-thread"));

    CHECK("thread: finalize succeeds",
          PRTE_SUCCESS == prte_progress_thread_finalize("unit-test-thread"));
    CHECK("thread: the finalized name is gone",
          PRTE_ERR_NOT_FOUND == prte_progress_thread_pause("unit-test-thread"));
    CHECK("thread: the other thread survived",
          PRTE_SUCCESS == prte_progress_thread_finalize("unit-test-other"));

    return failures;
}

/* The process-wide worker pool.
 *
 * Two things are worth pinning.  The rotation must actually rotate: the
 * bases sit on a ring and each assignment pops the oldest and pushes it
 * straight back, so N successive requests must visit all N bases and the
 * (N+1)th must come back around to the first.  A cursor kept alongside the
 * ring is exactly what this replaced, and the failure it invites - a cursor
 * that no longer matches the pool's size - is what "walk it twice around"
 * catches.
 *
 * And the empty pool must still answer.  Every caller uses the result
 * unconditionally, so "no workers configured" has to hand back
 * prte_event_base rather than NULL.  (In this bare test process
 * prte_event_base has never been created, so compare against it rather than
 * asserting non-NULL.)
 */
static int test_worker_pool(void)
{
    int failures = 0, i, save = prte_num_worker_threads;
    prte_event_base_t *first[3], *evb;

    /* no workers: every assignment is the main progress thread */
    prte_num_worker_threads = 0;
    CHECK("pool: an empty pool starts", PRTE_SUCCESS == prte_worker_pool_init());
    CHECK("pool: an empty pool has no threads", 0 == prte_worker_pool_size());
    for (i = 0; i < 3; i++) {
        CHECK("pool: no workers means the main base",
              prte_event_base == prte_worker_pool_assign());
    }
    prte_worker_pool_finalize();

    /* a negative request is a request for none, not a ring sized -1 */
    prte_num_worker_threads = -4;
    CHECK("pool: a negative request starts", PRTE_SUCCESS == prte_worker_pool_init());
    CHECK("pool: a negative request clamps to none", 0 == prte_worker_pool_size());
    CHECK("pool: a negative request still answers",
          prte_event_base == prte_worker_pool_assign());
    prte_worker_pool_finalize();

    /* three workers */
    prte_num_worker_threads = 3;
    CHECK("pool: three workers start", PRTE_SUCCESS == prte_worker_pool_init());
    CHECK("pool: three workers recorded", 3 == prte_worker_pool_size());

    for (i = 0; i < 3; i++) {
        first[i] = prte_worker_pool_assign();
        CHECK("pool: an assignment is a real base", NULL != first[i]);
        CHECK("pool: an assignment is not the main base", prte_event_base != first[i]);
    }
    CHECK("pool: the first lap visits three distinct bases",
          first[0] != first[1] && first[1] != first[2] && first[0] != first[2]);

    /* second lap: same order, which is what makes it a rotation rather than
     * an arbitrary shuffle */
    for (i = 0; i < 3; i++) {
        evb = prte_worker_pool_assign();
        CHECK("pool: the rotation wraps in order", first[i] == evb);
    }

    /* asking again while the pool is up must not build a second one */
    CHECK("pool: init is idempotent", PRTE_SUCCESS == prte_worker_pool_init());
    CHECK("pool: init did not add threads", 3 == prte_worker_pool_size());

    prte_worker_pool_finalize();
    CHECK("pool: finalize leaves no threads", 0 == prte_worker_pool_size());
    CHECK("pool: a harvested pool falls back to the main base",
          prte_event_base == prte_worker_pool_assign());
    /* and finalizing twice is not a fault */
    prte_worker_pool_finalize();

    prte_num_worker_threads = save;
    return failures;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int rc, failures = 0;
    pmix_status_t prc;

    /* must precede prte_init_util: it is the parameter-file handling inside
     * prte_init_minimum() that is under test */
    setup_paramfile();

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }
    /* several of the objects under test reference the event base through
     * their destructors (timers on a job, for one) */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_event_base_open failed: %d\n", rc);
        return 1;
    }
    /* PMIx_Data_pack refuses to run until PMIx itself is up, and the wire
     * tests below go straight through it.  A daemon reaches this state via
     * PMIx_server_init, so do the same. */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        return 1;
    }

    /* prte_session_t's destructor asks the ras base to release the
     * underlying allocation, and that walks prte_ras_base.selected_modules -
     * a list that does not exist until the framework is open */
    rc = pmix_mca_base_framework_open(&prte_ras_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "ras framework open failed: %d\n", rc);
        return 1;
    }
    setup_globals();
    PMIX_LOAD_PROCID(PRTE_PROC_MY_NAME, "prte-hnp", 0);

    failures += test_job_registry();
    failures += test_session_registry();
    failures += test_session_ownership();
    failures += test_proc_lookups();
    failures += test_node_matching();
    failures += test_node_copy();
    failures += test_app_copy();
    failures += test_map_copy();
    failures += test_pack_roundtrip();
    failures += test_node_pack_roundtrip();
    failures += test_object_lifetimes();
    failures += test_session_teardown();
    failures += test_exit_status();
    failures += test_data_server_objects();
    failures += test_data_server_persistence();
    failures += test_data_server_range();
    failures += test_data_server_search_range();
    failures += test_data_server_access();
    failures += test_data_server_ownership();
    failures += test_data_server_requestor();
    failures += test_progress_thread_cpus();
    failures += test_progress_thread_lifecycle();
    failures += test_worker_pool();
    failures += test_paramfile_ordering();

    if (paramfile_written) {
        unlink(paramfile);
    }

    (void) pmix_mca_base_framework_close(&prte_ras_base_framework);
    prte_event_base_close();
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all runtime unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d runtime unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
