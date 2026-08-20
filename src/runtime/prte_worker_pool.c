/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/class/pmix_ring_buffer.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/runtime/prte_worker_pool.h"

/* The requested pool size, from the prte_num_worker_threads MCA parameter
 * (registered in prte_register_params, which is where its default is set). */
int prte_num_worker_threads = 8;

/* The bases, on a ring.  A request pops the oldest and pushes it straight
 * back, which advances the ring by one - so the rotation is the ring's own
 * head/tail bookkeeping and there is no separate cursor that could fall out
 * of step with the pool's size. */
static pmix_ring_buffer_t bases = PMIX_RING_BUFFER_STATIC_INIT;

/* Whether `bases` is currently constructed.  PMIX_DESTRUCT zeroes an object's
 * magic id and a debug PMIx asserts on the next one, so "finalize is safe to
 * call twice" - and it is, from a failed startup and again from
 * prte_finalize - has to mean something more careful than destructing
 * unconditionally. */
static bool ring_built = false;

/* Names of the progress threads we started, in the order we started them.
 * prte_progress_thread_finalize() is keyed by name, so this is what lets us
 * give back exactly the threads we took. */
static char **thread_names = NULL;

/* How many workers are actually running.  Not the same as
 * prte_num_worker_threads: that is the request, this is the answer. */
static int nworkers = 0;

/* Assignment is expected to happen on the main progress thread - peers are
 * built there, and so is the launch walk - but this pool is now shared by
 * subsystems that used to have a cursor each, so serialize the rotation
 * rather than rest on that expectation.  It is two pointer updates on a
 * path that is already doing a connect() or a fork(). */
static pmix_mutex_t pool_lock = PMIX_MUTEX_STATIC_INIT;

int prte_worker_pool_init(void)
{
    prte_event_base_t *evb;
    char *tmp;
    int i, rc;

    if (0 < nworkers) {
        /* already up */
        return PRTE_SUCCESS;
    }

    if (0 >= prte_num_worker_threads) {
        /* a negative value only gets here because someone asked for one;
         * treat it as "no workers" rather than sizing a ring with it */
        nworkers = 0;
        return PRTE_SUCCESS;
    }

    PMIX_CONSTRUCT(&bases, pmix_ring_buffer_t);
    ring_built = true;
    rc = pmix_ring_buffer_init(&bases, prte_num_worker_threads);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&bases);
        ring_built = false;
        return prte_pmix_convert_status(rc);
    }

    for (i = 0; i < prte_num_worker_threads; i++) {
        pmix_asprintf(&tmp, "PRTE-WORKER-%d", i);
        evb = prte_progress_thread_init(tmp);
        if (NULL == evb) {
            /* we could not get another thread - keep the ones we did get.
             * The ring is only partially filled, which the rotation copes
             * with, and nworkers records what we actually have. */
            free(tmp);
            break;
        }
        pmix_ring_buffer_push(&bases, evb);
        PMIx_Argv_append_nosize(&thread_names, tmp);
        free(tmp);
        ++nworkers;
    }

    if (nworkers < prte_num_worker_threads) {
        /* Say so unconditionally rather than behind a verbosity: this is not
         * a tuning detail, it is the process failing to build what it was
         * asked for, and it runs before any debug stream exists to hide it
         * behind.  Everything still works - the shortfall costs occupancy,
         * not correctness - so it is a notice, not an error. */
        pmix_output(0, "PRRTE: started %d of the %d requested worker threads",
                    nworkers, prte_num_worker_threads);
    }

    if (0 == nworkers) {
        /* not one thread started - nothing is going to index this ring */
        PMIX_DESTRUCT(&bases);
        ring_built = false;
    }
    return PRTE_SUCCESS;
}

void prte_worker_pool_finalize(void)
{
    int i;

    if (NULL != thread_names) {
        for (i = 0; NULL != thread_names[i]; i++) {
            prte_progress_thread_finalize(thread_names[i]);
        }
        PMIx_Argv_free(thread_names);
        thread_names = NULL;
    }
    /* the ring holds borrowed pointers - the event bases belong to the
     * progress-thread trackers just released - so this frees the ring's own
     * storage and nothing else */
    if (ring_built) {
        PMIX_DESTRUCT(&bases);
        ring_built = false;
    }
    nworkers = 0;
}

prte_event_base_t *prte_worker_pool_assign(void)
{
    prte_event_base_t *evb;

    if (0 >= nworkers) {
        /* no pool: everything runs where it always ran */
        return prte_event_base;
    }

    pmix_mutex_lock(&pool_lock);
    evb = (prte_event_base_t *) pmix_ring_buffer_pop(&bases);
    if (NULL != evb) {
        /* put it straight back at the head, which is what makes the next
         * pop hand out the *next* base rather than this one again */
        pmix_ring_buffer_push(&bases, evb);
    }
    pmix_mutex_unlock(&pool_lock);

    if (NULL == evb) {
        return prte_event_base;
    }
    return evb;
}

int prte_worker_pool_size(void)
{
    return nworkers;
}
