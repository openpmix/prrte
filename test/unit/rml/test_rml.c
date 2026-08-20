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

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_worker_pool.h"
#include "src/runtime/runtime.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"

#include "src/pmix/pmix-internal.h"
#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"
#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_hdr.h"
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

/* Which base a peer's socket handlers run on.
 *
 * peer_cons does not decide - it asks the process-wide worker pool (see
 * src/runtime/prte_worker_pool.h), and what matters here is that it asks at
 * all, and that it copes with the answer in both shapes.  With no pool up -
 * the state a peer built before prte_init stood one up, or after finalize
 * took it down, is in - every peer must land on prte_event_base, because
 * every socket path uses peer->evbase unconditionally.  With a pool up,
 * successive peers must land on different bases, or the pool buys nothing.
 *
 * The rotation itself is tested where it lives, in test_runtime.
 */
static int test_peer_base_assignment(void)
{
    int failures = 0, i, save = prte_num_worker_threads;
    prte_oob_tcp_peer_t *peers[6];

    /* no pool: every peer takes the main base.  (prte_event_base has never
     * been created in this bare test process, so compare against it rather
     * than asserting non-NULL.) */
    prte_num_worker_threads = 0;
    CHECK("empty pool starts", PRTE_SUCCESS == prte_worker_pool_init());
    for (i = 0; i < 3; i++) {
        peers[i] = PMIX_NEW(prte_oob_tcp_peer_t);
        CHECK("peer takes the main base", prte_event_base == peers[i]->evbase);
    }
    for (i = 0; i < 3; i++) {
        PMIX_RELEASE(peers[i]);
    }
    prte_worker_pool_finalize();

    /* three workers, six peers: each peer takes a worker base, and the
     * assignment cycles with the pool's period rather than pinning them all
     * to one thread */
    prte_num_worker_threads = 3;
    CHECK("worker pool starts", PRTE_SUCCESS == prte_worker_pool_init());
    for (i = 0; i < 6; i++) {
        peers[i] = PMIX_NEW(prte_oob_tcp_peer_t);
    }
    for (i = 0; i < 6; i++) {
        CHECK("peer left the main base", prte_event_base != peers[i]->evbase);
        CHECK("peer assignment cycles with the pool",
              peers[i]->evbase == peers[i % 3]->evbase);
    }
    CHECK("consecutive peers get different bases",
          peers[0]->evbase != peers[1]->evbase
          && peers[1]->evbase != peers[2]->evbase
          && peers[0]->evbase != peers[2]->evbase);
    for (i = 0; i < 6; i++) {
        PMIX_RELEASE(peers[i]);
    }
    prte_worker_pool_finalize();
    prte_num_worker_threads = save;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_peer_base_assignment\n");
    }
    return failures;
}


/* The wire header.
 *
 * A DATA message carries no namespace at all: every OOB peer is a daemon of
 * this DVM, so the receiver rebuilds both procids with its own.  The connect
 * HANDSHAKE does carry one, because that is where the claim is checked.
 * Nothing here needs a socket - what matters is how long each of the two is
 * on the wire, that the receiver's read reconstructs exactly the names the
 * sender put in, and that the byte-order conversion is a round trip.  Those
 * are what a reader of oob_tcp_sendrecv.c has to take on trust, and getting
 * any of them wrong delivers a message under the wrong identity rather than
 * failing.
 */
static int test_wire_header(void)
{
    int failures = 0;
    prte_oob_tcp_hdr_t snd, rcv;
    pmix_proc_t origin, dest;
    char wire[sizeof(prte_oob_tcp_hdr_t)];
    size_t len;
    const char *ns = "prterun-somenode-12345@0";

    /* this process stands in for the receiving daemon */
    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, ns);

    /* --- a data message ------------------------------------------------ */
    memset(&snd, 0, sizeof(snd));
    snd.epoch = 7;
    snd.origin = 3;
    snd.dst = 11;
    snd.tag = PRTE_RML_TAG_DAEMON;
    snd.seq_num = 42;
    snd.nbytes = 4096;
    snd.type = MCA_OOB_TCP_USER;
    snd.nslen = 0;      /* what the queueing macros do */

    len = PRTE_OOB_TCP_HDR_LEN(&snd);
    CHECK("a data header is exactly the fixed part",
          len == PRTE_OOB_TCP_HDR_FIXED);
    /* the point of the exercise: the struct is much larger than the message */
    CHECK("hdr on the wire is far smaller than the struct", len < sizeof(snd) / 4);

    MCA_OOB_TCP_HDR_HTON(&snd);
    memcpy(wire, &snd, len);

    memset(&rcv, 0xff, sizeof(rcv));
    memcpy(&rcv, wire, PRTE_OOB_TCP_HDR_FIXED);
    CHECK("nslen needs no byte-order conversion to be usable",
          PRTE_OOB_TCP_HDR_LEN(&rcv) == len);
    PRTE_OOB_TCP_HDR_END_NSPACE(&rcv);
    MCA_OOB_TCP_HDR_NTOH(&rcv);

    CHECK("epoch survives the round trip", 7 == rcv.epoch);
    CHECK("tag survives the round trip", PRTE_RML_TAG_DAEMON == rcv.tag);
    CHECK("seq_num survives the round trip", 42 == rcv.seq_num);
    CHECK("nbytes survives the round trip", 4096 == rcv.nbytes);
    CHECK("type survives the round trip", MCA_OOB_TCP_USER == rcv.type);

    /* both names come back in OUR namespace, which is the whole point of
     * leaving it off the wire */
    PRTE_OOB_TCP_HDR_PROC(&rcv, rcv.origin, &origin);
    PRTE_OOB_TCP_HDR_PROC(&rcv, rcv.dst, &dest);
    CHECK("origin rank rebuilt", 3 == origin.rank);
    CHECK("dst rank rebuilt", 11 == dest.rank);
    CHECK("origin nspace is the receiver's own", PMIX_CHECK_NSPACE(origin.nspace, ns));
    CHECK("dst carries the same nspace", PMIX_CHECK_NSPACE(dest.nspace, ns));

    /* --- a handshake --------------------------------------------------- */
    memset(&snd, 0, sizeof(snd));
    snd.origin = 5;
    snd.dst = 0;
    snd.type = MCA_OOB_TCP_IDENT;
    PRTE_OOB_TCP_HDR_LOAD_NSPACE(&snd, ns);

    CHECK("hdr nslen excludes the terminator", strlen(ns) == snd.nslen);
    len = PRTE_OOB_TCP_HDR_LEN(&snd);
    CHECK("a handshake header is the fixed part plus the nspace",
          len == PRTE_OOB_TCP_HDR_FIXED + strlen(ns));

    MCA_OOB_TCP_HDR_HTON(&snd);
    memcpy(wire, &snd, len);

    /* what a receiver does: the fixed part first, then nslen characters,
     * then supply the terminator the sender did not send */
    memset(&rcv, 0xff, sizeof(rcv));
    memcpy(&rcv, wire, PRTE_OOB_TCP_HDR_FIXED);
    CHECK("the handshake's nspace length arrives with the fixed part",
          PRTE_OOB_TCP_HDR_LEN(&rcv) == len);
    memcpy(rcv.nspace, wire + PRTE_OOB_TCP_HDR_FIXED, rcv.nslen);
    PRTE_OOB_TCP_HDR_END_NSPACE(&rcv);
    MCA_OOB_TCP_HDR_NTOH(&rcv);
    CHECK("the nspace arrives terminated and intact", 0 == strcmp(rcv.nspace, ns));
    CHECK("a matching handshake nspace is what the peer check compares",
          PMIX_CHECK_NSPACE(rcv.nspace, PRTE_PROC_MY_NAME->nspace));

    /* a stranger's handshake is distinguishable - this is the comparison
     * tcp_peer_recv_connect_ack makes before adopting a peer */
    PMIX_LOAD_NSPACE(rcv.nspace, "prterun-othernode-999@0");
    CHECK("another DVM's handshake nspace does not match",
          !PMIX_CHECK_NSPACE(rcv.nspace, PRTE_PROC_MY_NAME->nspace));

    /* a maximum-length nspace still fits the single-byte length field */
    memset(&snd, 0, sizeof(snd));
    {
        char big[PMIX_MAX_NSLEN + 1];
        memset(big, 'x', PMIX_MAX_NSLEN);
        big[PMIX_MAX_NSLEN] = '\0';
        PRTE_OOB_TCP_HDR_LOAD_NSPACE(&snd, big);
        CHECK("a full-length nspace is carried whole", PMIX_MAX_NSLEN == snd.nslen);
        PRTE_OOB_TCP_HDR_END_NSPACE(&snd);
        CHECK("a full-length nspace round trips", 0 == strcmp(snd.nspace, big));
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_wire_header\n");
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
    failures += test_peer_base_assignment();
    failures += test_wire_header();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rml unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rml unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
