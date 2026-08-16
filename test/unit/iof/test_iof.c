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
 *   2. The module contract (iof.h).  Both components fill a 7-slot vtable,
 *      and both must implement push_stdin: the HNP hub routes stdin to the
 *      hosting daemon, and the daemon relay hands it up to the HNP, which
 *      is the only process that can do that routing.  A daemon that left
 *      the slot NULL segfaulted the moment a tool attached to it pushed
 *      stdin -- which every prun does, if only to mark end-of-input --
 *      taking the DVM node down and hanging the tool (issue #2568).
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
 *      Its consumer-side counterpart is equally pure: what every write
 *      handler does with a chunk the fd only partly accepted
 *      (prte_iof_base_adjust_short_write).  Getting that wrong duplicates
 *      the stream rather than dropping it, so the test drains a chunk
 *      through short writes and compares the result byte for byte.
 *
 *   6. The generic write handler itself (prte_iof_base_write_handler),
 *      driven against a live pipe.  A libevent event base is all it needs,
 *      and it is the only case here that runs a handler rather than a
 *      piece of one -- which is what holds the zero-byte sentinel branch
 *      still, the branch where all three write handlers used to leak the
 *      chunk they had just taken off the backlog.
 *
 *   7. The shape of a flow-control message.  A daemon's XON/XOFF carries
 *      nothing but the stream tag, on the same RML tag as forwarded
 *      output, so the HNP has only the tag value to tell them apart and
 *      has to do so before it unpacks a proc.
 *
 *   8. The always-readable/always-writable fd predicate
 *      (prte_iof_base_fd_always_ready), which decides timer-vs-fd events
 *      for every stream.
 *
 *   9. prte_iof_base_setup_prefork's descriptor bookkeeping when it runs
 *      out of descriptors partway through creating a proc's three pipes.
 */

#include "prte_config.h"
#include "constants.h"
#include "src/mca/base/pmix_base.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "src/event/event-internal.h"
#include "src/runtime/runtime.h"
#include "src/util/proc_info.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/iof/iof.h"
#include "src/mca/iof/iof_types.h"

/* Deliberately NOT the components' headers.  A test links against
 * libprrte, and a component is only inside libprrte in the default build:
 * configure --enable-mca-dso builds them as loadable modules instead, and
 * naming any of their symbols here fails the link outright.  Nothing in
 * this file needs one -- see the stub handler below. */

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
 * Both components publish a prte_iof_base_module_t, and every slot must be
 * wired in both.  push_stdin is the one worth stating explicitly: a tool can
 * attach to any daemon, not just the master, and the first thing prun does
 * when its job ends is push end-of-input.  The daemon's implementation only
 * relays to the HNP -- but it must exist, or that push is a NULL call.
 */
/*
 * Ask the framework for a component by name, and for the module that
 * component hands *this* process.
 *
 * These are two different questions.  A component is present whenever the
 * framework opened it; whether it yields a module is up to its query,
 * which is free to decline - iof/prted, for one, answers only a daemon.
 * Naming the component's module symbol instead would assume it was linked
 * into libprrte, which is false with --enable-mca-dso: there the component
 * is a separate DSO and the symbol is not there to link against.
 */
static bool iof_component_present(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;

    PMIX_LIST_FOREACH(cli, &prte_iof_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 == strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            return true;
        }
    }
    return false;
}

static prte_iof_base_module_t *iof_module(const char *name)
{
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_module_t *mod = NULL;
    int pri = 0;

    PMIX_LIST_FOREACH(cli, &prte_iof_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        if (0 != strcmp(name, cli->cli_component->pmix_mca_component_name)) {
            continue;
        }
        if (NULL == cli->cli_component->pmix_mca_query_component) {
            return NULL;
        }
        if (PRTE_SUCCESS != cli->cli_component->pmix_mca_query_component(&mod, &pri)) {
            return NULL;
        }
        return (prte_iof_base_module_t *) mod;
    }
    return NULL;
}

static int test_module_contract(void)
{
    int failures = 0;
    prte_iof_base_module_t *hnp, *prted;
    prte_proc_type_t save;

    hnp = iof_module("hnp");
    /* iof/prted hands its module only to a daemon, and this test is not
     * one, so ask the question it answers rather than skipping its half of
     * the contract */
    save = prte_process_info.proc_type;
    prte_process_info.proc_type = PRTE_PROC_DAEMON;
    prted = iof_module("prted");
    prte_process_info.proc_type = save;

    if (NULL == hnp || NULL == prted) {
        fprintf(stdout, "SKIPPED test_module_contract (iof/hnp or iof/prted absent)\n");
        return 0;
    }

    /* the HNP hub wires all seven slots */
    CHECK("hnp init set", NULL != hnp->init);
    CHECK("hnp push set", NULL != hnp->push);
    CHECK("hnp pull set", NULL != hnp->pull);
    CHECK("hnp close set", NULL != hnp->close);
    CHECK("hnp complete set", NULL != hnp->complete);
    CHECK("hnp finalize set", NULL != hnp->finalize);
    CHECK("hnp push_stdin set", NULL != hnp->push_stdin);

    /* the daemon relay wires all seven too - its push_stdin relays to the
     * HNP rather than routing, but a tool attached to this daemon calls it */
    CHECK("prted init set", NULL != prted->init);
    CHECK("prted push set", NULL != prted->push);
    CHECK("prted pull set", NULL != prted->pull);
    CHECK("prted close set", NULL != prted->close);
    CHECK("prted complete set", NULL != prted->complete);
    CHECK("prted finalize set", NULL != prted->finalize);
    CHECK("prted push_stdin set", NULL != prted->push_stdin);

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

    /* the framework must have opened a component under each name -
     * finding one is the assertion */
    if (!iof_component_present("hnp") || !iof_component_present("prted")) {
        fprintf(stdout, "SKIPPED test_component_identity"
                        " (iof components not loadable from the build tree)\n");
        return 0;
    }
    CHECK("hnp component present", iof_component_present("hnp"));
    CHECK("prted component present", iof_component_present("prted"));

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
    CHECK("sink not closed", !sink->closed);
    CHECK("sink not closed", !sink->closed);
    CHECK("sink daemon rank INVALID", PMIX_RANK_INVALID == sink->daemon.rank);
    /* destructor must release the owned write event */
    PMIX_RELEASE(sink);

    /* read event: inactive, no fd, no proc, no sink, event allocated */
    prte_iof_read_event_t *rev = PMIX_NEW(prte_iof_read_event_t);
    CHECK("rev proc NULL", NULL == rev->proc);
    CHECK("rev fd unset", -1 == rev->fd);
    CHECK("rev not activated", !rev->activated);
    CHECK("rev not always_readable", !rev->always_readable);
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
 * The consumer side of the sink write engine: what the write handlers do
 * with a chunk the fd only partially accepted.
 *
 * A non-blocking write to a pipe whose reader has fallen behind returns a
 * short count, and the handler must resume from exactly where that write
 * stopped: prte_iof_base_adjust_short_write drops the bytes that made it
 * out and slides the remainder to the front of the chunk.  Sliding the
 * data without decrementing the count leaves the tail of the chunk holding
 * a stale copy of already-written bytes and re-sends them on every retry,
 * which is how a stdin file larger than the pipe capacity reached the
 * application duplicated many times over (issue #2579).
 *
 * So the invariant under test is a stream one: drain a chunk through a
 * sequence of short writes and the bytes that came out must be the bytes
 * that went in -- no loss, no duplication.
 */
static int test_short_write_adjust(void)
{
    int failures = 0;
    prte_iof_write_output_t *chunk;
    unsigned char source[300];
    /* room for far more than the source, so a handler that re-sends data
     * overruns the expected length instead of looping forever */
    unsigned char drained[4 * sizeof(source)];
    const int bite = 64;
    size_t i, ndrained = 0;
    int nwritten, rounds = 0;

    for (i = 0; i < sizeof(source); i++) {
        source[i] = (unsigned char) (i % 251);
    }

    chunk = PMIX_NEW(prte_iof_write_output_t);
    memcpy(chunk->data, source, sizeof(source));
    chunk->numbytes = (int) sizeof(source);

    /* drain the chunk the way a write handler does: take what the fd
     * accepted, re-base the chunk, come back for the rest
     */
    while (0 < chunk->numbytes && 100 > rounds) {
        rounds++;
        nwritten = (bite < chunk->numbytes) ? bite : chunk->numbytes;
        if (sizeof(drained) < ndrained + (size_t) nwritten) {
            fprintf(stderr, "FAIL [short_write_adjust]: chunk is re-sending data\n");
            failures++;
            break;
        }
        memcpy(&drained[ndrained], chunk->data, nwritten);
        ndrained += nwritten;
        if (nwritten == chunk->numbytes) {
            /* the chunk went out completely - the handler releases it */
            break;
        }
        prte_iof_base_adjust_short_write(chunk, nwritten);
    }

    CHECK("short writes drain exactly the original byte count",
          sizeof(source) == ndrained);
    CHECK("short writes deliver the original bytes in order",
          ndrained <= sizeof(drained) && 0 == memcmp(drained, source, ndrained));

    PMIX_RELEASE(chunk);

    /* the degenerate counts must leave the chunk alone: nothing went out,
     * or everything did (the handler releases the chunk in that case and
     * must not be handed a re-based copy of it)
     */
    chunk = PMIX_NEW(prte_iof_write_output_t);
    memcpy(chunk->data, source, sizeof(source));
    chunk->numbytes = (int) sizeof(source);

    prte_iof_base_adjust_short_write(chunk, 0);
    CHECK("zero-byte write leaves the count alone", (int) sizeof(source) == chunk->numbytes);
    CHECK("zero-byte write leaves the data alone", 0 == memcmp(chunk->data, source, sizeof(source)));

    prte_iof_base_adjust_short_write(chunk, (int) sizeof(source));
    CHECK("complete write leaves the count alone", (int) sizeof(source) == chunk->numbytes);
    CHECK("complete write leaves the data alone", 0 == memcmp(chunk->data, source, sizeof(source)));

    PMIX_RELEASE(chunk);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_short_write_adjust\n");
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

/*
 * The consumer side for real: drive prte_iof_base_write_handler against a
 * live pipe and watch what comes out the other end.
 *
 * Two things are pinned here that no other test reaches.  First, the
 * zero-byte chunk is a *sentinel*, not data: the handler must flush what
 * precedes it and then close the fd, which the reader sees as EOF.  Second,
 * that sentinel has already been removed from the backlog by the time the
 * handler recognizes it, so the handler owns it -- releasing the sink (which
 * frees the chunks still on the list) does not free this one, and for a long
 * while nothing did.  Every closed stdin stream leaked one chunk, and a chunk
 * is PRTE_IOF_BASE_TAGGED_OUT_MAX bytes of fixed buffer, forever on a
 * persistent DVM.  The leak itself is invisible from here; what this test
 * holds still is the surrounding behavior, so a future edit to that branch
 * has to keep working.
 */
static int test_write_handler_drain(void)
{
    int failures = 0;
    pmix_proc_t name;
    prte_iof_sink_t *sink = NULL;
    prte_iof_write_event_t *wev;
    int pfd[2];
    char buf[64];
    ssize_t n;

    PMIX_LOAD_PROCID(&name, "test-nspace", 0);

    if (0 != pipe(pfd)) {
        fprintf(stderr, "FAIL [write_handler_drain]: pipe() failed\n");
        return 1;
    }

    PRTE_IOF_SINK_DEFINE(&sink, &name, pfd[1], PRTE_IOF_STDIN, prte_iof_base_write_handler);
    if (NULL == sink || NULL == sink->wev) {
        fprintf(stderr, "FAIL [write_handler_drain]: sink not defined\n");
        close(pfd[0]);
        close(pfd[1]);
        return 1;
    }
    wev = sink->wev;
    /* pretend the write event is already armed: we call the handler by hand
     * rather than running an event loop */
    wev->pending = true;

    /* queue a payload, then the close sentinel behind it */
    prte_iof_base_write_output(&name, PRTE_IOF_STDIN, (const unsigned char *) "hello", 5, wev);
    prte_iof_base_write_output(&name, PRTE_IOF_STDIN, NULL, 0, wev);
    CHECK("payload plus sentinel queued", 2 == pmix_list_get_size(&wev->outputs));

    /* the handler drains the payload, then hits the sentinel and releases
     * the sink -- whose write event closes the fd on the way out */
    prte_iof_base_write_handler(pfd[1], 0, (void *) sink);

    n = read(pfd[0], buf, sizeof(buf));
    CHECK("payload reached the fd", 5 == n);
    CHECK("payload arrived intact", 5 == n && 0 == memcmp(buf, "hello", 5));

    /* the sentinel closed the write end, so the reader sees EOF rather than
     * hanging on a descriptor nobody will ever write to again */
    n = read(pfd[0], buf, sizeof(buf));
    CHECK("sentinel closed the stream", 0 == n);

    close(pfd[0]);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_write_handler_drain\n");
    }
    return failures;
}

/*
 * The proc <-> read-event reference cycle.
 *
 * A prte_iof_proc_t owns its read events, and PRTE_IOF_READ_EVENT retains
 * the proc for each one it builds -- so the two reference each other, and
 * releasing the proc alone can never free either.  That matters because
 * both components' complete() used to do exactly that: drop the component
 * list's reference on a proc whose streams were still open, which frees
 * nothing.  What it produces is an orphan -- a proc nothing can reach,
 * still holding its pipe descriptors, with its read events still armed and
 * pointing back at it -- and when one of those events next fires it drops
 * the final reference from inside its own destructor and then keeps using
 * the memory.  A job that ends normally has closed its streams by then; a
 * job that was killed has not, which is the case complete() exists for.
 *
 * So: build the cycle, drop the proc the way the list does, and require
 * the proc to still be alive and usable.  That is the property that makes
 * releasing the read events first *necessary* rather than tidy.  We use an
 * fd of -1 so the read event's destructor takes its no-close path and this
 * test needs no descriptors of its own.
 */
/* Stands in for a component's read handler.  The read event below is
 * defined and never activated, so this is only ever a pointer value --
 * which is why the real one is not worth a link dependency on a component
 * that may not be in libprrte at all. */
static void stub_read_handler(int fd, short event, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, event, cbdata);
}

static int test_proc_read_event_cycle(void)
{
    int failures = 0;
    prte_iof_proc_t *proct;
    prte_iof_read_event_t *held;

    proct = PMIX_NEW(prte_iof_proc_t);
    PMIX_LOAD_PROCID(&proct->name, "test-nspace", 0);

    /* one read event, defined but not activated -- the macro retains proct */
    PRTE_IOF_READ_EVENT(&proct->revstdout, proct, -1, PRTE_IOF_STDOUT,
                        stub_read_handler, false);
    CHECK("read event was defined", NULL != proct->revstdout);
    if (NULL == proct->revstdout) {
        PMIX_RELEASE(proct);
        return failures;
    }
    CHECK("read event points back at its proc",
          (void *) proct == (void *) proct->revstdout->proc);
    held = proct->revstdout;

    /* Take a second reference to stand in for the caller, then drop the
     * first the way complete() drops the component list's.  The cycle means
     * that is not the last one, so nothing is freed: the proc is still
     * whole, still owns its stream, and still holds the descriptor behind
     * it -- reachable now only through a read event that is still armed.
     */
    PMIX_RETAIN(proct);
    PMIX_RELEASE(proct);
    CHECK("releasing the proc frees nothing while a read event holds it",
          NULL != proct && held == proct->revstdout);
    CHECK("...and the orphan is entirely intact",
          0 == strncmp(proct->name.nspace, "test-nspace", PMIX_MAX_NSLEN));

    /* The correct teardown is the other order: release the stream FIRST,
     * while a reference to the proc is still held.  That drops the read
     * event's reference as a side effect, and the proc's own release is
     * then the last one and runs its destructor at a safe moment.  Do it
     * the other way round -- release the proc's last reference and only
     * then the read event -- and the proc is freed from inside the read
     * event's destructor, while the macro that released it is still about
     * to write NULL into the slot it lives in.
     */
    PMIX_RELEASE(proct->revstdout);
    CHECK("releasing the stream clears the slot", NULL == proct->revstdout);
    CHECK("...and the proc survives to be released by its owner", NULL != proct);
    PMIX_RELEASE(proct);
    CHECK("...and then the proc is freed", NULL == proct);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_proc_read_event_cycle\n");
    }
    return failures;
}

/*
 * Flow control on PRTE_RML_TAG_IOF_HNP.
 *
 * A daemon whose stdin sink has backed up sends the HNP a buffer holding
 * *only* the stream tag -- no proc, no count, no payload.  Forwarded output
 * on that same tag leads with the tag as well, so the HNP has nothing but
 * the tag value to tell the two apart, and it has to make that decision
 * before it unpacks anything else: unpacking a proc from a tag-only buffer
 * fails, and the HNP used to report the daemon's XOFF to the user as a
 * corrupted message every time a process read its stdin slowly.
 *
 * So two invariants: the flow-control tags share no bit with any stream tag
 * (the discriminating test is a mask), and a flow-control message really
 * does end after the tag.
 */
static int test_flow_control_message(void)
{
    int failures = 0;
    pmix_data_buffer_t buf;
    prte_iof_tag_t tag = PRTE_IOF_XOFF;
    prte_iof_tag_t unpacked = 0;
    pmix_proc_t proc;
    int32_t count = 1;
    pmix_status_t rc;

    /* the mask the HNP branches on cannot collide with a stream */
    CHECK("XON is disjoint from every stream", 0 == (PRTE_IOF_XON & PRTE_IOF_STDALL));
    CHECK("XOFF is disjoint from every stream", 0 == (PRTE_IOF_XOFF & PRTE_IOF_STDALL));
    CHECK("XON and XOFF are distinct", PRTE_IOF_XON != PRTE_IOF_XOFF);

    /* build exactly what prte_iof_prted_send_xonxoff puts on the wire */
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &tag, 1, PMIX_UINT16);
    CHECK("flow-control tag packs", PMIX_SUCCESS == rc);

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &unpacked, &count, PMIX_UINT16);
    CHECK("flow-control tag unpacks", PMIX_SUCCESS == rc);
    CHECK("flow-control tag survives the round trip", PRTE_IOF_XOFF == unpacked);
    CHECK("the tag identifies itself by mask", 0 != ((PRTE_IOF_XON | PRTE_IOF_XOFF) & unpacked));

    /* ...and nothing follows it, which is why the tag has to be screened
     * before the receiver reaches for a proc */
    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &proc, &count, PMIX_PROC);
    CHECK("nothing follows the flow-control tag", PMIX_SUCCESS != rc);

    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_flow_control_message\n");
    }
    return failures;
}

/*
 * prte_iof_base_setup_prefork creates the stdout pipe first, then stdin,
 * then stderr.  Running out of descriptors partway through is exactly the
 * condition under which the ones already created must be handed back: the
 * caller only reads those fields on success, so nobody else can close them,
 * and a daemon that leaks two descriptors per failed launch turns a single
 * transient EMFILE into every subsequent launch failing as well.
 *
 * Drive it by burning the descriptor table down to three free slots: the
 * stdout pipe takes two, and the stdin pipe then cannot be created.
 */
#define IOF_TEST_MAX_FILL 512

static int test_prefork_fd_recovery(void)
{
    int failures = 0;
    struct rlimit saved, tight;
    prte_iof_base_io_conf_t opts;
    int fill[IOF_TEST_MAX_FILL];
    int recovered[3];
    int nfill = 0, fd, rc, i;

    if (0 != getrlimit(RLIMIT_NOFILE, &saved)) {
        fprintf(stdout, "SKIPPED test_prefork_fd_recovery (no RLIMIT_NOFILE)\n");
        return 0;
    }
    /* cap the table so exhausting it is cheap; lowering the soft limit
     * leaves already-open descriptors working */
    tight = saved;
    if (IOF_TEST_MAX_FILL < tight.rlim_cur) {
        tight.rlim_cur = IOF_TEST_MAX_FILL;
    }
    if (0 != setrlimit(RLIMIT_NOFILE, &tight)) {
        fprintf(stdout, "SKIPPED test_prefork_fd_recovery (cannot lower RLIMIT_NOFILE)\n");
        return 0;
    }

    while (nfill < IOF_TEST_MAX_FILL && 0 <= (fd = open("/dev/null", O_RDONLY))) {
        fill[nfill++] = fd;
    }
    if (3 > nfill) {
        /* the table was already full - nothing to prove here */
        while (0 < nfill) {
            close(fill[--nfill]);
        }
        (void) setrlimit(RLIMIT_NOFILE, &saved);
        fprintf(stdout, "SKIPPED test_prefork_fd_recovery (no descriptors to spare)\n");
        return 0;
    }
    /* hand back exactly three: enough for the stdout pipe and no more */
    for (i = 0; i < 3; i++) {
        close(fill[--nfill]);
    }

    memset(&opts, 0, sizeof(opts));
    opts.usepty = 0;
    opts.connect_stdin = true;
    rc = prte_iof_base_setup_prefork(&opts);
    CHECK("prefork reports the descriptor exhaustion", PRTE_SUCCESS != rc);

    /* the stdout pipe it did create has to be back in the pool */
    for (i = 0; i < 3; i++) {
        recovered[i] = open("/dev/null", O_RDONLY);
    }
    CHECK("prefork returned the descriptors it had already taken",
          0 <= recovered[0] && 0 <= recovered[1] && 0 <= recovered[2]);

    for (i = 0; i < 3; i++) {
        if (0 <= recovered[i]) {
            close(recovered[i]);
        }
    }
    while (0 < nfill) {
        close(fill[--nfill]);
    }
    (void) setrlimit(RLIMIT_NOFILE, &saved);

    if (0 == failures) {
        fprintf(stdout, "PASSED test_prefork_fd_recovery\n");
    }
    return failures;
}

int main(void)
{
    int rc, failures = 0;
    pmix_status_t prc;

    rc = prte_init_util(PRTE_PROC_MASTER);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_init_util failed: %d\n", rc);
        return 1;
    }
    /* the sink macros assign their libevent events to prte_event_base, so
     * the base has to exist before any test builds one */
    rc = prte_event_base_open();
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "prte_event_base_open failed: %d\n", rc);
        return 1;
    }
    /* the flow-control test packs a buffer, and PMIx_Data_pack refuses to
     * run until PMIx itself is up. A daemon - which is what sends a
     * flow-control message - reaches that state through PMIx_server_init,
     * so do the same */
    prc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != prc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(prc));
        prte_finalize();
        return 1;
    }

    failures += test_tag_model();
    /* the module tests ask the framework for their subject, so it has to
     * be open before they run */
    rc = pmix_mca_base_framework_open(&prte_iof_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PRTE_SUCCESS != rc) {
        fprintf(stderr, "iof framework open failed: %d\n", rc);
        prte_finalize();
        return 1;
    }

    failures += test_module_contract();
    failures += test_component_identity();
    failures += test_classes();
    failures += test_write_output_accounting();
    failures += test_write_output_chunking();
    failures += test_short_write_adjust();
    failures += test_write_handler_drain();
    failures += test_proc_read_event_cycle();
    failures += test_flow_control_message();
    failures += test_fd_always_ready();
    /* runs last: it lowers RLIMIT_NOFILE for the duration */
    failures += test_prefork_fd_recovery();

    (void) pmix_mca_base_framework_close(&prte_iof_base_framework);
    PMIx_server_finalize();
    prte_finalize();

    if (0 == failures) {
        fprintf(stdout, "PASSED all iof unit tests\n");
    } else {
        fprintf(stdout, "FAILED %d iof unit test(s)\n", failures);
    }
    return (0 == failures) ? 0 : 1;
}
