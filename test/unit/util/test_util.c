/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/util -- PRRTE's internal utility layer.
 *
 * Almost everything here is a pure function of its arguments, which is why
 * it is worth testing and also why it never was: the callers are launch,
 * routing and mapping paths that need a live DVM, so a defect in the helper
 * only ever showed up as strange behavior three subsystems away. Each group
 * below pins down one of those helpers, and each one of them is here because
 * this review found a defect in it:
 *
 *  - prte_util_compare_name_fields() compared namespaces by *length*. PRRTE
 *    hands out namespaces that differ only in a trailing job number, so rank
 *    N of one job compared EQUAL to rank N of every sibling job in the same
 *    DVM -- and that function is how errmgr/prted asks "is this proc me?",
 *    how iof/prted finds the proc whose stdin it holds, and how oob/tcp
 *    breaks a simultaneous-connect tie.
 *
 *  - prte_util_print_job_family(), prte_util_print_local_jobid() and
 *    prte_util_convert_string_to_process_name() each punched a temporary
 *    '\0' into the caller's namespace. The argument is normally a live
 *    jdata->nspace, so the truncation was visible to any other thread that
 *    read the name in that window -- and is undefined behavior outright for
 *    a string literal.
 *
 *  - prte_strerror() and the four state-name functions are the only way an
 *    error or a state is ever reported to a user, and a code with no entry
 *    prints "Unknown error" from PRTE_ERROR_LOG(). Five codes had no entry.
 *    test_error_strings() sweeps the whole numeric range rather than a
 *    hand-kept list, so the next code added without a string fails here.
 *
 *  - prte_attr_key_to_str() had a key returning another key's name (a
 *    copy/paste), which no test could see because each string is only ever
 *    printed. Checking that the names are *unique* across the whole key
 *    space catches that class directly.
 *
 *  - prte_attr_load()/prte_attr_unload() round-trip every attribute PRRTE
 *    stores. Reloading a PMIX_ENVAR attribute freed the old strings but only
 *    reassigned the fields the new value happened to supply, leaving a
 *    pointer to freed storage for the destructor to free again.
 *
 *  - prte_util_add_dash_host_nodes() carried its per-token slot state across
 *    tokens, so "--host a:*,b" handed b the auto-detect marker and gave it
 *    zero slots.
 *
 *  - the hostfile parser left its FILE* open and the flex buffer live on
 *    every error path, so the next hostfile parsed in the same process
 *    resumed in the middle of the failed one; and its exclusion pass in
 *    prte_util_get_ordered_host_list() walked the list through an item it
 *    had already released.
 *
 * What is deliberately NOT here: session_dir (creates directories under
 * the real tmpdir), stacktrace (installs signal handlers), daemon_init
 * (forks), and the parts of nidmap that need a populated DVM. Those belong
 * to the live smoke test and to contrib/dockerswarm.
 */

#include "prte_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "types.h"

#include "src/class/pmix_pointer_array.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/attr.h"
#include "src/util/dash_host/dash_host.h"
#include "src/util/error.h"
#include "src/util/error_strings.h"
#include "src/util/hostfile/hostfile.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_argv.h"
#include "src/util/proc_info.h"
#include "src/util/sys_limits.h"

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);           \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* name_fns                                                           */
/* ------------------------------------------------------------------ */

static int test_compare_name_fields(void)
{
    int failures = 0;
    pmix_proc_t a, b, wild;

    /* Two namespaces of the SAME LENGTH but different content. This is not a
     * contrived case: every job in a DVM is named "<dvm>@<n>", so these are
     * the first two jobs a single prte launches. */
    PMIX_LOAD_PROCID(&a, "prterun-node01-1234@1", 0);
    PMIX_LOAD_PROCID(&b, "prterun-node01-1234@2", 0);
    CHECK("same-length nspaces are not equal",
          PRTE_EQUAL != prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b));
    CHECK("nspace comparison is antisymmetric",
          prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b)
              != prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &b, &a));
    /* ...and it orders them, so the oob tie-break has a winner */
    CHECK("lower nspace loses",
          PRTE_VALUE2_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b));
    CHECK("higher nspace wins",
          PRTE_VALUE1_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &b, &a));

    /* identical names really are equal */
    PMIX_LOAD_PROCID(&b, "prterun-node01-1234@1", 0);
    CHECK("identical names are equal",
          PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b));

    /* same job, different rank */
    b.rank = 1;
    CHECK("rank orders within a job",
          PRTE_VALUE2_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b));
    CHECK("jobid-only comparison ignores rank",
          PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_JOBID, &a, &b));
    CHECK("vpid-only comparison ignores nspace",
          PRTE_VALUE2_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_VPID, &a, &b));

    /* a namespace of a different length still compares */
    PMIX_LOAD_PROCID(&b, "prterun-node01-1234@10", 0);
    CHECK("differing-length nspaces are not equal",
          PRTE_EQUAL != prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &b));

    /* wildcards, when asked for */
    PMIX_LOAD_PROCID(&wild, NULL, PMIX_RANK_WILDCARD);
    CHECK("wildcard matches with CMP_WILD",
          PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL | PRTE_NS_CMP_WILD,
                                                      &a, &wild));
    CHECK("wildcard does not match without CMP_WILD",
          PRTE_EQUAL != prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, &wild));

    /* NULL handling */
    CHECK("both NULL is equal", PRTE_EQUAL == prte_util_compare_name_fields(PRTE_NS_CMP_ALL,
                                                                           NULL, NULL));
    CHECK("NULL loses to a name",
          PRTE_VALUE2_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, NULL, &a));
    CHECK("a name beats NULL",
          PRTE_VALUE1_GREATER == prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &a, NULL));

    return failures;
}

static int test_name_printing(void)
{
    int failures = 0;
    pmix_nspace_t job;
    pmix_proc_t proc;
    char *str = NULL;
    int rc;

    /* the printers must not modify what they are handed */
    PMIX_LOAD_NSPACE(job, "prterun-node01-1234@7");
    CHECK("job family strips the local jobid",
          0 == strcmp("prterun-node01-1234", prte_util_print_job_family(job)));
    CHECK("print_job_family does not modify its argument",
          0 == strcmp("prterun-node01-1234@7", job));
    CHECK("local jobid is the part after the @",
          0 == strcmp("7", prte_util_print_local_jobid(job)));
    CHECK("print_local_jobid does not modify its argument",
          0 == strcmp("prterun-node01-1234@7", job));

    /* a name with no '@' is not a PRRTE job - it comes back whole */
    PMIX_LOAD_NSPACE(job, "someones-nspace");
    CHECK("non-PRRTE nspace passes through job family",
          0 == strcmp("someones-nspace", prte_util_print_job_family(job)));

    /* the ring of buffers has to give distinct answers for distinct
     * arguments used in one printf - that is its whole reason to exist */
    {
        pmix_proc_t p1, p2;
        char *s1, *s2;
        PMIX_LOAD_PROCID(&p1, "nspace-one", 1);
        PMIX_LOAD_PROCID(&p2, "nspace-two", 2);
        s1 = prte_util_print_name_args(&p1);
        s2 = prte_util_print_name_args(&p2);
        CHECK("two names in flight do not share a buffer", s1 != s2);
        CHECK("first name survived the second", NULL != strstr(s1, "nspace-one"));
        CHECK("second name is correct", NULL != strstr(s2, "nspace-two"));
    }

    /* NULL is safe */
    CHECK("NULL name prints a placeholder", NULL != prte_util_print_name_args(NULL));

    /* string <-> proc conversion, and the input must survive it */
    {
        const char *fixed = "prterun-node01-1234@7.5";
        char buf[64];

        pmix_strncpy(buf, fixed, sizeof(buf) - 1);
        rc = prte_util_convert_string_to_process_name(&proc, buf);
        CHECK("convert_string_to_process_name succeeds", PRTE_SUCCESS == rc);
        CHECK("nspace parsed", 0 == strcmp("prterun-node01-1234@7", proc.nspace));
        CHECK("rank parsed", 5 == proc.rank);
        CHECK("convert_string_to_process_name does not modify its argument",
              0 == strcmp(fixed, buf));

        /* a string with no '.' is not a process name */
        rc = prte_util_convert_string_to_process_name(&proc, "no-rank-here");
        CHECK("a name with no rank is rejected", PRTE_SUCCESS != rc);
        rc = prte_util_convert_string_to_process_name(&proc, NULL);
        CHECK("a NULL name is rejected", PRTE_SUCCESS != rc);
    }

    /* round trip */
    PMIX_LOAD_PROCID(&proc, "prterun-node01-1234@7", 5);
    rc = prte_util_convert_process_name_to_string(&str, &proc);
    CHECK("convert_process_name_to_string succeeds", PRTE_SUCCESS == rc);
    if (NULL != str) {
        pmix_proc_t back;
        CHECK("round trip parses back",
              PRTE_SUCCESS == prte_util_convert_string_to_process_name(&back, str));
        CHECK("round trip preserves nspace", 0 == strcmp(proc.nspace, back.nspace));
        CHECK("round trip preserves rank", proc.rank == back.rank);
        free(str);
        str = NULL;
    }

    /* the version string builder - the interesting cases are the ones with
     * nothing to say, since it used to strdup(NULL) or read uninitialized
     * stack for them */
    str = prte_util_make_version_string("all", 3, 1, 0, NULL, NULL);
    CHECK("version 'all' renders", NULL != str && 0 == strcmp("3.1.0", str));
    free(str);
    str = prte_util_make_version_string("major", 3, 1, 0, NULL, NULL);
    CHECK("version 'major' renders", NULL != str && 0 == strcmp("3", str));
    free(str);
    str = prte_util_make_version_string("greek", 3, 1, 0, NULL, NULL);
    CHECK("version 'greek' with no greek is empty, not a crash",
          NULL != str && 0 == strlen(str));
    free(str);
    str = prte_util_make_version_string("nonsense", 3, 1, 0, NULL, NULL);
    CHECK("an unrecognized scope yields an empty string, not junk",
          NULL != str && 0 == strlen(str));
    free(str);

    return failures;
}

/* ------------------------------------------------------------------ */
/* error.c and error_strings.c                                        */
/* ------------------------------------------------------------------ */

/*
 * Sweep the two contiguous error-code ranges. Every code that constants.h
 * assigns must have a prte_strerror() entry, because PRTE_ERROR_LOG() is the
 * only report a user ever gets for most of them. The offsets listed here are
 * the ones constants.h deliberately skips; keeping them explicit means both
 * "added a code and forgot the string" and "reused a skipped offset" (which
 * the coding rules call out by name) show up as a failure here.
 */
/* The code list is one contiguous run of offsets from PRTE_ERR_BASE, with a
 * deliberate hole between the two groups (see constants.h): offsets 1..72
 * are the codes mirrored from PMIx, 101.. are PRRTE's own. Nothing is
 * assigned in between. */
#define GROUP1_LAST  72  /* PRTE_OPERATION_SUCCEEDED */
#define GROUP2_FIRST 101 /* PRTE_ERR_RECV_LESS_THAN_POSTED */
/* The end of the list is NOT written out here. It used to be -- as "155 /
 * PRTE_ERR_SLURM_SHRINK_FAILURE" -- and it stayed that way when
 * PRTE_ERR_PRELOAD_CONFLICT was appended at offset 156, so this sweep, whose
 * entire job is to catch a code that was added without a string, stopped one
 * short of the code most likely to be missing one. It now derives from
 * PRTE_ERR_MAX in constants.h, and test_error_bound() below fails if that
 * bound has gone stale in either direction. */
#define GROUP2_LAST  (PRTE_ERR_BASE - PRTE_ERR_MAX - 1)

static int test_error_strings(void)
{
    int failures = 0;
    int off;
    const char *s;

    CHECK("success has a string", 0 == strcmp("Success", prte_strerror(PRTE_SUCCESS)));

    for (off = 1; off <= GROUP2_LAST; off++) {
        if (off > GROUP1_LAST && off < GROUP2_FIRST) {
            continue; /* the reserved hole between the groups */
        }
        s = prte_strerror(PRTE_ERR_BASE - off);
        if (NULL == s || 0 == strcmp("Unknown error", s)) {
            fprintf(stderr, "FAIL [strerror]: PRTE_ERR_BASE - %d has no string\n", off);
            failures++;
        }
    }
    /* an offset in the reserved hole must stay unassigned - if it acquires a
     * string, somebody put a code there without saying so */
    CHECK("the reserved hole stays unassigned",
          0 == strcmp("Unknown error", prte_strerror(PRTE_ERR_BASE - (GROUP1_LAST + 1))));
    /* and something well outside the range is still handled */
    CHECK("an unknown code is reported as unknown",
          0 == strcmp("Unknown error", prte_strerror(PRTE_ERR_BASE - 1000)));

    /* PRTE_ERR_SILENT must never be logged - PRTE_ERROR_LOG() suppresses it -
     * but it still needs a name for anything that prints it directly */
    CHECK("silent has a string", NULL != prte_strerror(PRTE_ERR_SILENT));

    return failures;
}

/* The sweep above is only as good as the bound it runs to, and a bound is
 * exactly the kind of thing that goes stale without anyone noticing -- this
 * one did, the first time a code was appended after it was written, and the
 * result was a sweep that quietly stopped short of the newest code.
 *
 * So check the bound itself. prte_strerror() is the oracle: PRTE_ERR_MAX is
 * one past the end, so the offset just inside it must be a real named code
 * and PRTE_ERR_MAX itself must not be. That catches both directions --
 * appending a code and forgetting to bump the bound, and bumping the bound
 * past the last code.
 */
static int test_error_bound(void)
{
    int failures = 0;

    CHECK("the bound is not stale (a code was added without bumping it)",
          0 != strcmp("Unknown error", prte_strerror(PRTE_ERR_MAX + 1)));
    CHECK("the bound is not past the end (nothing is assigned at it)",
          0 == strcmp("Unknown error", prte_strerror(PRTE_ERR_MAX)));

    /* and it really is one past the end of the run the sweep walks */
    CHECK("the bound agrees with the sweep", (PRTE_ERR_BASE - GROUP2_LAST) == (PRTE_ERR_MAX + 1));

    return failures;
}

/*
 * The state-name functions.
 *
 * A numeric sweep is not possible here: unlike the error codes, the state
 * families are #defines with large intentional holes, and reserved-but-unused
 * values must NOT acquire names. So each family is listed explicitly, which
 * buys three checks the code base has no other way to make:
 *
 *   - every state has a name (the coding rules require it: an unnamed state
 *     surfaces to the user as "UNKNOWN STATE!" from --display-map,
 *     --report-state and every errmgr diagnostic);
 *   - no two states share a name (a copy/paste in the switch);
 *   - no two states share a *value*, which the coding rules call out by name
 *     as "a miserable bug to track down" because it silently makes two
 *     distinct states compare equal. The header's numbering already skips
 *     some offsets, and nothing but this check stops a new state from
 *     claiming one that is taken.
 *
 * Adding a state means adding it here too.
 */
typedef struct {
    const char *sym;
    int value;
} state_entry_t;

#define ST(x) {#x, (int) (x)}

static const state_entry_t job_states[] = {
    ST(PRTE_JOB_STATE_UNDEF),          ST(PRTE_JOB_STATE_INIT),
    ST(PRTE_JOB_STATE_INIT_COMPLETE),  ST(PRTE_JOB_STATE_ALLOCATE),
    ST(PRTE_JOB_STATE_ALLOCATION_COMPLETE), ST(PRTE_JOB_STATE_MAP),
    ST(PRTE_JOB_STATE_MAP_COMPLETE),   ST(PRTE_JOB_STATE_SYSTEM_PREP),
    ST(PRTE_JOB_STATE_LAUNCH_DAEMONS), ST(PRTE_JOB_STATE_DAEMONS_LAUNCHED),
    ST(PRTE_JOB_STATE_DAEMONS_REPORTED), ST(PRTE_JOB_STATE_VM_READY),
    ST(PRTE_JOB_STATE_LAUNCH_APPS),    ST(PRTE_JOB_STATE_SEND_LAUNCH_MSG),
    ST(PRTE_JOB_STATE_RUNNING),        ST(PRTE_JOB_STATE_SUSPENDED),
    ST(PRTE_JOB_STATE_REGISTERED),     ST(PRTE_JOB_STATE_LOCAL_LAUNCH_COMPLETE),
    ST(PRTE_JOB_STATE_READY_FOR_DEBUG), ST(PRTE_JOB_STATE_STARTED),
    ST(PRTE_JOB_STATE_UNTERMINATED),   ST(PRTE_JOB_STATE_TERMINATED),
    ST(PRTE_JOB_STATE_NOTIFY_COMPLETED), ST(PRTE_JOB_STATE_NOTIFIED),
    ST(PRTE_JOB_STATE_ALL_JOBS_COMPLETE), ST(PRTE_JOB_STATE_ERROR),
    ST(PRTE_JOB_STATE_KILLED_BY_CMD),  ST(PRTE_JOB_STATE_ABORTED),
    ST(PRTE_JOB_STATE_FAILED_TO_START), ST(PRTE_JOB_STATE_ABORTED_BY_SIG),
    ST(PRTE_JOB_STATE_ABORTED_WO_SYNC), ST(PRTE_JOB_STATE_COMM_FAILED),
    ST(PRTE_JOB_STATE_SENSOR_BOUND_EXCEEDED), ST(PRTE_JOB_STATE_CALLED_ABORT),
    ST(PRTE_JOB_STATE_HEARTBEAT_FAILED), ST(PRTE_JOB_STATE_NEVER_LAUNCHED),
    ST(PRTE_JOB_STATE_ABORT_ORDERED),  ST(PRTE_JOB_STATE_NON_ZERO_TERM),
    ST(PRTE_JOB_STATE_FAILED_TO_LAUNCH), ST(PRTE_JOB_STATE_FORCED_EXIT),
    ST(PRTE_JOB_STATE_DAEMONS_TERMINATED), ST(PRTE_JOB_STATE_SILENT_ABORT),
    ST(PRTE_JOB_STATE_REPORT_PROGRESS), ST(PRTE_JOB_STATE_ALLOC_FAILED),
    ST(PRTE_JOB_STATE_MAP_FAILED),     ST(PRTE_JOB_STATE_CANNOT_LAUNCH),
    ST(PRTE_JOB_STATE_FILES_POSN_FAILED), ST(PRTE_JOB_STATE_WAITING_FOR_DAEMONS),
    ST(PRTE_JOB_STATE_FT_CHECKPOINT),  ST(PRTE_JOB_STATE_FT_CONTINUE),
    ST(PRTE_JOB_STATE_FT_RESTART),     ST(PRTE_JOB_STATE_ANY),
};

static const state_entry_t proc_states[] = {
    ST(PRTE_PROC_STATE_UNDEF),         ST(PRTE_PROC_STATE_INIT),
    ST(PRTE_PROC_STATE_RESTART),       ST(PRTE_PROC_STATE_TERMINATE),
    ST(PRTE_PROC_STATE_RUNNING),       ST(PRTE_PROC_STATE_REGISTERED),
    ST(PRTE_PROC_STATE_IOF_COMPLETE),  ST(PRTE_PROC_STATE_WAITPID_FIRED),
    ST(PRTE_PROC_STATE_MODEX_READY),   ST(PRTE_PROC_STATE_READY_FOR_DEBUG),
    ST(PRTE_PROC_STATE_UNTERMINATED),  ST(PRTE_PROC_STATE_TERMINATED),
    ST(PRTE_PROC_STATE_ERROR),         ST(PRTE_PROC_STATE_KILLED_BY_CMD),
    ST(PRTE_PROC_STATE_ABORTED),       ST(PRTE_PROC_STATE_FAILED_TO_START),
    ST(PRTE_PROC_STATE_ABORTED_BY_SIG), ST(PRTE_PROC_STATE_TERM_WO_SYNC),
    ST(PRTE_PROC_STATE_COMM_FAILED),   ST(PRTE_PROC_STATE_SENSOR_BOUND_EXCEEDED),
    ST(PRTE_PROC_STATE_CALLED_ABORT),  ST(PRTE_PROC_STATE_HEARTBEAT_FAILED),
    ST(PRTE_PROC_STATE_MIGRATING),     ST(PRTE_PROC_STATE_CANNOT_RESTART),
    ST(PRTE_PROC_STATE_TERM_NON_ZERO), ST(PRTE_PROC_STATE_FAILED_TO_LAUNCH),
    ST(PRTE_PROC_STATE_UNABLE_TO_SEND_MSG), ST(PRTE_PROC_STATE_LIFELINE_LOST),
    ST(PRTE_PROC_STATE_NO_PATH_TO_TARGET), ST(PRTE_PROC_STATE_FAILED_TO_CONNECT),
    ST(PRTE_PROC_STATE_PEER_UNKNOWN),  ST(PRTE_PROC_STATE_ANY),
};

static const state_entry_t node_states[] = {
    ST(PRTE_NODE_STATE_UNDEF),   ST(PRTE_NODE_STATE_UNKNOWN),
    ST(PRTE_NODE_STATE_DOWN),    ST(PRTE_NODE_STATE_UP),
    ST(PRTE_NODE_STATE_REBOOT),  ST(PRTE_NODE_STATE_DO_NOT_USE),
    ST(PRTE_NODE_STATE_NOT_INCLUDED), ST(PRTE_NODE_STATE_ADDED),
};

static const state_entry_t app_states[] = {
    ST(PRTE_APP_STATE_UNDEF),      ST(PRTE_APP_STATE_INIT),
    ST(PRTE_APP_STATE_ALL_MAPPED), ST(PRTE_APP_STATE_RUNNING),
    ST(PRTE_APP_STATE_COMPLETED),
};

static int check_state_family(const char *what, const state_entry_t *tbl, size_t n,
                              const char *(*to_str)(int))
{
    int failures = 0;
    size_t i, j;

    for (i = 0; i < n; i++) {
        const char *s = to_str(tbl[i].value);

        if (NULL == s || 0 == strcmp("UNKNOWN STATE!", s)) {
            fprintf(stderr, "FAIL [%s]: %s (%d) has no name\n", what, tbl[i].sym,
                    tbl[i].value);
            failures++;
            continue;
        }
        for (j = 0; j < i; j++) {
            if (tbl[j].value == tbl[i].value) {
                fprintf(stderr, "FAIL [%s]: %s and %s share the value %d\n", what,
                        tbl[j].sym, tbl[i].sym, tbl[i].value);
                failures++;
            }
            if (0 == strcmp(to_str(tbl[j].value), s)) {
                fprintf(stderr, "FAIL [%s]: %s and %s both render as \"%s\"\n", what,
                        tbl[j].sym, tbl[i].sym, s);
                failures++;
            }
        }
    }
    return failures;
}

/* the to_str functions take typedef'd state types, not int */
static const char *job_to_str(int s)
{
    return prte_job_state_to_str((prte_job_state_t) s);
}
static const char *proc_to_str(int s)
{
    return prte_proc_state_to_str((prte_proc_state_t) s);
}
static const char *node_to_str(int s)
{
    return prte_node_state_to_str((prte_node_state_t) s);
}
static const char *app_to_str(int s)
{
    return prte_app_ctx_state_to_str((prte_app_state_t) s);
}

static int test_state_strings(void)
{
    int failures = 0;

    failures += check_state_family("job state", job_states,
                                   sizeof(job_states) / sizeof(job_states[0]), job_to_str);
    failures += check_state_family("proc state", proc_states,
                                   sizeof(proc_states) / sizeof(proc_states[0]), proc_to_str);
    failures += check_state_family("node state", node_states,
                                   sizeof(node_states) / sizeof(node_states[0]), node_to_str);
    failures += check_state_family("app state", app_states,
                                   sizeof(app_states) / sizeof(app_states[0]), app_to_str);

    /* a value that is not a state is reported as such rather than indexing
     * something */
    CHECK("an undefined job state is reported",
          0 == strcmp("UNKNOWN STATE!", prte_job_state_to_str(PRTE_JOB_STATE_FT_RESTART + 100)));
    CHECK("an undefined proc state is reported",
          0 == strcmp("UNKNOWN STATE!",
                      prte_proc_state_to_str(PRTE_PROC_STATE_PEER_UNKNOWN + 100)));

    return failures;
}

/* ------------------------------------------------------------------ */
/* attr.c                                                             */
/* ------------------------------------------------------------------ */

/*
 * Every attribute key must render to a name, and no two keys may render to
 * the same one: a key returning a neighbor's name is invisible in any test
 * that only checks "is it non-empty", and that is exactly the defect this
 * review found (PRTE_JOB_ADD_ENVAR printed "PRTE_APP_ADD_ENVAR").
 */
static int test_attr_key_names(void)
{
    int failures = 0;
    char **seen = NULL;
    int key, i, n;

    for (key = PRTE_ATTR_KEY_BASE + 1; key < PRTE_ATTR_KEY_MAX; key++) {
        const char *name = prte_attr_key_to_str(key);

        if (NULL == name) {
            fprintf(stderr, "FAIL [attr key]: %d rendered NULL\n", key);
            failures++;
            continue;
        }
        if (0 == strncmp(name, "UNKNOWN-KEY", strlen("UNKNOWN-KEY"))) {
            /* an unassigned slot in the key space - not every value between
             * the two markers is a key */
            continue;
        }
        n = PMIx_Argv_count(seen);
        for (i = 0; i < n; i++) {
            if (0 == strcmp(seen[i], name)) {
                fprintf(stderr, "FAIL [attr key]: key %d and an earlier key both "
                                "render as \"%s\"\n", key, name);
                failures++;
                break;
            }
        }
        PMIx_Argv_append_nosize(&seen, name);
    }
    PMIx_Argv_free(seen);

    /* an out-of-range key is reported, not silently accepted */
    CHECK("an unknown key says so",
          0 == strncmp("UNKNOWN-KEY", prte_attr_key_to_str(PRTE_ATTR_KEY_MAX + 1000),
                       strlen("UNKNOWN-KEY")));

    return failures;
}

static int test_attr_round_trip(void)
{
    int failures = 0;
    pmix_list_t attrs;
    int ival = 42, iout = 0, *iptr = &iout;
    bool bout = false, *bptr = &bout;
    char *sout = NULL;
    pmix_envar_t envar, *eout = NULL;
    prte_attribute_t *kv;

    PMIX_CONSTRUCT(&attrs, pmix_list_t);

    /* absent attribute */
    CHECK("an unset attribute is not found",
          !prte_get_attribute(&attrs, PRTE_JOB_ROOM_NUM, NULL, PMIX_INT));

    /* int */
    CHECK("set an int", PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_ROOM_NUM,
                                                           PRTE_ATTR_GLOBAL, &ival, PMIX_INT));
    CHECK("get an int", prte_get_attribute(&attrs, PRTE_JOB_ROOM_NUM, (void **) &iptr, PMIX_INT));
    CHECK("int round trips", 42 == iout);
    /* reload it with a new value */
    ival = 99;
    CHECK("reset an int", PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_ROOM_NUM,
                                                             PRTE_ATTR_GLOBAL, &ival, PMIX_INT));
    iptr = &iout;
    prte_get_attribute(&attrs, PRTE_JOB_ROOM_NUM, (void **) &iptr, PMIX_INT);
    CHECK("reset int round trips", 99 == iout);
    CHECK("a reset does not duplicate the entry", 1 == pmix_list_get_size(&attrs));

    /* asking for the wrong type must fail rather than reinterpret bytes */
    CHECK("a type mismatch on get is refused",
          !prte_get_attribute(&attrs, PRTE_JOB_ROOM_NUM, (void **) &bptr, PMIX_BOOL));
    CHECK("a type mismatch on set is refused",
          PRTE_ERR_TYPE_MISMATCH == prte_set_attribute(&attrs, PRTE_JOB_ROOM_NUM,
                                                       PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL));

    /* bool: presence means true, and setting false removes the entry */
    CHECK("set a bool by presence",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_DEBUG_TARGET, PRTE_ATTR_GLOBAL,
                                             NULL, PMIX_BOOL));
    bptr = &bout;
    CHECK("a present bool reads true",
          prte_get_attribute(&attrs, PRTE_JOB_DEBUG_TARGET, (void **) &bptr, PMIX_BOOL)
              && bout);
    bout = false;
    CHECK("setting a bool false removes it",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_DEBUG_TARGET, PRTE_ATTR_GLOBAL,
                                             &bout, PMIX_BOOL));
    CHECK("a removed bool is absent",
          !prte_get_attribute(&attrs, PRTE_JOB_DEBUG_TARGET, NULL, PMIX_BOOL));

    /* string - unload allocates */
    CHECK("set a string",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL,
                                             "0-3", PMIX_STRING));
    CHECK("get a string", prte_get_attribute(&attrs, PRTE_JOB_CPUSET, (void **) &sout,
                                             PMIX_STRING));
    CHECK("string round trips", NULL != sout && 0 == strcmp("0-3", sout));
    free(sout);
    sout = NULL;
    /* reloading a string must free the old one, not leak or double free */
    CHECK("reset a string",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_CPUSET, PRTE_ATTR_GLOBAL,
                                             "4-7", PMIX_STRING));
    prte_get_attribute(&attrs, PRTE_JOB_CPUSET, (void **) &sout, PMIX_STRING);
    CHECK("reset string round trips", NULL != sout && 0 == strcmp("4-7", sout));
    free(sout);
    sout = NULL;

    /*
     * envar. The reload below is the regression: the second value carries no
     * "value" string, so the old one was freed and the field left pointing
     * at it - the list destruct at the end of this function then freed it a
     * second time.
     */
    envar.envar = strdup("PRTE_TEST_VAR");
    envar.value = strdup("one");
    envar.separator = ':';
    CHECK("set an envar",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                                             &envar, PMIX_ENVAR));
    free(envar.envar);
    free(envar.value);
    CHECK("get an envar", prte_get_attribute(&attrs, PRTE_JOB_SET_ENVAR, (void **) &eout,
                                             PMIX_ENVAR));
    CHECK("envar name round trips", NULL != eout && NULL != eout->envar
                                        && 0 == strcmp("PRTE_TEST_VAR", eout->envar));
    CHECK("envar value round trips", NULL != eout && NULL != eout->value
                                         && 0 == strcmp("one", eout->value));
    CHECK("envar separator round trips", NULL != eout && ':' == eout->separator);
    if (NULL != eout) {
        PMIX_ENVAR_FREE(eout, 1);
        eout = NULL;
    }

    envar.envar = strdup("PRTE_TEST_VAR");
    envar.value = NULL; /* <-- the case that left a dangling pointer */
    envar.separator = ';';
    CHECK("reset an envar with no value",
          PRTE_SUCCESS == prte_set_attribute(&attrs, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                                             &envar, PMIX_ENVAR));
    free(envar.envar);
    CHECK("get the reset envar", prte_get_attribute(&attrs, PRTE_JOB_SET_ENVAR,
                                                    (void **) &eout, PMIX_ENVAR));
    CHECK("the value really was cleared", NULL != eout && NULL == eout->value);
    if (NULL != eout) {
        PMIX_ENVAR_FREE(eout, 1);
        eout = NULL;
    }

    /* append/fetch: more than one attribute may share a key, and
     * prte_fetch_attribute walks them */
    CHECK("append a second envar",
          PRTE_SUCCESS == prte_append_attribute(&attrs, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                                                NULL, PMIX_ENVAR));
    kv = prte_fetch_attribute(&attrs, NULL, PRTE_JOB_SET_ENVAR);
    CHECK("fetch finds the first", NULL != kv);
    kv = prte_fetch_attribute(&attrs, kv, PRTE_JOB_SET_ENVAR);
    CHECK("fetch finds the second", NULL != kv);
    kv = prte_fetch_attribute(&attrs, kv, PRTE_JOB_SET_ENVAR);
    CHECK("fetch stops at the end", NULL == kv);
    CHECK("fetch of an absent key finds nothing",
          NULL == prte_fetch_attribute(&attrs, NULL, PRTE_JOB_TIMEOUT));

    /* remove */
    prte_remove_attribute(&attrs, PRTE_JOB_CPUSET);
    CHECK("a removed attribute is gone",
          !prte_get_attribute(&attrs, PRTE_JOB_CPUSET, NULL, PMIX_STRING));

    /* this releases every attribute above - a dangling envar pointer shows
     * up here as a double free */
    PMIX_LIST_DESTRUCT(&attrs);

    return failures;
}

/* ------------------------------------------------------------------ */
/* dash_host                                                          */
/* ------------------------------------------------------------------ */

static prte_node_t *find_node(pmix_list_t *list, const char *name)
{
    prte_node_t *nd;

    PMIX_LIST_FOREACH(nd, list, prte_node_t)
    {
        if (0 == strcmp(nd->name, name)) {
            return nd;
        }
    }
    return NULL;
}

static int test_dash_host(void)
{
    int failures = 0;
    pmix_list_t nodes;
    prte_node_t *nd;
    char *spec;

    /* plain names get one slot each */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    spec = strdup("nodeA,nodeB");
    CHECK("add two plain hosts",
          PRTE_SUCCESS == prte_util_add_dash_host_nodes(&nodes, spec));
    free(spec);
    CHECK("two nodes were added", 2 == pmix_list_get_size(&nodes));
    nd = find_node(&nodes, "nodeA");
    CHECK("nodeA present with one slot", NULL != nd && 1 == nd->slots);
    nd = find_node(&nodes, "nodeB");
    CHECK("nodeB present with one slot", NULL != nd && 1 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /* a repeated name bumps the slot count instead of duplicating */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    spec = strdup("nodeA,nodeA,nodeA");
    CHECK("add a repeated host", PRTE_SUCCESS == prte_util_add_dash_host_nodes(&nodes, spec));
    free(spec);
    CHECK("a repeat does not duplicate the node", 1 == pmix_list_get_size(&nodes));
    nd = find_node(&nodes, "nodeA");
    CHECK("a repeat accumulates slots", NULL != nd && 3 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /* an explicit slot count */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    spec = strdup("nodeA:4,nodeB:2");
    CHECK("add hosts with slot counts",
          PRTE_SUCCESS == prte_util_add_dash_host_nodes(&nodes, spec));
    free(spec);
    nd = find_node(&nodes, "nodeA");
    CHECK("nodeA got its 4 slots", NULL != nd && 4 == nd->slots);
    CHECK("nodeA is marked slots-given",
          NULL != nd && PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_SLOTS_GIVEN));
    nd = find_node(&nodes, "nodeB");
    CHECK("nodeB got its 2 slots", NULL != nd && 2 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /*
     * The regression: an auto-detect marker on the FIRST token used to leak
     * into every token after it, because "slots" was not reset per token.
     * nodeB names no count, so it must come out with one slot - not with the
     * zero that "-1 means auto-detect" produced.
     */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    spec = strdup("nodeA:*,nodeB");
    CHECK("add an auto-detect host followed by a plain one",
          PRTE_SUCCESS == prte_util_add_dash_host_nodes(&nodes, spec));
    free(spec);
    nd = find_node(&nodes, "nodeA");
    CHECK("the auto-detect node defers its slot count",
          NULL != nd && !PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_SLOTS_GIVEN));
    nd = find_node(&nodes, "nodeB");
    CHECK("the following plain host still gets one slot", NULL != nd && 1 == nd->slots);
    CHECK("the following plain host is marked slots-given",
          NULL != nd && PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_SLOTS_GIVEN));
    PMIX_LIST_DESTRUCT(&nodes);

    /*
     * ...and the same for the increment marker: "nodeA:+2" asks to add two
     * slots to whatever nodeA was discovered to have, which is recorded as
     * an attribute. nodeB asked for nothing of the sort.
     */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    spec = strdup("nodeA:+2,nodeB:3");
    CHECK("add an increment host followed by an absolute one",
          PRTE_SUCCESS == prte_util_add_dash_host_nodes(&nodes, spec));
    free(spec);
    nd = find_node(&nodes, "nodeA");
    CHECK("the increment is recorded on nodeA",
          NULL != nd && prte_get_attribute(&nd->attributes, PRTE_NODE_ADD_SLOTS, NULL,
                                           PMIX_BOOL));
    nd = find_node(&nodes, "nodeB");
    CHECK("the increment does not leak onto nodeB",
          NULL != nd && !prte_get_attribute(&nd->attributes, PRTE_NODE_ADD_SLOTS, NULL,
                                            PMIX_BOOL));
    CHECK("nodeB's absolute count is intact", NULL != nd && 3 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /* compute_slots reads a spec against an already-known node */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    nd = PMIX_NEW(prte_node_t);
    nd->name = strdup("nid0015");
    nd->slots = 8;
    nd->slots_inuse = 2;
    pmix_list_append(&nodes, &nd->super);
    spec = strdup("nid0015:4");
    CHECK("an explicit count is what compute_slots reports",
          4 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    spec = strdup("nid0015:*");
    CHECK("an auto-detect count reports what is free",
          6 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    spec = strdup("nid0015");
    CHECK("a bare name counts as one slot",
          1 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    /* a bare launch id selects the node whose name ends in that number */
    spec = strdup("15:3");
    CHECK("a launch id selects the node",
          3 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    spec = strdup("16:3");
    CHECK("a launch id that does not match selects nothing",
          0 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    spec = strdup("othernode:3");
    CHECK("a name that does not match selects nothing",
          0 == prte_util_dash_host_compute_slots(nd, spec));
    free(spec);
    PMIX_LIST_DESTRUCT(&nodes);

    return failures;
}

/* ------------------------------------------------------------------ */
/* hostfile                                                           */
/* ------------------------------------------------------------------ */

static char *write_hostfile(const char *body, char *path, size_t pathlen)
{
    FILE *fp;

    snprintf(path, pathlen, "prte_test_hostfile_%lu_%p.txt",
             (unsigned long) getpid(), (void *) body);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return NULL;
    }
    fputs(body, fp);
    fclose(fp);
    return path;
}

static int test_hostfile(void)
{
    int failures = 0;
    pmix_list_t nodes;
    prte_node_t *nd;
    char path[256], badpath[256];
    int rc;

    /* the ordinary case: names, slot counts and comments */
    if (NULL == write_hostfile("# a comment\n"
                               "hostA slots=4\n"
                               "hostB slots=2 max_slots=8\n"
                               "\n"
                               "hostC\n",
                               path, sizeof(path))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        return 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, path);
    CHECK("a plain hostfile parses", PRTE_SUCCESS == rc);
    CHECK("three hosts were read", 3 == pmix_list_get_size(&nodes));
    nd = find_node(&nodes, "hostA");
    CHECK("hostA slots", NULL != nd && 4 == nd->slots);
    CHECK("hostA is slots-given",
          NULL != nd && PRTE_FLAG_TEST(nd, PRTE_NODE_FLAG_SLOTS_GIVEN));
    nd = find_node(&nodes, "hostB");
    CHECK("hostB slots", NULL != nd && 2 == nd->slots);
    CHECK("hostB max_slots", NULL != nd && 8 == nd->slots_max);
    nd = find_node(&nodes, "hostC");
    CHECK("a host with no count gets one slot", NULL != nd && 1 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);

    /*
     * The exclusion pass. get_ordered_host_list keeps duplicates, so a name
     * that appears twice and is then excluded exercised the loop that walked
     * on through an item it had already released.
     */
    if (NULL == write_hostfile("hostA\n"
                               "hostB\n"
                               "hostA\n"
                               "hostC\n"
                               "^hostA\n",
                               badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_get_ordered_host_list(&nodes, badpath);
    CHECK("an ordered list with an exclusion parses", PRTE_SUCCESS == rc);
    CHECK("both copies of the excluded host are gone", NULL == find_node(&nodes, "hostA"));
    CHECK("the other hosts survived", NULL != find_node(&nodes, "hostB")
                                          && NULL != find_node(&nodes, "hostC"));
    CHECK("exactly the survivors remain", 2 == pmix_list_get_size(&nodes));
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    /* a hostfile that does not exist is an error, not a silent no-op */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, "prte-no-such-hostfile-anywhere");
    CHECK("a missing hostfile is refused", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);

    /*
     * ...and so is a path that exists but is not a regular file. This is not
     * pedantry: fopen() opens a directory quite happily, every read from the
     * result fails with EISDIR, and the lexer does not tell that apart from
     * "no input yet" - so it spun, and "prun --hostfile /tmp" hung the tool
     * forever instead of reporting a typo. The refusal has to happen before
     * the file is opened, which is what this pins.
     */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, ".");
    CHECK("a directory is refused, not parsed", PRTE_SUCCESS != rc);
    CHECK("and contributed no nodes", 0 == pmix_list_get_size(&nodes));
    PMIX_LIST_DESTRUCT(&nodes);

    /*
     * A syntax error has to be refused - and, because the parser is a
     * process-global flex scanner reading a process-global FILE*, it also has
     * to leave nothing behind: the good hostfile parsed immediately
     * afterwards must come out right. The error path used to skip both the
     * fclose() and the lex_destroy() (a descriptor leaked per failed parse,
     * and the flex buffer stayed live), which this pins down by requiring
     * back-to-back parses to be independent. The leak itself is not
     * observable from here; the ordering this asserts is.
     */
    if (NULL == write_hostfile("hostA slots=notanumber\n", badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, badpath);
    CHECK("a malformed slot count is refused", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);

    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, path);
    CHECK("a good hostfile still parses after a failed one", PRTE_SUCCESS == rc);
    CHECK("and reads the same three hosts", 3 == pmix_list_get_size(&nodes));
    nd = find_node(&nodes, "hostA");
    CHECK("with the same slot count", NULL != nd && 4 == nd->slots);
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    /*
     * "user@host" names a host and the account to reach it with. Anything
     * carrying a second "@" is neither a hostname nor a "user@hostname",
     * and it has to be refused - by name, file and line, like every other
     * parse failure here. It used to print a bare "WARNING: Unhandled
     * user@host-combination" through pmix_output and abandon the rest of
     * the file, naming neither the file nor the line, which is the least
     * that was said about any failure and the one a user is most likely to
     * reach by typo. Both token classes that can carry a name have to
     * refuse it, so check the plain entry and the "rank N=<host>" form.
     */
    if (NULL == write_hostfile("hostA\n"
                               "someone@hostB\n",
                               badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, badpath);
    CHECK("a user@host entry parses", PRTE_SUCCESS == rc);
    nd = find_node(&nodes, "hostB");
    CHECK("and the host is the part after the @", NULL != nd);
    if (NULL != nd) {
        char *uname = NULL;
        CHECK("and the user is the part before it",
              prte_get_attribute(&nd->attributes, PRTE_NODE_USERNAME, (void **) &uname,
                                 PMIX_STRING)
                  && NULL != uname && 0 == strcmp("someone", uname));
        if (NULL != uname) {
            free(uname);
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    if (NULL == write_hostfile("hostA\n"
                               "someone@else@hostB\n"
                               "hostC\n",
                               badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, badpath);
    CHECK("a second @ in a host entry is refused", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    if (NULL == write_hostfile("rank 0=someone@else@hostB\n",
                               badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, badpath);
    CHECK("a second @ in a rank entry is refused", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    /*
     * A "rank N" that never reaches its "=" runs the lexer to the end of the
     * file looking for one, and reported nothing at all when it got there -
     * the user was handed an error logged against a line of our own source.
     */
    if (NULL == write_hostfile("rank 0\n", badpath, sizeof(badpath))) {
        fprintf(stderr, "FAIL [hostfile]: could not write a temp hostfile\n");
        unlink(path);
        return failures + 1;
    }
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, badpath);
    CHECK("a rank entry with no host is refused", PRTE_SUCCESS != rc);
    PMIX_LIST_DESTRUCT(&nodes);
    unlink(badpath);

    /* and the file the parser gave up on left nothing behind for the next */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, path);
    CHECK("a good hostfile still parses after a refused @", PRTE_SUCCESS == rc);
    CHECK("and reads its three hosts", 3 == pmix_list_get_size(&nodes));
    PMIX_LIST_DESTRUCT(&nodes);

    /* filtering an allocation down to what a hostfile names */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, path);
    if (PRTE_SUCCESS == rc) {
        char filterpath[256];

        if (NULL != write_hostfile("hostB\n", filterpath, sizeof(filterpath))) {
            rc = prte_util_filter_hostfile_nodes(&nodes, filterpath, true);
            CHECK("filtering succeeds", PRTE_SUCCESS == rc);
            CHECK("only the named host is left", 1 == pmix_list_get_size(&nodes));
            CHECK("and it is the right one", NULL != find_node(&nodes, "hostB"));
            unlink(filterpath);
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);

    /* a hostfile naming a host the allocation does not have is refused */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);
    rc = prte_util_add_hostfile_nodes(&nodes, path);
    if (PRTE_SUCCESS == rc) {
        char filterpath[256];

        if (NULL != write_hostfile("hostZ\n", filterpath, sizeof(filterpath))) {
            rc = prte_util_filter_hostfile_nodes(&nodes, filterpath, true);
            CHECK("filtering on an unknown host is refused", PRTE_SUCCESS != rc);
            unlink(filterpath);
        }
    }
    PMIX_LIST_DESTRUCT(&nodes);

    unlink(path);
    return failures;
}

/* ------------------------------------------------------------------ */
/* sys_limits                                                         */
/* ------------------------------------------------------------------ */

/*
 * prte_util_init_sys_limits() reads the prte_set_max_sys_limits global. The
 * cases worth checking are the ones it must REFUSE, since obeying them
 * lowers a limit: a non-numeric value used to reach setrlimit() as zero, so
 * "--set-max-sys-limits openfiles:many" left the daemon unable to open a
 * file. Nothing below raises or lowers an actual limit.
 */
static int test_sys_limits(void)
{
    int failures = 0;
    char *errmsg = NULL;
    char *saved = prte_set_max_sys_limits;
    int rc;

    prte_set_max_sys_limits = NULL;
    CHECK("no limits requested is a no-op", PRTE_SUCCESS == prte_util_init_sys_limits(&errmsg));
    CHECK("...and says nothing", NULL == errmsg);

    prte_set_max_sys_limits = "openfiles:many";
    errmsg = NULL;
    rc = prte_util_init_sys_limits(&errmsg);
    CHECK("a non-numeric limit is refused", PRTE_SUCCESS != rc);
    CHECK("...and explains why", NULL != errmsg);
    free(errmsg);

    prte_set_max_sys_limits = "openfiles:-4";
    errmsg = NULL;
    rc = prte_util_init_sys_limits(&errmsg);
    CHECK("a negative limit is refused", PRTE_SUCCESS != rc);
    free(errmsg);

    prte_set_max_sys_limits = "nosuchresource:4";
    errmsg = NULL;
    rc = prte_util_init_sys_limits(&errmsg);
    CHECK("an unknown resource is refused", PRTE_SUCCESS != rc);
    CHECK("...and explains why", NULL != errmsg);
    free(errmsg);

    prte_set_max_sys_limits = "0";
    errmsg = NULL;
    CHECK("an explicit \"0\" asks for nothing and succeeds",
          PRTE_SUCCESS == prte_util_init_sys_limits(&errmsg));
    free(errmsg);

    prte_set_max_sys_limits = saved;

    CHECK("the page size is sane",
          0 < prte_getpagesize() && 0 == (prte_getpagesize() & (prte_getpagesize() - 1)));

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

    /* the hostfile and dash-host parsers ask whether each name they read is
     * one of ours, and the relative-syntax paths index the node pool */
    prte_node_pool = PMIX_NEW(pmix_pointer_array_t);
    pmix_pointer_array_init(prte_node_pool, 8, INT_MAX, 8);

    failures += test_compare_name_fields();
    failures += test_name_printing();
    failures += test_error_strings();
    failures += test_error_bound();
    failures += test_state_strings();
    failures += test_attr_key_names();
    failures += test_attr_round_trip();
    failures += test_dash_host();
    failures += test_hostfile();
    failures += test_sys_limits();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all util unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d util unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
