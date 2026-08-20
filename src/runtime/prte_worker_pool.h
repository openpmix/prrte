/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 *
 * The process-wide pool of worker progress threads.
 *
 * PRRTE has two kinds of work it wants off the main progress thread: a
 * peer's socket send/recv handlers (so the wire keeps moving while the main
 * thread computes) and the fork/exec of a local child (so a big local proc
 * count is not a serial walk).  Both used to carry their own private pool,
 * their own sizing rules and their own round-robin cursor, which meant two
 * sets of threads competing for the same cores and two sets of parameters
 * to reason about.  There is now one pool, and both ask it for a base.
 *
 * The pool is built once, in prte_init(), for the roles that have peers and
 * children - the DVM master and the daemons.  A tool builds nothing.
 *
 * Assignment is a rotation, not a placement decision: the bases live on a
 * ring, and each request pops the oldest and pushes it back, so successive
 * requests cycle around the ring with no cursor to keep in step with the
 * pool's size.
 */

#ifndef PRTE_WORKER_POOL_H
#define PRTE_WORKER_POOL_H

#include "prte_config.h"

#include "src/event/event-internal.h"

/** Number of worker threads the pool should run, from the MCA parameter
 * prte_num_worker_threads.  Zero (or less) means "no workers": every
 * assignment hands back prte_event_base and the process runs exactly as it
 * did before any of this existed. */
PRTE_EXPORT extern int prte_num_worker_threads;

/**
 * Start the pool.  Idempotent - a second call while the pool is up is a
 * no-op, which is what lets a caller ask for the pool without having to
 * know whether some other subsystem got there first.
 *
 * Returns PRTE_SUCCESS even when prte_num_worker_threads is zero; the pool
 * is then simply empty.  A thread that fails to start shrinks the pool
 * rather than failing the call: fewer workers is a degradation, not a
 * reason to refuse to run.
 */
PRTE_EXPORT int prte_worker_pool_init(void);

/**
 * Stop the pool and release it.  Safe to call when the pool was never
 * started, and safe to call twice.
 *
 * Note that prte_finalize() has already stopped every progress thread
 * (prte_progress_thread_pause(NULL)) before any framework teardown runs, so
 * by the time this is reached no worker is executing an event handler.
 */
PRTE_EXPORT void prte_worker_pool_finalize(void);

/**
 * Claim the next base in the rotation.
 *
 * Never returns NULL: with no workers - not configured, not yet started, or
 * already harvested - the answer is prte_event_base, so every caller can
 * use the result unconditionally.
 */
PRTE_EXPORT prte_event_base_t *prte_worker_pool_assign(void);

/**
 * How many worker threads are actually running.  Zero means every
 * assignment lands on the main progress thread.
 */
PRTE_EXPORT int prte_worker_pool_size(void);

#endif /* PRTE_WORKER_POOL_H */
