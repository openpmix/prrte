/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * I/O Forwarding Service
 */

#ifndef MCA_IOF_BASE_H
#define MCA_IOF_BASE_H

#include "prrte_config.h"
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>

#include "src/class/prrte_list.h"
#include "src/class/prrte_bitmap.h"
#include "src/mca/mca.h"
#include "src/event/event-internal.h"
#include "src/util/fd.h"

#include "src/mca/iof/iof.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rml/rml_types.h"
#include "src/threads/threads.h"
#include "src/mca/errmgr/errmgr.h"

BEGIN_C_DECLS

/*
 * MCA framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_iof_base_framework;
/*
 * Select an available component.
 */
PRRTE_EXPORT int prrte_iof_base_select(void);

/* track xon/xoff of processes */
typedef struct {
    prrte_object_t super;
    prrte_job_t *jdata;
    prrte_bitmap_t xoff;
} prrte_iof_job_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_job_t);

/*
 * Maximum size of single msg
 */
#define PRRTE_IOF_BASE_MSG_MAX           4096
#define PRRTE_IOF_BASE_TAG_MAX             50
#define PRRTE_IOF_BASE_TAGGED_OUT_MAX    8192
#define PRRTE_IOF_MAX_INPUT_BUFFERS        50

typedef struct {
    prrte_list_item_t super;
    bool pending;
    bool always_writable;
    prrte_event_t *ev;
    struct timeval tv;
    int fd;
    prrte_list_t outputs;
} prrte_iof_write_event_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_write_event_t);

typedef struct {
    prrte_list_item_t super;
    prrte_process_name_t name;
    prrte_process_name_t daemon;
    prrte_iof_tag_t tag;
    prrte_iof_write_event_t *wev;
    bool xoff;
    bool exclusive;
    bool closed;
} prrte_iof_sink_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_sink_t);

struct prrte_iof_proc_t;
typedef struct {
    prrte_object_t super;
    struct prrte_iof_proc_t *proc;
    prrte_event_t *ev;
    struct timeval tv;
    int fd;
    prrte_iof_tag_t tag;
    bool active;
    bool always_readable;
    prrte_iof_sink_t *sink;
} prrte_iof_read_event_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_read_event_t);

typedef struct {
    prrte_list_item_t super;
    prrte_process_name_t name;
    prrte_iof_sink_t *stdinev;
    prrte_iof_read_event_t *revstdout;
    prrte_iof_read_event_t *revstderr;
    prrte_list_t *subscribers;
    bool copy;
} prrte_iof_proc_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_proc_t);

typedef struct {
    prrte_list_item_t super;
    char data[PRRTE_IOF_BASE_TAGGED_OUT_MAX];
    int numbytes;
} prrte_iof_write_output_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_iof_write_output_t);

/* the iof globals struct */
struct prrte_iof_base_t {
    size_t                  output_limit;
    prrte_iof_sink_t         *iof_write_stdout;
    prrte_iof_sink_t         *iof_write_stderr;
    bool                    redirect_app_stderr_to_stdout;
};
typedef struct prrte_iof_base_t prrte_iof_base_t;

/* Write event macro's */

static inline bool
prrte_iof_base_fd_always_ready(int fd)
{
    return prrte_fd_is_regular(fd) ||
           (prrte_fd_is_chardev(fd) && !isatty(fd)) ||
           prrte_fd_is_blkdev(fd);
}

#define PRRTE_IOF_SINK_BLOCKSIZE (1024)

#define PRRTE_IOF_SINK_ACTIVATE(wev)                                     \
    do {                                                                \
        struct timeval *tv = NULL;                                      \
        wev->pending = true;                                            \
        PRRTE_POST_OBJECT(wev);                                          \
        if (wev->always_writable) {                                     \
            /* Regular is always write ready. Use timer to activate */  \
            tv = &wev->tv;                                        \
        }                                                               \
        if (prrte_event_add(wev->ev, tv)) {                              \
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);                         \
        }                                                               \
    } while(0);


/* define an output "sink", adding it to the provided
 * endpoint list for this proc */
#define PRRTE_IOF_SINK_DEFINE(snk, nm, fid, tg, wrthndlr)                \
    do {                                                                \
        prrte_iof_sink_t *ep;                                            \
        PRRTE_OUTPUT_VERBOSE((1,                                         \
                            prrte_iof_base_framework.framework_output,   \
                            "defining endpt: file %s line %d fd %d",    \
                            __FILE__, __LINE__, (fid)));                \
        ep = PRRTE_NEW(prrte_iof_sink_t);                                  \
        ep->name.jobid = (nm)->jobid;                                   \
        ep->name.vpid = (nm)->vpid;                                     \
        ep->tag = (tg);                                                 \
        if (0 <= (fid)) {                                               \
            ep->wev->fd = (fid);                                        \
            ep->wev->always_writable =                                  \
                    prrte_iof_base_fd_always_ready(fid);                 \
            if(ep->wev->always_writable) {                              \
                prrte_event_evtimer_set(prrte_event_base,                 \
                                       ep->wev->ev,  wrthndlr, ep);     \
            } else {                                                    \
                prrte_event_set(prrte_event_base,                         \
                               ep->wev->ev, ep->wev->fd,                \
                               PRRTE_EV_WRITE,                           \
                               wrthndlr, ep);                           \
            }                                                           \
            prrte_event_set_priority(ep->wev->ev, PRRTE_MSG_PRI);         \
        }                                                               \
        *(snk) = ep;                                                    \
        PRRTE_POST_OBJECT(ep);                                           \
    } while(0);

/* Read event macro's */
#define PRRTE_IOF_READ_ADDEV(rev)                                \
    do {                                                        \
        struct timeval *tv = NULL;                              \
        if (rev->always_readable) {                             \
            tv = &rev->tv;                                      \
        }                                                       \
        if (prrte_event_add(rev->ev, tv)) {                      \
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);                 \
        }                                                       \
    } while(0);

#define PRRTE_IOF_READ_ACTIVATE(rev)                             \
    do {                                                        \
        rev->active = true;                                     \
        PRRTE_POST_OBJECT(rev);                                  \
        PRRTE_IOF_READ_ADDEV(rev);                               \
    } while(0);


/* add list of structs that has name of proc + prrte_iof_tag_t - when
 * defining a read event, search list for proc, add flag to the tag.
 * when closing a read fd, find proc on list and zero out that flag
 * when all flags = 0, then iof is complete - set message event to
 * daemon processor indicating proc iof is terminated
 */
#define PRRTE_IOF_READ_EVENT(rv, p, fid, tg, cbfunc, actv)               \
    do {                                                                \
        prrte_iof_read_event_t *rev;                                     \
        PRRTE_OUTPUT_VERBOSE((1,                                         \
                            prrte_iof_base_framework.framework_output,   \
                            "%s defining read event for %s: %s %d",     \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            PRRTE_NAME_PRINT(&(p)->name),                \
                            __FILE__, __LINE__));                       \
        rev = PRRTE_NEW(prrte_iof_read_event_t);                           \
        PRRTE_RETAIN((p));                                                \
        rev->proc = (struct prrte_iof_proc_t*)(p);                       \
        rev->tag = (tg);                                                \
        rev->fd = (fid);                                                \
        rev->always_readable = prrte_iof_base_fd_always_ready(fid);      \
        *(rv) = rev;                                                    \
        if(rev->always_readable) {                                      \
            prrte_event_evtimer_set(prrte_event_base,                     \
                                   rev->ev, (cbfunc), rev);             \
        } else {                                                        \
            prrte_event_set(prrte_event_base,                             \
                           rev->ev, (fid),                              \
                           PRRTE_EV_READ,                                \
                           (cbfunc), rev);                              \
        }                                                               \
        prrte_event_set_priority(rev->ev, PRRTE_MSG_PRI);                 \
        if ((actv)) {                                                   \
            PRRTE_IOF_READ_ACTIVATE(rev)                                 \
        }                                                               \
    } while(0);


PRRTE_EXPORT int prrte_iof_base_flush(void);

PRRTE_EXPORT extern prrte_iof_base_t prrte_iof_base;

/* base functions */
PRRTE_EXPORT int prrte_iof_base_write_output(const prrte_process_name_t *name, prrte_iof_tag_t stream,
                                             const unsigned char *data, int numbytes,
                                             prrte_iof_write_event_t *channel);
PRRTE_EXPORT void prrte_iof_base_static_dump_output(prrte_iof_read_event_t *rev);
PRRTE_EXPORT void prrte_iof_base_write_handler(int fd, short event, void *cbdata);

END_C_DECLS

#endif /* MCA_IOF_BASE_H */
