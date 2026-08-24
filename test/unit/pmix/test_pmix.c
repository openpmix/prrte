/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/pmix -- the shim between PRRTE's code space and PMIx's.
 *
 * Everything in src/pmix/pmix.c is a total function from one integer space to
 * another, with no state and no I/O, which makes it about as testable as code
 * gets. It had never been tested, and the reason is worth recording: a
 * mistranslation here is invisible. Both spaces are small signed integers,
 * every value looks plausible in the other, and the wrong answer surfaces as a
 * misleading error message or a wrong proc state somewhere else entirely --
 * never as a failure in this file.
 *
 * The traps these tests pin down:
 *
 *  - The two spaces used to OVERLAP numerically, and the tests here were
 *    written when they did: PRRTE's errors were based at 0 and -100 while a
 *    PMIx status runs -1 down past -160, so 46 PRRTE codes had the value of
 *    a live PMIx status meaning something else.
 *    prte_pmix_convert_status() used to `return status` for anything its
 *    switch did not name, which did not yield a recognisably foreign code --
 *    it yielded a confidently wrong one. PMIX_ERR_PACK_FAILURE arrived as
 *    PRTE_ERR_FILE_OPEN_FAILURE and PMIX_ERR_EMPTY as PRTE_ERR_NODE_DOWN.
 *    PRRTE's codes now hang off PMIX_EXTERNAL_ERR_BASE, which PMIx reserves
 *    for projects layered on it, so the error spaces are disjoint -- but the
 *    *state* spaces still are not, and a converter that is not total is
 *    still a converter that lies. test_status_no_passthrough() sweeps the
 *    whole PMIx range to prove no input can produce a code that is not
 *    deliberately chosen.
 *
 *  - The two PROC STATE spaces share a numbering scheme but not a numbering.
 *    PRRTE has no PREPPED, so below the error boundary its states sit one
 *    under PMIx's; from PRTE_PROC_STATE_ERROR / PMIX_PROC_STATE_ERROR (both
 *    50) on up, the offsets do line up. prte_pmix_convert_state() was written
 *    with bare integer cases, and `case 59` -- PRTE_PROC_STATE_HEARTBEAT_FAILED
 *    -- returned PMIX_PROC_STATE_MIGRATING, while the real MIGRATING (60) fell
 *    through to UNDEF. PRTE_PROC_STATE_TERMINATED was missing outright, so
 *    PMIX_QUERY_PROC_TABLE reported every proc of a finished job as UNDEF.
 *
 *  - A converter that answers UNDEF/PMIX_ERROR for an input it simply forgot
 *    is indistinguishable from one answering correctly. So the state tests are
 *    written as an explicit table over every named constant rather than as
 *    spot checks, and test_state_roundtrip() requires the pair to agree.
 *
 * What is deliberately NOT here: the lock macros and prte_pmix_shifted_wakeup(),
 * which need a live prte_event_base and a second thread to say anything
 * meaningful. Their contract is a threading one and belongs to the live smoke
 * test and to contrib/dockerswarm.
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>

#include "constants.h"

#include "src/mca/plm/plm_types.h"
#include "src/pmix/pmix-internal.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* A release kill has its own status and state only on a PMIx that
 * defines them */
#ifdef PMIX_ERR_JOB_KILLED_BY_RELEASE
#    define RELEASE_STATUS PMIX_ERR_JOB_KILLED_BY_RELEASE
#else
#    define RELEASE_STATUS PMIX_ERR_JOB_CANCELED
#endif
#ifdef PMIX_PROC_STATE_KILLED_BY_RELEASE
#    define RELEASE_PSTATE PMIX_PROC_STATE_KILLED_BY_RELEASE
#else
#    define RELEASE_PSTATE PMIX_PROC_STATE_KILLED_BY_CMD
#endif

/* ------------------------------------------------------------------ */
/* proc state translation                                             */
/* ------------------------------------------------------------------ */

/* Every state PRRTE can put on a prte_proc_t, and the PMIx state a tool
 * querying that proc must be told. Where PRRTE is more specific than PMIx the
 * mapping collapses; nothing may map to UNDEF except UNDEF itself. */
static struct {
    const char *name;
    int prte;
    pmix_proc_state_t pmix;
} state_map[] = {
    {"UNDEF", PRTE_PROC_STATE_UNDEF, PMIX_PROC_STATE_UNDEF},
    {"INIT", PRTE_PROC_STATE_INIT, PMIX_PROC_STATE_LAUNCH_UNDERWAY},
    {"RESTART", PRTE_PROC_STATE_RESTART, PMIX_PROC_STATE_RESTART},
    {"TERMINATE", PRTE_PROC_STATE_TERMINATE, PMIX_PROC_STATE_TERMINATE},
    {"RUNNING", PRTE_PROC_STATE_RUNNING, PMIX_PROC_STATE_RUNNING},
    {"REGISTERED", PRTE_PROC_STATE_REGISTERED, PMIX_PROC_STATE_CONNECTED},

    /* PRRTE tracks more milestones on the way to reaping a proc than PMIx
     * names. All sit below the UNTERMINATED boundary, so the proc is still
     * alive as far as a querying tool is concerned. */
    {"IOF_COMPLETE", PRTE_PROC_STATE_IOF_COMPLETE, PMIX_PROC_STATE_RUNNING},
    {"WAITPID_FIRED", PRTE_PROC_STATE_WAITPID_FIRED, PMIX_PROC_STATE_RUNNING},
    {"MODEX_READY", PRTE_PROC_STATE_MODEX_READY, PMIX_PROC_STATE_RUNNING},
    {"READY_FOR_DEBUG", PRTE_PROC_STATE_READY_FOR_DEBUG, PMIX_PROC_STATE_RUNNING},

    {"UNTERMINATED", PRTE_PROC_STATE_UNTERMINATED, PMIX_PROC_STATE_UNTERMINATED},
    {"TERMINATED", PRTE_PROC_STATE_TERMINATED, PMIX_PROC_STATE_TERMINATED},

    {"KILLED_BY_CMD", PRTE_PROC_STATE_KILLED_BY_CMD, PMIX_PROC_STATE_KILLED_BY_CMD},
    {"ABORTED", PRTE_PROC_STATE_ABORTED, PMIX_PROC_STATE_ABORTED},
    {"FAILED_TO_START", PRTE_PROC_STATE_FAILED_TO_START, PMIX_PROC_STATE_FAILED_TO_START},
    {"ABORTED_BY_SIG", PRTE_PROC_STATE_ABORTED_BY_SIG, PMIX_PROC_STATE_ABORTED_BY_SIG},
    {"TERM_WO_SYNC", PRTE_PROC_STATE_TERM_WO_SYNC, PMIX_PROC_STATE_TERM_WO_SYNC},
    {"COMM_FAILED", PRTE_PROC_STATE_COMM_FAILED, PMIX_PROC_STATE_COMM_FAILED},
    {"SENSOR_BOUND_EXCEEDED", PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED,
     PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED},
    {"CALLED_ABORT", PRTE_PROC_STATE_CALLED_ABORT, PMIX_PROC_STATE_CALLED_ABORT},
    {"HEARTBEAT_FAILED", PRTE_PROC_STATE_HEARTBEAT_FAILED, PMIX_PROC_STATE_HEARTBEAT_FAILED},
    {"MIGRATING", PRTE_PROC_STATE_MIGRATING, PMIX_PROC_STATE_MIGRATING},
    {"CANNOT_RESTART", PRTE_PROC_STATE_CANNOT_RESTART, PMIX_PROC_STATE_CANNOT_RESTART},
    {"TERM_NON_ZERO", PRTE_PROC_STATE_TERM_NON_ZERO, PMIX_PROC_STATE_TERM_NON_ZERO},
    {"FAILED_TO_LAUNCH", PRTE_PROC_STATE_FAILED_TO_LAUNCH, PMIX_PROC_STATE_FAILED_TO_LAUNCH},

    /* every way PRRTE describes losing touch with a peer collapses onto
     * PMIx's single bucket */
    {"UNABLE_TO_SEND_MSG", PRTE_PROC_STATE_UNABLE_TO_SEND_MSG, PMIX_PROC_STATE_COMM_FAILED},
    {"LIFELINE_LOST", PRTE_PROC_STATE_LIFELINE_LOST, PMIX_PROC_STATE_COMM_FAILED},
    {"NO_PATH_TO_TARGET", PRTE_PROC_STATE_NO_PATH_TO_TARGET, PMIX_PROC_STATE_COMM_FAILED},
    {"FAILED_TO_CONNECT", PRTE_PROC_STATE_FAILED_TO_CONNECT, PMIX_PROC_STATE_COMM_FAILED},
    {"PEER_UNKNOWN", PRTE_PROC_STATE_PEER_UNKNOWN, PMIX_PROC_STATE_COMM_FAILED},

    {"KILLED_BY_RELEASE", PRTE_PROC_STATE_KILLED_BY_RELEASE, RELEASE_PSTATE},
};

static int test_convert_state(void)
{
    int failures = 0;
    size_t n;

    for (n = 0; n < sizeof(state_map) / sizeof(state_map[0]); n++) {
        pmix_proc_state_t got = prte_pmix_convert_state(state_map[n].prte);
        if (got != state_map[n].pmix) {
            fprintf(stderr,
                    "FAIL [convert_state]: PRTE_PROC_STATE_%s (%d) -> %d, expected %d\n",
                    state_map[n].name, state_map[n].prte, (int) got, (int) state_map[n].pmix);
            failures++;
        }
    }

    /* The two defects that motivated the table. 59 and 60 are adjacent and
     * were transposed; TERMINATED was simply absent. */
    CHECK("heartbeat is not migrating",
          PMIX_PROC_STATE_HEARTBEAT_FAILED
              == prte_pmix_convert_state(PRTE_PROC_STATE_HEARTBEAT_FAILED));
    CHECK("migrating survives",
          PMIX_PROC_STATE_MIGRATING == prte_pmix_convert_state(PRTE_PROC_STATE_MIGRATING));
    CHECK("a finished proc is not UNDEF",
          PMIX_PROC_STATE_TERMINATED == prte_pmix_convert_state(PRTE_PROC_STATE_TERMINATED));

    /* UNDEF is a real answer for a real input, and must not double as the
     * "I forgot this one" answer for anything else. */
    CHECK("undef is reserved",
          PMIX_PROC_STATE_UNDEF == prte_pmix_convert_state(PRTE_PROC_STATE_UNDEF));
    CHECK("unknown state is undef", PMIX_PROC_STATE_UNDEF == prte_pmix_convert_state(-12345));

    return failures;
}

static int test_convert_pstate(void)
{
    int failures = 0;
    size_t n;

    /* The reverse direction. Several PRRTE states share a PMIx state, so only
     * check the entries where the PMIx side is unambiguous -- i.e. where this
     * PMIx state appears exactly once in the table. */
    for (n = 0; n < sizeof(state_map) / sizeof(state_map[0]); n++) {
        size_t m, count = 0;
        int got;

        for (m = 0; m < sizeof(state_map) / sizeof(state_map[0]); m++) {
            if (state_map[m].pmix == state_map[n].pmix) {
                count++;
            }
        }
        if (1 != count) {
            continue;
        }
        got = prte_pmix_convert_pstate(state_map[n].pmix);
        if (got != state_map[n].prte) {
            fprintf(stderr, "FAIL [convert_pstate]: PMIx state %d -> %d, expected %d (%s)\n",
                    (int) state_map[n].pmix, got, state_map[n].prte, state_map[n].name);
            failures++;
        }
    }

    /* PMIx has two states PRRTE spells one way */
    CHECK("prepped is init", PRTE_PROC_STATE_INIT == prte_pmix_convert_pstate(PMIX_PROC_STATE_PREPPED));

    /* these were missing entirely, so a heartbeat failure or a sensor trip
     * arriving from PMIx was recorded as PRTE_PROC_STATE_UNDEF */
    CHECK("heartbeat survives inbound",
          PRTE_PROC_STATE_HEARTBEAT_FAILED
              == prte_pmix_convert_pstate(PMIX_PROC_STATE_HEARTBEAT_FAILED));
    CHECK("sensor bound survives inbound",
          PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED
              == prte_pmix_convert_pstate(PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED));

    return failures;
}

/* A state that survives a round trip is one both converters agree about. This
 * is what would have caught the 59/60 transposition on the day it was written:
 * convert_pstate had MIGRATING -> 60 all along, so the pair disagreed. */
static int test_state_roundtrip(void)
{
    int failures = 0;
    size_t n;

    for (n = 0; n < sizeof(state_map) / sizeof(state_map[0]); n++) {
        pmix_proc_state_t out;
        int back;

        /* the lossy collapses cannot round trip and are not expected to */
        if (PMIX_PROC_STATE_RUNNING == state_map[n].pmix
            && PRTE_PROC_STATE_RUNNING != state_map[n].prte) {
            continue;
        }
        if (PMIX_PROC_STATE_COMM_FAILED == state_map[n].pmix
            && PRTE_PROC_STATE_COMM_FAILED != state_map[n].prte) {
            continue;
        }
        /* only lossy where PMIx has no state of its own for it */
        if (PMIX_PROC_STATE_KILLED_BY_CMD == state_map[n].pmix
            && PRTE_PROC_STATE_KILLED_BY_CMD != state_map[n].prte) {
            continue;
        }

        out = prte_pmix_convert_state(state_map[n].prte);
        back = prte_pmix_convert_pstate(out);
        if (back != state_map[n].prte) {
            fprintf(stderr, "FAIL [state roundtrip]: %s: %d -> %d -> %d\n", state_map[n].name,
                    state_map[n].prte, (int) out, back);
            failures++;
        }
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* status translation                                                 */
/* ------------------------------------------------------------------ */

/* How far below PRTE_ERR_BASE the code list is allowed to run. Local to this
 * test on purpose: constants.h used to export a PRTE_ERR_MAX for it, which
 * nothing in the tree ever used and which -- because the base it was
 * computed from had the wrong sign -- evaluated to PRTE_SUCCESS. */
#define PRTE_CODE_SPAN 200

/* Is this value a legal PRRTE return code? They are all PRTE_ERR_BASE - n,
 * descending, so this is a range test rather than a membership test -- which
 * is all we need to catch a raw PMIx status leaking through.
 *
 * The base sits below PMIX_EXTERNAL_ERR_BASE, so this range does not overlap
 * PMIx's status space at all and the predicate is now exact rather than
 * merely indicative. PRTE_SUCCESS is the one code that is not an offset from
 * the base -- it is zero -- so it is named separately. */
static bool is_prte_code(int rc)
{
    if (PRTE_SUCCESS == rc) {
        return true;
    }
    return (rc <= PRTE_ERR_BASE && rc > (PRTE_ERR_BASE - PRTE_CODE_SPAN));
}

/* The headline defect: nothing prte_pmix_convert_status() returns may be a
 * value it was simply handed. Sweep the whole plausible PMIx range. */
static int test_status_no_passthrough(void)
{
    int failures = 0;
    pmix_status_t s;
    int leaked = 0;

    for (s = -1; s > -2000; s--) {
        int rc = prte_pmix_convert_status(s);

        if (!is_prte_code(rc)) {
            if (0 == leaked) {
                fprintf(stderr,
                        "FAIL [status passthrough]: PMIx status %d converted to %d, which is "
                        "not a PRRTE code\n",
                        (int) s, rc);
            }
            leaked++;
        }
        /* PRTE_SUCCESS means "the operation worked". No error status may
         * produce it -- a caller that checks `if (PRTE_SUCCESS != rc)` would
         * walk straight on. */
        if (PRTE_SUCCESS == rc && PMIX_OPERATION_SUCCEEDED != s) {
            fprintf(stderr, "FAIL [status success]: PMIx status %d converted to PRTE_SUCCESS\n",
                    (int) s);
            failures++;
        }
    }
    if (0 < leaked) {
        fprintf(stderr, "       (%d PMIx statuses leaked through unconverted)\n", leaked);
        failures++;
    }

    CHECK("success converts", PRTE_SUCCESS == prte_pmix_convert_status(PMIX_SUCCESS));
    CHECK("op succeeded converts", PRTE_SUCCESS == prte_pmix_convert_status(PMIX_OPERATION_SUCCEEDED));

    return failures;
}

/* Likewise outbound: a PRRTE code must never be handed to PMIx unconverted.
 * Sweep the whole PRRTE range. */
static int test_rc_no_passthrough(void)
{
    int failures = 0;
    int rc;
    int leaked = 0;

    for (rc = PRTE_ERR_BASE - 1; rc > (PRTE_ERR_BASE - PRTE_CODE_SPAN); rc--) {
        pmix_status_t s = prte_pmix_convert_rc(rc);
        if (PMIX_SUCCESS == s) {
            fprintf(stderr, "FAIL [rc success]: PRRTE code %d converted to PMIX_SUCCESS\n", rc);
            failures++;
        }
        if (s == rc) {
            /* Any code coming back unchanged is a value that fell through
             * the switch. This check used to need an exemption for
             * PRTE_ERROR, because it and PMIX_ERROR were both -1; now that
             * PRRTE's codes hang off PMIX_EXTERNAL_ERR_BASE there is no
             * value the two schemes share, so there is nothing to exempt. */
            fprintf(stderr, "FAIL [rc passthrough]: PRRTE code %d returned unconverted\n", rc);
            leaked++;
        }
    }
    if (0 < leaked) {
        fprintf(stderr, "FAIL [rc passthrough]: %d PRRTE codes returned unconverted\n", leaked);
        failures++;
    }

    CHECK("success converts", PMIX_SUCCESS == prte_pmix_convert_rc(PRTE_SUCCESS));

    return failures;
}

/* The specific mistranslations the old default produced. Each of these is a
 * real PMIx status whose numeric value is also a real, unrelated PRRTE code. */
static int test_status_known_collisions(void)
{
    int failures = 0;

    CHECK("pack failure is not a file-open failure",
          PRTE_ERR_PACK_FAILURE == prte_pmix_convert_status(PMIX_ERR_PACK_FAILURE));
    CHECK("unpack failure is not a file-write failure",
          PRTE_ERR_UNPACK_FAILURE == prte_pmix_convert_status(PMIX_ERR_UNPACK_FAILURE));
    CHECK("inadequate space is not a file-read failure",
          PRTE_ERR_UNPACK_INADEQUATE_SPACE
              == prte_pmix_convert_status(PMIX_ERR_UNPACK_INADEQUATE_SPACE));
    CHECK("read past end survives",
          PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER
              == prte_pmix_convert_status(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER));
    CHECK("type mismatch is not out-of-bounds",
          PRTE_ERR_TYPE_MISMATCH == prte_pmix_convert_status(PMIX_ERR_TYPE_MISMATCH));
    CHECK("nomem is out of resource",
          PRTE_ERR_OUT_OF_RESOURCE == prte_pmix_convert_status(PMIX_ERR_NOMEM));
    CHECK("comm failure is comm failure",
          PRTE_ERR_COMM_FAILURE == prte_pmix_convert_status(PMIX_ERR_COMM_FAILURE));

    /* PMIX_ERR_EMPTY (-60) has the value of PRTE_ERR_NODE_DOWN. Nothing in
     * PRRTE should ever conclude a node died because a buffer was empty. */
    CHECK("empty is not node-down", PRTE_ERR_NODE_DOWN != prte_pmix_convert_status(PMIX_ERR_EMPTY));
    /* PMIX_ERR_NOT_AVAILABLE (-64) has the value of PRTE_ERR_PROC_CHECKPOINT */
    CHECK("not-available is not a checkpoint",
          PRTE_ERR_PROC_CHECKPOINT != prte_pmix_convert_status(PMIX_ERR_NOT_AVAILABLE));

    /* "you may not" and "I could not get there" used to share
     * PMIX_ERR_UNREACH, so a job refused by the allocation ownership check
     * reached the user as a connectivity failure. */
    CHECK("a permission failure says so",
          PMIX_ERR_NO_PERMISSIONS == prte_pmix_convert_rc(PRTE_ERR_PERM));
    CHECK("a permission failure is not unreachable",
          PMIX_ERR_UNREACH != prte_pmix_convert_rc(PRTE_ERR_PERM));
    CHECK("unreachable is still unreachable",
          PMIX_ERR_UNREACH == prte_pmix_convert_rc(PRTE_ERR_UNREACH));
    CHECK("no-permissions comes back as a permission failure",
          PRTE_ERR_PERM == prte_pmix_convert_status(PMIX_ERR_NO_PERMISSIONS));

    return failures;
}

/* PRTE_ERR_SILENT means "already reported to the user, do not report again".
 * It has to survive the trip out to PMIx or the tool prints a second,
 * redundant error. */
static int test_status_roundtrip(void)
{
    int failures = 0;
    static const int codes[] = {
        PRTE_ERR_SILENT,
        PRTE_ERR_OUT_OF_RESOURCE,
        PRTE_ERR_BAD_PARAM,
        PRTE_ERR_NOT_FOUND,
        PRTE_ERR_NOT_SUPPORTED,
        PRTE_ERR_TIMEOUT,
        PRTE_ERR_WOULD_BLOCK,
        PRTE_EXISTS,
        PRTE_ERR_PACK_FAILURE,
        PRTE_ERR_UNPACK_FAILURE,
        PRTE_ERR_UNPACK_INADEQUATE_SPACE,
        PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER,
        PRTE_ERR_TYPE_MISMATCH,
        PRTE_ERR_UNKNOWN_DATA_TYPE,
        PRTE_ERR_COMM_FAILURE,
        PRTE_ERR_RESOURCE_BUSY,
        PRTE_ERR_NOT_AVAILABLE,
        PRTE_ERR_NODE_DOWN,
        PRTE_ERR_NODE_OFFLINE,
        PRTE_ERR_PROC_ABORTED,
        PRTE_ERR_PROC_RESTART,
        PRTE_ERR_PROC_MIGRATE,
        PRTE_ERR_EVENT_REGISTRATION,
        PRTE_ERR_DATA_VALUE_NOT_FOUND,
        PRTE_ERR_PERM,
        PRTE_ERR_UNREACH,
        PRTE_SUCCESS,
    };
    size_t n;

    for (n = 0; n < sizeof(codes) / sizeof(codes[0]); n++) {
        pmix_status_t out = prte_pmix_convert_rc(codes[n]);
        int back = prte_pmix_convert_status(out);
        if (back != codes[n]) {
            fprintf(stderr, "FAIL [status roundtrip]: %d -> %d -> %d\n", codes[n], (int) out, back);
            failures++;
        }
    }

    CHECK("silent stays silent", PMIX_ERR_SILENT == prte_pmix_convert_rc(PRTE_ERR_SILENT));

    return failures;
}

/* ------------------------------------------------------------------ */
/* job/proc state -> error                                            */
/* ------------------------------------------------------------------ */

/* These two feed the spawn response and the failure notification a tool
 * receives, so PMIX_ERROR ("something went wrong, no idea what") is the answer
 * of last resort and must not be the answer for a state PRRTE actually
 * reaches. */
static int test_job_state_to_error(void)
{
    int failures = 0;
    static const struct {
        const char *name;
        int state;
        pmix_status_t expect;
    } tbl[] = {
        {"ALLOC_FAILED", PRTE_JOB_STATE_ALLOC_FAILED, PMIX_ERR_JOB_ALLOC_FAILED},
        {"MAP_FAILED", PRTE_JOB_STATE_MAP_FAILED, PMIX_ERR_JOB_FAILED_TO_MAP},
        {"NEVER_LAUNCHED", PRTE_JOB_STATE_NEVER_LAUNCHED, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"FAILED_TO_LAUNCH", PRTE_JOB_STATE_FAILED_TO_LAUNCH, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"FAILED_TO_START", PRTE_JOB_STATE_FAILED_TO_START, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"CANNOT_LAUNCH", PRTE_JOB_STATE_CANNOT_LAUNCH, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"KILLED_BY_CMD", PRTE_JOB_STATE_KILLED_BY_CMD, PMIX_ERR_JOB_CANCELED},
        {"KILLED_BY_RELEASE", PRTE_JOB_STATE_KILLED_BY_RELEASE, RELEASE_STATUS},
        {"ABORTED", PRTE_JOB_STATE_ABORTED, PMIX_ERR_JOB_ABORTED},
        {"CALLED_ABORT", PRTE_JOB_STATE_CALLED_ABORT, PMIX_ERR_JOB_ABORTED},
        {"SILENT_ABORT", PRTE_JOB_STATE_SILENT_ABORT, PMIX_ERR_JOB_ABORTED},
        {"ABORTED_BY_SIG", PRTE_JOB_STATE_ABORTED_BY_SIG, PMIX_ERR_JOB_ABORTED_BY_SIG},
        {"ABORTED_WO_SYNC", PRTE_JOB_STATE_ABORTED_WO_SYNC, PMIX_ERR_JOB_TERM_WO_SYNC},
        {"TERMINATED", PRTE_JOB_STATE_TERMINATED, PMIX_EVENT_JOB_END},
    };
    size_t n;

    for (n = 0; n < sizeof(tbl) / sizeof(tbl[0]); n++) {
        pmix_status_t got = prte_pmix_convert_job_state_to_error(tbl[n].state);
        if (got != tbl[n].expect) {
            fprintf(stderr, "FAIL [job state]: %s (%d) -> %d, expected %d\n", tbl[n].name,
                    tbl[n].state, (int) got, (int) tbl[n].expect);
            failures++;
        }
    }

    CHECK("unknown job state is a generic error",
          PMIX_ERROR == prte_pmix_convert_job_state_to_error(-12345));

    return failures;
}

static int test_proc_state_to_error(void)
{
    int failures = 0;
    static const struct {
        const char *name;
        int state;
        pmix_status_t expect;
    } tbl[] = {
        {"KILLED_BY_CMD", PRTE_PROC_STATE_KILLED_BY_CMD, PMIX_ERR_JOB_CANCELED},
        {"KILLED_BY_RELEASE", PRTE_PROC_STATE_KILLED_BY_RELEASE, RELEASE_STATUS},
        {"ABORTED", PRTE_PROC_STATE_ABORTED, PMIX_ERR_JOB_ABORTED},
        {"CALLED_ABORT", PRTE_PROC_STATE_CALLED_ABORT, PMIX_ERR_JOB_ABORTED},
        {"ABORTED_BY_SIG", PRTE_PROC_STATE_ABORTED_BY_SIG, PMIX_ERR_JOB_ABORTED_BY_SIG},
        {"FAILED_TO_LAUNCH", PRTE_PROC_STATE_FAILED_TO_LAUNCH, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"FAILED_TO_START", PRTE_PROC_STATE_FAILED_TO_START, PMIX_ERR_JOB_FAILED_TO_LAUNCH},
        {"TERM_WO_SYNC", PRTE_PROC_STATE_TERM_WO_SYNC, PMIX_ERR_JOB_TERM_WO_SYNC},
        {"COMM_FAILED", PRTE_PROC_STATE_COMM_FAILED, PMIX_ERR_COMM_FAILURE},
        {"UNABLE_TO_SEND_MSG", PRTE_PROC_STATE_UNABLE_TO_SEND_MSG, PMIX_ERR_COMM_FAILURE},
        {"LIFELINE_LOST", PRTE_PROC_STATE_LIFELINE_LOST, PMIX_ERR_COMM_FAILURE},
        {"NO_PATH_TO_TARGET", PRTE_PROC_STATE_NO_PATH_TO_TARGET, PMIX_ERR_COMM_FAILURE},
        {"FAILED_TO_CONNECT", PRTE_PROC_STATE_FAILED_TO_CONNECT, PMIX_ERR_COMM_FAILURE},
        {"PEER_UNKNOWN", PRTE_PROC_STATE_PEER_UNKNOWN, PMIX_ERR_COMM_FAILURE},
        {"CANNOT_RESTART", PRTE_PROC_STATE_CANNOT_RESTART, PMIX_ERR_PROC_RESTART},
        {"TERM_NON_ZERO", PRTE_PROC_STATE_TERM_NON_ZERO, PMIX_ERR_JOB_NON_ZERO_TERM},
        {"SENSOR_BOUND_EXCEEDED", PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED,
         PMIX_ERR_JOB_SENSOR_BOUND_EXCEEDED},
    };
    size_t n;

    for (n = 0; n < sizeof(tbl) / sizeof(tbl[0]); n++) {
        pmix_status_t got = prte_pmix_convert_proc_state_to_error(tbl[n].state);
        if (got != tbl[n].expect) {
            fprintf(stderr, "FAIL [proc state]: %s (%d) -> %d, expected %d\n", tbl[n].name,
                    tbl[n].state, (int) got, (int) tbl[n].expect);
            failures++;
        }
    }

    /* The comm-failure group must be treated identically by both the
     * state translator and the error translator -- they describe the same
     * event to the same tool by two different routes. */
    CHECK("comm group agrees",
          (PMIX_PROC_STATE_COMM_FAILED == prte_pmix_convert_state(PRTE_PROC_STATE_LIFELINE_LOST))
              && (PMIX_ERR_COMM_FAILURE
                  == prte_pmix_convert_proc_state_to_error(PRTE_PROC_STATE_LIFELINE_LOST)));

    return failures;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int failures = 0;

    failures += test_convert_state();
    failures += test_convert_pstate();
    failures += test_state_roundtrip();
    failures += test_status_no_passthrough();
    failures += test_rc_no_passthrough();
    failures += test_status_known_collisions();
    failures += test_status_roundtrip();
    failures += test_job_state_to_error();
    failures += test_proc_state_to_error();

    if (0 == failures) {
        fprintf(stdout, "PASSED all pmix shim unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d pmix shim unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
