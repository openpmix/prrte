/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/include -- PRRTE's constants, base types and the
 * configure-driven portability layer.
 *
 * Nothing here is a subsystem, which is the reason it went untested and the
 * reason it is worth testing: these are the definitions every other
 * directory builds against, so a defect in one of them is invisible at the
 * point of the mistake and shows up somewhere else entirely. The cases
 * below each pin down something this review found or something the headers
 * assert about themselves but nothing checked:
 *
 *  - constants.h warns that every error value must be unique. Uniqueness
 *    itself is already enforced by the compiler (prte_strerror() switches
 *    on every code, so a repeated value is a duplicate-case error), but
 *    nothing checked the properties the numbering scheme rests on: that
 *    every code is negative, and that none of them lands on a value PMIx
 *    is also using.
 *
 *    Both were broken, in two separate ways.
 *
 *    The codes used to run from two bases, PRTE_ERR_BASE and a second one
 *    called PRTE_ERR_SPLIT -- an ORTE inheritance, where the two groups
 *    belonged to two separate projects (OPAL and ORTE) that each needed to
 *    append without coordinating. PRTE_ERR_SPLIT was ORTE's OPAL_ERR_MAX,
 *    which is -100; the minus sign was dropped when the two layers were
 *    flattened into src/ in 2019. So the whole lower group became
 *    *positive* error codes (+99 down to +43), and PRTE_ERR_MAX, defined
 *    as (PRTE_ERR_SPLIT - 100), evaluated to 0 -- the value of
 *    PRTE_SUCCESS. There is one base now: OPAL and ORTE have been one code
 *    base for years, and a second base for the second half of one enum in
 *    one file bought nothing but the chance to get this wrong.
 *
 *    And the codes sat inside the range PMIx uses for its own statuses: 46
 *    of them were numerically identical to a PMIx status meaning something
 *    else. They now hang off PMIX_EXTERNAL_ERR_BASE, which PMIx reserves
 *    for exactly this. test_error_code_numbering() states the intended
 *    numbering in a form that fails if the base is moved again.
 *
 *  - types.h defined PRTE_LOCAL_RANK_MAX as "UINT16_MAX - 1" with no
 *    parentheses, so any use in a larger expression bound differently than
 *    it reads.
 *
 *  - prte_hton64()/prte_ntoh64() are the OOB's 64-bit wire conversion --
 *    oob_tcp_hdr.h runs every message header's origin epoch through them --
 *    and had no coverage at all. They are also the one thing in this
 *    directory that a heterogeneous DVM depends on being right.
 *
 *  - PRIsize_t comes out of a configure size comparison and is used in
 *    verbose output across the tree; a wrong one is undefined behavior in
 *    printf, not a compile error.
 *
 * What is NOT here: the attribute macros in prte_config_bottom.h. Whether
 * __prte_attribute_unused__ expanded or silently vanished is not observable
 * from inside the program, so a build with -Wunused is the only check that
 * exists for those.
 */

#include "prte_config.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include <pmix_common.h>

#include "constants.h"
#include "types.h"

/* PMIx's printf replacements -- the four names prte_config_bottom.h maps a
 * missing libc printf onto. prte_config_bottom.h includes this itself, but
 * only on a platform that needs it. */
#include "src/util/pmix_printf.h"

#include "src/include/prte_stdatomic.h"
#include "src/runtime/runtime.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* Where the two documentation groups in constants.h start and end. The
 * group boundaries are a convention for readers and are stated by hand; the
 * *end* of the list is not, because a hand-written end goes stale silently.
 * This one used to read "155 / PRTE_ERR_SLURM_SHRINK_FAILURE" and stayed
 * that way when PRTE_ERR_PRELOAD_CONFLICT was appended at offset 156, so the
 * sweep below stopped one short of the newest code -- the code most likely
 * to be wrong. It now comes from PRTE_ERR_MAX, which lives in constants.h
 * next to the append site and which test/unit/util fails on if it is
 * stale. */
#define GROUP1_LAST  72  /* PRTE_OPERATION_SUCCEEDED */
#define GROUP2_FIRST 101 /* PRTE_ERR_RECV_LESS_THAN_POSTED */
#define GROUP2_LAST  (PRTE_ERR_BASE - PRTE_ERR_MAX - 1)

/* ------------------------------------------------------------------ */
/* error code numbering                                               */
/* ------------------------------------------------------------------ */

static int test_error_code_numbering(void)
{
    int failures = 0;
    int i;

    CHECK("success is zero", 0 == PRTE_SUCCESS);

    /* GOLDEN RULE: the base is PMIX_EXTERNAL_ERR_BASE.
     *
     * PMIx reserves everything below that constant for projects layered on
     * top of it, and PRRTE is one. Based anywhere else, a PRRTE code that
     * escapes prte_pmix_convert_rc()/prte_pmix_convert_status() does not
     * look foreign -- it looks like a real PMIx status with a different
     * meaning. Based at 0, as PRRTE was, 46 of its codes were numerically
     * identical to a live PMIx status: PRTE_ERR_SLURM_SHRINK_FAILURE was
     * PMIX_OPERATION_SUCCEEDED, and PRTE_ERR_TAKE_NEXT_OPTION -- a
     * control-flow signal, not a failure -- was PMIX_ERR_NOT_FOUND.
     *
     * Sweep the whole range rather than spot-checking the base, so that a
     * list grown long enough to climb back into PMIx's space is caught too. */
    for (i = 1; i <= GROUP2_LAST; i++) {
        if (PRTE_ERR_BASE - i >= PMIX_EXTERNAL_ERR_BASE) {
            fprintf(stderr, "FAIL [codes]: PRTE_ERR_BASE - %d is inside PMIx's status space\n", i);
            failures++;
        }
    }

    /* being below PMIX_EXTERNAL_ERR_BASE makes every code negative, which is
     * the other property a reader assumes -- and the one the dropped minus
     * sign broke, leaving a whole group of *positive* error codes that any
     * "rc < 0" test would have read as success */
    CHECK("the first code is negative", (PRTE_ERR_BASE - 1) < 0);
    CHECK("the last code is negative", (PRTE_ERR_BASE - GROUP2_LAST) < 0);

    CHECK("PRTE_ERROR is the first code", (PRTE_ERR_BASE - 1) == PRTE_ERROR);
    CHECK("the second group starts where the header says",
          (PRTE_ERR_BASE - GROUP2_FIRST) == PRTE_ERR_RECV_LESS_THAN_POSTED);

    /* PRTE_ERR_MAX is one past the end, so it must sit below every code and
     * must not itself be one. The check that it is not stale -- that the
     * offset just inside it is a real, named code -- needs prte_strerror()
     * and lives in test/unit/util; here we only pin the arithmetic. */
    CHECK("the bound is below the last code", PRTE_ERR_MAX < PRTE_ERR_PRELOAD_CONFLICT);
    CHECK("the bound is one past the end", (PRTE_ERR_MAX + 1) == PRTE_ERR_PRELOAD_CONFLICT);

    /* PRTE_ERROR must NOT be PMIX_ERROR any more. They were both -1, which
     * made a PRRTE code handed to PMIx unconverted invisible in the one
     * case a test would most likely construct. */
    CHECK("PRTE_ERROR is distinct from PMIX_ERROR", PRTE_ERROR != PMIX_ERROR);

    /* the two groups are a convention for readers, not arithmetic, but the
     * hole between them is real: nothing may be assigned inside it, or the
     * "append at the end of your group" rule starts overwriting */
    CHECK("the groups do not overlap", GROUP1_LAST < GROUP2_FIRST);

    return failures;
}

/* ------------------------------------------------------------------ */
/* types.h                                                            */
/* ------------------------------------------------------------------ */

static int test_rank_types(void)
{
    int failures = 0;
    prte_local_rank_t lrank;
    prte_node_rank_t nrank;

    CHECK("a local rank is 16 bits", 2 == sizeof(prte_local_rank_t));
    CHECK("a node rank is 16 bits", 2 == sizeof(prte_node_rank_t));
    CHECK("an app index is 32 bits", 4 == sizeof(prte_app_idx_t));

    /* PRTE_LOCAL_RANK_MAX was written as "UINT16_MAX - 1" with no
     * parentheses. Used alone it reads correctly; used in any larger
     * expression it binds the wrong way, and this is a header included by
     * everything that maps or ranks a process. */
    CHECK("the local rank max parenthesizes", 2 * PRTE_LOCAL_RANK_MAX == 2 * (UINT16_MAX - 1));
    CHECK("the node rank max parenthesizes", 2 * PRTE_NODE_RANK_MAX == 2 * (UINT16_MAX - 1));

    /* the INVALID sentinel has to sit outside the usable range, or a real
     * rank reads as "no rank" */
    CHECK("invalid is above the local max", PRTE_LOCAL_RANK_INVALID > PRTE_LOCAL_RANK_MAX);
    CHECK("invalid is above the node max", PRTE_NODE_RANK_INVALID > PRTE_NODE_RANK_MAX);

    /* and both sentinels survive a round trip through the type they label */
    lrank = PRTE_LOCAL_RANK_INVALID;
    nrank = PRTE_NODE_RANK_INVALID;
    CHECK("the local sentinel is representable", PRTE_LOCAL_RANK_INVALID == lrank);
    CHECK("the node sentinel is representable", PRTE_NODE_RANK_INVALID == nrank);

    /* prte_socklen_t is handed to accept()/getsockname() by oob/tcp, so it
     * has to be whatever the platform's socket API actually wants */
    CHECK("socklen is sanely sized", sizeof(prte_socklen_t) >= sizeof(int));
#if defined(HAVE_SOCKLEN_T)
    CHECK("socklen matches the system type", sizeof(prte_socklen_t) == sizeof(socklen_t));
#endif

    return failures;
}

/* ------------------------------------------------------------------ */
/* byte order                                                         */
/* ------------------------------------------------------------------ */

static int test_byte_order(void)
{
    int failures = 0;
    static const uint64_t values[] = {0ULL,
                                      1ULL,
                                      0x00000000FFFFFFFFULL,
                                      0x0123456789ABCDEFULL,
                                      0xFFFFFFFFFFFFFFFFULL,
                                      0x8000000000000000ULL};
    size_t n;
    uint64_t net;
    unsigned char bytes[8];
    int big_endian;

    /* whichever way this build swaps, the pair must be inverses -- the OOB
     * writes a header with hton64 on one daemon and reads it with ntoh64 on
     * another, and both ends run this same code */
    for (n = 0; n < sizeof(values) / sizeof(values[0]); n++) {
        if (values[n] != prte_ntoh64(prte_hton64(values[n]))) {
            fprintf(stderr, "FAIL [byteorder]: 0x%016llx did not round trip\n",
                    (unsigned long long) values[n]);
            failures++;
        }
    }

    /* and the wire form really is big-endian, not merely self-consistent.
     * A pair of functions that both do nothing round-trips perfectly and
     * still puts the wrong bytes on the wire between a big- and a
     * little-endian daemon, so the round trip above cannot be the only
     * check. */
    net = prte_hton64(0x0123456789ABCDEFULL);
    memcpy(bytes, &net, sizeof(bytes));

    /* HAVE_UNIX_BYTESWAP is AC_DEFINE'd only when the check succeeds, so
     * #ifdef is the correct test for it (#if would be wrong). Where it is
     * absent, prte_config_bottom.h supplies identity htonl/htons stubs for
     * a machine assumed to need no conversion, and these follow suit. */
#ifdef HAVE_UNIX_BYTESWAP
    big_endian = (1 == htonl(1));
#else
    big_endian = 1;
#endif

    if (big_endian) {
        CHECK("nothing to swap on a big-endian build", 0x0123456789ABCDEFULL == net);
    } else {
        CHECK("the wire form is big-endian",
              0x01 == bytes[0] && 0x23 == bytes[1] && 0x45 == bytes[2] && 0x67 == bytes[3]
                  && 0x89 == bytes[4] && 0xAB == bytes[5] && 0xCD == bytes[6] && 0xEF == bytes[7]);
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* the configure-driven portability layer                             */
/* ------------------------------------------------------------------ */

static int test_config_bottom(void)
{
    int failures = 0;
    char buf[64];
    size_t val = 1234567;
    prte_atomic_bool_t flag = false;

    /* PRTE_PATH_MAX and PRTE_MAXHOSTNAMELEN size real buffers all over the
     * tree, and both are chosen by a chain of #elif fallbacks that ends in a
     * hard-coded guess. A build that lands on the guess still has to produce
     * something usable. */
    CHECK("PRTE_PATH_MAX is usable", PRTE_PATH_MAX > 64);
    CHECK("PRTE_MAXHOSTNAMELEN is usable", PRTE_MAXHOSTNAMELEN > 64);
    CHECK("PRTE_PATH_SEP is a separator", 0 == strcmp("/", PRTE_PATH_SEP));

    /* PRIsize_t is picked by comparing SIZEOF_SIZE_T against SIZEOF_LONG and
     * SIZEOF_LONG_LONG. Get it wrong and every verbose message using it is
     * undefined behavior rather than a compile error, because the format
     * string is assembled by concatenation. */
    snprintf(buf, sizeof(buf), "%" PRIsize_t, val);
    CHECK("PRIsize_t formats a size_t", 0 == strcmp("1234567", buf));

    /* PRTE_ENABLE_IPV6 is narrowed in prte_config_bottom.h from what
     * configure asked for to what the platform can honor. It must come out
     * of that as a plain 0 or 1 -- it is tested with #if throughout the
     * tree -- and it must not claim IPv6 on a platform with no
     * sockaddr_in6, which is precisely what the narrowing is for. */
    CHECK("IPv6 support is a logical value", 0 == PRTE_ENABLE_IPV6 || 1 == PRTE_ENABLE_IPV6);
#if !defined(HAVE_STRUCT_SOCKADDR_IN6)
    CHECK("IPv6 is off without sockaddr_in6", 0 == PRTE_ENABLE_IPV6);
#endif

    /* the OOB's listener thread spins on one of these while the main thread
     * clears it; it is the only atomic type PRRTE declares for itself */
    CHECK("an atomic bool starts false", false == flag);
    flag = true;
    CHECK("an atomic bool holds true", true == flag);
    flag = false;
    CHECK("an atomic bool holds false", false == flag);

    return failures;
}

/* ------------------------------------------------------------------ */
/* the printf fallbacks                                               */
/* ------------------------------------------------------------------ */

/* prte_config_bottom.h maps asprintf/snprintf/vasprintf/vsnprintf onto PMIx
 * replacements on any platform whose libc is missing one. PRRTE has no
 * printf replacements of its own and never had -- all four come from PMIx's
 * src/util/pmix_printf.h.
 *
 * One of the four named prte_vsnprintf, a symbol that exists in neither
 * tree, so a platform without vsnprintf() would have failed to link every
 * tool. Nothing caught it because the mapping is compiled only where the
 * libc function is absent, which is nowhere anyone builds.
 *
 * Be honest about what this proves: it cannot exercise the mapping itself
 * on a platform that does not need it. What it does do is take the address
 * of each of the four names the mapping resolves to, so they have to exist
 * and be linkable here -- which catches both a typo of the kind above and
 * PMIx dropping or renaming one out from under us. PRRTE builds against
 * PMIx's *internal* headers, so the latter is a live risk, not a
 * hypothetical.
 */
static int test_printf_fallbacks(void)
{
    int failures = 0;
    const void *fns[4];

    fns[0] = (const void *) (uintptr_t) pmix_asprintf;
    fns[1] = (const void *) (uintptr_t) pmix_snprintf;
    fns[2] = (const void *) (uintptr_t) pmix_vasprintf;
    fns[3] = (const void *) (uintptr_t) pmix_vsnprintf;

    CHECK("the asprintf replacement exists", NULL != fns[0]);
    CHECK("the snprintf replacement exists", NULL != fns[1]);
    CHECK("the vasprintf replacement exists", NULL != fns[2]);
    CHECK("the vsnprintf replacement exists", NULL != fns[3]);

    return failures;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int failures = 0;

    /* Deliberately no prte_init_util() here. Everything in src/include has
     * to be correct before any of PRRTE is running -- constants.h and
     * types.h are what prte_init_util itself is compiled against -- so a
     * test that needed the runtime up would be testing the wrong thing. */

    failures += test_error_code_numbering();
    failures += test_rank_types();
    failures += test_byte_order();
    failures += test_config_bottom();
    failures += test_printf_fallbacks();

    if (0 == failures) {
        fprintf(stdout, "PASSED all include unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d include unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
