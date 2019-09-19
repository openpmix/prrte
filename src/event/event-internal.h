/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 */

#ifndef PRRTE_MCA_EVENT_H
#define PRRTE_MCA_EVENT_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <stdint.h>
#include <stdarg.h>

#include PRRTE_EVENT_HEADER
#if ! PRRTE_HAVE_LIBEV
#include PRRTE_EVENT2_THREAD_HEADER
#endif

#include "src/util/output.h"

typedef event_callback_fn prrte_event_cbfunc_t;

BEGIN_C_DECLS

/* set the number of event priority levels */
#define PRRTE_EVENT_NUM_PRI   8

#define PRRTE_EV_ERROR_PRI         0
#define PRRTE_EV_MSG_HI_PRI        1
#define PRRTE_EV_SYS_HI_PRI        2
#define PRRTE_EV_MSG_LO_PRI        3
#define PRRTE_EV_SYS_LO_PRI        4
#define PRRTE_EV_INFO_HI_PRI       5
#define PRRTE_EV_INFO_LO_PRI       6
#define PRRTE_EV_LOWEST_PRI        7

#define PRRTE_EVENT_SIGNAL(ev)   prrte_event_get_signal(ev)

#define PRRTE_TIMEOUT_DEFAULT    {1, 0}

typedef struct event_base prrte_event_base_t;
typedef struct event prrte_event_t;

PRRTE_EXPORT extern prrte_event_base_t *prrte_sync_event_base;

PRRTE_EXPORT int prrte_event_base_open(void);
PRRTE_EXPORT int prrte_event_base_close(void);
PRRTE_EXPORT prrte_event_t* prrte_event_alloc(void);


#define PRRTE_EV_TIMEOUT EV_TIMEOUT
#define PRRTE_EV_READ    EV_READ
#define PRRTE_EV_WRITE   EV_WRITE
#define PRRTE_EV_SIGNAL  EV_SIGNAL
/* Persistent event: won't get removed automatically when activated. */
#define PRRTE_EV_PERSIST EV_PERSIST

#define PRRTE_EVLOOP_ONCE     EVLOOP_ONCE        /**< Block at most once. */
#define PRRTE_EVLOOP_NONBLOCK EVLOOP_NONBLOCK    /**< Do not block. */

#define prrte_event_base_create() event_base_new()

#define prrte_event_base_free(x) event_base_free(x)

#define prrte_event_reinit(b) event_reinit((b))

#define prrte_event_base_init_common_timeout (b, t) event_base_init_common_timeout((b), (t))

#define prrte_event_base_loopexit(b) event_base_loopexit(b, NULL)

#if PRRTE_HAVE_LIBEV
#define prrte_event_use_threads()
#define prrte_event_free(b) free(b)
#define prrte_event_get_signal(x) (x)->ev_fd
#else

/* thread support APIs */
#define prrte_event_use_threads() evthread_use_pthreads()
#define prrte_event_base_loopbreak(b) event_base_loopbreak(b)
#define prrte_event_free(x) event_free(x)
#define prrte_event_get_signal(x) event_get_signal(x)
#endif

/* Event priority APIs */
#define prrte_event_base_priority_init(b, n) event_base_priority_init((b), (n))

#define prrte_event_set_priority(x, n) event_priority_set((x), (n))

/* Basic event APIs */
#define prrte_event_enable_debug_mode() event_enable_debug_mode()

PRRTE_EXPORT int prrte_event_assign(struct event *ev, prrte_event_base_t *evbase,
                                  int fd, short arg, event_callback_fn cbfn, void *cbd);

#define prrte_event_set(b, x, fd, fg, cb, arg) prrte_event_assign((x), (b), (fd), (fg), (event_callback_fn) (cb), (arg))

#if PRRTE_HAVE_LIBEV
PRRTE_EXPORT int prrte_event_add(struct event *ev, struct timeval *tv);
PRRTE_EXPORT int prrte_event_del(struct event *ev);
PRRTE_EXPORT void prrte_event_active (struct event *ev, int res, short ncalls);
PRRTE_EXPORT void prrte_event_base_loopbreak (prrte_event_base_t *b);
#else
#define prrte_event_add(ev, tv) event_add((ev), (tv))
#define prrte_event_del(ev) event_del((ev))
#define prrte_event_active(x, y, z) event_active((x), (y), (z))
#define prrte_event_base_loopbreak(b) event_base_loopbreak(b)

#endif

PRRTE_EXPORT prrte_event_t* prrte_event_new(prrte_event_base_t *b, int fd,
                                         short fg, event_callback_fn cbfn, void *cbd);

/* Timer APIs */
#define prrte_event_evtimer_new(b, cb, arg) prrte_event_new((b), -1, 0, (cb), (arg))

#define prrte_event_evtimer_add(x, tv) prrte_event_add((x), (tv))

#define prrte_event_evtimer_set(b, x, cb, arg) prrte_event_assign((x), (b), -1, 0, (event_callback_fn) (cb), (arg))

#define prrte_event_evtimer_del(x) prrte_event_del((x))

#define prrte_event_evtimer_pending(x, tv) event_pending((x), EV_TIMEOUT, (tv))

#define prrte_event_evtimer_initialized(x) event_initialized((x))

/* Signal APIs */
#define prrte_event_signal_add(x, tv) event_add((x), (tv))

#define prrte_event_signal_set(b, x, fd, cb, arg) prrte_event_assign((x), (b), (fd), EV_SIGNAL|EV_PERSIST, (event_callback_fn) (cb), (arg))

#define prrte_event_signal_del(x) event_del((x))

#define prrte_event_signal_pending(x, tv) event_pending((x), EV_SIGNAL, (tv))

#define prrte_event_signal_initalized(x) event_initialized((x))

#define prrte_event_loop(b, fg) event_base_loop((b), (fg))

END_C_DECLS

#endif /* PRRTE_EVENT_H_ */
