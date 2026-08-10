/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the RML's OOB interface-selection logic.
 *
 * prte_oob_split_and_resolve() turns an if_include/if_exclude string into
 * the list of interface names the TCP transport will bind to.  It runs
 * once, very early, inside prte_oob_open() -- before any socket exists and
 * before the progress thread is up -- so it is pure, self-contained logic
 * that can be driven directly with no DVM.
 *
 * The behavior worth pinning down:
 *
 *   1. Each comma-separated entry is either an interface *name*, taken
 *      as-is, or an IPv4 *subnet* in a.b.c.d/e notation, which is matched
 *      against the local interfaces and replaced by their names.
 *
 *   2. Both branches dedupe against the list accumulated so far, and the
 *      caller's list is carried across calls.  This is where issue #2553
 *      lived: the dedup loops walked the `char ***` parameter itself
 *      instead of the list it points at, so index 0 worked by accident
 *      while index 1 and beyond read past the caller's stack variable and
 *      handed a wild pointer to strcmp().  Any specification naming two or
 *      more interfaces crashed prte at startup.  The tests below therefore
 *      always push the list past a single element before expecting a match,
 *      and check the *count* rather than just the absence of a crash -- a
 *      dedup that silently stops working would otherwise look fine on a
 *      platform where the bad read happens not to fault.
 *
 *   3. Unresolvable entries (no "/", an unparseable address, a subnet no
 *      local interface sits in) are reported and dropped rather than
 *      propagated.
 *
 *   4. orig_str is freed and rebuilt from the resulting list, since that
 *      string is what the rest of prte_oob_open() consults.
 *
 * The subnet tests derive their CIDR from a real local interface, so they
 * make no assumption about what this host's interfaces are named or
 * addressed; if the host exposes no IPv4 interface at all they are skipped.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif

#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"

#include "src/pmix/pmix-internal.h"
#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"
#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_peer.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* a name no real interface will carry, used to push the accumulated list
 * past index 0 so the dedup loops have to actually iterate */
#define DUMMY_IF "prte-test-dummy0"

static int count_matches(char **argv, const char *needle)
{
    int n, count = 0;

    if (NULL == argv) {
        return 0;
    }
    for (n = 0; NULL != argv[n]; n++) {
        if (0 == strcmp(needle, argv[n])) {
            count++;
        }
    }
    return count;
}

/*
 * Interface names are collected in the order given, and orig_str is
 * rebuilt from the result.
 */
static int test_names_collected(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char *str = strdup("eth0,eth1,eth2");

    prte_oob_split_and_resolve(&str, "include", &interfaces);

    CHECK("three names collected", 3 == PMIx_Argv_count(interfaces));
    if (3 == PMIx_Argv_count(interfaces)) {
        CHECK("first name", 0 == strcmp("eth0", interfaces[0]));
        CHECK("second name", 0 == strcmp("eth1", interfaces[1]));
        CHECK("third name", 0 == strcmp("eth2", interfaces[2]));
    }
    CHECK("orig_str rebuilt", NULL != str && 0 == strcmp("eth0,eth1,eth2", str));

    free(str);
    PMIx_Argv_free(interfaces);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_names_collected\n");
    }
    return failures;
}

/*
 * Repeated names collapse to one entry.  This is the #2553 regression: the
 * duplicates here sit at indices 2 and 4, so the dedup loop must correctly
 * walk a list that is already two or more entries long.  With the broken
 * loop the comparison ran against garbage, never matched, and the
 * duplicates were appended -- so a wrong count fails this test even on a
 * platform where the bad read does not happen to fault.
 */
static int test_duplicate_names_collapse(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char *str = strdup("eth0,eth1,eth0,eth2,eth1");

    prte_oob_split_and_resolve(&str, "include", &interfaces);

    CHECK("duplicates dropped", 3 == PMIx_Argv_count(interfaces));
    CHECK("eth0 appears once", 1 == count_matches(interfaces, "eth0"));
    CHECK("eth1 appears once", 1 == count_matches(interfaces, "eth1"));
    CHECK("eth2 appears once", 1 == count_matches(interfaces, "eth2"));
    CHECK("orig_str deduped", NULL != str && 0 == strcmp("eth0,eth1,eth2", str));

    free(str);
    PMIx_Argv_free(interfaces);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_duplicate_names_collapse\n");
    }
    return failures;
}

/*
 * The caller's list accumulates across calls, and the second call dedupes
 * against what the first one left behind.
 */
static int test_list_accumulates(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char *first = strdup("eth0,eth1");
    char *second = strdup("eth1,eth2");

    prte_oob_split_and_resolve(&first, "include", &interfaces);
    CHECK("first call collected two", 2 == PMIx_Argv_count(interfaces));

    prte_oob_split_and_resolve(&second, "include", &interfaces);
    CHECK("second call added only the new name", 3 == PMIx_Argv_count(interfaces));
    CHECK("carried-over name not duplicated", 1 == count_matches(interfaces, "eth1"));
    CHECK("new name appended", 1 == count_matches(interfaces, "eth2"));

    free(first);
    free(second);
    PMIx_Argv_free(interfaces);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_list_accumulates\n");
    }
    return failures;
}

/*
 * Degenerate inputs must not fault.  A NULL orig_str, a NULL *orig_str, and
 * a NULL interfaces list are all reachable: prte_oob_open() only calls this
 * when one of if_include/if_exclude is set, but the routine guards them all.
 */
static int test_null_inputs(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char *str = NULL;

    /* no orig_str at all */
    prte_oob_split_and_resolve(NULL, "include", &interfaces);
    CHECK("NULL orig_str left list alone", NULL == interfaces);

    /* orig_str present but empty */
    prte_oob_split_and_resolve(&str, "include", &interfaces);
    CHECK("NULL *orig_str left list alone", NULL == interfaces);
    CHECK("NULL *orig_str unchanged", NULL == str);

    /* no list to collect into: orig_str is simply cleared */
    str = strdup("eth0,eth1");
    prte_oob_split_and_resolve(&str, "include", NULL);
    CHECK("no list clears orig_str", NULL == str);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_null_inputs\n");
    }
    return failures;
}

/*
 * Malformed and unmatchable subnet specifications are dropped, leaving the
 * valid entries around them intact.  Each of these emits a show_help
 * warning -- that is the intended behavior, not test noise.
 */
static int test_bad_subnets_dropped(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char *str;

    /* missing the "/" that makes it a subnet */
    str = strdup("eth0,eth1,1.2.3.4");
    prte_oob_split_and_resolve(&str, "include", &interfaces);
    CHECK("no-slash entry dropped", 2 == PMIx_Argv_count(interfaces));
    free(str);
    PMIx_Argv_free(interfaces);
    interfaces = NULL;

    /* not a parseable IPv4 address */
    str = strdup("eth0,eth1,999.999.999.999/24");
    prte_oob_split_and_resolve(&str, "include", &interfaces);
    CHECK("unparseable address dropped", 2 == PMIx_Argv_count(interfaces));
    free(str);
    PMIx_Argv_free(interfaces);
    interfaces = NULL;

    /* well-formed, but TEST-NET-3 (RFC 5737) is reserved for documentation
     * and will not be configured on a real interface */
    str = strdup("eth0,eth1,203.0.113.0/24");
    prte_oob_split_and_resolve(&str, "include", &interfaces);
    CHECK("unmatched subnet dropped", 2 == PMIx_Argv_count(interfaces));
    free(str);
    PMIx_Argv_free(interfaces);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_bad_subnets_dropped\n");
    }
    return failures;
}

/*
 * Pick any local IPv4 interface and report its name plus a /32 CIDR naming
 * its exact address.  A /32 matches that interface and no other, which
 * keeps the subnet tests independent of how this host is addressed.
 */
static bool find_ipv4_interface(char *name, int namelen, char *cidr, size_t cidrlen)
{
    pmix_pif_t *ifp;
    struct sockaddr_in *sin;
    char addr[INET_ADDRSTRLEN];

    PMIX_LIST_FOREACH(ifp, &pmix_if_list, pmix_pif_t)
    {
        if (AF_INET != ((struct sockaddr *) &ifp->if_addr)->sa_family) {
            continue;
        }
        sin = (struct sockaddr_in *) &ifp->if_addr;
        if (NULL == inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr))) {
            continue;
        }
        if (PMIX_SUCCESS != pmix_ifkindextoname(ifp->if_kernel_index, name, namelen)) {
            continue;
        }
        snprintf(cidr, cidrlen, "%s/32", addr);
        return true;
    }
    return false;
}

/*
 * A subnet resolves to the name of the interface it covers, and that name
 * is deduped against the list built so far.  DUMMY_IF occupies index 0 so
 * the real interface sits at index 1 -- the position the broken dedup loop
 * could not read.  A regression there appends the name a second time.
 */
static int test_subnet_resolves_and_dedupes(void)
{
    int failures = 0;
    char **interfaces = NULL;
    char ifname[IF_NAMESIZE], cidr[INET_ADDRSTRLEN + 4], spec[256];
    char *str;

    if (!find_ipv4_interface(ifname, sizeof(ifname), cidr, sizeof(cidr))) {
        fprintf(stdout, "SKIPPED test_subnet_resolves_and_dedupes"
                        " (no local IPv4 interface)\n");
        return 0;
    }

    /* the subnet names an interface already in the list, at index 1 */
    snprintf(spec, sizeof(spec), "%s,%s,%s", DUMMY_IF, ifname, cidr);
    str = strdup(spec);
    prte_oob_split_and_resolve(&str, "include", &interfaces);

    CHECK("named interface kept", 1 == count_matches(interfaces, ifname));
    CHECK("placeholder kept", 1 == count_matches(interfaces, DUMMY_IF));
    CHECK("subnet added nothing new", 2 == PMIx_Argv_count(interfaces));

    free(str);
    PMIx_Argv_free(interfaces);
    interfaces = NULL;

    /* same subnet given twice, with nothing else to resolve it against:
     * the first occurrence adds the name, the second must find it */
    snprintf(spec, sizeof(spec), "%s,%s,%s", DUMMY_IF, cidr, cidr);
    str = strdup(spec);
    prte_oob_split_and_resolve(&str, "include", &interfaces);

    CHECK("subnet resolved to a name", 1 == count_matches(interfaces, ifname));
    CHECK("repeated subnet added once", 2 == PMIx_Argv_count(interfaces));
    CHECK("orig_str rebuilt from names", NULL != str && NULL == strchr(str, '/'));

    free(str);
    PMIx_Argv_free(interfaces);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_subnet_resolves_and_dedupes\n");
    }
    return failures;
}

/*
 * A shared payload outlives the sends that transmit it.
 *
 * This is the one ownership rule in the RML that differs from every other:
 * an ordinary prte_rml_send_t owns its dbuf and frees it in its destructor,
 * while a send carrying a payload owns only a *reference* and must leave the
 * buffer alone.  Getting that backwards is a use-after-free in the middle of
 * a broadcast -- the second child would transmit a buffer the first child's
 * completion had already freed -- and it would show up as corruption on the
 * wire under load rather than as anything a smoke test notices.  So pin it
 * here, where it needs no DVM: build the send by hand, release it, and
 * confirm the payload and its bytes are still there.
 */
static int test_payload_outlives_sends(void)
{
    int failures = 0;
    prte_rml_payload_t *payload;
    prte_rml_send_t *snd;
    pmix_data_buffer_t *dbuf;
    pmix_byte_object_t bo;
    static const char pattern[] = "shared-payload-canary";
    pmix_status_t prc;
    int rc;

    /* PMIx refuses to move bytes into a buffer until it is up, and a daemon
     * reaches that state through PMIx_server_init - so do the same, and only
     * around this test, since nothing else in this binary needs it */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "FAIL [payload test]: PMIx_server_init: %s\n",
                PMIx_Error_string(prc));
        return 1;
    }

    payload = PMIX_NEW(prte_rml_payload_t);
    CHECK("payload constructs with no buffer", NULL == payload->dbuf);
    CHECK("payload starts at one reference", 1 == payload->super.obj_reference_count);

    PMIX_DATA_BUFFER_CREATE(dbuf);
    bo.size = sizeof(pattern);
    bo.bytes = malloc(bo.size);
    memcpy(bo.bytes, pattern, bo.size);
    rc = PMIx_Data_load(dbuf, &bo);
    CHECK("payload loads", PMIX_SUCCESS == rc);
    payload->dbuf = dbuf;

    /* what a send does when it accepts the payload */
    snd = PMIX_NEW(prte_rml_send_t);
    CHECK("a send starts with no payload", NULL == snd->payload);
    PMIX_RETAIN(payload);
    snd->payload = payload;
    snd->dbuf = payload->dbuf;
    CHECK("send took a reference", 2 == payload->super.obj_reference_count);

    /* ...and what it must do when it completes */
    PMIX_RELEASE(snd);
    CHECK("send dropped its reference", 1 == payload->super.obj_reference_count);
    CHECK("payload still holds its buffer", dbuf == payload->dbuf);
    CHECK("payload bytes survived the send", sizeof(pattern) == payload->dbuf->bytes_used);
    CHECK("payload contents survived the send",
          NULL != payload->dbuf->base_ptr
              && 0 == memcmp(pattern, payload->dbuf->base_ptr, sizeof(pattern)));

    /* the last reference is the one that frees the buffer */
    PMIX_RELEASE(payload);

    /* a payload with nothing in it is refused rather than sent */
    rc = prte_rml_send_payload_cb_nb(1, NULL, PRTE_RML_TAG_XCAST, NULL, NULL);
    CHECK("NULL payload refused", PRTE_ERR_BAD_PARAM == rc);
    payload = PMIX_NEW(prte_rml_payload_t);
    rc = prte_rml_send_payload_cb_nb(1, payload, PRTE_RML_TAG_XCAST, NULL, NULL);
    CHECK("empty payload refused", PRTE_ERR_BAD_PARAM == rc);
    CHECK("a refused send takes no reference", 1 == payload->super.obj_reference_count);
    PMIX_RELEASE(payload);

    PMIx_server_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_payload_outlives_sends\n");
    }
    return failures;
}

/*
 * The pool of event bases that peer sockets are serviced on.
 *
 * Two properties matter and neither needs a socket to check.  First, the
 * "no worker threads" case - which is the default, and therefore the shape
 * every existing deployment runs - must still leave ev_bases holding exactly
 * one entry, prte_event_base, because every call site indexes the array
 * unconditionally; a NULL array or a zero-length one is a segfault on the
 * first peer.  Second, peers must be handed out round-robin and the cursor
 * must wrap, since a cursor that ran off the end would index past the array.
 *
 * The multi-thread case is driven with a hand-built array rather than by
 * asking for real threads: prte_progress_thread_init spins an actual engine
 * thread per base, which a bare test process has no business starting.  What
 * is under test here is the assignment arithmetic, not libevent.
 */
static int test_progress_thread_pool(void)
{
    int failures = 0, i;
    prte_event_base_t *fake[3];
    prte_oob_tcp_peer_t *peers[7];

    /* the default: no dedicated threads */
    prte_oob_base.num_progress_threads = 0;
    prte_oob_base.ev_bases = NULL;
    prte_oob_base.ev_threads = NULL;
    prte_oob_base.next_base = 0;
    CHECK("pool starts", PRTE_SUCCESS == prte_oob_start_progress_threads());
    CHECK("array exists with no threads", NULL != prte_oob_base.ev_bases);
    CHECK("no threads means the main base",
          NULL != prte_oob_base.ev_bases && prte_event_base == prte_oob_base.ev_bases[0]);
    CHECK("no threads recorded", 0 == prte_oob_base.num_progress_threads);
    CHECK("no thread names recorded", NULL == prte_oob_base.ev_threads);

    /* every peer lands on that one entry, and the cursor does not move */
    for (i = 0; i < 3; i++) {
        peers[i] = PMIX_NEW(prte_oob_tcp_peer_t);
        CHECK("peer takes the main base", prte_event_base == peers[i]->evbase);
    }
    CHECK("cursor pinned with no threads", 0 == prte_oob_base.next_base);
    for (i = 0; i < 3; i++) {
        PMIX_RELEASE(peers[i]);
    }

    prte_oob_harvest_progress_threads();
    CHECK("harvest clears the array", NULL == prte_oob_base.ev_bases);
    CHECK("harvest clears the count", 0 == prte_oob_base.num_progress_threads);

    /* a negative request is a request for none, not an array sized -1 */
    prte_oob_base.num_progress_threads = -4;
    CHECK("negative pool starts", PRTE_SUCCESS == prte_oob_start_progress_threads());
    CHECK("negative clamps to none", 0 == prte_oob_base.num_progress_threads);
    CHECK("negative still yields the main base",
          NULL != prte_oob_base.ev_bases && prte_event_base == prte_oob_base.ev_bases[0]);
    prte_oob_harvest_progress_threads();

    /* three bases, seven peers: 0,1,2,0,1,2,0 */
    for (i = 0; i < 3; i++) {
        /* distinct non-NULL values are all the assignment arithmetic sees */
        fake[i] = (prte_event_base_t *) &fake[i];
    }
    prte_oob_base.num_progress_threads = 3;
    prte_oob_base.ev_bases = fake;
    prte_oob_base.next_base = 0;
    for (i = 0; i < 7; i++) {
        peers[i] = PMIX_NEW(prte_oob_tcp_peer_t);
    }
    for (i = 0; i < 7; i++) {
        CHECK("peer assigned round-robin", fake[i % 3] == peers[i]->evbase);
    }
    CHECK("cursor wrapped", 1 == prte_oob_base.next_base);
    for (i = 0; i < 7; i++) {
        PMIX_RELEASE(peers[i]);
    }
    /* the array is on our stack - do not let harvest free it */
    prte_oob_base.ev_bases = NULL;
    prte_oob_base.num_progress_threads = 0;
    prte_oob_base.next_base = 0;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_progress_thread_pool\n");
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

    failures += test_names_collected();
    failures += test_duplicate_names_collapse();
    failures += test_list_accumulates();
    failures += test_null_inputs();
    fprintf(stdout, "-- the next test drives invalid specifications;"
                    " the warnings it prints are expected --\n");
    failures += test_bad_subnets_dropped();
    failures += test_subnet_resolves_and_dedupes();
    failures += test_payload_outlives_sends();
    failures += test_progress_thread_pool();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rml unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rml unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
