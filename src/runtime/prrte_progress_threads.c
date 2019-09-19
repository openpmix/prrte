/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"
#include "src/threads/threads.h"
#include "src/util/error.h"
#include "src/util/fd.h"

#include "src/runtime/prrte_progress_threads.h"


/* create a tracking object for progress threads */
typedef struct {
    prrte_list_item_t super;

    int refcount;
    char *name;

    prrte_event_base_t *ev_base;

    /* This will be set to false when it is time for the progress
       thread to exit */
    volatile bool ev_active;

    /* This event will always be set on the ev_base (so that the
       ev_base is not empty!) */
    prrte_event_t block;

    bool engine_constructed;
    prrte_thread_t engine;
} prrte_progress_tracker_t;

static void tracker_constructor(prrte_progress_tracker_t *p)
{
    p->refcount = 1;  // start at one since someone created it
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = false;
    p->engine_constructed = false;
}

static void tracker_destructor(prrte_progress_tracker_t *p)
{
    prrte_event_del(&p->block);

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->ev_base) {
        prrte_event_base_free(p->ev_base);
    }
    if (p->engine_constructed) {
        PRRTE_DESTRUCT(&p->engine);
    }
}

static PRRTE_CLASS_INSTANCE(prrte_progress_tracker_t,
                          prrte_list_item_t,
                          tracker_constructor,
                          tracker_destructor);

static bool inited = false;
static prrte_list_t tracking;
static struct timeval long_timeout = {
    .tv_sec = 3600,
    .tv_usec = 0
};
static const char *shared_thread_name = "PRRTE-wide async progress thread";

/*
 * If this event is fired, just restart it so that this event base
 * continues to have something to block on.
 */
static void dummy_timeout_cb(int fd, short args, void *cbdata)
{
    prrte_progress_tracker_t *trk = (prrte_progress_tracker_t*)cbdata;

    prrte_event_add(&trk->block, &long_timeout);
}

/*
 * Main for the progress thread
 */
static void* progress_engine(prrte_object_t *obj)
{
    prrte_thread_t *t = (prrte_thread_t*)obj;
    prrte_progress_tracker_t *trk = (prrte_progress_tracker_t*)t->t_arg;

    while (trk->ev_active) {
        prrte_event_loop(trk->ev_base, PRRTE_EVLOOP_ONCE);
    }

    return PRRTE_THREAD_CANCELLED;
}

static void stop_progress_engine(prrte_progress_tracker_t *trk)
{
    assert(trk->ev_active);
    trk->ev_active = false;

    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    prrte_event_base_loopbreak(trk->ev_base);

    prrte_thread_join(&trk->engine, NULL);
}

static int start_progress_engine(prrte_progress_tracker_t *trk)
{
    assert(!trk->ev_active);
    trk->ev_active = true;

    /* fork off a thread to progress it */
    trk->engine.t_run = progress_engine;
    trk->engine.t_arg = trk;

    int rc = prrte_thread_start(&trk->engine);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

prrte_event_base_t *prrte_progress_thread_init(const char *name)
{
    prrte_progress_tracker_t *trk;
    int rc;

    if (!inited) {
        PRRTE_CONSTRUCT(&tracking, prrte_list_t);
        inited = true;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* check if we already have this thread */
    PRRTE_LIST_FOREACH(trk, &tracking, prrte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* we do, so up the refcount on it */
            ++trk->refcount;
            /* return the existing base */
            return trk->ev_base;
        }
    }

    trk = PRRTE_NEW(prrte_progress_tracker_t);
    if (NULL == trk) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    trk->name = strdup(name);
    if (NULL == trk->name) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        PRRTE_RELEASE(trk);
        return NULL;
    }

    if (NULL == (trk->ev_base = prrte_event_base_create())) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        PRRTE_RELEASE(trk);
        return NULL;
    }

    /* add an event to the new event base (if there are no events,
       prrte_event_loop() will return immediately) */
    prrte_event_set(trk->ev_base, &trk->block, -1, PRRTE_EV_PERSIST,
                   dummy_timeout_cb, trk);
    prrte_event_add(&trk->block, &long_timeout);

    /* construct the thread object */
    PRRTE_CONSTRUCT(&trk->engine, prrte_thread_t);
    trk->engine_constructed = true;
    if (PRRTE_SUCCESS != (rc = start_progress_engine(trk))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(trk);
        return NULL;
    }
    prrte_list_append(&tracking, &trk->super);

    return trk->ev_base;
}

int prrte_progress_thread_finalize(const char *name)
{
    prrte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRRTE_LIST_FOREACH(trk, &tracking, prrte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* decrement the refcount */
            --trk->refcount;

            /* If the refcount is still above 0, we're done here */
            if (trk->refcount > 0) {
                return PRRTE_SUCCESS;
            }

            /* If the progress thread is active, stop it */
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            prrte_list_remove_item(&tracking, &trk->super);
            PRRTE_RELEASE(trk);
            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_NOT_FOUND;
}

/*
 * Stop the progress thread, but don't delete the tracker (or event base)
 */
int prrte_progress_thread_pause(const char *name)
{
    prrte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRRTE_LIST_FOREACH(trk, &tracking, prrte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_NOT_FOUND;
}

int prrte_progress_thread_resume(const char *name)
{
    prrte_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PRRTE_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    PRRTE_LIST_FOREACH(trk, &tracking, prrte_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                return PRRTE_ERR_RESOURCE_BUSY;
            }

            return start_progress_engine(trk);
        }
    }

    return PRRTE_ERR_NOT_FOUND;
}
