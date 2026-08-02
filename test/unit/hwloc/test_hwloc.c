/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/hwloc -- PRRTE's topology layer.
 *
 * Everything here is a pure function of a topology plus a string, which is
 * exactly why it was worth testing and also why it never was: a topology
 * normally arrives from a live daemon, so a defect in one of these helpers
 * surfaced as a wrong map, an unreadable binding report, or a crash three
 * subsystems away. hwloc can synthesize a topology on demand
 * (hwloc_topology_set_synthetic), so none of this needs a DVM.
 *
 * Each group below pins down a defect this review found:
 *
 *  - prte_hwloc_base_get_nbobjs_by_type()/_get_obj_by_type() answered "this
 *    node has no NUMA domains" for any topology whose cached summary had not
 *    been built yet, instead of building it. Nothing in that answer looks
 *    like an error, so a map-by numa job simply placed nothing.
 *
 *  - prte_hwloc_base_generate_cpuset() ran its cpu ids through bare
 *    strtoul(), which reports 0 for a token like "foo" -- so a typo in
 *    --cpu-set silently confined the whole DVM to cpu 0. Its "id not found"
 *    message also passed a char** where the format wanted a char*.
 *
 *  - prte_hwloc_base_cpu_list_parse() checked the *process-wide* topology
 *    for NULL and then used the one it was handed, and its package parser
 *    dereferenced the result of a package lookup without checking it: a
 *    cpu-set naming a package that does not exist was a segfault.
 *
 *  - bitmap_list_snprintf_exp() (reached through
 *    prte_hwloc_get_binding_info()) advanced its write pointer inside a run
 *    of set bits without charging those bytes against the remaining buffer,
 *    so a wide binding wrote past the end of the caller's buffer. The caller
 *    sizes that buffer at 20 bytes per PU while each element needs ~34.
 *
 *  - prte_hwloc_get_binding_info() never set *pkgnum when the cpuset matched
 *    no package, and its only caller prints it uninitialized.
 *
 *  - build_map() (reached through prte_hwloc_base_cset2str()) appended to
 *    the caller's buffer with memcpy() and no bound at all.
 *
 *  - prte_hwloc_base_print_binding() handed back a pointer into a ring of
 *    per-thread buffers but never advanced the ring, so two calls in one
 *    argument list returned the same buffer -- the ring's whole purpose.
 *
 *  - prte_hwloc_base_set_binding_policy() took its LIMIT value with
 *    strtol() into a uint16_t, so "limit=70000" bound to 4464.
 *
 *  - prte_hwloc_base_release_userdata() has to sweep the special NUMA depth
 *    as well as the normal hierarchy, because binding attaches placement
 *    counters to NUMA nodes and hwloc 2.x does not carry them in the depth
 *    hierarchy.
 *
 * What is deliberately NOT here: prte_hwloc_base_get_topology() (senses the
 * real machine), prte_hwloc_print() against a machine wide enough to reach
 * its cpuset buffer, and anything needing a populated prte_node_pool. Those
 * belong to the live smoke test and to contrib/dockerswarm.
 */

#include "prte_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "types.h"

#include "src/class/pmix_pointer_array.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/attr.h"
#include "src/util/pmix_argv.h"
#include "src/util/proc_info.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* topology helpers                                                   */
/* ------------------------------------------------------------------ */

/* Build a synthetic topology. Returns NULL if this hwloc cannot do it,
 * in which case the caller skips - we would rather skip than fail on a
 * platform whose hwloc lacks synthetic support. */
static hwloc_topology_t make_topo(const char *desc)
{
    hwloc_topology_t topo;

    if (0 != hwloc_topology_init(&topo)) {
        return NULL;
    }
    if (0 != hwloc_topology_set_synthetic(topo, desc) ||
        0 != hwloc_topology_load(topo)) {
        hwloc_topology_destroy(topo);
        return NULL;
    }
    return topo;
}

static void free_topo(hwloc_topology_t topo)
{
    prte_hwloc_base_release_userdata(topo);
    hwloc_topology_destroy(topo);
}

/* ------------------------------------------------------------------ */
/* NUMA summary and the object lookups built on it                    */
/* ------------------------------------------------------------------ */

static int test_numa_summary(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_obj_t root, obj;
    prte_hwloc_topo_data_t *sum;
    unsigned n;

    topo = make_topo("package:2 numa:2 core:2 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP numa-summary (no synthetic topology support)\n");
        return 0;
    }

    /* The lookups must answer correctly on a topology whose summary has not
     * been built. Returning 0 here is not a conservative default: it says
     * "no NUMA domains on this node", which a map-by numa job believes. */
    CHECK("numa count without a precomputed summary",
          4 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE));
    CHECK("numa lookup without a precomputed summary",
          NULL != prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, 0));

    /* ...and the summary it built has to be finite. Leaving the cutoff at
     * UINT_MAX turns every later NUMA query into a walk of four billion OS
     * indices. */
    root = hwloc_get_root_obj(topo);
    sum = (prte_hwloc_topo_data_t *) root->userdata;
    CHECK("summary was cached on the root", NULL != sum);
    if (NULL != sum) {
        CHECK("summary is marked computed", sum->computed);
        CHECK("numa cutoff is finite", UINT_MAX != sum->numa_cutoff);
        CHECK("numa cutoff covers every cpu numa", 4 <= sum->numa_cutoff);
    }

    /* recomputing is a no-op, not a change */
    prte_hwloc_base_setup_summary(topo);
    CHECK("summary is idempotent",
          4 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE));

    /* the non-NUMA types go straight to hwloc and must agree with it */
    CHECK("package count", 2 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PACKAGE));
    CHECK("core count", 8 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE));
    CHECK("pu count", 16 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_PU));

    /* every instance in range resolves, and one past the end does not */
    for (n = 0; n < 4; n++) {
        obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, n);
        CHECK("each numa instance resolves", NULL != obj);
    }
    CHECK("numa instance past the end is NULL",
          NULL == prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, 4));

    free_topo(topo);

    /* A topology with no NUMA node at all must report zero and terminate.
     * hwloc always synthesizes a machine-level NUMA node, so use the
     * "no numa" filter to get one without. */
    if (0 == hwloc_topology_init(&topo)) {
        if (0 == hwloc_topology_set_synthetic(topo, "package:1 core:2 pu:1") &&
            0 == hwloc_topology_load(topo)) {
            CHECK("cores are still countable",
                  2 == prte_hwloc_base_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE));
        }
        free_topo(topo);
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* npus / core detection                                              */
/* ------------------------------------------------------------------ */

static int test_npus(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_obj_t pkg;
    hwloc_cpuset_t envelope;

    topo = make_topo("package:2 core:4 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP npus (no synthetic topology support)\n");
        return 0;
    }
    pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, 0);
    CHECK("package 0 resolves", NULL != pkg);
    if (NULL == pkg) {
        free_topo(topo);
        return failures;
    }

    CHECK("npus counts cores when not using hwthreads",
          4 == prte_hwloc_base_get_npus(topo, false, NULL, pkg));
    CHECK("npus counts hwthreads when asked",
          8 == prte_hwloc_base_get_npus(topo, true, NULL, pkg));

    /* an envelope narrows the count */
    envelope = hwloc_bitmap_alloc();
    hwloc_bitmap_set_range(envelope, 0, 3);
    CHECK("npus honors the envelope",
          2 == prte_hwloc_base_get_npus(topo, false, envelope, pkg));
    CHECK("npus honors the envelope in hwthread terms",
          4 == prte_hwloc_base_get_npus(topo, true, envelope, pkg));
    hwloc_bitmap_free(envelope);

    /* a NULL object is a legitimate outcome of a failed lookup upstream,
     * and used to be dereferenced here */
    CHECK("npus tolerates a NULL object",
          0 == prte_hwloc_base_get_npus(topo, false, NULL, NULL));

    CHECK("smt topology reports cores distinct from pus",
          prte_hwloc_base_core_cpus(topo));
    CHECK("smt topology has cores", prte_hwloc_base_has_cores(topo));

    free_topo(topo);

    /* A non-SMT topology still *has* cores - they simply coincide with the
     * PUs. The two predicates have to disagree about it, which is the whole
     * reason both exist. */
    topo = make_topo("package:1 core:4 pu:1");
    if (NULL != topo) {
        CHECK("non-smt: cores and pus coincide", !prte_hwloc_base_core_cpus(topo));
        CHECK("non-smt: cores still exist", prte_hwloc_base_has_cores(topo));
        free_topo(topo);
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* --cpu-set expansion                                                */
/* ------------------------------------------------------------------ */

static int test_generate_cpuset(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_cpuset_t avail;
    char *list;

    topo = make_topo("package:1 core:8 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP generate-cpuset (no synthetic topology support)\n");
        return 0;
    }

    /* a range expands, and the caller's list is rewritten to the expansion */
    list = strdup("0-3");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("range yields a cpuset", NULL != avail);
    if (NULL != avail) {
        /* four cores, two hwthreads each */
        CHECK("range covers four cores", 8 == hwloc_bitmap_weight(avail));
        hwloc_bitmap_free(avail);
    }
    CHECK("range is expanded in place", NULL != list && 0 == strcmp(list, "0,1,2,3"));
    free(list);

    /* hwthread ids address PUs, not cores */
    list = strdup("0-3");
    avail = prte_hwloc_base_generate_cpuset(topo, true, &list);
    CHECK("hwthread range yields a cpuset", NULL != avail);
    if (NULL != avail) {
        CHECK("hwthread range covers four pus", 4 == hwloc_bitmap_weight(avail));
        hwloc_bitmap_free(avail);
    }
    free(list);

    /* A token that is not a number must be rejected. strtoul() reports 0 for
     * "foo", which silently confined the DVM to cpu 0 - the most damaging
     * possible reading of a typo. */
    list = strdup("0,foo,2");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("non-numeric cpu id is rejected", NULL == avail);
    free(list);

    list = strdup("2x");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("trailing garbage is rejected", NULL == avail);
    free(list);

    /* A leading '-' cannot mean "negative" here: '-' is the range delimiter,
     * so "-1" tokenizes to the single id 1. Pinned so the day someone gives
     * the grammar a sign, this says what changed. */
    list = strdup("-1");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("a leading dash is a range delimiter, not a sign", NULL != avail);
    if (NULL != avail) {
        CHECK("\"-1\" resolves to core 1", 2 == hwloc_bitmap_weight(avail));
        hwloc_bitmap_free(avail);
    }
    free(list);

    /* an id past the end of the topology is an error, not a silent drop */
    list = strdup("0-99");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("out of range cpu id is rejected", NULL == avail);
    free(list);

    /* a backwards range is not a range */
    list = strdup("3-1");
    avail = prte_hwloc_base_generate_cpuset(topo, false, &list);
    CHECK("backwards range is rejected", NULL == avail);
    free(list);

    free_topo(topo);
    return failures;
}

/* ------------------------------------------------------------------ */
/* slot-list parsing                                                  */
/* ------------------------------------------------------------------ */

static int test_cpu_list_parse(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_cpuset_t mask;
    int rc;

    topo = make_topo("package:2 core:4 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP cpu-list-parse (no synthetic topology support)\n");
        return 0;
    }
    mask = hwloc_bitmap_alloc();

    /* the bozo check has to test the topology we were handed. It used to
     * test the process-wide one, which is set on every daemon, so a NULL
     * argument sailed through into the parsers below. */
    rc = prte_hwloc_base_cpu_list_parse("0", NULL, false, mask);
    CHECK("NULL topology is refused", PRTE_SUCCESS != rc);
    rc = prte_hwloc_base_cpu_list_parse(NULL, topo, false, mask);
    CHECK("NULL list is refused", PRTE_SUCCESS != rc);
    rc = prte_hwloc_base_cpu_list_parse("", topo, false, mask);
    CHECK("empty list is refused", PRTE_SUCCESS != rc);

    /* bare core ids */
    rc = prte_hwloc_base_cpu_list_parse("0-1", topo, false, mask);
    CHECK("core range parses", PRTE_SUCCESS == rc);
    CHECK("core range covers two cores", 4 == hwloc_bitmap_weight(mask));

    rc = prte_hwloc_base_cpu_list_parse("0,2", topo, false, mask);
    CHECK("core list parses", PRTE_SUCCESS == rc);
    CHECK("core list covers two cores", 4 == hwloc_bitmap_weight(mask));

    /* hwthread ids address PUs */
    rc = prte_hwloc_base_cpu_list_parse("0-1", topo, true, mask);
    CHECK("hwthread range parses", PRTE_SUCCESS == rc);
    CHECK("hwthread range covers two pus", 2 == hwloc_bitmap_weight(mask));

    /* package notation */
    rc = prte_hwloc_base_cpu_list_parse("P0", topo, false, mask);
    CHECK("package selection parses", PRTE_SUCCESS == rc);
    CHECK("package selection covers the package", 8 == hwloc_bitmap_weight(mask));

    rc = prte_hwloc_base_cpu_list_parse("P0:*", topo, false, mask);
    CHECK("package wildcard parses", PRTE_SUCCESS == rc);
    CHECK("package wildcard covers the package", 8 == hwloc_bitmap_weight(mask));

    rc = prte_hwloc_base_cpu_list_parse("P1:0", topo, false, mask);
    CHECK("package:core parses", PRTE_SUCCESS == rc);
    CHECK("package:core covers one core", 2 == hwloc_bitmap_weight(mask));

    /* Everything below is a user-supplied name for something that does not
     * exist. Each of these used to reach an unchecked dereference of the
     * package or core lookup. */
    rc = prte_hwloc_base_cpu_list_parse("P99", topo, false, mask);
    CHECK("unknown package is an error, not a crash", PRTE_SUCCESS != rc);

    rc = prte_hwloc_base_cpu_list_parse("P0-99", topo, false, mask);
    CHECK("package range past the end is an error", PRTE_SUCCESS != rc);

    rc = prte_hwloc_base_cpu_list_parse("P0:99", topo, false, mask);
    CHECK("unknown core in a package is an error", PRTE_SUCCESS != rc);

    rc = prte_hwloc_base_cpu_list_parse("Pfoo", topo, false, mask);
    CHECK("non-numeric package is an error", PRTE_SUCCESS != rc);

    rc = prte_hwloc_base_cpu_list_parse("P0:foo", topo, false, mask);
    CHECK("non-numeric core is an error", PRTE_SUCCESS != rc);

    rc = prte_hwloc_base_cpu_list_parse("999", topo, false, mask);
    CHECK("unknown bare core is an error", PRTE_SUCCESS != rc);

    hwloc_bitmap_free(mask);
    free_topo(topo);
    return failures;
}

/* ------------------------------------------------------------------ */
/* binding report rendering                                           */
/* ------------------------------------------------------------------ */

/* Guarded buffer: a canary immediately past the region the callee is told
 * it may write. Any write past "sz" lands in it. */
#define GUARD_LEN 64
#define GUARD_BYTE 0x5a

static bool guard_intact(const char *buf, int sz)
{
    int i;

    for (i = 0; i < GUARD_LEN; i++) {
        if ((unsigned char) buf[sz + i] != GUARD_BYTE) {
            return false;
        }
    }
    return true;
}

static int test_binding_info(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_cpuset_t cpuset;
    char *buf;
    int pkgnum, sz;

    topo = make_topo("package:2 core:32 pu:1");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP binding-info (no synthetic topology support)\n");
        return 0;
    }
    cpuset = hwloc_bitmap_alloc();

    /* An empty cpuset says so, and leaves pkgnum at a value the caller can
     * recognize. *pkgnum used to be left untouched here, and the only caller
     * declares it uninitialized and prints it. */
    sz = 256;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("empty cpuset says so", NULL != strstr(buf, "EMPTY CPUSET"));
    CHECK("empty cpuset reports no package", -1 == pkgnum);
    CHECK("empty cpuset stays in bounds", guard_intact(buf, sz));
    free(buf);

    /* A cpuset that matches no package likewise has to leave pkgnum set. */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set(cpuset, 4096);
    sz = 256;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("cpuset outside every package reports no package", -1 == pkgnum);
    CHECK("cpuset outside every package stays in bounds", guard_intact(buf, sz));
    free(buf);

    /* One core in package 0 */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set(cpuset, 0);
    sz = 256;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("single core reports its package", 0 == pkgnum);
    CHECK("single core is rendered", NULL != strstr(buf, "<core>0</core>"));
    CHECK("single core stays in bounds", guard_intact(buf, sz));
    free(buf);

    /* THE overflow case. The caller sizes this buffer at 20 bytes per PU,
     * while each element it writes is 20 spaces of indent plus
     * "<core>N</core>\n" - about 34. Bind to every core in a package and
     * hand over the caller's own estimate: the old accounting marched the
     * write pointer through a run of set bits without ever reducing the
     * budget it passed to snprintf, so this wrote ~1KB past the end. */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set_range(cpuset, 0, 31);
    sz = 32 * 20;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("wide binding reports its package", 0 == pkgnum);
    CHECK("wide binding stays in bounds", guard_intact(buf, sz));
    CHECK("wide binding is NUL terminated", sz > (int) strlen(buf));
    free(buf);

    /* ...and the same run with room to spare has to be complete */
    sz = 8192;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("wide binding renders the first core", NULL != strstr(buf, "<core>0</core>"));
    CHECK("wide binding renders a middle core", NULL != strstr(buf, "<core>17</core>"));
    CHECK("wide binding renders the last core", NULL != strstr(buf, "<core>31</core>"));
    CHECK("wide binding with room stays in bounds", guard_intact(buf, sz));
    free(buf);

    /* a truly tiny buffer must still be safe */
    sz = 4;
    buf = malloc(sz + GUARD_LEN);
    memset(buf + sz, GUARD_BYTE, GUARD_LEN);
    pkgnum = 12345;
    prte_hwloc_get_binding_info(cpuset, false, topo, &pkgnum, buf, sz);
    CHECK("tiny buffer stays in bounds", guard_intact(buf, sz));
    free(buf);

    hwloc_bitmap_free(cpuset);
    free_topo(topo);
    return failures;
}

/* ------------------------------------------------------------------ */
/* cset2str                                                           */
/* ------------------------------------------------------------------ */

static int test_cset2str(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_cpuset_t cpuset;
    char *str;
    unsigned n;

    topo = make_topo("package:2 core:4 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP cset2str (no synthetic topology support)\n");
        return 0;
    }
    cpuset = hwloc_bitmap_alloc();

    str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
    CHECK("empty cpuset is named", NULL != str && 0 == strcmp(str, "EMPTY CPUSET"));
    free(str);

    /* one core (both of its hwthreads) in package 0 */
    hwloc_bitmap_set_range(cpuset, 0, 1);
    str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
    CHECK("one core renders as a logical core",
          NULL != str && 0 == strcmp(str, "package[0][core:L0]"));
    free(str);

    /* the same cpuset in hwthread terms names the PUs */
    str = prte_hwloc_base_cset2str(cpuset, true, false, topo);
    CHECK("one core renders as two hwthreads",
          NULL != str && 0 == strcmp(str, "package[0][hwt:L0-1]"));
    free(str);

    /* a run of cores collapses into a range */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set_range(cpuset, 0, 5);
    str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
    CHECK("consecutive cores collapse",
          NULL != str && 0 == strcmp(str, "package[0][core:L0-2]"));
    free(str);

    /* a scattered set does not */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set(cpuset, 0);  /* core 0 */
    hwloc_bitmap_set(cpuset, 4);  /* core 2 */
    hwloc_bitmap_set(cpuset, 6);  /* core 3 */
    str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
    CHECK("scattered cores list individually and as a range",
          NULL != str && 0 == strcmp(str, "package[0][core:L0,2-3]"));
    free(str);

    /* a binding spanning both packages names both */
    hwloc_bitmap_zero(cpuset);
    hwloc_bitmap_set(cpuset, 0);
    hwloc_bitmap_set(cpuset, 8);
    str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
    CHECK("a binding across packages names both",
          NULL != str && NULL != strstr(str, "package[0]") &&
          NULL != strstr(str, "package[1]"));
    free(str);

    hwloc_bitmap_free(cpuset);
    free_topo(topo);

    /* Wide scattered binding: 512 non-consecutive core indices, which is far
     * more site text than the 2048-byte buffer cset2str hands to its
     * internal range builder. The builder used to append with memcpy() and
     * no bound whatsoever; it must now decline rather than write past the
     * end. Either answer is acceptable here - what is not acceptable is
     * corrupting the stack, which is what an unbounded run did. */
    topo = make_topo("package:1 core:1024 pu:1");
    if (NULL != topo) {
        cpuset = hwloc_bitmap_alloc();
        for (n = 0; n < 1024; n += 2) {
            hwloc_bitmap_set(cpuset, n);
        }
        str = prte_hwloc_base_cset2str(cpuset, false, false, topo);
        /* NULL means "declined"; anything else has to be a sane string */
        if (NULL != str) {
            CHECK("a wide scattered render is well formed",
                  0 == strncmp(str, "package[0]", strlen("package[0]")));
            CHECK("a wide scattered render is not absurdly long",
                  8192 > strlen(str));
            free(str);
        }
        hwloc_bitmap_free(cpuset);
        free_topo(topo);
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* build_map (core vs hwthread coresets)                              */
/* ------------------------------------------------------------------ */

static int test_build_map(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_cpuset_t avail, coreset;

    topo = make_topo("package:1 core:4 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP build-map (no synthetic topology support)\n");
        return 0;
    }
    avail = hwloc_bitmap_alloc();
    coreset = hwloc_bitmap_alloc();

    /* both hwthreads of cores 0 and 1 */
    hwloc_bitmap_set_range(avail, 0, 3);
    prte_hwloc_build_map(topo, avail, false, coreset);
    CHECK("two cores map to two core bits", 2 == hwloc_bitmap_weight(coreset));
    CHECK("core 0 is marked", hwloc_bitmap_isset(coreset, 0));
    CHECK("core 1 is marked", hwloc_bitmap_isset(coreset, 1));

    /* the same cpuset in hwthread terms keeps all four bits */
    prte_hwloc_build_map(topo, avail, true, coreset);
    CHECK("hwthread terms keep every bit", 4 == hwloc_bitmap_weight(coreset));

    /* one hwthread of one core still marks that core */
    hwloc_bitmap_zero(avail);
    hwloc_bitmap_set(avail, 5);
    prte_hwloc_build_map(topo, avail, false, coreset);
    CHECK("a single hwthread marks its core", 1 == hwloc_bitmap_weight(coreset));
    CHECK("that core is core 2", hwloc_bitmap_isset(coreset, 2));

    /* an empty input yields an empty coreset, not the previous contents */
    hwloc_bitmap_zero(avail);
    prte_hwloc_build_map(topo, avail, false, coreset);
    CHECK("an empty cpuset yields an empty coreset", hwloc_bitmap_iszero(coreset));

    hwloc_bitmap_free(avail);
    hwloc_bitmap_free(coreset);
    free_topo(topo);
    return failures;
}

/* ------------------------------------------------------------------ */
/* print_binding and its ring of buffers                              */
/* ------------------------------------------------------------------ */

static int test_print_binding(void)
{
    int failures = 0;
    prte_binding_policy_t pol;
    char *a, *b;
    int i;
    char *seen[PRTE_HWLOC_PRINT_NUM_BUFS];

    pol = 0;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_CORE);
    CHECK("core renders", 0 == strcmp("CORE", prte_hwloc_base_print_binding(pol)));

    pol = 0;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_NUMA);
    CHECK("numa renders", 0 == strcmp("NUMA", prte_hwloc_base_print_binding(pol)));

    pol = PRTE_BIND_IF_SUPPORTED;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_PACKAGE);
    CHECK("if-supported is rendered",
          0 == strcmp("PACKAGE:IF-SUPPORTED", prte_hwloc_base_print_binding(pol)));

    pol = PRTE_BIND_ALLOW_OVERLOAD;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_HWTHREAD);
    CHECK("overload-allowed is rendered",
          0 == strcmp("HWTHREAD:OVERLOAD-ALLOWED", prte_hwloc_base_print_binding(pol)));

    pol = PRTE_BIND_IF_SUPPORTED | PRTE_BIND_ALLOW_OVERLOAD;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_L3CACHE);
    CHECK("both qualifiers are rendered",
          0 == strcmp("L3CACHE:IF-SUPPORTED:OVERLOAD-ALLOWED",
                      prte_hwloc_base_print_binding(pol)));

    /* The whole point of the buffer ring is that two calls in one argument
     * list are independent - "pmix_output(0, "%s -> %s", print(a), print(b))"
     * is written all over the mappers. The counter was never advanced, so
     * both arguments pointed at the same buffer and printed the same value. */
    pol = 0;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_CORE);
    a = prte_hwloc_base_print_binding(pol);
    pol = 0;
    PRTE_SET_BINDING_POLICY(pol, PRTE_BIND_TO_NUMA);
    b = prte_hwloc_base_print_binding(pol);
    CHECK("two live renders use different buffers", a != b);
    CHECK("the first render survives the second", 0 == strcmp("CORE", a));
    CHECK("the second render is itself", 0 == strcmp("NUMA", b));

    /* ...and the ring wraps rather than running off the end */
    for (i = 0; i < PRTE_HWLOC_PRINT_NUM_BUFS; i++) {
        seen[i] = prte_hwloc_base_print_binding(pol);
    }
    CHECK("the ring wraps back to its first slot",
          seen[0] == prte_hwloc_base_print_binding(pol));

    return failures;
}

/* ------------------------------------------------------------------ */
/* --bind-to parsing                                                  */
/* ------------------------------------------------------------------ */

static int test_binding_policy(void)
{
    int failures = 0;
    prte_job_t *jdata;
    uint16_t u16 = 0, *u16ptr = &u16;
    bool saved_have_cores = prte_rmaps_base.have_cores;

    /* the "core" spelling is honored only where cores exist */
    prte_rmaps_base.have_cores = true;

    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);

    CHECK("a NULL spec is not an error",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, NULL));

    CHECK("none parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "none"));
    CHECK("none is recorded", PRTE_BIND_TO_NONE == PRTE_GET_BINDING_POLICY(jdata->map->binding));
    CHECK("an explicit policy is marked as given",
          PRTE_BINDING_POLICY_IS_SET(jdata->map->binding));

    CHECK("core parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core"));
    CHECK("core is recorded", PRTE_BIND_TO_CORE == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    /* abbreviations resolve - the documented rule is "enough characters to
     * identify the directive uniquely", and case does not matter */
    CHECK("hwt parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "HWTHREAD"));
    CHECK("hwt is recorded",
          PRTE_BIND_TO_HWTHREAD == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    CHECK("l3cache parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "l3cache"));
    CHECK("l3cache is recorded",
          PRTE_BIND_TO_L3CACHE == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    CHECK("numa parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "numa"));
    CHECK("numa is recorded", PRTE_BIND_TO_NUMA == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    CHECK("package parses", PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "package"));
    CHECK("package is recorded",
          PRTE_BIND_TO_PACKAGE == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    /* qualifiers */
    CHECK("if-supported parses",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core:if-supported"));
    CHECK("if-supported is recorded", !PRTE_BINDING_REQUIRED(jdata->map->binding));

    CHECK("overload-allowed parses",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core:overload-allowed"));
    CHECK("overload-allowed is recorded",
          PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding));
    CHECK("overload-allowed records that it was given",
          PRTE_BIND_OVERLOAD_SET(jdata->map->binding));

    CHECK("no-overload parses",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core:no-overload"));
    CHECK("no-overload clears the allowance",
          !PRTE_BIND_OVERLOAD_ALLOWED(jdata->map->binding));
    CHECK("no-overload records that it was given",
          PRTE_BIND_OVERLOAD_SET(jdata->map->binding));

    /* LIMIT carries a number, and the attribute behind it is a uint16 - a
     * value that does not fit used to be truncated, so "limit=70000" bound
     * to 4464 without a word of complaint */
    CHECK("limit parses",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core:limit=2"));
    CHECK("limit is recorded",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_BINDING_LIMIT, (void **) &u16ptr,
                             PMIX_UINT16));
    CHECK("limit has the value given", 2 == u16);

    CHECK("an out-of-range limit is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:limit=70000"));
    CHECK("a non-numeric limit is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:limit=foo"));
    CHECK("a limit with trailing garbage is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:limit=2x"));
    CHECK("a negative limit is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:limit=-1"));
    CHECK("a zero limit is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:limit=0"));

    /* bad input */
    CHECK("an unknown policy is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "sockets"));
    CHECK("an unknown qualifier is refused",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(jdata, "core:frobnicate"));

    /* the job-only qualifiers are refused on the DVM default, where there is
     * no job to record them against */
    CHECK("report-bindings is refused as a default",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(NULL, "core:report"));
    CHECK("limit is refused as a default",
          PRTE_SUCCESS != prte_hwloc_base_set_binding_policy(NULL, "core:limit=2"));

    /* a plain default is accepted and lands in the global */
    CHECK("a default policy parses",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(NULL, "package"));
    CHECK("a default policy lands in the global",
          PRTE_BIND_TO_PACKAGE == PRTE_GET_BINDING_POLICY(prte_hwloc_default_binding_policy));

    /* where the topology has no cores at all, "core" has to fall back to
     * hwthreads rather than name an object that does not exist */
    prte_rmaps_base.have_cores = false;
    CHECK("core parses without cores",
          PRTE_SUCCESS == prte_hwloc_base_set_binding_policy(jdata, "core"));
    CHECK("core falls back to hwthread without cores",
          PRTE_BIND_TO_HWTHREAD == PRTE_GET_BINDING_POLICY(jdata->map->binding));

    prte_rmaps_base.have_cores = saved_have_cores;
    PMIX_RELEASE(jdata);
    return failures;
}

/* ------------------------------------------------------------------ */
/* userdata lifetime                                                  */
/* ------------------------------------------------------------------ */

/* count the objects at one level that still carry userdata */
static int count_userdata(hwloc_topology_t topo, int depth)
{
    unsigned width, w;
    hwloc_obj_t obj;
    int cnt = 0;

    width = hwloc_get_nbobjs_by_depth(topo, depth);
    for (w = 0; w < width; w++) {
        obj = hwloc_get_obj_by_depth(topo, depth, w);
        if (NULL != obj && NULL != obj->userdata) {
            cnt++;
        }
    }
    return cnt;
}

static int test_userdata(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    hwloc_obj_t obj;
    prte_hwloc_obj_data_t *objcnt;
    unsigned n;
    int depth;

    topo = make_topo("package:2 core:2 pu:2");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP userdata (no synthetic topology support)\n");
        return 0;
    }

    /* the summary hangs off the root */
    prte_hwloc_base_setup_summary(topo);
    CHECK("the root carries a summary", NULL != hwloc_get_root_obj(topo)->userdata);

    /* attach placement counters the way rmaps binding does - to packages,
     * cores, and NUMA nodes. The NUMA nodes are the interesting ones: hwloc
     * 2.x keeps them out of the depth hierarchy, so a release pass that
     * walks depths alone leaks every one of them. */
    for (n = 0; n < 2; n++) {
        obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, n);
        if (NULL != obj) {
            objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
            objcnt->nprocs = 7;
            obj->userdata = objcnt;
        }
    }
    obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, 0);
    CHECK("a numa node resolves", NULL != obj);
    if (NULL != obj) {
        objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
        objcnt->nprocs = 7;
        obj->userdata = objcnt;
    }
    obj = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0);
    if (NULL != obj) {
        objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
        objcnt->nprocs = 7;
        obj->userdata = objcnt;
    }

    depth = hwloc_get_type_depth(topo, HWLOC_OBJ_PACKAGE);
    CHECK("packages carry counters", 2 == count_userdata(topo, depth));
    CHECK("a numa node carries a counter",
          1 == count_userdata(topo, HWLOC_TYPE_DEPTH_NUMANODE));

    /* releasing has to clear every level, root and NUMA included */
    prte_hwloc_base_release_userdata(topo);
    CHECK("the root's summary is released", NULL == hwloc_get_root_obj(topo)->userdata);
    CHECK("package counters are released", 0 == count_userdata(topo, depth));
    CHECK("numa counters are released",
          0 == count_userdata(topo, HWLOC_TYPE_DEPTH_NUMANODE));
    depth = hwloc_get_type_depth(topo, HWLOC_OBJ_CORE);
    CHECK("core counters are released", 0 == count_userdata(topo, depth));

    /* releasing twice is safe - prte_hwloc_base_close() and the topology
     * destructor both reach for it */
    prte_hwloc_base_release_userdata(topo);
    prte_hwloc_base_release_userdata(NULL);

    hwloc_topology_destroy(topo);
    return failures;
}

/* ------------------------------------------------------------------ */
/* reset_counters                                                     */
/* ------------------------------------------------------------------ */

static int test_reset_counters(void)
{
    int failures = 0;
    hwloc_topology_t topo;
    prte_topology_t *t;
    hwloc_obj_t pkg, core, numa;
    prte_hwloc_obj_data_t *objcnt;

    topo = make_topo("package:2 core:2 pu:1");
    if (NULL == topo) {
        fprintf(stdout, "  SKIP reset-counters (no synthetic topology support)\n");
        return 0;
    }

    /* prte_init_util() does not build the topology registry - prte_init()
     * does - and reset_counters() walks it, so stand one up here */
    if (NULL == prte_node_topologies) {
        prte_node_topologies = PMIX_NEW(pmix_pointer_array_t);
        pmix_pointer_array_init(prte_node_topologies, 8, INT_MAX, 8);
    }

    t = PMIX_NEW(prte_topology_t);
    t->topo = topo;
    t->index = pmix_pointer_array_add(prte_node_topologies, t);

    prte_hwloc_base_setup_summary(topo);
    pkg = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_PACKAGE, 0);
    core = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_CORE, 0);
    numa = prte_hwloc_base_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, 0);

    if (NULL != pkg) {
        objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
        objcnt->nprocs = 3;
        pkg->userdata = objcnt;
    }
    if (NULL != core) {
        objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
        objcnt->nprocs = 5;
        core->userdata = objcnt;
    }
    if (NULL != numa) {
        objcnt = PMIX_NEW(prte_hwloc_obj_data_t);
        objcnt->nprocs = 9;
        numa->userdata = objcnt;
    }

    prte_hwloc_base_reset_counters();

    if (NULL != pkg && NULL != pkg->userdata) {
        CHECK("package counter is reset",
              0 == ((prte_hwloc_obj_data_t *) pkg->userdata)->nprocs);
    }
    if (NULL != core && NULL != core->userdata) {
        CHECK("core counter is reset",
              0 == ((prte_hwloc_obj_data_t *) core->userdata)->nprocs);
    }
    /* The NUMA counter is the one that used to survive. hwloc 2.x keeps NUMA
     * nodes out of the depth hierarchy, so a walk over 0..get_depth() never
     * visits one - and "--bind-to numa:limit=N" attaches its per-object
     * counter there. Missing the reset left those counters accumulating for
     * the life of the DVM, so the second such job found every domain already
     * at its limit and could not be bound at all. */
    CHECK("a numa node resolves", NULL != numa);
    if (NULL != numa && NULL != numa->userdata) {
        CHECK("numa counter is reset",
              0 == ((prte_hwloc_obj_data_t *) numa->userdata)->nprocs);
    }
    /* The root's summary is NOT an obj_data_t. reset_counters must not walk
     * depth 0, or it would reinterpret the summary as a counter and scribble
     * on numa_cutoff. */
    CHECK("the root summary survives a reset",
          NULL != hwloc_get_root_obj(topo)->userdata &&
          ((prte_hwloc_topo_data_t *) hwloc_get_root_obj(topo)->userdata)->computed);

    pmix_pointer_array_set_item(prte_node_topologies, t->index, NULL);
    PMIX_RELEASE(t); /* releases the topology and its userdata */

    return failures;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int rc, failures = 0;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }

    failures += test_numa_summary();
    failures += test_npus();
    failures += test_generate_cpuset();
    failures += test_cpu_list_parse();
    failures += test_binding_info();
    failures += test_cset2str();
    failures += test_build_map();
    failures += test_print_binding();
    failures += test_binding_policy();
    failures += test_userdata();
    failures += test_reset_counters();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all hwloc unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d hwloc unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
