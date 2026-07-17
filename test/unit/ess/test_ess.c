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

    failures += test_setup_signals_none();
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
