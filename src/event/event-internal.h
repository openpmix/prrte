/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 */

#ifndef PRTE_MCA_EVENT_H
#define PRTE_MCA_EVENT_H

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#include <stdarg.h>
#include <stdint.h>

#include PRTE_EVENT_HEADER
#if !PRTE_HAVE_LIBEV
#    include PRTE_EVENT2_THREAD_HEADER
#endif

#include "src/class/prte_list.h"
#include "src/util/output.h"

typedef event_callback_fn prte_event_cbfunc_t;

BEGIN_C_DECLS

/* set the number of event priority levels */
#define PRTE_EVENT_NUM_PRI 8

#define PRTE_EV_ERROR_PRI   0
#define PRTE_EV_MSG_HI_PRI  1
#define PRTE_EV_SYS_HI_PRI  2
#define PRTE_EV_MSG_LO_PRI  3
#define PRTE_EV_SYS_LO_PRI  4
#define PRTE_EV_INFO_HI_PRI 5
#define PRTE_EV_INFO_LO_PRI 6
#define PRTE_EV_LOWEST_PRI  7

#define PRTE_EVENT_SIGNAL(ev) prte_event_get_signal(ev)

#define PRTE_TIMEOUT_DEFAULT \
    {                        \
        1, 0                 \
    }

typedef struct event_base prte_event_base_t;
typedef struct event prte_event_t;

PRTE_EXPORT extern prte_event_base_t *prte_sync_event_base;
PRTE_EXPORT extern prte_event_base_t *prte_event_base;

PRTE_EXPORT int prte_event_base_open(void);
PRTE_EXPORT int prte_event_base_close(void);
PRTE_EXPORT prte_event_t *prte_event_alloc(void);

#define PRTE_EV_TIMEOUT EV_TIMEOUT
#define PRTE_EV_READ    EV_READ
#define PRTE_EV_WRITE   EV_WRITE
#define PRTE_EV_SIGNAL  EV_SIGNAL
/* Persistent event: won't get removed automatically when activated. */
#define PRTE_EV_PERSIST EV_PERSIST

#define PRTE_EVLOOP_ONCE     EVLOOP_ONCE     /**< Block at most once. */
#define PRTE_EVLOOP_NONBLOCK EVLOOP_NONBLOCK /**< Do not block. */

#define prte_event_base_create() event_base_new()

#define prte_event_base_free(x) event_base_free(x)

#define prte_event_reinit(b) event_reinit((b))

#define prte_event_base_init_common_timeout (b, t) event_base_init_common_timeout((b), (t))

#if PRTE_HAVE_LIBEV
#    define prte_event_use_threads()
#    define prte_event_free(b)       free(b)
#    define prte_event_get_signal(x) (x)->ev_fd
#else

/* thread support APIs */
#    define prte_event_use_threads() evthread_use_pthreads()
#    define prte_event_free(x)       event_free(x)
#    define prte_event_get_signal(x) event_get_signal(x)
#endif

/* Event priority APIs */
#define prte_event_base_priority_init(b, n) event_base_priority_init((b), (n))

#define prte_event_set_priority(x, n) event_priority_set((x), (n))

/* Basic event APIs */
#define prte_event_enable_debug_mode() event_enable_debug_mode()

PRTE_EXPORT int prte_event_assign(struct event *ev, prte_event_base_t *evbase, int fd, short arg,
                                  event_callback_fn cbfn, void *cbd);

#define prte_event_set(b, x, fd, fg, cb, arg) \
    prte_event_assign((x), (b), (fd), (fg), (event_callback_fn)(cb), (arg))

#if PRTE_HAVE_LIBEV
PRTE_EXPORT int prte_event_add(struct event *ev, struct timeval *tv);
PRTE_EXPORT int prte_event_del(struct event *ev);
PRTE_EXPORT void prte_event_active(struct event *ev, int res, short ncalls);
PRTE_EXPORT void prte_event_base_loopexit(prte_event_base_t *b);
#else
#    define prte_event_add(ev, tv)      event_add((ev), (tv))
#    define prte_event_del(ev)          event_del((ev))
#    define prte_event_active(x, y, z)  event_active((x), (y), (z))
#    define prte_event_base_loopexit(b) event_base_loopexit(b, NULL)

#endif

PRTE_EXPORT prte_event_t *prte_event_new(prte_event_base_t *b, int fd, short fg,
                                         event_callback_fn cbfn, void *cbd);

/* Timer APIs */
#define prte_event_evtimer_new(b, cb, arg) prte_event_new((b), -1, 0, (cb), (arg))

#define prte_event_evtimer_add(x, tv) prte_event_add((x), (tv))

#define prte_event_evtimer_set(b, x, cb, arg) \
    prte_event_assign((x), (b), -1, 0, (event_callback_fn)(cb), (arg))

#define prte_event_evtimer_del(x) prte_event_del((x))

#define prte_event_evtimer_pending(x, tv) event_pending((x), EV_TIMEOUT, (tv))

#define prte_event_evtimer_initialized(x) event_initialized((x))

/* Signal APIs */
#define prte_event_signal_add(x, tv) event_add((x), (tv))

#define prte_event_signal_set(b, x, fd, cb, arg) \
    prte_event_assign((x), (b), (fd), EV_SIGNAL | EV_PERSIST, (event_callback_fn)(cb), (arg))

#define prte_event_signal_del(x) event_del((x))

#define prte_event_signal_pending(x, tv) event_pending((x), EV_SIGNAL, (tv))

#define prte_event_signal_initalized(x) event_initialized((x))

#define prte_event_loop(b, fg) event_base_loop((b), (fg))

typedef struct {
    prte_list_item_t super;
    prte_event_t ev;
} prte_event_list_item_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_event_list_item_t);

END_C_DECLS

#endif /* PRTE_EVENT_H_ */
