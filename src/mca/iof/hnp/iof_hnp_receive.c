/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#else
#    ifdef HAVE_SYS_FCNTL_H
#        include <sys/fcntl.h>
#    endif
#endif

#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"

#include "iof_hnp.h"

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t *) cbdata;

    PRTE_POST_OBJECT(lk);
    lk->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lk);
}

void prte_iof_hnp_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                       prte_rml_tag_t tag, void *cbdata)
{
    pmix_proc_t origin, requestor;
    unsigned char data[PRTE_IOF_BASE_MSG_MAX];
    prte_iof_tag_t stream;
    int32_t count, numbytes;
    prte_iof_sink_t *sink, *next;
    int rc;
    bool exclusive;
    prte_iof_proc_t *proct;
    prte_iof_request_t *preq;

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s received IOF msg from proc %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* unpack the stream first as this may be flow control info */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stream, &count, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    if (PRTE_IOF_XON & stream) {
        /* re-start the stdin read event */
        if (NULL != prte_iof_hnp_component.stdinev && !prte_job_term_ordered
            && !prte_iof_hnp_component.stdinev->active) {
            PRTE_IOF_READ_ACTIVATE(prte_iof_hnp_component.stdinev);
        }
        goto CLEAN_RETURN;
    } else if (PRTE_IOF_XOFF & stream) {
        /* stop the stdin read event */
        if (NULL != prte_iof_hnp_component.stdinev && !prte_iof_hnp_component.stdinev->active) {
            prte_event_del(prte_iof_hnp_component.stdinev->ev);
            prte_iof_hnp_component.stdinev->active = false;
        }
        goto CLEAN_RETURN;
    }

    /* get name of the process whose io we are discussing */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &origin, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s received IOF cmd for source %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(&origin)));

    /* check to see if a tool has requested something */
    if (PRTE_IOF_PULL & stream) {
        /* get name of the process wishing to be the sink */
        count = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto CLEAN_RETURN;
        }

        PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                             "%s received pull cmd from remote tool %s for proc %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&requestor),
                             PRTE_NAME_PRINT(&origin)));

        if (PRTE_IOF_EXCLUSIVE & stream) {
            exclusive = true;
        } else {
            exclusive = false;
        }
        /* there is a race condition between when a requestor might pull
         * a stream and when the procs might be launched, so store the
         * request so we can check all newly-spawned procs against it
         * later */
        preq = PRTE_NEW(prte_iof_request_t);
        PMIX_XFER_PROCID(&preq->requestor, &requestor);
        PMIX_XFER_PROCID(&preq->target, &origin);
        preq->stream = stream;
        prte_list_append(&prte_iof_base.requests, &preq->super);

        /* do we already have this process in our list? */
        PRTE_LIST_FOREACH(proct, &prte_iof_hnp_component.procs, prte_iof_proc_t)
        {
            if (PMIX_CHECK_PROCID(&proct->name, &origin)) {
                /* a tool is requesting that we send it a copy of the specified stream(s)
                 * from the specified process(es), so create a sink for it
                 */
                if (NULL == proct->subscribers) {
                    proct->subscribers = PRTE_NEW(prte_list_t);
                }
                if (PRTE_IOF_STDOUT & stream) {
                    PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDOUT, NULL);
                    PMIX_XFER_PROCID(&sink->daemon, &requestor);
                    sink->exclusive = exclusive;
                    prte_list_append(proct->subscribers, &sink->super);
                }
                if (PRTE_IOF_STDERR & stream) {
                    PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDERR, NULL);
                    PMIX_XFER_PROCID(&sink->daemon, &requestor);
                    sink->exclusive = exclusive;
                    prte_list_append(proct->subscribers, &sink->super);
                }
                if (PRTE_IOF_STDDIAG & stream) {
                    PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDDIAG, NULL);
                    PMIX_XFER_PROCID(&sink->daemon, &requestor);
                    sink->exclusive = exclusive;
                    prte_list_append(proct->subscribers, &sink->super);
                }
            }
        }
        goto CLEAN_RETURN;
    }

    if (PRTE_IOF_CLOSE & stream) {
        PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                             "%s received close cmd from remote tool %s for proc %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender),
                             PRTE_NAME_PRINT(&origin)));
        /* a tool is requesting that we no longer forward a copy of the
         * specified stream(s) from the specified process(es) - remove the sink
         */
        PRTE_LIST_FOREACH(proct, &prte_iof_hnp_component.procs, prte_iof_proc_t)
        {
            if (!PMIX_CHECK_PROCID(&proct->name, &origin)) {
                continue;
            }
            PRTE_LIST_FOREACH_SAFE(sink, next, proct->subscribers, prte_iof_sink_t)
            {
                /* if the target isn't set, then this sink is for another purpose - ignore it */
                if (PMIX_NSPACE_INVALID(sink->daemon.nspace)) {
                    continue;
                }
                /* if this sink is the designated one, then remove it from list */
                if ((stream & sink->tag) && PMIX_CHECK_PROCID(&sink->name, &origin)) {
                    /* send an ack message to the requestor - this ensures that the RML has
                     * completed sending anything to that requestor before it exits
                     */
                    prte_iof_hnp_send_data_to_endpoint(&sink->daemon, &origin, PRTE_IOF_CLOSE, NULL,
                                                       0);
                    prte_list_remove_item(proct->subscribers, &sink->super);
                    PRTE_RELEASE(sink);
                }
            }
        }
        goto CLEAN_RETURN;
    }

    /* this must have come from a daemon forwarding output - unpack the data */
    numbytes = PRTE_IOF_BASE_MSG_MAX;
    rc = PMIx_Data_unpack(NULL, buffer, data, &numbytes, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }
    /* numbytes will contain the actual #bytes that were sent */

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s unpacked %d bytes from remote proc %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes, PRTE_NAME_PRINT(&origin)));

    /* do we already have this process in our list? */
    PRTE_LIST_FOREACH(proct, &prte_iof_hnp_component.procs, prte_iof_proc_t)
    {
        if (PMIX_CHECK_PROCID(&proct->name, &origin)) {
            /* found it */
            goto NSTEP;
        }
    }

    /* if we get here, then we don't yet have this proc in our list */
    proct = PRTE_NEW(prte_iof_proc_t);
    PMIX_XFER_PROCID(&proct->name, &origin);
    prte_list_append(&prte_iof_hnp_component.procs, &proct->super);
    prte_iof_base_check_target(proct);

NSTEP:
    /* cycle through the endpoints to see if someone else wants a copy */
    exclusive = false;
    if (NULL != proct->subscribers) {
        PRTE_LIST_FOREACH(sink, proct->subscribers, prte_iof_sink_t)
        {
            /* if the target isn't set, then this sink is for another purpose - ignore it */
            if (PMIX_NSPACE_INVALID(sink->daemon.nspace)) {
                continue;
            }
            if ((stream & sink->tag) && PMIX_CHECK_PROCID(&sink->name, &origin)) {
                /* send the data to the tool */
                /* don't pass along zero byte blobs */
                if (0 < numbytes) {
                    PRTE_OUTPUT_VERBOSE(
                        (1, prte_iof_base_framework.framework_output,
                         "%s sending data from proc %s of size %d via PMIx to tool %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&origin),
                         (int) numbytes, PRTE_NAME_PRINT(&sink->daemon)));
                    pmix_byte_object_t bo;
                    pmix_iof_channel_t pchan;
                    prte_pmix_lock_t lock;
                    pmix_status_t prc;
                    pchan = 0;
                    if (PRTE_IOF_STDIN & stream) {
                        pchan |= PMIX_FWD_STDIN_CHANNEL;
                    }
                    if (PRTE_IOF_STDOUT & stream) {
                        pchan |= PMIX_FWD_STDOUT_CHANNEL;
                    }
                    if (PRTE_IOF_STDERR & stream) {
                        pchan |= PMIX_FWD_STDERR_CHANNEL;
                    }
                    if (PRTE_IOF_STDDIAG & stream) {
                        pchan |= PMIX_FWD_STDDIAG_CHANNEL;
                    }
                    /* setup the byte object */
                    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
                    bo.bytes = (char *) data;
                    bo.size = numbytes;
                    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
                    prc = PMIx_server_IOF_deliver(&origin, pchan, &bo, NULL, 0, lkcbfunc,
                                                  (void *) &lock);
                    if (PMIX_SUCCESS != prc) {
                        PMIX_ERROR_LOG(prc);
                    } else {
                        /* wait for completion */
                        PRTE_PMIX_WAIT_THREAD(&lock);
                    }
                    PRTE_PMIX_DESTRUCT_LOCK(&lock);
                }
                if (sink->exclusive) {
                    exclusive = true;
                }
            }
        }
    }
    /* if the user doesn't want a copy written to the screen, then we are done */
    if (!proct->copy) {
        return;
    }

    /* output this to our local output unless one of the sinks was exclusive */
    if (!exclusive) {
        if (PRTE_IOF_STDOUT & stream) {
            prte_iof_base_write_output(&origin, stream, data, numbytes,
                                       prte_iof_base.iof_write_stdout->wev);
        } else {
            prte_iof_base_write_output(&origin, stream, data, numbytes,
                                       prte_iof_base.iof_write_stderr->wev);
        }
    }

CLEAN_RETURN:
    return;
}
