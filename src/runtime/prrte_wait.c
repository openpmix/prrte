/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2008      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include <string.h>
#include <assert.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_QUEUE_H
#include <sys/queue.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "src/dss/dss_types.h"
#include "src/class/prrte_object.h"
#include "src/util/output.h"
#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"
#include "src/threads/mutex.h"
#include "src/sys/atomic.h"

#include "constants.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"

#include "src/runtime/prrte_wait.h"

/* Timer Object Declaration */
static void timer_const(prrte_timer_t *tm)
{
    tm->ev = prrte_event_alloc();
    tm->payload = NULL;
}
static void timer_dest(prrte_timer_t *tm)
{
    prrte_event_free(tm->ev);
}
PRRTE_CLASS_INSTANCE(prrte_timer_t,
                   prrte_object_t,
                   timer_const,
                   timer_dest);


static void wccon(prrte_wait_tracker_t *p)
{
    p->child = NULL;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void wcdes(prrte_wait_tracker_t *p)
{
    if (NULL != p->child) {
        PRRTE_RELEASE(p->child);
    }
}
PRRTE_CLASS_INSTANCE(prrte_wait_tracker_t,
                   prrte_list_item_t,
                   wccon, wcdes);

/* Local Variables */
static prrte_event_t handler;
static prrte_list_t pending_cbs;

/* Local Function Prototypes */
static void wait_signal_callback(int fd, short event, void *arg);

/* Interface Functions */

void prrte_wait_disable(void)
{
    prrte_event_del(&handler);
}

void prrte_wait_enable(void)
{
    prrte_event_add(&handler, NULL);
}

int prrte_wait_init(void)
{
    PRRTE_CONSTRUCT(&pending_cbs, prrte_list_t);

    prrte_event_set(prrte_event_base,
                   &handler, SIGCHLD, PRRTE_EV_SIGNAL|PRRTE_EV_PERSIST,
                   wait_signal_callback,
                   &handler);
    prrte_event_set_priority(&handler, PRRTE_SYS_PRI);

    prrte_event_add(&handler, NULL);
    return PRRTE_SUCCESS;
}


int prrte_wait_finalize(void)
{
    prrte_event_del(&handler);

    /* clear out the pending cbs */
    PRRTE_LIST_DESTRUCT(&pending_cbs);

    return PRRTE_SUCCESS;
}

/* this function *must* always be called from
 * within an event in the prrte_event_base */
void prrte_wait_cb(prrte_proc_t *child, prrte_wait_cbfunc_t callback,
                   prrte_event_base_t *evb, void *data)
{
    prrte_wait_tracker_t *t2;

    if (NULL == child || NULL == callback) {
        /* bozo protection */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return;
    }

    /* see if this proc is still alive */
    if (!PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
        if (NULL != callback) {
            /* already heard this proc is dead, so just do the callback */
            t2 = PRRTE_NEW(prrte_wait_tracker_t);
            PRRTE_RETAIN(child);  // protect against race conditions
            t2->child = child;
            t2->evb = evb;
            t2->cbfunc = callback;
            t2->cbdata = data;
            prrte_event_set(t2->evb, &t2->ev, -1,
                           PRRTE_EV_WRITE, t2->cbfunc, t2);
            prrte_event_set_priority(&t2->ev, PRRTE_MSG_PRI);
            prrte_event_active(&t2->ev, PRRTE_EV_WRITE, 1);
        }
        return;
    }

   /* we just override any existing registration */
    PRRTE_LIST_FOREACH(t2, &pending_cbs, prrte_wait_tracker_t) {
        if (t2->child == child) {
            t2->cbfunc = callback;
            t2->cbdata = data;
            return;
        }
    }
    /* get here if this is a new registration */
    t2 = PRRTE_NEW(prrte_wait_tracker_t);
    PRRTE_RETAIN(child);  // protect against race conditions
    t2->child = child;
    t2->evb = evb;
    t2->cbfunc = callback;
    t2->cbdata = data;
    prrte_list_append(&pending_cbs, &t2->super);
}

static void cancel_callback(int fd, short args, void *cbdata)
{
    prrte_wait_tracker_t *trk = (prrte_wait_tracker_t*)cbdata;
    prrte_wait_tracker_t *t2;

    PRRTE_ACQUIRE_OBJECT(trk);

    PRRTE_LIST_FOREACH(t2, &pending_cbs, prrte_wait_tracker_t) {
        if (t2->child == trk->child) {
            prrte_list_remove_item(&pending_cbs, &t2->super);
            PRRTE_RELEASE(t2);
            PRRTE_RELEASE(trk);
            return;
        }
    }

    PRRTE_RELEASE(trk);
}

void prrte_wait_cb_cancel(prrte_proc_t *child)
{
    prrte_wait_tracker_t *trk;

    if (NULL == child) {
        /* bozo protection */
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return;
    }

    /* push this into the event library for handling */
    trk = PRRTE_NEW(prrte_wait_tracker_t);
    PRRTE_RETAIN(child);  // protect against race conditions
    trk->child = child;
    PRRTE_THREADSHIFT(trk, prrte_event_base, cancel_callback, PRRTE_SYS_PRI);
}


/* callback from the event library whenever a SIGCHLD is received */
static void wait_signal_callback(int fd, short event, void *arg)
{
    prrte_event_t *signal = (prrte_event_t*) arg;
    int status;
    pid_t pid;
    prrte_wait_tracker_t *t2;

    PRRTE_ACQUIRE_OBJECT(signal);

    if (SIGCHLD != PRRTE_EVENT_SIGNAL(signal)) {
        return;
    }

    /* we can have multiple children leave but only get one
     * sigchild callback, so reap all the waitpids until we
     * don't get anything valid back */
    while (1) {
        pid = waitpid(-1, &status, WNOHANG);
        if (-1 == pid && EINTR == errno) {
            /* try it again */
            continue;
        }
        /* if we got garbage, then nothing we can do */
        if (pid <= 0) {
            return;
        }

        /* we are already in an event, so it is safe to access the list */
        PRRTE_LIST_FOREACH(t2, &pending_cbs, prrte_wait_tracker_t) {
            if (pid == t2->child->pid) {
                /* found it! */
                t2->child->exit_code = status;
                prrte_list_remove_item(&pending_cbs, &t2->super);
                if (NULL != t2->cbfunc) {
                    prrte_event_set(t2->evb, &t2->ev, -1,
                                   PRRTE_EV_WRITE, t2->cbfunc, t2);
                    prrte_event_set_priority(&t2->ev, PRRTE_MSG_PRI);
                    prrte_event_active(&t2->ev, PRRTE_EV_WRITE, 1);
                } else {
                    PRRTE_RELEASE(t2);
                }
                break;
            }
        }
    }
}
