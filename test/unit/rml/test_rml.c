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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
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
#include "src/util/prte_dvm_key.h"
#include "src/util/prte_hmac.h"

#include "src/pmix/pmix-internal.h"
#include "src/event/event-internal.h"
#include "src/rml/rml.h"
#include "src/rml/oob/oob.h"
#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_hdr.h"
#include "src/rml/oob/oob_tcp_listener.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_peer.h"
#include "src/rml/oob/oob_tcp_sendrecv.h"
#include "src/mca/prtereachable/base/base.h"

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

/* Giving up on a peer must not take its queued messages with it.
 *
 * A send handed to the OOB is owed a callback: PRTE_RML_SEND_COMPLETE is how
 * the originator learns the message went out or died, and RELM is waiting on
 * exactly that to decide whether to replay.  PMIX_RELEASE on the send frees
 * the buffer and tells nobody, which looks like nothing at all happening.
 *
 * Every path that gives up on a peer therefore has to drain the queue and
 * complete each message with a failure status - the peer teardown here, and
 * the arms of prte_oob_tcp_peer_try_connect that cannot get a socket at all
 * (a create or bind failure spans every interface, so there is no other
 * address to try).  Those arms are a reconnect path as well as a
 * first-connect one, so what is queued can be real work rather than a
 * handshake; they used to drop it.  They share this drain, so exercising it
 * through the one entry point that needs no sockets covers them.
 *
 * The on-deck message is checked as well as the queue: it is held in
 * peer->send_msg rather than on the list, and is the one most easily missed.
 */
static int completed_sends = 0;
static int last_send_status = PRTE_SUCCESS;

static void record_completion(int status, pmix_proc_t *peer,
                              pmix_data_buffer_t *buffer,
                              prte_rml_tag_t tag, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(peer, tag, cbdata);
    if (NULL != buffer) {
        PMIX_DATA_BUFFER_RELEASE(buffer);
    }
    completed_sends++;
    last_send_status = status;
}

static prte_oob_tcp_send_t *queued_send(prte_oob_tcp_peer_t *peer)
{
    prte_oob_tcp_send_t *snd;
    prte_rml_send_t *msg;

    msg = PMIX_NEW(prte_rml_send_t);
    PMIX_XFER_PROCID(&msg->dst, &peer->name);
    PMIX_XFER_PROCID(&msg->origin, PRTE_PROC_MY_NAME);
    msg->tag = PRTE_RML_TAG_XCAST;
    msg->cbfunc = record_completion;
    PMIX_DATA_BUFFER_CREATE(msg->dbuf);

    snd = PMIX_NEW(prte_oob_tcp_send_t);
    snd->msg = msg;
    return snd;
}

static int test_queued_sends_complete_on_close(void)
{
    int failures = 0, i;
    prte_oob_tcp_peer_t *peer;

    peer = PMIX_NEW(prte_oob_tcp_peer_t);
    PMIX_LOAD_PROCID(&peer->name, PRTE_PROC_MY_NAME->nspace, 1);
    peer->sd = -1;
    /* a connection that was up: "established" is what keeps peer_close out of
     * the rotate-to-the-next-address branch, which deliberately keeps the
     * queue so it can go out over the next address */
    peer->established = true;
    peer->state = MCA_OOB_TCP_FAILED;

    completed_sends = 0;
    last_send_status = PRTE_SUCCESS;

    peer->send_msg = queued_send(peer);
    for (i = 0; i < 2; i++) {
        prte_oob_tcp_send_t *snd = queued_send(peer);
        pmix_list_append(&peer->send_queue, &snd->super);
    }

    prte_oob_tcp_peer_close(peer);

    CHECK("every queued send was completed, on-deck message included",
          3 == completed_sends);
    CHECK("and completed as a failure", PRTE_SUCCESS != last_send_status);
    CHECK("the on-deck slot was cleared", NULL == peer->send_msg);
    CHECK("the queue was drained", 0 == pmix_list_get_size(&peer->send_queue));

    PMIX_RELEASE(peer);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_queued_sends_complete_on_close\n");
    }
    return failures;
}

/* A connect handshake is read as its bytes arrive, never by waiting for them.
 *
 * The OOB's listening port accepts anyone, and the handshake is read on the
 * progress thread.  It used to be read with a loop that stayed in recv() until
 * every byte was in, so a single byte sent to the port - by a port scanner, a
 * stray client, anything - parked the progress thread for as long as the
 * sender kept the connection open: blocked in the kernel on Linux, spinning
 * on EAGAIN elsewhere.  A running job could not even finish.
 *
 * Driven over a socketpair: a well-formed IDENT delivered one byte at a time
 * must answer "not yet" after every byte but the last and then complete, and
 * connections that are not ours are turned away on the header alone, before
 * any payload is sized from them.
 */
static size_t build_ident(char *out, const char *nspace, uint32_t nbytes_override)
{
    prte_oob_tcp_hdr_t hdr;
    uint16_t ack = htons(1);
    size_t vlen = strlen(prte_version_string) + 1, hlen, len;

    memset(&hdr, 0, sizeof(hdr));
    hdr.origin = 3;
    hdr.dst = PRTE_PROC_MY_NAME->rank;
    hdr.type = MCA_OOB_TCP_IDENT;
    hdr.epoch = 1;
    PRTE_OOB_TCP_HDR_LOAD_NSPACE(&hdr, nspace);
    hdr.nbytes = (0 < nbytes_override) ? nbytes_override : (uint32_t) (sizeof(ack) + vlen);
    hlen = PRTE_OOB_TCP_HDR_LEN(&hdr);
    MCA_OOB_TCP_HDR_HTON(&hdr);
    memcpy(out, &hdr, hlen);
    len = hlen;
    memcpy(out + len, &ack, sizeof(ack));
    len += sizeof(ack);
    memcpy(out + len, prte_version_string, vlen);
    len += vlen;
    return len;
}

static bool make_pair(int sv[2])
{
    int flags;

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        return false;
    }
    flags = fcntl(sv[0], F_GETFL, 0);
    return (0 <= flags && 0 == fcntl(sv[0], F_SETFL, flags | O_NONBLOCK));
}

static bool fd_is_open(int fd)
{
    return (-1 != fcntl(fd, F_GETFD) || EBADF != errno);
}

/* an authentication message from `origin`: a header and one or two fields */
static size_t build_auth(char *out, prte_oob_tcp_msg_type_t type, pmix_rank_t origin,
                         pmix_rank_t dst, const uint8_t *first, const uint8_t *second)
{
    prte_oob_tcp_hdr_t hdr;
    size_t hlen, len;

    memset(&hdr, 0, sizeof(hdr));
    hdr.origin = origin;
    hdr.dst = dst;
    hdr.type = type;
    hdr.epoch = 1;
    PRTE_OOB_TCP_HDR_LOAD_NSPACE(&hdr, PRTE_PROC_MY_NAME->nspace);
    hdr.nbytes = (NULL == second) ? PRTE_OOB_TCP_AUTH_LEN : 2 * PRTE_OOB_TCP_AUTH_LEN;
    hlen = PRTE_OOB_TCP_HDR_LEN(&hdr);
    MCA_OOB_TCP_HDR_HTON(&hdr);
    memcpy(out, &hdr, hlen);
    len = hlen;
    memcpy(out + len, first, PRTE_OOB_TCP_AUTH_LEN);
    len += PRTE_OOB_TCP_AUTH_LEN;
    if (NULL != second) {
        memcpy(out + len, second, PRTE_OOB_TCP_AUTH_LEN);
        len += PRTE_OOB_TCP_AUTH_LEN;
    }
    return len;
}

/* read one handshake message off the blocking end of a pair: its header in
 * host order, and its payload into `payload` (at most `max` bytes) */
static bool read_message(int fd, prte_oob_tcp_hdr_t *hdr, uint8_t *payload, size_t max)
{
    size_t have = 0, want;
    ssize_t n;

    memset(hdr, 0, sizeof(*hdr));
    want = PRTE_OOB_TCP_HDR_FIXED;
    while (have < want) {
        n = read(fd, (char *) hdr + have, want - have);
        if (0 >= n) {
            return false;
        }
        have += (size_t) n;
        if (PRTE_OOB_TCP_HDR_FIXED == have) {
            want = PRTE_OOB_TCP_HDR_LEN(hdr);
        }
    }
    MCA_OOB_TCP_HDR_NTOH(hdr);
    if (hdr->nbytes > max) {
        return false;
    }
    for (have = 0; have < hdr->nbytes; have += (size_t) n) {
        n = read(fd, payload + have, hdr->nbytes - have);
        if (0 >= n) {
            return false;
        }
    }
    return true;
}

/* How a test dialer answers the listener's challenge */
#define ANSWER_HONEST  0
#define ANSWER_WRONG   1   // a proof made with the wrong key
#define ANSWER_REFLECT 2   // the listener's own proof, sent back to it

/* Act as a dialer claiming `rank` against the listener reading sv[0]:
 * HELLO, take the CHALLENGE, check the listener's proof, and answer as told.
 * Returns what the listener made of the answer. */
static int authenticate(int sv[2], prte_oob_tcp_handshake_t *hs, pmix_rank_t rank, int how,
                        bool *listener_proved)
{
    uint8_t dnonce[PRTE_OOB_TCP_AUTH_LEN], payload[2 * PRTE_OOB_TCP_AUTH_LEN];
    uint8_t expect[PRTE_SHA256_DIGEST_LEN], proof[PRTE_SHA256_DIGEST_LEN];
    char wire[PMIX_MAX_NSLEN + 512];
    prte_oob_tcp_hdr_t hdr;
    size_t len;
    int rc;

    *listener_proved = false;
    memset(dnonce, 0x5a, sizeof(dnonce));
    len = build_auth(wire, MCA_OOB_TCP_AUTH_HELLO, rank, PRTE_PROC_MY_NAME->rank, dnonce, NULL);
    if ((ssize_t) len != write(sv[1], wire, len)) {
        return PRTE_ERROR;
    }
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], hs, &hdr);
    if (PRTE_ERR_WOULD_BLOCK != rc) {
        return rc;
    }
    if (!read_message(sv[1], &hdr, payload, sizeof(payload))
        || MCA_OOB_TCP_AUTH_CHALLENGE != hdr.type || 2 * PRTE_OOB_TCP_AUTH_LEN != hdr.nbytes) {
        return PRTE_ERROR;
    }
    prte_oob_tcp_auth_mac('L', PRTE_PROC_MY_NAME->nspace, rank, PRTE_PROC_MY_NAME->rank, dnonce,
                          payload, expect);
    *listener_proved = (0 == memcmp(expect, payload + PRTE_OOB_TCP_AUTH_LEN, sizeof(expect)));

    if (ANSWER_REFLECT == how) {
        memcpy(proof, payload + PRTE_OOB_TCP_AUTH_LEN, sizeof(proof));
    } else {
        prte_oob_tcp_auth_mac('D', PRTE_PROC_MY_NAME->nspace, rank, PRTE_PROC_MY_NAME->rank,
                              dnonce, payload, proof);
        if (ANSWER_WRONG == how) {
            proof[0] ^= 0x01;
        }
    }
    len = build_auth(wire, MCA_OOB_TCP_AUTH_RESPONSE, rank, PRTE_PROC_MY_NAME->rank, proof, NULL);
    if ((ssize_t) len != write(sv[1], wire, len)) {
        return PRTE_ERROR;
    }
    return prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], hs, &hdr);
}

static bool peer_recorded(pmix_rank_t rank)
{
    pmix_proc_t p;

    PMIX_LOAD_PROCID(&p, PRTE_PROC_MY_NAME->nspace, rank);
    return (NULL != prte_oob_tcp_peer_lookup(&p));
}

static int test_handshake_never_waits(void)
{
    int failures = 0, sv[2], rc = PRTE_ERROR;
    char wire[PMIX_MAX_NSLEN + 512];
    size_t len, i;
    prte_oob_tcp_handshake_t hs;
    prte_oob_tcp_hdr_t hdr;
    prte_oob_tcp_peer_t *peer;
    bool early = false, proved = false;

    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-test-dvm");
    PRTE_PROC_MY_NAME->rank = 0;
    memset(&hs, 0, sizeof(hs));

    /* a well-formed ident, one byte at a time - once the dialer has proved
     * itself, which is the only time an ident is listened to */
    CHECK("socketpair", make_pair(sv));
    CHECK("the dialer authenticated",
          PRTE_ERR_WOULD_BLOCK == authenticate(sv, &hs, 3, ANSWER_HONEST, &proved));
    CHECK("and so did the listener", proved);
    len = build_ident(wire, PRTE_PROC_MY_NAME->nspace, 0);
    for (i = 0; i < len; i++) {
        CHECK("wrote a byte", 1 == write(sv[1], wire + i, 1));
        memset(&hdr, 0, sizeof(hdr));
        rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
        if (i + 1 < len && PRTE_ERR_WOULD_BLOCK != rc) {
            early = true;
            break;
        }
    }
    CHECK("every partial handshake answered 'not yet'", !early);
    CHECK("the complete handshake was accepted", PRTE_SUCCESS == rc);
    CHECK("and names its sender", 3 == hdr.origin && MCA_OOB_TCP_IDENT == hdr.type);
    CHECK("the record was left ready for another message",
          NULL == hs.payload && 0 == hs.hdr_rcvd && !hs.sized);
    prte_oob_tcp_handshake_clear(&hs);
    peer = prte_oob_tcp_peer_lookup(&(pmix_proc_t){.nspace = "prte-test-dvm", .rank = 3});
    CHECK("a peer was recorded for the sender", NULL != peer);
    if (NULL != peer) {
        pmix_list_remove_item(&prte_oob_base.peers, &peer->super);
        PMIX_RELEASE(peer);
    }
    close(sv[0]);
    close(sv[1]);

    /* one byte and then silence: the call must come straight back */
    CHECK("socketpair", make_pair(sv));
    CHECK("wrote a byte", 1 == write(sv[1], wire, 1));
    CHECK("a lone byte does not hold the caller",
          PRTE_ERR_WOULD_BLOCK == prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr));
    CHECK("nor does a second look with nothing new",
          PRTE_ERR_WOULD_BLOCK == prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[0]);
    close(sv[1]);

    /* another DVM's daemon is refused on its header, and its socket closed */
    CHECK("socketpair", make_pair(sv));
    len = build_ident(wire, "some-other-dvm", 0);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("a foreign namespace is refused", PRTE_ERR_CONNECTION_REFUSED == rc);
    CHECK("and the connection disposed of", !fd_is_open(sv[0]));
    CHECK("with nothing allocated for it", NULL == hs.payload);
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* a payload too short to hold the ack flag is refused, not over-read */
    CHECK("socketpair", make_pair(sv));
    len = build_ident(wire, PRTE_PROC_MY_NAME->nspace, 1);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("a one-byte ident payload is refused", PRTE_SUCCESS != rc && PRTE_ERR_WOULD_BLOCK != rc);
    CHECK("and the connection disposed of", !fd_is_open(sv[0]));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* an ident is an ack flag and a version string, so one that claims a
     * payload far longer than that is refused on its header - well under
     * prte_max_msg_size, which bounds messages, not handshakes */
    CHECK("socketpair", make_pair(sv));
    len = build_ident(wire, PRTE_PROC_MY_NAME->nspace, 4096);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("an overlong ident is refused", PRTE_SUCCESS != rc && PRTE_ERR_WOULD_BLOCK != rc);
    CHECK("and the connection disposed of", !fd_is_open(sv[0]));
    CHECK("with nothing allocated for it", NULL == hs.payload);
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* a namespace that is not text is refused like any other, and the
     * refusal - which quotes it - prints it with '?' for each control byte */
    CHECK("socketpair", make_pair(sv));
    len = build_ident(wire, "\x1b[2J\x07not-a-dvm", 0);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("a namespace of control bytes is refused", PRTE_ERR_CONNECTION_REFUSED == rc);
    CHECK("and the connection disposed of", !fd_is_open(sv[0]));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_handshake_never_waits\n");
    }
    return failures;
}

/* Nothing that cannot prove it holds the DVM key is treated as a daemon.
 *
 * The listening port answers whatever connects to it, and before
 * authentication the only thing that told a daemon of this DVM from any other
 * connection was the DVM's namespace - which is on every prted's command
 * line.  Each case here is a way of not proving it, and each must be refused
 * with the connection closed and no peer recorded: recording one, or letting
 * an unproven IDENT reach the connection-race handling, is what let a foreign
 * connection stand in for a daemon or knock a real one off its connection.
 */
static int test_handshake_authenticates(void)
{
    int failures = 0, sv[2], rc;
    char wire[PMIX_MAX_NSLEN + 512];
    uint8_t nonce[PRTE_OOB_TCP_AUTH_LEN], key[PRTE_DVM_KEY_LEN];
    prte_oob_tcp_handshake_t hs;
    prte_oob_tcp_hdr_t hdr;
    size_t len;
    bool proved;

    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-test-dvm");
    PRTE_PROC_MY_NAME->rank = 0;
    memset(&hs, 0, sizeof(hs));
    memset(nonce, 0x33, sizeof(nonce));

    /* an IDENT straight away - right namespace, right version: everything
     * that used to be enough */
    CHECK("socketpair", make_pair(sv));
    len = build_ident(wire, PRTE_PROC_MY_NAME->nspace, 0);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("an ident before authenticating is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    CHECK("with no peer recorded", !peer_recorded(3));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* a proof made with some other key */
    CHECK("socketpair", make_pair(sv));
    rc = authenticate(sv, &hs, 3, ANSWER_WRONG, &proved);
    CHECK("the listener proved itself to an honest check", proved);
    CHECK("a wrong proof is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    CHECK("with no peer recorded", !peer_recorded(3));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* the listener's own proof, handed back as the dialer's */
    CHECK("socketpair", make_pair(sv));
    rc = authenticate(sv, &hs, 3, ANSWER_REFLECT, &proved);
    CHECK("a reflected proof is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* proved as rank 4, then claims to be rank 5 */
    CHECK("socketpair", make_pair(sv));
    rc = authenticate(sv, &hs, 4, ANSWER_HONEST, &proved);
    CHECK("an honest proof is accepted", PRTE_ERR_WOULD_BLOCK == rc);
    len = build_ident(wire, PRTE_PROC_MY_NAME->nspace, 0);
    ((prte_oob_tcp_hdr_t *) wire)->origin = htonl(5);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("an ident for another rank than the one proved is refused",
          PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    CHECK("with no peer recorded for either rank", !peer_recorded(4) && !peer_recorded(5));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* ranks no daemon of this DVM would claim */
    CHECK("socketpair", make_pair(sv));
    len = build_auth(wire, MCA_OOB_TCP_AUTH_HELLO, PMIX_RANK_WILDCARD, PRTE_PROC_MY_NAME->rank,
                     nonce, NULL);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("the wildcard rank is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    CHECK("socketpair", make_pair(sv));
    len = build_auth(wire, MCA_OOB_TCP_AUTH_HELLO, PRTE_PROC_MY_NAME->rank,
                     PRTE_PROC_MY_NAME->rank, nonce, NULL);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("our own rank is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* an authentication message of the wrong size is turned away on its
     * header, before anything is allocated for it */
    CHECK("socketpair", make_pair(sv));
    len = build_auth(wire, MCA_OOB_TCP_AUTH_HELLO, 3, PRTE_PROC_MY_NAME->rank, nonce, nonce);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("a HELLO of the wrong size is refused",
          PRTE_SUCCESS != rc && PRTE_ERR_WOULD_BLOCK != rc);
    CHECK("and the connection closed", !fd_is_open(sv[0]));
    CHECK("with nothing allocated for it", NULL == hs.payload);
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[1]);

    /* a listener holding a different key cannot prove itself */
    memcpy(key, prte_dvm_key, sizeof(key));
    CHECK("socketpair", make_pair(sv));
    prte_dvm_key[0] ^= 0xff;
    len = build_auth(wire, MCA_OOB_TCP_AUTH_HELLO, 3, PRTE_PROC_MY_NAME->rank, nonce, NULL);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(NULL, sv[0], &hs, &hdr);
    CHECK("the listener answered", PRTE_ERR_WOULD_BLOCK == rc);
    memcpy(prte_dvm_key, key, sizeof(key));
    {
        uint8_t payload[2 * PRTE_OOB_TCP_AUTH_LEN], expect[PRTE_SHA256_DIGEST_LEN];
        CHECK("its challenge arrived",
              read_message(sv[1], &hdr, payload, sizeof(payload))
                  && MCA_OOB_TCP_AUTH_CHALLENGE == hdr.type);
        prte_oob_tcp_auth_mac('L', PRTE_PROC_MY_NAME->nspace, 3, PRTE_PROC_MY_NAME->rank, nonce,
                              payload, expect);
        CHECK("and its proof fails against the real key",
              0 != memcmp(expect, payload + PRTE_OOB_TCP_AUTH_LEN, sizeof(expect)));
    }
    prte_oob_tcp_handshake_clear(&hs);
    close(sv[0]);
    close(sv[1]);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_handshake_authenticates\n");
    }
    return failures;
}

/* A dialer gives its proof only to a listener that has given its own.
 *
 * Whatever answers at a daemon's address - some other process now on a port
 * a dead daemon left behind, say - gets nothing from a dialer that checks the
 * challenge first, and an honest challenge gets the proof and, straight
 * behind it, the IDENT the connection has always opened with.
 */
static int test_dialer_checks_listener(void)
{
    int failures = 0, sv[2], rc;
    char wire[PMIX_MAX_NSLEN + 512];
    uint8_t lnonce[PRTE_OOB_TCP_AUTH_LEN], proof[PRTE_SHA256_DIGEST_LEN];
    uint8_t payload[PMIX_MAX_NSLEN + 256], expect[PRTE_SHA256_DIGEST_LEN];
    prte_oob_tcp_peer_t *peer;
    prte_oob_tcp_hdr_t hdr;
    size_t len;
    ssize_t n;
    char byte;

    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, "prte-test-dvm");
    PRTE_PROC_MY_NAME->rank = 0;
    memset(lnonce, 0x77, sizeof(lnonce));

    /* an honest listener */
    CHECK("socketpair", make_pair(sv));
    peer = PMIX_NEW(prte_oob_tcp_peer_t);
    PMIX_LOAD_PROCID(&peer->name, PRTE_PROC_MY_NAME->nspace, 6);
    peer->sd = sv[0];
    peer->state = MCA_OOB_TCP_CONNECT_ACK;
    peer->hshake.auth.phase = PRTE_OOB_TCP_AUTH_HELLO_SENT;
    peer->hshake.auth.rank = 6;
    memset(peer->hshake.auth.dialer_nonce, 0x44, PRTE_OOB_TCP_AUTH_LEN);
    prte_oob_tcp_auth_mac('L', PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_NAME->rank, 6,
                          peer->hshake.auth.dialer_nonce, lnonce, proof);
    len = build_auth(wire, MCA_OOB_TCP_AUTH_CHALLENGE, 6, PRTE_PROC_MY_NAME->rank, lnonce, proof);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(peer, sv[0], &peer->hshake, NULL);
    CHECK("an honest challenge is answered", PRTE_ERR_WOULD_BLOCK == rc);
    CHECK("and authentication is done", PRTE_OOB_TCP_AUTH_DONE == peer->hshake.auth.phase);
    CHECK("the response arrived",
          read_message(sv[1], &hdr, payload, sizeof(payload))
              && MCA_OOB_TCP_AUTH_RESPONSE == hdr.type);
    prte_oob_tcp_auth_mac('D', PRTE_PROC_MY_NAME->nspace, PRTE_PROC_MY_NAME->rank, 6,
                          peer->hshake.auth.dialer_nonce, lnonce, expect);
    CHECK("carrying the dialer's proof", 0 == memcmp(expect, payload, sizeof(expect)));
    CHECK("followed by the ident",
          read_message(sv[1], &hdr, payload, sizeof(payload)) && MCA_OOB_TCP_IDENT == hdr.type
              && PRTE_PROC_MY_NAME->rank == hdr.origin);
    peer->sd = -1;
    PMIX_RELEASE(peer);
    close(sv[0]);
    close(sv[1]);

    /* something at the peer's address that does not hold the key */
    CHECK("socketpair", make_pair(sv));
    peer = PMIX_NEW(prte_oob_tcp_peer_t);
    PMIX_LOAD_PROCID(&peer->name, PRTE_PROC_MY_NAME->nspace, 6);
    peer->sd = sv[0];
    peer->state = MCA_OOB_TCP_CONNECT_ACK;
    peer->established = true;
    peer->hshake.auth.phase = PRTE_OOB_TCP_AUTH_HELLO_SENT;
    peer->hshake.auth.rank = 6;
    memset(peer->hshake.auth.dialer_nonce, 0x44, PRTE_OOB_TCP_AUTH_LEN);
    memset(proof, 0x99, sizeof(proof));
    len = build_auth(wire, MCA_OOB_TCP_AUTH_CHALLENGE, 6, PRTE_PROC_MY_NAME->rank, lnonce, proof);
    CHECK("wrote it", (ssize_t) len == write(sv[1], wire, len));
    rc = prte_oob_tcp_peer_recv_connect_ack(peer, sv[0], &peer->hshake, NULL);
    CHECK("a challenge without a valid proof is refused", PRTE_ERR_AUTHENTICATION_FAILED == rc);
    CHECK("and the connection closed", 0 > peer->sd && !fd_is_open(sv[0]));
    n = read(sv[1], &byte, 1);
    CHECK("having sent it nothing", 0 == n);
    PMIX_RELEASE(peer);
    close(sv[1]);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_dialer_checks_listener\n");
    }
    return failures;
}

/* An inbound connection cannot hold a descriptor for long, nor can many.
 *
 * Every connection that has not finished its handshake holds a descriptor,
 * and anything able to reach the port can open them.  With nothing to bound
 * that, idle connections used up every descriptor the daemon had - and the
 * listener that met EMFILE closed itself for good.  Now each is counted
 * against a limit overall and per address, and given a deadline.
 */
/* run the event loop until nothing is pending, for at most `secs` */
static void run_until_none_pending(int secs)
{
    int i;

    for (i = 0; i < secs * 100 && 0 < prte_oob_base.num_pending; i++) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
        if (0 < prte_oob_base.num_pending) {
            usleep(10000);
        }
    }
}

/* a connected pair of loopback TCP sockets - what accept() really hands
 * over, socket options and all */
static bool tcp_pair(int sv[2])
{
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int ls;

    sv[0] = sv[1] = -1;
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > ls) {
        return false;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != bind(ls, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(ls, 4)
        || 0 != getsockname(ls, (struct sockaddr *) &sa, &slen)) {
        close(ls);
        return false;
    }
    sv[1] = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sv[1] || 0 != connect(sv[1], (struct sockaddr *) &sa, sizeof(sa))) {
        close(ls);
        return false;
    }
    sv[0] = accept(ls, NULL, NULL);
    close(ls);
    return (0 <= sv[0]);
}

static int test_pending_handshakes_bounded(void)
{
    int failures = 0, sv[4][2], i;
    struct sockaddr_in a, b;
    int save_max = prte_oob_base.max_pending;
    int save_host = prte_oob_base.max_pending_per_host;
    int save_tmo = prte_oob_base.handshake_timeout;
    prte_event_base_t *save_base = prte_event_base;

    /* The accept path arms its events on the main base, and this test runs
     * that base.  Give it one of its own, so that what it runs is only what
     * it armed - not whatever an earlier test left queued there. */
    prte_event_base = prte_event_base_create();
    CHECK("event base", NULL != prte_event_base);

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = htons(40000);
    b = a;
    b.sin_addr.s_addr = htonl(0x7f000002);

    CHECK("nothing pending to begin with", 0 == prte_oob_base.num_pending);

    /* at most three in all, and two from any one address */
    prte_oob_base.max_pending = 3;
    prte_oob_base.max_pending_per_host = 2;
    prte_oob_base.handshake_timeout = 0;
    for (i = 0; i < 4; i++) {
        CHECK("tcp pair", tcp_pair(sv[i]));
    }
    prte_oob_accept_connection(sv[0][0], (struct sockaddr *) &a);
    prte_oob_accept_connection(sv[1][0], (struct sockaddr *) &a);
    CHECK("two from one address are taken", 2 == prte_oob_base.num_pending
          && fd_is_open(sv[0][0]) && fd_is_open(sv[1][0]));
    fprintf(stdout, "-- the next case is refused; the message it prints is expected --\n");
    prte_oob_accept_connection(sv[2][0], (struct sockaddr *) &a);
    CHECK("a third from the same address is not", 2 == prte_oob_base.num_pending
          && !fd_is_open(sv[2][0]));
    prte_oob_accept_connection(sv[3][0], (struct sockaddr *) &b);
    CHECK("one from another address is", 3 == prte_oob_base.num_pending && fd_is_open(sv[3][0]));
    close(sv[2][1]);
    CHECK("tcp pair", tcp_pair(sv[2]));
    prte_oob_accept_connection(sv[2][0], (struct sockaddr *) &b);
    CHECK("and none past the overall limit", 3 == prte_oob_base.num_pending
          && !fd_is_open(sv[2][0]));
    close(sv[2][1]);

    /* a connection that goes away gives its place back */
    for (i = 0; i < 4; i++) {
        if (2 != i) {
            close(sv[i][1]);
        }
    }
    run_until_none_pending(5);
    CHECK("closed connections are released", 0 == prte_oob_base.num_pending
          && 0 == pmix_list_get_size(&prte_oob_base.pending_hosts));

    /* and one that just sits there is dropped at its deadline */
    prte_oob_base.handshake_timeout = 1;
    CHECK("tcp pair", tcp_pair(sv[0]));
    prte_oob_accept_connection(sv[0][0], (struct sockaddr *) &a);
    CHECK("an idle connection is taken", 1 == prte_oob_base.num_pending);
    run_until_none_pending(5);
    CHECK("and dropped at its deadline", 0 == prte_oob_base.num_pending
          && !fd_is_open(sv[0][0]));
    close(sv[0][1]);

    prte_oob_base.max_pending = save_max;
    prte_oob_base.max_pending_per_host = save_host;
    prte_oob_base.handshake_timeout = save_tmo;
    prte_event_base_free(prte_event_base);
    prte_event_base = save_base;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_pending_handshakes_bounded\n");
    }
    return failures;
}

/* A connection attempt that has been overtaken must not dial.
 *
 * prte_oob_tcp_peer_try_connect can run long after it was scheduled - a retry
 * waits on a timer - and by then the peer may have connected by dialing us.
 * Carrying on closed the socket that live connection was using.  Everything
 * that schedules an attempt sets CONNECTING first, so any other state means
 * the attempt is stale.  (The reachability stub is here so that, were the
 * guard missing, the test would see the socket closed rather than crash on a
 * framework a unit test never opens.)
 */
static prte_reachable_t *no_route(pmix_list_t *local_ifs, pmix_list_t *remote_ifs)
{
    PRTE_HIDE_UNUSED_PARAMS(local_ifs, remote_ifs);
    return NULL;
}

static int test_stale_attempt_does_not_dial(void)
{
    int failures = 0, sv[2];
    prte_oob_tcp_peer_t *peer;
    prte_oob_tcp_conn_op_t *op;
    prte_reachable_base_module_reachable_fn_t save = prte_reachable.reachable;

    prte_reachable.reachable = no_route;
    CHECK("socketpair", make_pair(sv));

    peer = PMIX_NEW(prte_oob_tcp_peer_t);
    PMIX_LOAD_PROCID(&peer->name, PRTE_PROC_MY_NAME->nspace, 5);
    peer->sd = sv[0];
    peer->state = MCA_OOB_TCP_CONNECTED;
    peer->established = true;

    op = PMIX_NEW(prte_oob_tcp_conn_op_t);
    op->peer = peer;
    prte_oob_tcp_peer_try_connect(-1, 0, op);

    CHECK("the connected peer kept its socket", sv[0] == peer->sd);
    CHECK("which is still open", fd_is_open(sv[0]));
    CHECK("and it is still connected", MCA_OOB_TCP_CONNECTED == peer->state);

    /* the socket is ours to close, not the peer's */
    peer->sd = -1;
    PMIX_RELEASE(peer);
    close(sv[0]);
    close(sv[1]);
    prte_reachable.reachable = save;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_stale_attempt_does_not_dial\n");
    }
    return failures;
}

/*
 * The interfaces prte_oob_open selects, and the sockets it listens on.
 *
 * Two things were wrong here, and a user confining a single-node job to
 * loopback met both.  Loopback was discarded before the include list was
 * consulted, whenever the host had any other interface - so an include list
 * naming only the loopback interface left nothing, and the DVM refused to
 * start.  And the listener was bound to the wildcard address regardless of
 * what had been selected, so an excluded interface - every external one, in
 * that case - still accepted connections.
 *
 * These drive prte_oob_open for real, with real sockets, as the DVM master;
 * prte_oob_close then tears the listen thread down again.
 */
static bool find_loopback_ipv4(char *name, int namelen, char *addr, size_t addrlen)
{
    pmix_pif_t *ifp;

    PMIX_LIST_FOREACH(ifp, &pmix_if_list, pmix_pif_t)
    {
        if (AF_INET != ((struct sockaddr *) &ifp->if_addr)->sa_family ||
            !(ifp->if_flags & IFF_LOOPBACK)) {
            continue;
        }
        if (NULL == inet_ntop(AF_INET, &((struct sockaddr_in *) &ifp->if_addr)->sin_addr,
                              addr, addrlen)) {
            continue;
        }
        if (PMIX_SUCCESS != pmix_ifkindextoname(ifp->if_kernel_index, name, namelen)) {
            continue;
        }
        return true;
    }
    return false;
}

/* every listener is bound to an address we advertise - never the wildcard -
 * and all listeners of a family share the one port the URI carries */
static int check_listeners(const char *label)
{
    int failures = 0;
    prte_oob_tcp_listener_t *listener;
    char addr[INET6_ADDRSTRLEN], lbl[256];
    char **conns;
    int port4 = -1, port6 = -1, *port;

    CHECK(label, 0 < pmix_list_get_size(&prte_oob_base.listeners));
    PMIX_LIST_FOREACH(listener, &prte_oob_base.listeners, prte_oob_tcp_listener_t)
    {
        if (listener->tcp6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *) &listener->addr)->sin6_addr,
                      addr, sizeof(addr));
            conns = prte_oob_base.ipv6conns;
            port = &port6;
        } else {
            inet_ntop(AF_INET, &((struct sockaddr_in *) &listener->addr)->sin_addr,
                      addr, sizeof(addr));
            conns = prte_oob_base.ipv4conns;
            port = &port4;
        }
        snprintf(lbl, sizeof(lbl), "%s: listener on %s is advertised", label, addr);
        CHECK(lbl, 0 < count_matches(conns, addr));
        snprintf(lbl, sizeof(lbl), "%s: listener on %s is not the wildcard", label, addr);
        CHECK(lbl, 0 != strcmp(addr, "0.0.0.0") && 0 != strcmp(addr, "::"));
        if (0 > *port) {
            *port = listener->port;
        }
        snprintf(lbl, sizeof(lbl), "%s: listener on %s shares its family's port", label, addr);
        CHECK(lbl, *port == listener->port);
    }
    if (0 <= port4) {
        snprintf(lbl, sizeof(lbl), "%s: one IPv4 port advertised", label);
        CHECK(lbl, 1 == PMIx_Argv_count(prte_oob_base.ipv4ports) &&
                   port4 == atoi(prte_oob_base.ipv4ports[0]));
    }
    return failures;
}

static int test_listeners_bound_to_selection(void)
{
    int failures = 0, rc;

    rc = prte_oob_open();
    CHECK("default open", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        failures += check_listeners("default");
    }
    prte_oob_close();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_listeners_bound_to_selection\n");
    }
    return failures;
}

/* is there a non-loopback IPv4 interface, and the names of every
 * non-loopback interface as an exclude list */
static bool have_other_ipv4_interface(void)
{
    pmix_pif_t *ifp;

    PMIX_LIST_FOREACH(ifp, &pmix_if_list, pmix_pif_t)
    {
        if (AF_INET == ((struct sockaddr *) &ifp->if_addr)->sa_family &&
            !(ifp->if_flags & IFF_LOOPBACK)) {
            return true;
        }
    }
    return false;
}

static char *all_but_loopback(void)
{
    pmix_pif_t *ifp;
    char **names = NULL, *str;

    PMIX_LIST_FOREACH(ifp, &pmix_if_list, pmix_pif_t)
    {
        if (!(ifp->if_flags & IFF_LOOPBACK)) {
            PMIx_Argv_append_unique_nosize(&names, ifp->if_name);
        }
    }
    str = PMIx_Argv_join(names, ',');
    PMIx_Argv_free(names);
    return str;
}

static int test_loopback_include_honored(void)
{
    int failures = 0, rc;
    char loname[IF_NAMESIZE], loaddr[INET_ADDRSTRLEN];
    char *save_include = prte_if_include;
    prte_oob_tcp_listener_t *listener;
    char addr[INET_ADDRSTRLEN];
    prte_proc_type_t save_type = prte_process_info.proc_type;

    if (!find_loopback_ipv4(loname, sizeof(loname), loaddr, sizeof(loaddr))) {
        fprintf(stdout, "SKIPPED test_loopback_include_honored (no IPv4 loopback)\n");
        return 0;
    }

    /* the master takes loopback when it is all the user asked for */
    prte_if_include = strdup(loname);
    rc = prte_oob_open();
    CHECK("loopback-only include opens", PRTE_SUCCESS == rc);
    if (PRTE_SUCCESS == rc) {
        CHECK("only loopback advertised", 1 == PMIx_Argv_count(prte_oob_base.ipv4conns) &&
                                          0 == strcmp(loaddr, prte_oob_base.ipv4conns[0]));
        CHECK("masks match addresses", 1 == PMIx_Argv_count(prte_oob_base.ipv4masks));
        failures += check_listeners("loopback");
        /* where IPv6 is enabled, naming the interface selects its IPv6
         * loopback address too - but nothing else */
        PMIX_LIST_FOREACH(listener, &prte_oob_base.listeners, prte_oob_tcp_listener_t)
        {
            if (listener->tcp6) {
                CHECK("IPv6 listener bound to loopback",
                      IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *) &listener->addr)->sin6_addr));
            } else {
                inet_ntop(AF_INET, &((struct sockaddr_in *) &listener->addr)->sin_addr,
                          addr, sizeof(addr));
                CHECK("IPv4 listener bound to loopback", 0 == strcmp(loaddr, addr));
            }
        }
    }
    prte_oob_close();
    free(prte_if_include);

    /* an exclude list is not a request for loopback: one that leaves nothing
     * else must still fail here, rather than start a DVM whose remote daemons
     * are handed an address they cannot use */
    if (have_other_ipv4_interface()) {
        fprintf(stdout, "-- the next case excludes every other interface;"
                        " the error it prints is expected --\n");
        prte_if_include = NULL;
        prte_if_exclude = all_but_loopback();
        rc = prte_oob_open();
        CHECK("exclude leaving only loopback refused", PRTE_ERR_NOT_AVAILABLE == rc);
        prte_oob_close();
        free(prte_if_exclude);
        prte_if_exclude = NULL;
    }

    /* ...and a daemon never does: nothing on another node could reach it */
    fprintf(stdout, "-- the next case refuses a loopback-only daemon;"
                    " the error it prints is expected --\n");
    prte_process_info.proc_type = PRTE_PROC_DAEMON;
    prte_if_include = strdup(loname);
    rc = prte_oob_open();
    CHECK("daemon refuses loopback-only", PRTE_ERR_NOT_AVAILABLE == rc);
    CHECK("daemon advertises nothing", NULL == prte_oob_base.ipv4conns);
    prte_oob_close();
    free(prte_if_include);
    prte_process_info.proc_type = save_type;
    prte_if_include = save_include;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_loopback_include_honored\n");
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
    /* daemons prove to each other that they hold the DVM key */
    if (PRTE_SUCCESS != prte_dvm_key_generate()) {
        fprintf(stderr, "prte_dvm_key_generate failed\n");
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
    failures += test_listeners_bound_to_selection();
    failures += test_loopback_include_honored();
    /* reports refusals with pmix_net_get_hostname(), so it too has to run
     * before PMIx is finalized below */
    failures += test_pending_handshakes_bounded();
    /* PMIx_server_finalize, at the end of this one, takes PMIx's utility
     * layer down with it - the interface list included - so anything that
     * needs a local interface has to run before it */
    failures += test_payload_outlives_sends();
    failures += test_peer_base_assignment();
    failures += test_queued_sends_complete_on_close();
    failures += test_wire_header();
    failures += test_handshake_never_waits();
    fprintf(stdout, "-- the next two tests drive refused connections; the messages they print "
                    "are expected --\n");
    failures += test_handshake_authenticates();
    failures += test_dialer_checks_listener();
    failures += test_stale_attempt_does_not_dial();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rml unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rml unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
