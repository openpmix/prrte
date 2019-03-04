/*
 * Copyright (c) 2014-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"
#include "opal/constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>

#include "opal/class/opal_list.h"
#include "opal/event/event-internal.h"
#include "opal/threads/threads.h"
#include "opal/util/error.h"
#include "opal/util/fd.h"

#include "opal/runtime/opal_progress_threads.h"


/* create a tracking object for progress threads */
typedef struct {
    opal_list_item_t super;

    int refcount;
    char *name;

    opal_event_base_t *ev_base;

    /* This will be set to false when it is time for the progress
       thread to exit */
    volatile bool ev_active;

    /* This event will always be set on the ev_base (so that the
       ev_base is not empty!) */
    opal_event_t block;

    bool engine_constructed;
    opal_thread_t engine;
#if OPAL_HAVE_LIBEV
    ev_async async;
    pthread_mutex_t mutex;
    opal_list_t list;
#endif
} opal_progress_tracker_t;

static void tracker_constructor(opal_progress_tracker_t *p)
{
    p->refcount = 1;  // start at one since someone created it
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = false;
    p->engine_constructed = false;
#if OPAL_HAVE_LIBEV
    pthread_mutex_init(&p->mutex, NULL);
    OBJ_CONSTRUCT(&p->list, opal_list_t);
#endif
}

static void tracker_destructor(opal_progress_tracker_t *p)
{
    opal_event_del(&p->block);

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->ev_base) {
        opal_event_base_free(p->ev_base);
    }
    if (p->engine_constructed) {
        OBJ_DESTRUCT(&p->engine);
    }
#if OPAL_HAVE_LIBEV
    pthread_mutex_destroy(&p->mutex);
    OPAL_LIST_DESTRUCT(&p->list);
#endif
}

static OBJ_CLASS_INSTANCE(opal_progress_tracker_t,
                          opal_list_item_t,
                          tracker_constructor,
                          tracker_destructor);

#if OPAL_HAVE_LIBEV

typedef enum {
    OPAL_EVENT_ACTIVE,
    OPAL_EVENT_ADD,
    OPAL_EVENT_DEL
} opal_event_type_t;

typedef struct {
    opal_list_item_t super;
    struct event *ev;
    struct timeval *tv;
    int res;
    short ncalls;
    opal_event_type_t type;
} opal_event_caddy_t;

static OBJ_CLASS_INSTANCE(opal_event_caddy_t,
                          opal_list_item_t,
                          NULL, NULL);

static opal_progress_tracker_t* opal_progress_tracker_get_by_base(struct event_base *);

static void opal_libev_ev_async_cb (EV_P_ ev_async *w, int revents)
{
    opal_progress_tracker_t *trk = opal_progress_tracker_get_by_base((struct event_base *)EV_A);
    assert(NULL != trk);
    pthread_mutex_lock (&trk->mutex);
    opal_event_caddy_t *cd, *next;
    OPAL_LIST_FOREACH_SAFE(cd, next, &trk->list, opal_event_caddy_t) {
        switch (cd->type) {
            case OPAL_EVENT_ADD:
                (void)event_add(cd->ev, cd->tv);
                break;
            case OPAL_EVENT_DEL:
                (void)event_del(cd->ev);
                break;
            case OPAL_EVENT_ACTIVE:
                (void)event_active(cd->ev, cd->res, cd->ncalls);
                break;
        }
        opal_list_remove_item(&trk->list, &cd->super);
        OBJ_RELEASE(cd);
    }
    pthread_mutex_unlock (&trk->mutex);
}

int opal_event_add(struct event *ev, struct timeval *tv) {
    int res;
    opal_progress_tracker_t *trk = opal_progress_tracker_get_by_base(ev->ev_base);
    if ((NULL != trk) && !pthread_equal(pthread_self(), trk->engine.t_handle)) {
        opal_event_caddy_t *cd = OBJ_NEW(opal_event_caddy_t);
        cd->type = OPAL_EVENT_ADD;
        cd->ev = ev;
        cd->tv = tv;
        pthread_mutex_lock(&trk->mutex);
        opal_list_append(&trk->list, &cd->super);
        ev_async_send ((struct ev_loop *)trk->ev_base, &trk->async);
        pthread_mutex_unlock(&trk->mutex);
        res = OPAL_SUCCESS;
    } else {
        res = event_add(ev, tv);
    }
    return res;
}

int opal_event_del(struct event *ev) {
    int res;
    opal_progress_tracker_t *trk = opal_progress_tracker_get_by_base(ev->ev_base);
    if ((NULL != trk) && !pthread_equal(pthread_self(), trk->engine.t_handle)) {
        opal_event_caddy_t *cd = OBJ_NEW(opal_event_caddy_t);
        cd->type = OPAL_EVENT_DEL;
        cd->ev = ev;
        pthread_mutex_lock(&trk->mutex);
        opal_list_append(&trk->list, &cd->super);
        ev_async_send ((struct ev_loop *)trk->ev_base, &trk->async);
        pthread_mutex_unlock(&trk->mutex);
        res = OPAL_SUCCESS;
    } else {
        res = event_del(ev);
    }
    return res;
}

void opal_event_active (struct event *ev, int res, short ncalls) {
    opal_progress_tracker_t *trk = opal_progress_tracker_get_by_base(ev->ev_base);
    if ((NULL != trk) && !pthread_equal(pthread_self(), trk->engine.t_handle)) {
        opal_event_caddy_t *cd = OBJ_NEW(opal_event_caddy_t);
        cd->type = OPAL_EVENT_ACTIVE;
        cd->ev = ev;
        cd->res = res;
        cd->ncalls = ncalls;
        pthread_mutex_lock(&trk->mutex);
        opal_list_append(&trk->list, &cd->super);
        ev_async_send ((struct ev_loop *)trk->ev_base, &trk->async);
        pthread_mutex_unlock(&trk->mutex);
    } else {
        event_active(ev, res, ncalls);
    }
}

void opal_event_base_loopbreak (opal_event_base_t *ev_base) {
    opal_progress_tracker_t *trk = opal_progress_tracker_get_by_base(ev_base);
    assert(NULL != trk);
    ev_async_send ((struct ev_loop *)trk->ev_base, &trk->async);
}
#endif

static bool inited = false;
static opal_list_t tracking;
static struct timeval long_timeout = {
    .tv_sec = 3600,
    .tv_usec = 0
};
static const char *shared_thread_name = "OPAL-wide async progress thread";

/*
 * If this event is fired, just restart it so that this event base
 * continues to have something to block on.
 */
static void dummy_timeout_cb(int fd, short args, void *cbdata)
{
    opal_progress_tracker_t *trk = (opal_progress_tracker_t*)cbdata;

    opal_event_add(&trk->block, &long_timeout);
}

/*
 * Main for the progress thread
 */
static void* progress_engine(opal_object_t *obj)
{
    opal_thread_t *t = (opal_thread_t*)obj;
    opal_progress_tracker_t *trk = (opal_progress_tracker_t*)t->t_arg;

    while (trk->ev_active) {
        opal_event_loop(trk->ev_base, OPAL_EVLOOP_ONCE);
    }

    return OPAL_THREAD_CANCELLED;
}

static void stop_progress_engine(opal_progress_tracker_t *trk)
{
    assert(trk->ev_active);
#if OPAL_HAVE_LIBEV
    pthread_mutex_lock(&trk->mutex);
    trk->ev_active = false;
    pthread_mutex_unlock(&trk->mutex);
    ev_async_send ((struct ev_loop *)trk->ev_base, &trk->async);
#else
    trk->ev_active = false;
    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    opal_event_base_loopbreak(trk->ev_base);
#endif

    opal_thread_join(&trk->engine, NULL);
}

static int start_progress_engine(opal_progress_tracker_t *trk)
{
    assert(!trk->ev_active);
    trk->ev_active = true;

    /* fork off a thread to progress it */
    trk->engine.t_run = progress_engine;
    trk->engine.t_arg = trk;

    int rc = opal_thread_start(&trk->engine);
    if (OPAL_SUCCESS != rc) {
        OPAL_ERROR_LOG(rc);
    }

    return rc;
}

opal_event_base_t *opal_progress_thread_init(const char *name)
{
    opal_progress_tracker_t *trk;
    int rc;

    if (!inited) {
        OBJ_CONSTRUCT(&tracking, opal_list_t);
        inited = true;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* check if we already have this thread */
    OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* we do, so up the refcount on it */
            ++trk->refcount;
            /* return the existing base */
            return trk->ev_base;
        }
    }

    trk = OBJ_NEW(opal_progress_tracker_t);
    if (NULL == trk) {
        OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    trk->name = strdup(name);
    if (NULL == trk->name) {
        OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);
        OBJ_RELEASE(trk);
        return NULL;
    }

    if (NULL == (trk->ev_base = opal_event_base_create())) {
        OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);
        OBJ_RELEASE(trk);
        return NULL;
    }

    /* add an event to the new event base (if there are no events,
       opal_event_loop() will return immediately) */
    opal_event_assign(&trk->block, trk->ev_base, -1, OPAL_EV_PERSIST,
                      dummy_timeout_cb, trk);
    opal_event_add(&trk->block, &long_timeout);

#if OPAL_HAVE_LIBEV
    ev_async_init (&trk->async, opal_libev_ev_async_cb);
    ev_async_start((struct ev_loop *)trk->ev_base, &trk->async);
#endif

    /* construct the thread object */
    OBJ_CONSTRUCT(&trk->engine, opal_thread_t);
    trk->engine_constructed = true;
    if (OPAL_SUCCESS != (rc = start_progress_engine(trk))) {
        OPAL_ERROR_LOG(rc);
        OBJ_RELEASE(trk);
        return NULL;
    }
    opal_list_append(&tracking, &trk->super);

    return trk->ev_base;
}

int opal_progress_thread_attach(opal_event_base_t *ev_base, const char *name)
{
    opal_progress_tracker_t *trk;

    if (!inited) {
        OBJ_CONSTRUCT(&tracking, opal_list_t);
        inited = true;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* check if we already have this thread */
    OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* we do, so up the refcount on it */
            ++trk->refcount;
            /* return the existing base */
            return OPAL_SUCCESS;
        }
    }

    trk = OBJ_NEW(opal_progress_tracker_t);
    if (NULL == trk) {
        OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);
        return OPAL_ERROR;
    }

    trk->name = strdup(name);
    if (NULL == trk->name) {
        OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);
        OBJ_RELEASE(trk);
        return OPAL_ERROR;
    }

    trk->ev_base = ev_base;

    /* construct the thread object */
    OBJ_CONSTRUCT(&trk->engine, opal_thread_t);
    trk->engine_constructed = true;
    trk->engine.t_handle = pthread_self();
    opal_list_append(&tracking, &trk->super);
    trk->ev_active = true;
#if OPAL_HAVE_LIBEV
    ev_async_init (&trk->async, opal_libev_ev_async_cb);
    ev_async_start((struct ev_loop *)trk->ev_base, &trk->async);
#endif

    return OPAL_SUCCESS;
}

int opal_progress_thread_finalize(const char *name)
{
    opal_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return OPAL_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* decrement the refcount */
            --trk->refcount;

            /* If the refcount is still above 0, we're done here */
            if (trk->refcount > 0) {
                return OPAL_SUCCESS;
            }

            /* If the progress thread is active, stop it */
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            opal_list_remove_item(&tracking, &trk->super);
            OBJ_RELEASE(trk);
            return OPAL_SUCCESS;
        }
    }

    return OPAL_ERR_NOT_FOUND;
}

/*
 * Stop the progress thread, but don't delete the tracker (or event base)
 */
int opal_progress_thread_pause(const char *name)
{
    opal_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return OPAL_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            return OPAL_SUCCESS;
        }
    }

    return OPAL_ERR_NOT_FOUND;
}

#if OPAL_HAVE_LIBEV
static opal_progress_tracker_t* opal_progress_tracker_get_by_base(opal_event_base_t *base) {
    opal_progress_tracker_t *trk;

    if (inited)  {
        OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
            if(trk->ev_base == base) {
                return trk;
            }
        }
    }
    return NULL;
}
#endif

int opal_progress_thread_resume(const char *name)
{
    opal_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return OPAL_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* find the specified engine */
    OPAL_LIST_FOREACH(trk, &tracking, opal_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                return OPAL_ERR_RESOURCE_BUSY;
            }

            return start_progress_engine(trk);
        }
    }

    return OPAL_ERR_NOT_FOUND;
}

int opal_event_assign(struct event *ev, opal_event_base_t *evbase,
                      int fd, short arg, event_callback_fn cbfn, void *cbd)
{
#if OPAL_HAVE_LIBEV
    event_set(ev, fd, arg, cbfn, cbd);
    event_base_set(evbase, ev);
#else
    event_assign(ev, evbase, fd, arg, cbfn, cbd);
#endif
    return 0;
}
