/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the state framework.
 *
 * Almost every consequential thing PRRTE does is sequenced by this
 * framework: a subsystem activates a state, and the state machine fires the
 * callback registered for it on the progress thread.  The handlers
 * themselves (launch, teardown, notify) need a live DVM and are covered by
 * the integration/dockerswarm harnesses.  What *is* testable in isolation is
 * the machinery underneath them, and that machinery is where the subtle
 * defects live:
 *
 *  - the state->callback tables (add/set/remove and their return protocol);
 *  - the dispatcher, including the ERROR/ANY fallback rules that decide
 *    which handler an unregistered state lands on;
 *  - the caddy handed to every handler, which must arrive fully
 *    initialized.  PMIX_NEW does not zero its allocation, and
 *    activate_job_state used to fill in caddy->job_state only when a job
 *    accompanied the activation.  Since 80-odd call sites activate an error
 *    state with a NULL job -- PRTE_ACTIVATE_JOB_STATE(NULL,
 *    PRTE_JOB_STATE_COMM_FAILED) and friends -- and those land on the
 *    errmgr's ERROR handler, which does "jdata->state = caddy->job_state",
 *    the daemon job's state was being set from uninitialized heap.  The
 *    caddy tests below pin that down;
 *  - prte_state_base_set_runtime_options, whose directive walk reused its
 *    loop index as a scratch variable and so ran off the end of the option
 *    array after a "timeout=" directive.
 *
 * The tests drive the base functions directly rather than through the
 * PRTE_ACTIVATE_* macros so that no component has to win selection first;
 * the macros add only a verbose trace on top of these same entry points.
 */

#include "prte_config.h"
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "src/event/event-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* what the last handler to run observed, so a test can assert on the caddy
 * the dispatcher actually built */
static int seen_calls = 0;
static prte_job_state_t seen_job_state;
static prte_proc_state_t seen_proc_state;
static prte_job_t *seen_jdata;
static pmix_proc_t seen_name;

static void reset_observations(void)
{
    seen_calls = 0;
    seen_job_state = PRTE_JOB_STATE_UNDEF;
    seen_proc_state = PRTE_PROC_STATE_UNDEF;
    seen_jdata = NULL;
    memset(&seen_name, 0, sizeof(seen_name));
}

static void recorder(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    ++seen_calls;
    seen_job_state = caddy->job_state;
    seen_proc_state = caddy->proc_state;
    seen_jdata = caddy->jdata;
    seen_name = caddy->name;
    PMIX_RELEASE(caddy);
}

/* a second, distinguishable handler for the "set replaces the callback" case */
static int other_calls = 0;
static void other_recorder(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(caddy);
    ++other_calls;
    PMIX_RELEASE(caddy);
}

/* Activation only queues an event; nothing has run when it returns.  Drain
 * the base so the handler above has actually executed before we assert. */
static void drain(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
    }
}

static void fresh_machines(void)
{
    PMIX_CONSTRUCT(&prte_job_states, pmix_list_t);
    PMIX_CONSTRUCT(&prte_proc_states, pmix_list_t);
}

static void drop_machines(void)
{
    PMIX_LIST_DESTRUCT(&prte_job_states);
    PMIX_LIST_DESTRUCT(&prte_proc_states);
}

/*
 * The table API's return protocol.  add_* enforces uniqueness; remove_*
 * reports a state it never held; and the two set_* variants deliberately
 * differ -- the job one appends when the state is absent (so it doubles as
 * "add if missing"), the proc one refuses.  That asymmetry is easy to
 * "tidy up" by accident, and callers rely on it, so pin it down.
 */
static int test_table_api(void)
{
    int failures = 0;

    fresh_machines();

    CHECK("add job state", PRTE_SUCCESS == prte_state_base_add_job_state(PRTE_JOB_STATE_INIT,
                                                                        recorder));
    CHECK("add job dup rejected",
          PRTE_ERR_BAD_PARAM == prte_state_base_add_job_state(PRTE_JOB_STATE_INIT, recorder));
    CHECK("one job entry", 1 == pmix_list_get_size(&prte_job_states));

    /* set on a known state replaces the callback in place */
    CHECK("set job existing",
          PRTE_SUCCESS == prte_state_base_set_job_state_callback(PRTE_JOB_STATE_INIT,
                                                                 other_recorder));
    CHECK("set did not append", 1 == pmix_list_get_size(&prte_job_states));

    /* set on an unknown state appends it */
    CHECK("set job absent",
          PRTE_SUCCESS == prte_state_base_set_job_state_callback(PRTE_JOB_STATE_MAP, recorder));
    CHECK("set appended", 2 == pmix_list_get_size(&prte_job_states));

    CHECK("remove job state", PRTE_SUCCESS == prte_state_base_remove_job_state(PRTE_JOB_STATE_MAP));
    CHECK("remove job again",
          PRTE_ERR_NOT_FOUND == prte_state_base_remove_job_state(PRTE_JOB_STATE_MAP));

    /* the proc machine: same add/remove protocol ... */
    CHECK("add proc state",
          PRTE_SUCCESS == prte_state_base_add_proc_state(PRTE_PROC_STATE_RUNNING, recorder));
    CHECK("add proc dup rejected",
          PRTE_ERR_BAD_PARAM == prte_state_base_add_proc_state(PRTE_PROC_STATE_RUNNING, recorder));
    CHECK("remove proc state",
          PRTE_SUCCESS == prte_state_base_remove_proc_state(PRTE_PROC_STATE_RUNNING));
    CHECK("remove proc again",
          PRTE_ERR_NOT_FOUND == prte_state_base_remove_proc_state(PRTE_PROC_STATE_RUNNING));

    /* ... but set_ on an absent proc state refuses rather than appending */
    CHECK("set proc absent refuses",
          PRTE_ERR_NOT_FOUND == prte_state_base_set_proc_state_callback(PRTE_PROC_STATE_RUNNING,
                                                                        recorder));
    CHECK("nothing appended", 0 == pmix_list_get_size(&prte_proc_states));

    drop_machines();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_table_api\n");
    }
    return failures;
}

/*
 * Dispatch: an exactly-registered state runs its own callback; an
 * unregistered one routes to ERROR when it sorts above PRTE_JOB_STATE_ERROR
 * and to ANY otherwise; with neither fallback present it is dropped
 * silently.  Registering an ERROR or ANY catch-all therefore changes the
 * behavior of every state you did NOT register, which is exactly why this
 * needs a regression test.
 */
static int test_job_dispatch(void)
{
    int failures = 0;

    fresh_machines();
    prte_state_base_add_job_state(PRTE_JOB_STATE_INIT, recorder);

    /* exact match */
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_INIT);
    drain();
    CHECK("exact match fired", 1 == seen_calls);
    CHECK("exact match state", PRTE_JOB_STATE_INIT == seen_job_state);

    /* unregistered, no fallback -> dropped */
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_MAP);
    drain();
    CHECK("no fallback drops", 0 == seen_calls);

    /* an ANY catch-all now takes the non-error state ... */
    prte_state_base_add_job_state(PRTE_JOB_STATE_ANY, recorder);
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_MAP);
    drain();
    CHECK("ANY fallback fired", 1 == seen_calls);
    CHECK("ANY reports real state", PRTE_JOB_STATE_MAP == seen_job_state);

    /* ... and an error state still prefers the ERROR entry over ANY */
    other_calls = 0;
    prte_state_base_add_job_state(PRTE_JOB_STATE_ERROR, other_recorder);
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_COMM_FAILED);
    drain();
    CHECK("ERROR fallback preferred", 1 == other_calls);
    CHECK("ANY not used for error state", 0 == seen_calls);

    /* a registered state whose callback is NULL is ignored, not crashed on */
    prte_state_base_set_job_state_callback(PRTE_JOB_STATE_INIT, NULL);
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_INIT);
    drain();
    CHECK("NULL cbfunc ignored", 0 == seen_calls);

    drop_machines();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_job_dispatch\n");
    }
    return failures;
}

/*
 * The proc machine is the mirror image, keyed on PRTE_PROC_STATE_ANY /
 * _ERROR, and it copies the proc NAME into the caddy by value (the machine
 * has to be usable where no prte_proc_t exists).  A NULL name has nothing
 * to dispatch on and must be refused rather than dereferenced.
 */
static int test_proc_dispatch(void)
{
    int failures = 0;
    pmix_proc_t p;

    fresh_machines();
    prte_state_base_add_proc_state(PRTE_PROC_STATE_RUNNING, recorder);
    PMIX_LOAD_PROCID(&p, "unit-test-nspace", 7);

    reset_observations();
    prte_state_base_activate_proc_state(&p, PRTE_PROC_STATE_RUNNING);
    drain();
    CHECK("proc exact fired", 1 == seen_calls);
    CHECK("proc state carried", PRTE_PROC_STATE_RUNNING == seen_proc_state);
    CHECK("proc name carried by value", PMIX_CHECK_PROCID(&p, &seen_name));
    CHECK("no job on a proc activation", NULL == seen_jdata);

    /* unregistered with no fallback -> dropped */
    reset_observations();
    prte_state_base_activate_proc_state(&p, PRTE_PROC_STATE_REGISTERED);
    drain();
    CHECK("proc no fallback drops", 0 == seen_calls);

    /* ERROR catch-all takes an error state */
    other_calls = 0;
    prte_state_base_add_proc_state(PRTE_PROC_STATE_ERROR, other_recorder);
    reset_observations();
    prte_state_base_activate_proc_state(&p, PRTE_PROC_STATE_COMM_FAILED);
    drain();
    CHECK("proc ERROR fallback fired", 1 == other_calls);

    /* a NULL name must be refused, not dereferenced */
    reset_observations();
    prte_state_base_activate_proc_state(NULL, PRTE_PROC_STATE_RUNNING);
    drain();
    CHECK("NULL proc refused", 0 == seen_calls);

    drop_machines();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_proc_dispatch\n");
    }
    return failures;
}

/*
 * The caddy contract.  PMIX_NEW mallocs without zeroing, so every field a
 * handler may read has to be set by the constructor, and the dispatcher has
 * to fill in the state it fired REGARDLESS of whether a job accompanied it.
 *
 * The second half of this test is the real regression guard: activating
 * with a NULL job used to leave caddy->job_state as whatever bytes the
 * allocator handed back.  Those activations are not exotic -- a comm
 * failure, a daemon that never launched, a forced exit all use them, and
 * all land on the errmgr's ERROR handler, which assigns caddy->job_state
 * straight into the daemon job's state field.
 */
static int test_caddy_contract(void)
{
    int failures = 0;
    prte_state_caddy_t *caddy;
    prte_job_t *jdata;

    /* a freshly constructed caddy is fully defined */
    caddy = PMIX_NEW(prte_state_caddy_t);
    CHECK("caddy jdata NULL", NULL == caddy->jdata);
    CHECK("caddy job_state defined", PRTE_JOB_STATE_UNDEF == caddy->job_state);
    CHECK("caddy proc_state defined", PRTE_PROC_STATE_UNDEF == caddy->proc_state);
    CHECK("caddy rank defined", PMIX_RANK_INVALID == caddy->name.rank);
    PMIX_RELEASE(caddy);

    fresh_machines();
    prte_state_base_add_job_state(PRTE_JOB_STATE_FORCED_EXIT, recorder);

    /* activation WITHOUT a job still carries the state that fired */
    reset_observations();
    prte_state_base_activate_job_state(NULL, PRTE_JOB_STATE_FORCED_EXIT);
    drain();
    CHECK("NULL-job activation fired", 1 == seen_calls);
    CHECK("NULL-job carries its state", PRTE_JOB_STATE_FORCED_EXIT == seen_job_state);
    CHECK("NULL-job carries no job", NULL == seen_jdata);

    /* activation WITH a job carries both, and holds a reference across the
     * asynchronous hop so the job cannot be freed under the handler */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-job");
    reset_observations();
    prte_state_base_activate_job_state(jdata, PRTE_JOB_STATE_FORCED_EXIT);
    /* the caddy retained it: releasing our own reference here must not
     * destroy the object before the handler runs */
    PMIX_RELEASE(jdata);
    drain();
    CHECK("job activation fired", 1 == seen_calls);
    CHECK("job activation carries state", PRTE_JOB_STATE_FORCED_EXIT == seen_job_state);
    CHECK("job activation carries job", jdata == seen_jdata);

    drop_machines();

    if (0 == failures) {
        fprintf(stdout, "PASSED test_caddy_contract\n");
    }
    return failures;
}

/*
 * prte_state_base_set_runtime_options translates a PMIX_RUNTIME_OPTIONS
 * string into per-job attributes.  It walks a comma-separated list with an
 * index that two directives used to overwrite: "timeout=" assigned the
 * converted seconds to the loop variable, and "max-restarts=" reused it for
 * the app scan.  Either one made the walk resume at an arbitrary index --
 * silently dropping every later directive, and reading past the end of the
 * option array when the value exceeded the list length.
 */
static int test_runtime_options(void)
{
    int failures = 0;
    prte_job_t *jdata;
    int tmo, *tmoptr = &tmo;
    int32_t restarts, *rptr = &restarts;
    prte_app_context_t *app;
    char *spec;

    /* a directive AFTER "timeout=" must still be applied */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto");
    spec = strdup("timeout=60,recoverable=true");
    CHECK("timeout spec accepted", PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    tmo = 0;
    CHECK("timeout applied",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT, (void **) &tmoptr, PMIX_INT));
    CHECK("timeout value", 60 == tmo);
    CHECK("directive after timeout applied",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* and a directive AFTER "max-restarts=" must still be applied */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto2");
    app = PMIX_NEW(prte_app_context_t);
    app->app = strdup("hostname");
    pmix_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;
    spec = strdup("max-restarts=3,continuous=true");
    CHECK("max-restarts spec accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    restarts = 0;
    CHECK("max-restarts applied",
          prte_get_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, (void **) &rptr, PMIX_INT32));
    CHECK("max-restarts value", 3 == restarts);
    CHECK("directive after max-restarts applied",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* a bare directive carries no '=' and means "true" */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto3");
    spec = strdup("recoverable");
    CHECK("bare directive accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("bare directive means true",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* An explicit "=false" must leave the attribute ABSENT.  Every consumer
     * of a boolean runtime option tests it by presence, so storing a false
     * value under the key would read back as "enabled" everywhere - i.e.
     * "recoverable=false" would turn recovery ON. */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto4");
    spec = strdup("recoverable=false");
    CHECK("false directive accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("false directive leaves it unset",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* "aggregate-help" drives PRTE_JOB_NOAGG_HELP, which is its NEGATION:
     * asking to aggregate must not record "do not aggregate". */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto7");
    spec = strdup("aggregate-help=true");
    CHECK("aggregate-help accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("aggregate-help=true does not set NOAGG",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_NOAGG_HELP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto8");
    spec = strdup("aggregate-help=false");
    CHECK("no-aggregate-help accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("aggregate-help=false sets NOAGG",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_NOAGG_HELP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* The matcher accepts any unambiguous PREFIX of an option's name, so a
     * directive LONGER than the name matches nothing.  The name must
     * therefore be the full spelling the help text documents, or the
     * documented spelling is rejected. */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto9");
    spec = strdup("aggregate-help-messages=false");
    CHECK("the documented directive spelling is accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("and means the same thing as the short form",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_NOAGG_HELP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* "report-child-jobs-separately" was listed in the runtime-options help
     * text and accepted by schizo's validator, but had no branch in the
     * parser at all - so the documented directive was refused as unknown. */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rcs");
    spec = strdup("report-child-jobs-separately");
    CHECK("report-child-jobs-separately accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("report-child-jobs-separately recorded",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* and "=false" must leave it off, like every other boolean directive */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rcs2");
    spec = strdup("report-child-jobs-separately=false");
    CHECK("report-child-jobs-separately=false accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("report-child-jobs-separately=false leaves it unset",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);

    /* the defaults path seeds it from the MCA param */
    prte_report_child_jobs_separately = true;
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rcs3");
    CHECK("defaults accepted", PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, NULL));
    CHECK("the MCA param seeds the attribute",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP, NULL, PMIX_BOOL));
    PMIX_RELEASE(jdata);
    prte_report_child_jobs_separately = false;

    /* an unknown directive is refused (silently - the show_help was already
     * emitted) rather than ignored */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto5");
    spec = strdup("no-such-runtime-option");
    CHECK("unknown directive refused",
          PRTE_ERR_SILENT == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    PMIX_RELEASE(jdata);

    /* notify-errors without recoverable/continuous can never deliver a
     * notification - the combination has to be caught, not silently honored */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto6");
    spec = strdup("notifyerrors=true");
    CHECK("bad combination refused",
          PRTE_ERR_SILENT == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    PMIX_RELEASE(jdata);

    /* A boolean directive given a value that is neither true nor false is
     * refused.  PMIX_CHECK_TRUE reports anything it does not recognize as
     * FALSE, so this used to mean "donotlaunch=maybe" quietly launched. */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rto7");
    spec = strdup("donotlaunch=maybe");
    CHECK("non-boolean value refused",
          PRTE_ERR_SILENT == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    /* ...but the directives that carry a value of their own are untouched
     * by that check - "60" is a length of time, not a truth */
    spec = strdup("timeout=60,exec-agent=/bin/true,output-proctable=-");
    CHECK("valued directives unaffected",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_runtime_options\n");
    }
    return failures;
}

/*
 * prte_state_base_report_child_sep() reads the same directive the walk above
 * records, but for a caller with no job object: a launcher driving a
 * persistent DVM, deciding whether a child job's exit status becomes its own.
 *
 * The one thing that matters about it is that it AGREES with the walk.  Two
 * readers of one directive that disagree would make "prun --runtime-options
 * report-child-jobs-separately" and "prterun ..." return different exit
 * statuses for the same run, which is the failure the directive exists to
 * prevent.  So every case below is asserted against the attribute the walk
 * leaves behind, not against a hand-written expectation.
 */
static int test_report_child_sep_reader(void)
{
    int failures = 0;
    prte_job_t *jdata;
    char *spec;
    bool viawalk;
    size_t n;
    /* the spellings a command line can carry, including one that names the
     * directive alongside others and one that does not name it at all */
    const char *specs[] = {
        "report-child-jobs-separately",
        "report-child-jobs-separately=true",
        "report-child-jobs-separately=false",
        "recoverable,report-child-jobs-separately,notifyerrors",
        "recoverable,report-child-jobs-separately=false",
        "timeout=60",
        NULL
    };

    for (n = 0; NULL != specs[n]; n++) {
        jdata = PMIX_NEW(prte_job_t);
        PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-rcsr");
        /* set_runtime_options edits the string it is given, so each reader
         * gets its own copy */
        spec = strdup(specs[n]);
        CHECK(specs[n], PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
        free(spec);
        viawalk = prte_get_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP,
                                     NULL, PMIX_BOOL);
        spec = strdup(specs[n]);
        CHECK(specs[n], viawalk == prte_state_base_report_child_sep(spec));
        free(spec);
        PMIX_RELEASE(jdata);
    }

    /* a caller with nothing to read is not asking for it */
    CHECK("no spec reads as false", !prte_state_base_report_child_sep(NULL));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_report_child_sep_reader\n");
    }
    return failures;
}

/*
 * "stop-in-app" is the one runtime option whose value is not a truth value.
 * Written bare or with a boolean it asserts "stop wherever the application
 * chooses"; written with anything else, that text is the string ID of the
 * breakpoint the application is to stop at, which the daemons hand to the
 * process in the PMIX_BREAKPOINT envar.  Two things can go wrong quietly
 * here: the strict-boolean refusal that guards every other directive would
 * reject a breakpoint name outright, and a name left behind by an earlier
 * directive would send the job to a breakpoint nobody asked for.
 */
static int test_stop_in_app(void)
{
    int failures = 0;
    prte_job_t *jdata;
    char *spec, *bkpt;

    /* bare: stop somewhere, no particular place */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-sia1");
    spec = strdup("stop-in-app");
    CHECK("bare directive accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("bare sets stop-in-app",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, NULL, PMIX_BOOL));
    CHECK("bare names no breakpoint",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT, NULL, PMIX_STRING));

    /* a name: not a truth value, so it must not be refused as one, and the
     * directive after it must still be applied */
    spec = strdup("stop-in-app=mpi-init,recoverable=true");
    CHECK("named breakpoint accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("name sets stop-in-app",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, NULL, PMIX_BOOL));
    bkpt = NULL;
    CHECK("name recorded",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT, (void **) &bkpt,
                             PMIX_STRING));
    CHECK("name value", NULL != bkpt && 0 == strcmp(bkpt, "mpi-init"));
    if (NULL != bkpt) {
        free(bkpt);
    }
    CHECK("directive after the name applied",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, NULL, PMIX_BOOL));

    /* asserting the boolean afterwards drops the name: the job is no longer
     * to stop at that one place */
    spec = strdup("stop-in-app=true");
    CHECK("truth value accepted",
          PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("true keeps stop-in-app",
          prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, NULL, PMIX_BOOL));
    CHECK("true clears the name",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT, NULL, PMIX_STRING));

    /* and turning it off clears both */
    spec = strdup("stop-in-app=mpi-init");
    CHECK("name re-applied", PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    spec = strdup("stop-in-app=false");
    CHECK("false accepted", PRTE_SUCCESS == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    CHECK("false clears stop-in-app",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, NULL, PMIX_BOOL));
    CHECK("false clears the name",
          !prte_get_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT, NULL, PMIX_STRING));
    PMIX_RELEASE(jdata);

    /* the exemption is confined to this directive - its neighbors still
     * refuse a value that is neither true nor false */
    jdata = PMIX_NEW(prte_job_t);
    PMIX_LOAD_NSPACE(jdata->nspace, "unit-test-sia2");
    spec = strdup("stop-in-init=mpi-init");
    CHECK("stop-in-init still refuses a non-boolean",
          PRTE_ERR_SILENT == prte_state_base_set_runtime_options(jdata, spec));
    free(spec);
    PMIX_RELEASE(jdata);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_stop_in_app\n");
    }
    return failures;
}

/*
 * Component selection is role-driven: dvm answers only for the DVM master,
 * prted only for a daemon, both at priority 100.  Exactly one applies to
 * any given process, which is what makes a "pick one" framework safe here.
 */
static prte_state_base_module_t *state_module(const char *name, prte_proc_type_t as, int *pri)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    prte_proc_type_t save;

    *pri = 0;
    PMIX_LIST_FOREACH(cli, &prte_state_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 != strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL == cli->cli_component->pmix_mca_query_component) {
            return NULL;
        }
        save = prte_process_info.proc_type;
        prte_process_info.proc_type = as;
        if (PRTE_SUCCESS != cli->cli_component->pmix_mca_query_component(&mod, pri)) {
            mod = NULL;
        }
        prte_process_info.proc_type = save;
        return (prte_state_base_module_t *) mod;
    }
    return NULL;
}

static int test_component_selection(void)
{
    int failures = 0, pri = 0;
    prte_state_base_module_t *m;

    m = state_module("dvm", PRTE_PROC_MASTER, &pri);
    if (NULL == m) {
        fprintf(stdout, "  (skipping dvm module checks: component absent)\n");
    } else {
        CHECK("dvm priority", 100 == pri);
        CHECK("dvm init", NULL != m->init);
        CHECK("dvm finalize", NULL != m->finalize);
        /* every component delegates the ten API slots to the base */
        CHECK("dvm activate_job", prte_state_base_activate_job_state == m->activate_job_state);
        CHECK("dvm activate_proc", prte_state_base_activate_proc_state == m->activate_proc_state);
        CHECK("dvm add_job", prte_state_base_add_job_state == m->add_job_state);
        CHECK("dvm add_proc", prte_state_base_add_proc_state == m->add_proc_state);
        CHECK("dvm set_job", prte_state_base_set_job_state_callback == m->set_job_state_callback);
        CHECK("dvm set_proc", prte_state_base_set_proc_state_callback == m->set_proc_state_callback);
        CHECK("dvm remove_job", prte_state_base_remove_job_state == m->remove_job_state);
        CHECK("dvm remove_proc", prte_state_base_remove_proc_state == m->remove_proc_state);
        /* and must NOT answer for a daemon */
        CHECK("dvm declines daemon", NULL == state_module("dvm", PRTE_PROC_DAEMON, &pri));
    }

    m = state_module("prted", PRTE_PROC_DAEMON, &pri);
    if (NULL == m) {
        fprintf(stdout, "  (skipping prted module checks: component absent)\n");
    } else {
        CHECK("prted priority", 100 == pri);
        CHECK("prted init", NULL != m->init);
        CHECK("prted finalize", NULL != m->finalize);
        CHECK("prted activate_job", prte_state_base_activate_job_state == m->activate_job_state);
        CHECK("prted activate_proc", prte_state_base_activate_proc_state == m->activate_proc_state);
        /* and must NOT answer for the master */
        CHECK("prted declines master", NULL == state_module("prted", PRTE_PROC_MASTER, &pri));
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_component_selection\n");
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
    /* activation thread-shifts onto the event base, so we need one */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_event_base_open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }
    rc = pmix_mca_base_framework_open(&prte_state_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "state framework open failed: %d\n", rc);
        prte_event_base_close();
        prte_finalize();
        return 1;
    }

    failures += test_table_api();
    failures += test_job_dispatch();
    failures += test_proc_dispatch();
    failures += test_caddy_contract();
    failures += test_runtime_options();
    failures += test_report_child_sep_reader();
    failures += test_stop_in_app();
    failures += test_component_selection();

    (void) pmix_mca_base_framework_close(&prte_state_base_framework);
    prte_event_base_close();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all state unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d state unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
