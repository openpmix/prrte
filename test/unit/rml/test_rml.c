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

#include "src/rml/oob/oob_tcp.h"

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

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all rml unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d rml unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
