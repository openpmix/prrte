/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>

#include "src/class/prte_list.h"
#include "src/event/event-internal.h"
#include "src/threads/threads.h"
#include "src/util/error.h"
#include "src/util/fd.h"

#include "src/runtime/prte_progress_threads.h"


/* create a tracking object for progress threads */
typedef struct {
    prte_list_item_t super;

    int refcount;
    char *name;

    prte_event_base_t *ev_base;

    /* This will be set to false when it is time for the progress
       thread to exit */
    volatile bool ev_active;

    /* This event will always be set on the ev_base (so that the
       ev_base is not empty!) */
    prte_event_t block;

    bool engine_constructed;
    prte_thread_t engine;
} prte_progress_tracker_t;

static void tracker_constructor(prte_progress_tracker_t *p)
{
    p->refcount = 1;  // start at one since someone created it
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = false;
    p->engine_constructed = false;
}

static void tracker_destructor(prte_progress_tracker_t *p)
{
    prte_event_del(&p->block);

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->ev_base) {
        prte_event_base_free(p->ev_base);
    }
    if (p->engine_constructed) {
        PRTE_DESTRUCT(&p->engine);
    }
}

static PRTE_CLASS_INSTANCE(prte_progress_tracker_t,
                          prte_list_item_t,
                          tracker_constructor,
                          tracker_destructor);

static bool inited = false;
static prte_list_t tracking;
static struct timeval long_timeout = {
    .tv_sec = 3600,
    .tv_usec = 0
};
static const char *shared_thread_name = "PRTE-wide async progress thread";

/*
 * If this event is fired, just restart it so that this event base
 * continues to have something to block on.
 */
static void dummy_timeout_cb(int fd, short args, void *cbdata)
{
    prte_progress_tracker_t *trk = (prte_progress_tracker_t*)cbdata;

    prte_event_add(&trk->block, &long_timeout);
}

/*
 * Main for the progress thread
 */
static void* progress_engine(prte_object_t *obj)
{
    prte_thread_t *t = (prte_thread_t*)obj;
    prte_progress_tracker_t *trk = (prte_progress_tracker_t*)t->t_arg;

    while (trk->ev_active) {
        prte_event_loop(trk->ev_base, PRTE_EVLOOP_ONCE);
    }

    return PRTE_THREAD_CANCELLED;
}

static void stop_progress_engine(prte_progress_tracker_t *trk)
{
    assert(trk->ev_active);
    trk->ev_active = false;

    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    prte_event_base_loopbreak(trk->ev_base);

    prte_thread_join(&trk->engine, NULL);
}

static int start_progress_engine(prte_progress_tracker_t *trk)
{
    assert(!trk->ev_active);
    trk->ev_active = true;

    /* fork off a thread to progress it */
    trk->engine.t_run = progress_engine;
    trk->engine.t_arg = trk;

    int rc = prte_thread_start(&trk->engine);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

prte_event_base_t *prte_progress_thread_init(const char *name)
{
    prte_progress_tracker_t *trk;
    int rc;

    if (!inited) {
        PRTE_CONSTRUCT(&tracking, prte_list_t);
        inited = true;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* check if we already have this thread */
    PRTE_LIST_FOREACH(trk, &tracking, prte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* we do, so up the refcount on it */
            ++trk->refcount;
            /* return the existing base */
            return trk->ev_base;
        }
    }

    trk = PRTE_NEW(prte_progress_tracker_t);
    if (NULL == trk) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    trk->name = strdup(name);
    if (NULL == trk->name) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PRTE_RELEASE(trk);
        return NULL;
    }

    if (NULL == (trk->ev_base = prte_event_base_create())) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PRTE_RELEASE(trk);
        return NULL;
    }

    /* add an event to the new event base (if there are no events,
       prte_event_loop() will return immediately) */
    prte_event_set(trk->ev_base, &trk->block, -1, PRTE_EV_PERSIST,
                   dummy_timeout_cb, trk);
    prte_event_add(&trk->block, &long_timeout);

    /* construct the thread object */
    PRTE_CONSTRUCT(&trk->engine, prte_thread_t);
    trk->engine_constructed = true;
    if (PRTE_SUCCESS != (rc = start_progress_engine(trk))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(trk);
        return NULL;
    }
    prte_list_append(&tracking, &trk->super);

    return trk->ev_base;
}

int prte_progress_thread_finalize(const char *name)
{
    prte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRTE_LIST_FOREACH(trk, &tracking, prte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* decrement the refcount */
            --trk->refcount;

            /* If the refcount is still above 0, we're done here */
            if (trk->refcount > 0) {
                return PRTE_SUCCESS;
            }

            /* If the progress thread is active, stop it */
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            prte_list_remove_item(&tracking, &trk->super);
            PRTE_RELEASE(trk);
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

/*
 * Stop the progress thread, but don't delete the tracker (or event base)
 */
int prte_progress_thread_pause(const char *name)
{
    prte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRTE_LIST_FOREACH(trk, &tracking, prte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

int prte_progress_thread_resume(const char *name)
{
    prte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRTE_LIST_FOREACH(trk, &tracking, prte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                return PRTE_ERR_RESOURCE_BUSY;
            }

            return start_progress_engine(trk);
        }
    }

    return PRTE_ERR_NOT_FOUND;
}
