/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the ess framework's signal-forwarding parser.
 *
 * The bulk of the ess framework -- daemon/HNP bring-up
 * (prte_ess_base_prted_setup and the hnp module) -- cannot run without a
 * live DVM: it opens the entire downstream framework stack, starts the
 * PMIx server, and creates the session directory.  Those paths are
 * covered by the integration/dockerswarm harnesses.
 *
 * What *can* be exercised in isolation is the piece of the framework that
 * is pure input parsing: prte_ess_base_setup_signals().  It turns the
 * user's ess_base_forward_signals list into the global
 * prte_ess_base_signals list that prted_setup later installs signal
 * handlers from -- and each entry carries the integer signal number that
 * the forwarding callback packs and delivers to the application procs.
 * The correctness of that number is exactly what a recent regression got
 * wrong (the callback packed a constant instead of the signal), so it is
 * worth pinning down that the parser produces real, non-zero signal
 * numbers for the signals it accepts.
 *
 * NOTE: prte_ess_base_setup_signals() latches after its first
 * non-"none" invocation (it may only append to the global list once for
 * the life of the process).  A single process can therefore drive exactly
 * one real parse, so the "none" no-op case is checked first (it does not
 * latch) and the substantive multi-signal parse is checked second.
 */

#include "prte_config.h"
#include <signal.h>
#include <stdio.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/ess/base/base.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* count how many entries on the global list carry the given signal number */
static int count_signal(int signum)
{
    prte_ess_base_signal_t *sig;
    int n = 0;

    PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t) {
        if (signum == sig->signal) {
            n++;
        }
    }
    return n;
}

/*
 * "none" means "forward nothing": the parser must succeed and leave the
 * global list empty.  This path deliberately does NOT latch the one-shot
 * guard, so it is safe to run before the real parse below.
 */
static int test_setup_signals_none(void)
{
    int failures = 0;
    pmix_status_t rc;

    rc = prte_ess_base_setup_signals("none");
    CHECK("none returns success", PMIX_SUCCESS == rc);
    CHECK("none leaves list empty", 0 == pmix_list_get_size(&prte_ess_base_signals));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_setup_signals_none\n");
    }
    return failures;
}

/*
 * Requests the parser must refuse.  A rejection has to be loud: the handler
 * install that would follow simply fails on a signal this platform cannot
 * deliver, and fails silently, so anything let through here becomes a signal
 * the user asked to forward and that is never forwarded, with nothing said.
 *
 * The forwardability gate in particular has to hold on BOTH input forms --
 * the name and the number are two separate parse branches, and a check added
 * to one of them is easy to leave off the other.
 */
static int test_setup_signals_rejects(void)
{
    int failures = 0;
    size_t before;
    struct {
        const char *label;
        const char *input;
    } cases[] = {
        {"unknown name", "SIGNOTREAL"},
        {"non-forwardable by name", "SIGKILL"},
        {"non-forwardable by number", "9"},
        {"non-numeric", "12abc"},
        {"negative number", "-1"},
        {"signal zero", "0"},
        {"out of range number", "999"},
        {"bad entry in a good list", "SIGUSR1,SIGNOTREAL"},
        {NULL, NULL},
    };
    int i;

    for (i = 0; NULL != cases[i].label; i++) {
        before = pmix_list_get_size(&prte_ess_base_signals);
        CHECK(cases[i].label, PMIX_SUCCESS != prte_ess_base_setup_signals((char *) cases[i].input));
        /* a refusal must not leave the list latched or half-populated in a
         * way that survives -- the caller aborts, but the next parse in this
         * process must still be free to run */
        CHECK(cases[i].label, before <= pmix_list_get_size(&prte_ess_base_signals));
    }

    /* whatever a partial parse appended must not block a later good one */
    while (0 < pmix_list_get_size(&prte_ess_base_signals)) {
        pmix_list_item_t *item = pmix_list_remove_first(&prte_ess_base_signals);
        /* answers NULL for an empty list, and the loop bound above is the
         * only thing that says this one is not - screen it rather than hand
         * a NULL to the reference count */
        if (NULL == item) {
            break;
        }
        PMIX_RELEASE(item);
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_setup_signals_rejects\n");
    }
    return failures;
}

/*
 * A real, mixed request: three distinct forwardable signals plus a
 * case-insensitive duplicate of the first.  The parser must accept all
 * three, drop the duplicate, and record the correct (non-zero) integer
 * signal number for each -- the very value the forwarding callback packs.
 */
static int test_setup_signals_parse(void)
{
    int failures = 0;
    pmix_status_t rc;
    prte_ess_base_signal_t *sig;

    rc = prte_ess_base_setup_signals("SIGUSR1,SIGUSR2,SIGCONT,sigusr1");
    CHECK("parse returns success", PMIX_SUCCESS == rc);

    /* the duplicate SIGUSR1 must have been ignored -> exactly three */
    CHECK("three signals recorded", 3 == pmix_list_get_size(&prte_ess_base_signals));

    /* each requested signal is present exactly once, by its real number */
    CHECK("SIGUSR1 present once", 1 == count_signal(SIGUSR1));
    CHECK("SIGUSR2 present once", 1 == count_signal(SIGUSR2));
    CHECK("SIGCONT present once", 1 == count_signal(SIGCONT));

    /* every recorded entry must carry a valid, non-zero signal number and
     * a matching name -- a zero here is precisely the failure mode that
     * makes signal forwarding a silent no-op */
    PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t) {
        CHECK("entry has non-zero signal", 0 != sig->signal);
        CHECK("entry has a name", NULL != sig->signame);
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_setup_signals_parse\n");
    }
    return failures;
}

/*
 * prte_ess_base_set_identity() is the other DVM-free path in the framework:
 * it turns the identity a launcher published (the ess_base_nspace/vpid
 * parameters plus the RM's per-node index) into this daemon's name.
 *
 * Its inputs come from outside the process, so the interesting cases are the
 * bad ones.  The shorthand this function exists to replace was a bare
 * strtoul/atoi with no check at all, and its failure mode is silent and
 * severe: any non-numeric value reads as 0, and rank 0 is the DVM
 * controller -- so a daemon quietly adopts the HNP's identity and the DVM
 * comes apart later, nowhere near the bad input that caused it.  Each case
 * below therefore asserts that a bad input is REFUSED, not merely that a
 * good one is accepted.
 */
static int check_identity(const char *label, const char *nspace, const char *vpid,
                          const char *offset_envar, const char *offset_val,
                          int offset_adjust, bool expect_ok, pmix_rank_t expect_rank)
{
    int failures = 0;
    int rc;

    prte_ess_base_nspace = (char *) nspace;
    prte_ess_base_vpid = (char *) vpid;
    if (NULL != offset_envar) {
        if (NULL == offset_val) {
            unsetenv(offset_envar);
        } else {
            setenv(offset_envar, offset_val, 1);
        }
    }
    /* poison the name so a refusal that still wrote one is visible */
    PRTE_PROC_MY_NAME->rank = PMIX_RANK_INVALID;

    rc = prte_ess_base_set_identity(offset_envar, offset_adjust);

    if (expect_ok) {
        CHECK(label, PRTE_SUCCESS == rc);
        CHECK(label, expect_rank == PRTE_PROC_MY_NAME->rank);
    } else {
        CHECK(label, PRTE_SUCCESS != rc);
        /* a refused identity must leave no rank behind */
        CHECK(label, PMIX_RANK_INVALID == PRTE_PROC_MY_NAME->rank);
    }
    return failures;
}

static int test_set_identity(void)
{
    int failures = 0;

    /* the ssh case: the launcher assigns the vpid directly, no offset */
    failures += check_identity("plain vpid", "myjob", "3", NULL, NULL, 0, true, 3);

    /* an RM case: base vpid plus this node's index within the allocation */
    failures += check_identity("vpid + node index", "myjob", "1",
                               "PRTE_TEST_NODEID", "4", 0, true, 5);

    /* the LSF shape: tasks numbered from one, backed off to a zero-based rank */
    failures += check_identity("one-based index", "myjob", "0",
                               "PRTE_TEST_NODEID", "1", -1, true, 0);

    /* garbage must be refused, not silently read as rank 0 */
    failures += check_identity("non-numeric vpid", "myjob", "abc", NULL, NULL, 0, false, 0);
    failures += check_identity("trailing garbage", "myjob", "3x", NULL, NULL, 0, false, 0);
    failures += check_identity("empty vpid", "myjob", "", NULL, NULL, 0, false, 0);
    failures += check_identity("negative vpid", "myjob", "-1", NULL, NULL, 0, false, 0);
    failures += check_identity("non-numeric index", "myjob", "1",
                               "PRTE_TEST_NODEID", "two", 0, false, 0);
    failures += check_identity("negative index", "myjob", "1",
                               "PRTE_TEST_NODEID", "-2", 0, false, 0);

    /* a missing input is an error, never a default */
    failures += check_identity("missing nspace", NULL, "1", NULL, NULL, 0, false, 0);
    failures += check_identity("missing vpid", "myjob", NULL, NULL, NULL, 0, false, 0);
    failures += check_identity("missing index", "myjob", "1",
                               "PRTE_TEST_NODEID", NULL, 0, false, 0);

    /* a rank that cannot exist must be refused rather than wrapped: a
     * one-based adjustment applied to a zero index would otherwise land up
     * near UINT32_MAX, and a value past the end of the rank space would be
     * mistaken for one of PMIx's reserved sentinels */
    failures += check_identity("index underflow", "myjob", "0",
                               "PRTE_TEST_NODEID", "0", -1, false, 0);
    failures += check_identity("vpid past rank space", "myjob", "4294967290",
                               NULL, NULL, 0, false, 0);
    failures += check_identity("sum past rank space", "myjob", "4294967200",
                               "PRTE_TEST_NODEID", "1000", 0, false, 0);

    unsetenv("PRTE_TEST_NODEID");
    prte_ess_base_nspace = NULL;
    prte_ess_base_vpid = NULL;

    if (0 == failures) {
        fprintf(stdout, "PASSED test_set_identity\n");
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

    /* open the framework so its verbosity channel is valid and the global
     * signal list is constructed */
    rc = pmix_mca_base_framework_open(&prte_ess_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "ess framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_set_identity();
    failures += test_setup_signals_none();
    failures += test_setup_signals_rejects();
    failures += test_setup_signals_parse();

    (void) pmix_mca_base_framework_close(&prte_ess_base_framework);

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all ess unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d ess unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
