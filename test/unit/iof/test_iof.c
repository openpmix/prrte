/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the iof (I/O forwarding) framework.
 *
 * The end-to-end behavior of iof -- capturing a live proc's stdout/stderr,
 * relaying it over the RML to the HNP, and injecting stdin down a proc's
 * pipe -- runs only inside a live DVM with real file descriptors and a
 * running progress thread, and is covered by the integration harness.
 *
 * What *can* be exercised in isolation is the framework's structural
 * contract and the pure, event-loop-independent logic its handlers rely
 * on:
 *
 *   1. The tag model (iof_types.h).  The PRTE_IOF_* stream tags are a
 *      bitmask, and the composite tags (STDMERGE, STDOUTALL, STDALL) are
 *      hand-assigned constants that MUST equal the OR of their parts, or
 *      the many `tag & PRTE_IOF_STDOUT` tests scattered through the
 *      handlers would silently mis-route a stream.  The control-flag tags
 *      (EXCLUSIVE, XON/XOFF, PULL/CLOSE) must not collide with the stream
 *      bits, since a single uint16 carries both.
 *
 *   2. The module contract (iof.h).  Both components fill a 7-slot vtable.
 *      The daemon relay intentionally leaves push_stdin NULL (stdin
 *      injection is master-only); the HNP hub must implement all seven.  A
 *      regression that swapped those would crash or silently drop stdin.
 *
 *   3. The component identities: "hnp" and "prted" (selection needs them).
 *
 *   4. The core reference-counted classes (base.h).  Their constructors
 *      establish the NULL/zero/INVALID defaults the push/pull paths depend
 *      on, and their destructors must free every owned member -- including
 *      the freshly-constructed, never-used case -- without crashing.
 *
 *   5. The producer side of the sink write engine
 *      (prte_iof_base_write_output).  Its backlog accounting is what the
 *      XON/XOFF flow control keys on, and the zero-byte sentinel is the
 *      close-this-stream signal every write path must preserve.  We drive
 *      it with the write event pre-marked pending so no libevent activation
 *      is needed, exercising the enqueue/accounting logic directly.
 *
 *   6. The always-readable/always-writable fd predicate
 *      (prte_iof_base_fd_always_ready), which decides timer-vs-fd events
 *      for every stream.
 */

#include "prte_config.h"
#include "constants.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/iof/iof_types.h"
#include "src/mca/iof/hnp/iof_hnp.h"
#include "src/mca/iof/prted/iof_prted.h"

#define CHECK(label, cond)                                    \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/*
 * The stream tags are bit flags OR'd into one uint16.  The composite
 * values are hand-written literals in iof_types.h; pin them to the OR of
 * their parts so a future edit can't drift them apart from the
 * `tag & PRTE_IOF_STDXXX` tests the handlers use.
 */
static int test_tag_model(void)
{
    int failures = 0;

    /* the three primitive streams are distinct single bits */
    CHECK("STDIN is a single bit", PRTE_IOF_STDIN == 0x0001);
    CHECK("STDOUT is a single bit", PRTE_IOF_STDOUT == 0x0002);
    CHECK("STDERR is a single bit", PRTE_IOF_STDERR == 0x0004);
    CHECK("STDDIAG is a single bit", PRTE_IOF_STDDIAG == 0x0008);

    /* composites are exactly the OR of their documented parts */
    CHECK("STDMERGE == STDOUT|STDERR",
          PRTE_IOF_STDMERGE == (PRTE_IOF_STDOUT | PRTE_IOF_STDERR));
    CHECK("STDOUTALL == STDOUT|STDERR|STDDIAG",
          PRTE_IOF_STDOUTALL == (PRTE_IOF_STDOUT | PRTE_IOF_STDERR | PRTE_IOF_STDDIAG));
    CHECK("STDALL == STDIN|STDOUT|STDERR|STDDIAG",
          PRTE_IOF_STDALL
              == (PRTE_IOF_STDIN | PRTE_IOF_STDOUT | PRTE_IOF_STDERR | PRTE_IOF_STDDIAG));

    /* the handlers rely on these implications when masking a tag */
    CHECK("STDMERGE selects STDOUT", 0 != (PRTE_IOF_STDMERGE & PRTE_IOF_STDOUT));
    CHECK("STDMERGE selects STDERR", 0 != (PRTE_IOF_STDMERGE & PRTE_IOF_STDERR));
    CHECK("STDALL selects every stream",
          (PRTE_IOF_STDALL & PRTE_IOF_STDIN) && (PRTE_IOF_STDALL & PRTE_IOF_STDOUT)
              && (PRTE_IOF_STDALL & PRTE_IOF_STDERR) && (PRTE_IOF_STDALL & PRTE_IOF_STDDIAG));

    /* control flags must live above the stream bits so a single tag word
     * can carry a stream selector and a control flag without collision */
    CHECK("EXCLUSIVE clear of streams", 0 == (PRTE_IOF_EXCLUSIVE & PRTE_IOF_STDALL));
    CHECK("XON clear of streams", 0 == (PRTE_IOF_XON & PRTE_IOF_STDALL));
    CHECK("XOFF clear of streams", 0 == (PRTE_IOF_XOFF & PRTE_IOF_STDALL));
    CHECK("PULL clear of streams", 0 == (PRTE_IOF_PULL & PRTE_IOF_STDALL));
    CHECK("CLOSE clear of streams", 0 == (PRTE_IOF_CLOSE & PRTE_IOF_STDALL));

    /* and the control flags are pairwise distinct */
    CHECK("XON != XOFF", PRTE_IOF_XON != PRTE_IOF_XOFF);
    CHECK("PULL != CLOSE", PRTE_IOF_PULL != PRTE_IOF_CLOSE);
    CHECK("XON clear of XOFF", 0 == (PRTE_IOF_XON & PRTE_IOF_XOFF));
    CHECK("PULL clear of CLOSE", 0 == (PRTE_IOF_PULL & PRTE_IOF_CLOSE));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_tag_model\n");
    }
    return failures;
}

/*
 * Both components publish a prte_iof_base_module_t.  Only the HNP hub
 * implements push_stdin (injecting stdin is a master-side operation); the
 * daemon relay must leave it NULL and callers guard on that.  Every other
 * slot must be wired in both, since the base and its callers only NULL-check
 * the optional entry points.
 */
static int test_module_contract(void)
{
    int failures = 0;
    prte_iof_base_module_t *hnp = &prte_iof_hnp_module;
    prte_iof_base_module_t *prted = &prte_iof_prted_module;

    /* the HNP hub wires all seven slots */
    CHECK("hnp init set", NULL != hnp->init);
    CHECK("hnp push set", NULL != hnp->push);
    CHECK("hnp pull set", NULL != hnp->pull);
    CHECK("hnp close set", NULL != hnp->close);
    CHECK("hnp complete set", NULL != hnp->complete);
    CHECK("hnp finalize set", NULL != hnp->finalize);
    CHECK("hnp push_stdin set", NULL != hnp->push_stdin);

    /* the daemon relay wires six and deliberately leaves push_stdin NULL */
    CHECK("prted init set", NULL != prted->init);
    CHECK("prted push set", NULL != prted->push);
    CHECK("prted pull set", NULL != prted->pull);
    CHECK("prted close set", NULL != prted->close);
    CHECK("prted complete set", NULL != prted->complete);
    CHECK("prted finalize set", NULL != prted->finalize);
    CHECK("prted push_stdin NULL (HNP-only)", NULL == prted->push_stdin);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_module_contract\n");
    }
    return failures;
}

/*
 * Component selection is keyed on the component name string; guard both.
 */
static int test_component_identity(void)
{
    int failures = 0;

    CHECK("hnp component name",
          0 == strcmp(prte_mca_iof_hnp_component.super.pmix_mca_component_name, "hnp"));
    CHECK("prted component name",
          0 == strcmp(prte_mca_iof_prted_component.super.pmix_mca_component_name, "prted"));

    if (0 == failures) {
        fprintf(stdout, "PASSED test_component_identity\n");
    }
    return failures;
}

/*
 * Constructor defaults and destructor safety for the framework's core
 * classes.  The defaults are load-bearing: the push/pull/close paths test
 * these NULL/false/INVALID fields before allocating or tearing down, and
 * the destructors are the only place the owned events, sinks, and byte
 * buffers are freed.
 */
static int test_classes(void)
{
    int failures = 0;

    /* proc endpoint bundle: all three stream slots NULL */
    prte_iof_proc_t *proc = PMIX_NEW(prte_iof_proc_t);
    CHECK("proc stdinev NULL", NULL == proc->stdinev);
    CHECK("proc revstdout NULL", NULL == proc->revstdout);
    CHECK("proc revstderr NULL", NULL == proc->revstderr);
    /* the all-NULL destructor path must be safe */
    PMIX_RELEASE(proc);

    /* write event: not pending, no fd, empty output list, event allocated */
    prte_iof_write_event_t *wev = PMIX_NEW(prte_iof_write_event_t);
    CHECK("wev not pending", !wev->pending);
    CHECK("wev not always_writable", !wev->always_writable);
    CHECK("wev fd unset", -1 == wev->fd);
    CHECK("wev ev allocated", NULL != wev->ev);
    CHECK("wev outputs empty", 0 == pmix_list_get_size(&wev->outputs));
    PMIX_RELEASE(wev);

    /* sink: constructs its own write event, flags clear, daemon INVALID */
    prte_iof_sink_t *sink = PMIX_NEW(prte_iof_sink_t);
    CHECK("sink wev allocated", NULL != sink->wev);
    CHECK("sink not xoff", !sink->xoff);
    CHECK("sink not exclusive", !sink->exclusive);
    CHECK("sink not closed", !sink->closed);
    CHECK("sink daemon rank INVALID", PMIX_RANK_INVALID == sink->daemon.rank);
    /* destructor must release the owned write event */
    PMIX_RELEASE(sink);

    /* read event: inactive, no fd, no proc, no sink, event allocated */
    prte_iof_read_event_t *rev = PMIX_NEW(prte_iof_read_event_t);
    CHECK("rev proc NULL", NULL == rev->proc);
    CHECK("rev fd unset", -1 == rev->fd);
    CHECK("rev not active", !rev->active);
    CHECK("rev not activated", !rev->activated);
    CHECK("rev not always_readable", !rev->always_readable);
    CHECK("rev sink NULL", NULL == rev->sink);
    CHECK("rev ev allocated", NULL != rev->ev);
    /* fd == -1 so the destructor takes the free-without-close path */
    PMIX_RELEASE(rev);

    /* deliver carrier: empty byte object, and the destructor frees a
     * malloc'd payload */
    prte_iof_deliver_t *dlv = PMIX_NEW(prte_iof_deliver_t);
    CHECK("deliver bytes NULL", NULL == dlv->bo.bytes);
    CHECK("deliver size 0", 0 == dlv->bo.size);
    dlv->bo.bytes = (char *) malloc(16);
    dlv->bo.size = 16;
    PMIX_RELEASE(dlv);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_classes\n");
    }
    return failures;
}

/*
 * The producer side of the sink write engine.  prte_iof_base_write_output
 * appends a *copy* of the caller's bytes to the write event's backlog and
 * returns the new backlog length -- the value XON/XOFF back-pressure keys
 * on.  A zero-byte call is not a no-op: it enqueues the flush-then-close
 * sentinel.  A NULL channel is a documented no-op returning 0.
 *
 * We pre-mark the write event pending so write_output never tries to arm a
 * libevent event (there is no progress thread in this test), isolating the
 * enqueue/accounting logic.
 */
static int test_write_output_accounting(void)
{
    int failures = 0;
    pmix_proc_t name;
    prte_iof_write_output_t *chunk;
    int n;

    PMIX_LOAD_PROCID(&name, "test-nspace", 0);

    prte_iof_write_event_t *wev = PMIX_NEW(prte_iof_write_event_t);
    /* suppress event-base activation: pretend the write event is already
     * armed so write_output only does its enqueue/accounting work */
    wev->pending = true;

    /* NULL channel is a no-op returning 0 */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, (const unsigned char *) "x", 1, NULL);
    CHECK("NULL channel returns 0", 0 == n);

    /* first chunk -> backlog of 1 */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, (const unsigned char *) "hello", 5, wev);
    CHECK("first write backlog 1", 1 == n);
    CHECK("backlog list size 1", 1 == pmix_list_get_size(&wev->outputs));

    /* the bytes are copied verbatim into a fresh chunk */
    chunk = (prte_iof_write_output_t *) pmix_list_get_first(&wev->outputs);
    CHECK("chunk numbytes 5", 5 == chunk->numbytes);
    CHECK("chunk data copied", 0 == memcmp(chunk->data, "hello", 5));

    /* second chunk -> backlog of 2 */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, (const unsigned char *) "world", 5, wev);
    CHECK("second write backlog 2", 2 == n);

    /* zero-byte call still enqueues the close sentinel -> backlog of 3 */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, NULL, 0, wev);
    CHECK("zero-byte write backlog 3", 3 == n);
    CHECK("backlog list size 3", 3 == pmix_list_get_size(&wev->outputs));

    /* the sentinel is the last item and carries numbytes == 0 */
    chunk = (prte_iof_write_output_t *) pmix_list_get_last(&wev->outputs);
    CHECK("sentinel numbytes 0", 0 == chunk->numbytes);

    /* releasing the write event must drain and free the queued chunks */
    PMIX_RELEASE(wev);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_write_output_accounting\n");
    }
    return failures;
}

/*
 * The queued chunk carries a fixed PRTE_IOF_BASE_TAGGED_OUT_MAX buffer, but
 * callers (notably the HNP's push_stdin, which hands us whatever the PMIx
 * server produced) are under no obligation to respect that limit. An
 * oversized write must be split across chunks rather than overrun the
 * buffer, and no byte may be lost in the split.
 */
static int test_write_output_chunking(void)
{
    int failures = 0;
    pmix_proc_t name;
    prte_iof_write_output_t *chunk;
    prte_iof_write_event_t *wev;
    unsigned char *big;
    size_t bigsize = (2 * PRTE_IOF_BASE_TAGGED_OUT_MAX) + 17;
    size_t i, total = 0;
    int n;

    PMIX_LOAD_PROCID(&name, "test-nspace", 0);

    big = (unsigned char *) malloc(bigsize);
    if (NULL == big) {
        fprintf(stderr, "FAIL [write_output_chunking]: malloc failed\n");
        return 1;
    }
    for (i = 0; i < bigsize; i++) {
        big[i] = (unsigned char) (i % 251);
    }

    wev = PMIX_NEW(prte_iof_write_event_t);
    wev->pending = true;

    /* 2 full chunks + a 17-byte remainder */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, big, (int) bigsize, wev);
    CHECK("oversized write split into 3 chunks", 3 == n);

    /* every chunk is within the buffer, and the pieces reassemble exactly */
    PMIX_LIST_FOREACH(chunk, &wev->outputs, prte_iof_write_output_t)
    {
        if (PRTE_IOF_BASE_TAGGED_OUT_MAX < chunk->numbytes) {
            fprintf(stderr, "FAIL [write_output_chunking]: chunk of %d bytes exceeds %d\n",
                    chunk->numbytes, PRTE_IOF_BASE_TAGGED_OUT_MAX);
            failures++;
            break;
        }
        if (0 != memcmp(chunk->data, &big[total], chunk->numbytes)) {
            fprintf(stderr, "FAIL [write_output_chunking]: chunk at offset %lu differs\n",
                    (unsigned long) total);
            failures++;
            break;
        }
        total += chunk->numbytes;
    }
    CHECK("chunks reassemble to the original length", bigsize == total);

    /* a negative count cannot be copied - it must degrade to the close
     * sentinel rather than being handed to write() as a huge size
     */
    n = prte_iof_base_write_output(&name, PRTE_IOF_STDIN, big, -1, wev);
    CHECK("negative write enqueues sentinel", 4 == n);
    chunk = (prte_iof_write_output_t *) pmix_list_get_last(&wev->outputs);
    CHECK("negative write numbytes 0", 0 == chunk->numbytes);

    PMIX_RELEASE(wev);
    free(big);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_write_output_chunking\n");
    }
    return failures;
}

/*
 * prte_iof_base_fd_always_ready decides whether a stream is driven by a
 * zero-length timer (regular files / non-tty char devs / block devs, which
 * never signal readiness through the event loop) or by a real fd event.
 * A pipe -- the normal stdout/stderr/stdin transport -- must be the fd-event
 * case; a regular file and /dev/null must be the timer case.
 */
static int test_fd_always_ready(void)
{
    int failures = 0;
    int pfd[2];
    int rfd, nulfd;

    /* a pipe is NOT always ready -- it is driven by a real read/write event */
    if (0 == pipe(pfd)) {
        CHECK("pipe read end not always-ready", !prte_iof_base_fd_always_ready(pfd[0]));
        CHECK("pipe write end not always-ready", !prte_iof_base_fd_always_ready(pfd[1]));
        close(pfd[0]);
        close(pfd[1]);
    } else {
        fprintf(stderr, "FAIL [fd_always_ready]: pipe() failed\n");
        failures++;
    }

    /* a regular file IS always ready -- it never blocks, so it is timer-driven */
    rfd = open("test_iof_tmpfile.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (0 <= rfd) {
        CHECK("regular file always-ready", prte_iof_base_fd_always_ready(rfd));
        close(rfd);
        unlink("test_iof_tmpfile.dat");
    } else {
        fprintf(stderr, "FAIL [fd_always_ready]: could not create temp file\n");
        failures++;
    }

    /* /dev/null is a non-tty character device -> always ready */
    nulfd = open("/dev/null", O_WRONLY);
    if (0 <= nulfd) {
        CHECK("/dev/null always-ready", prte_iof_base_fd_always_ready(nulfd));
        close(nulfd);
    } else {
        fprintf(stderr, "FAIL [fd_always_ready]: could not open /dev/null\n");
        failures++;
    }

    if (0 == failures) {
        fprintf(stdout, "PASSED test_fd_always_ready\n");
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

    failures += test_tag_model();
    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_classes();
    failures += test_write_output_accounting();
    failures += test_write_output_chunking();
    failures += test_fd_always_ready();

    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all iof unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d iof unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
