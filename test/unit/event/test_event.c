/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for src/event -- PRRTE's thin wrapper over libevent/libev.
 *
 * This directory is two files and almost all macros, which is exactly why
 * it had no tests and why it accumulated the defects this review found:
 * a macro nobody expands is never compiled, and a function nobody calls is
 * never linked. Concretely:
 *
 *  - prte_event_new() was declared in event-internal.h and defined nowhere,
 *    and prte_event_evtimer_new() was a macro on top of it. The first use of
 *    either would have been an undefined-symbol link failure, not a compile
 *    error, so it could sit in the header indefinitely.
 *
 *  - prte_event_base_init_common_timeout was written with a space between
 *    the macro name and its parameter list, which makes it an *object-like*
 *    macro expanding to "(b, t) event_base_init_common_timeout((b), (t))".
 *    Any use of it was a syntax error.
 *
 *  - prte_event_base_close() freed prte_sync_event_base and left both it and
 *    prte_event_base -- which is an alias for the same base, not a second
 *    one -- pointing at freed memory. Nothing called it, so nothing noticed;
 *    prte_finalize() now does, which is what makes test_base_lifecycle()
 *    below worth having.
 *
 *  - prte_event_assign() returned a literal 0 rather than a PRTE code and
 *    threw away the underlying library's failure return.
 *
 * The rest of the cases pin down behavior the tree relies on but never
 * states: that prte_event_alloc() hands back *zeroed* storage (an event is
 * allocated here and only assigned to a base later, and every such object's
 * destructor frees it -- on uninitialized storage that free crashes), that
 * PRTE_EVENT_SIGNAL() recovers the signal number an event was set for, and
 * that PRTE_PMIX_THREADSHIFT delivers its caddy.
 *
 * What is NOT here: anything involving a second thread. The progress-thread
 * machinery lives in src/runtime and is covered by test/unit/runtime.
 */

#include "prte_config.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* the event base                                                     */
/* ------------------------------------------------------------------ */

static int test_base_open(void)
{
    int failures = 0;

    CHECK("open succeeds", PRTE_SUCCESS == prte_event_base_open());
    CHECK("a base was created", NULL != prte_sync_event_base);

    /* This aliasing is load-bearing, not incidental: PRRTE's tools block in
     * their own loop over this base rather than running a progress thread
     * for it, so the "sync" base and the general base have to be the same
     * object. Code that posts to prte_event_base and code that loops over
     * prte_sync_event_base must meet. */
    CHECK("prte_event_base is the sync base", prte_event_base == prte_sync_event_base);

    /* every tool calls open, and prte_init calls it again underneath them */
    CHECK("open is idempotent", PRTE_SUCCESS == prte_event_base_open());
    CHECK("the base did not change", prte_event_base == prte_sync_event_base);

    return failures;
}

/* ------------------------------------------------------------------ */
/* allocation                                                         */
/* ------------------------------------------------------------------ */

static int test_alloc(void)
{
    int failures = 0;
    prte_event_t *ev;
    unsigned char zeros[sizeof(prte_event_t)];

    ev = prte_event_alloc();
    CHECK("alloc returns storage", NULL != ev);
    if (NULL == ev) {
        return failures;
    }

    /* The zeroing is the whole point of having this function rather than
     * malloc(). Callers (the odls launch-local caddy, prte_timer_t, the iof
     * read/write events) allocate here in a constructor and event_assign()
     * only when the object is activated -- but every one of those
     * destructors frees the event unconditionally. libevent's event_free()
     * calls event_del(), which dereferences the event's internal fields; on
     * raw malloc'd storage that is garbage. A zeroed event has a NULL base,
     * which event_del() rejects cleanly. */
    memset(zeros, 0, sizeof(zeros));
    CHECK("alloc returns zeroed storage", 0 == memcmp(ev, zeros, sizeof(zeros)));

    /* ...and this is that path: free an event that was never assigned to a
     * base. libevent prints "event_del_: event has no event_base set" to
     * stderr here; that warning is the expected outcome, not a failure. */
    prte_event_free(ev);

    /* the ordinary path: allocate, assign, free */
    ev = prte_event_alloc();
    if (NULL != ev) {
        CHECK("assign reports PRTE_SUCCESS",
              PRTE_SUCCESS == prte_event_assign(ev, prte_event_base, -1, 0, NULL, NULL));
        prte_event_free(ev);
    }

    return failures;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

static int fired = 0;
static void *fired_arg = NULL;

static void count_cb(int fd, short flags, void *arg)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, flags);
    fired++;
    fired_arg = arg;
}

static int test_dispatch(void)
{
    int failures = 0;
    int marker = 42;
    prte_event_t ev;
    struct timeval tv = {0, 1000};

    /* an activated non-persistent event fires exactly once and carries the
     * caller's argument through */
    fired = 0;
    fired_arg = NULL;
    CHECK("set on a live base succeeds",
          PRTE_SUCCESS == prte_event_set(prte_event_base, &ev, -1, PRTE_EV_WRITE, count_cb, &marker));
    prte_event_active(&ev, PRTE_EV_WRITE, 1);
    prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
    CHECK("an activated event fires", 1 == fired);
    CHECK("the callback gets its argument", &marker == fired_arg);

    /* and does not fire again without being reactivated */
    prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
    CHECK("a non-persistent event fires only once", 1 == fired);

    /* a timer set with the evtimer macros and added with a real timeout is
     * delivered by a blocking loop pass */
    fired = 0;
    prte_event_evtimer_set(prte_event_base, &ev, count_cb, &marker);
    prte_event_evtimer_add(&ev, &tv);
    prte_event_loop(prte_event_base, PRTE_EVLOOP_ONCE);
    CHECK("a timer fires", 1 == fired);

    /* a timer deleted before the loop runs never fires. This is what every
     * PRRTE timeout path does when the thing it was guarding completes in
     * time, so a del that did not take would fire a spurious timeout. */
    fired = 0;
    prte_event_evtimer_set(prte_event_base, &ev, count_cb, &marker);
    prte_event_evtimer_add(&ev, &tv);
    prte_event_evtimer_del(&ev);
    prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
    CHECK("a deleted timer does not fire", 0 == fired);

    return failures;
}

/* ------------------------------------------------------------------ */
/* signal events                                                      */
/* ------------------------------------------------------------------ */

static int test_signal_events(void)
{
    int failures = 0;
    prte_event_t ev;

    /* PRTE_EVENT_SIGNAL() is how ess/base and prte_wait.c recover which
     * signal an event was set for -- prted forwards that number to its
     * children, so getting it wrong forwards the wrong signal. It is also
     * one of the few places where the libev and libevent arms of
     * event-internal.h differ (a struct field vs. an accessor), which makes
     * it worth pinning down rather than assuming. */
    prte_event_signal_set(prte_event_base, &ev, SIGUSR1, count_cb, NULL);
    CHECK("PRTE_EVENT_SIGNAL recovers the signal number", SIGUSR1 == PRTE_EVENT_SIGNAL(&ev));

    /* the macro must also set the event PERSIST -- a signal handler that
     * disarmed itself after the first delivery would forward one SIGTERM
     * and ignore the next */
    CHECK("a signal event is persistent", 0 != (EV_PERSIST & ev.ev_events));
    CHECK("a signal event is a signal event", 0 != (EV_SIGNAL & ev.ev_events));

    /* add and remove it against a live base: the wrappers must both accept
     * an event whose "fd" is a signal number */
    CHECK("a signal event can be armed", 0 == prte_event_signal_add(&ev, NULL));
    CHECK("a signal event can be disarmed", 0 == prte_event_signal_del(&ev));

    /* Deliberately NOT tested here: that raising the signal runs the
     * callback. Whether a signal raised before the loop is entered is
     * reported at all is a property of the event library's backend, not of
     * PRRTE -- libevent's kqueue backend on macOS does not report it, and a
     * three-line program using event_assign/event_add/raise directly hangs
     * in event_base_loop() exactly the same way. Testing it here would be
     * testing libevent, on one platform. What PRRTE actually needs -- that
     * a signal sent to prun reaches the job's processes on other nodes --
     * is covered by contrib/dockerswarm (the test_prted and test_event
     * phases), where the signal arrives at a running process rather than at
     * a program that is about to enter its first loop pass. */

    return failures;
}

/* ------------------------------------------------------------------ */
/* the threadshift caddy                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_object_t super;
    prte_event_t ev; /* must be named "ev" -- PRTE_PMIX_THREADSHIFT says so */
    int value;
} test_caddy_t;
static PMIX_CLASS_INSTANCE(test_caddy_t, pmix_object_t, NULL, NULL);

static int shifted = 0;

static void shift_cb(int fd, short flags, void *arg)
{
    test_caddy_t *cd = (test_caddy_t *) arg;
    PRTE_HIDE_UNUSED_PARAMS(fd, flags);

    shifted = cd->value;
    PMIX_RELEASE(cd);
}

static int test_threadshift(void)
{
    int failures = 0;
    test_caddy_t *cd;

    /* PRTE_PMIX_THREADSHIFT is the single most-used macro in this header
     * (over a hundred call sites) and the only mechanism by which anything
     * arriving on the PMIx progress thread reaches PRRTE state. It has to
     * hand the handler the caddy itself, not a copy and not the base. */
    cd = PMIX_NEW(test_caddy_t);
    cd->value = 7;
    shifted = 0;
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, shift_cb);
    CHECK("the shift has not run yet", 0 == shifted);
    prte_event_loop(prte_event_base, PRTE_EVLOOP_NONBLOCK);
    CHECK("the shifted handler ran with its caddy", 7 == shifted);

    return failures;
}

/* ------------------------------------------------------------------ */
/* prte_event_list_item_t                                             */
/* ------------------------------------------------------------------ */

static int test_list_item(void)
{
    int failures = 0;
    pmix_list_t list;
    prte_event_list_item_t *item;
    size_t n;

    /* prun keeps its forwarded-signal handlers on a list of these, so the
     * class has to be a real list item -- registering it against the wrong
     * parent is a mistake this tree has made before (prte_session_t), and
     * the symptom is an assert inside PMIx's list teardown rather than
     * anything that points here. */
    PMIX_CONSTRUCT(&list, pmix_list_t);
    for (n = 0; n < 3; n++) {
        item = PMIX_NEW(prte_event_list_item_t);
        CHECK("a list item is allocated", NULL != item);
        if (NULL != item) {
            pmix_list_append(&list, &item->super);
        }
    }
    CHECK("all three are on the list", 3 == pmix_list_get_size(&list));
    PMIX_LIST_DESTRUCT(&list);

    return failures;
}

/* ------------------------------------------------------------------ */
/* close and reopen                                                   */
/* ------------------------------------------------------------------ */

static int test_base_lifecycle(void)
{
    int failures = 0;

    /* close() must clear BOTH globals. It used to free the base and leave
     * them set, so every subsequent prte_event_set() would have handed
     * libevent a pointer to freed memory -- and because the base is passed
     * as an argument rather than looked up, that use would have looked like
     * perfectly ordinary code. Nothing called close(), which is why it went
     * unnoticed; prte_finalize() calls it now. */
    CHECK("close succeeds", PRTE_SUCCESS == prte_event_base_close());
    CHECK("the sync base is cleared", NULL == prte_sync_event_base);
    CHECK("the general base is cleared", NULL == prte_event_base);

    /* closing twice is not an error -- prte_finalize can be reached more
     * than once on a shutdown path */
    CHECK("close is idempotent", PRTE_SUCCESS == prte_event_base_close());

    /* and the pair really is a lifecycle, not a one-shot */
    CHECK("reopen succeeds", PRTE_SUCCESS == prte_event_base_open());
    CHECK("reopen produces a base", NULL != prte_event_base);
    CHECK("reopen keeps the alias", prte_event_base == prte_sync_event_base);

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

    failures += test_base_open();
    failures += test_alloc();
    failures += test_dispatch();
    failures += test_signal_events();
    failures += test_threadshift();
    failures += test_list_item();
    /* last: it tears the base down and builds a new one */
    failures += test_base_lifecycle();

    (void) prte_event_base_close();

    if (0 == failures) {
        fprintf(stdout, "PASSED all event unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d event unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
