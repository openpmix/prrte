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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#else
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#endif

#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

#include "iof_hnp.h"

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_POST_OBJECT(lk);
    lk->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lk);
}

void prrte_iof_hnp_recv(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata)
{
    prrte_process_name_t origin, requestor;
    unsigned char data[PRRTE_IOF_BASE_MSG_MAX];
    prrte_iof_tag_t stream;
    int32_t count, numbytes;
    prrte_iof_sink_t *sink, *next;
    int rc;
    bool exclusive;
    prrte_iof_proc_t *proct;
    prrte_ns_cmp_bitmask_t mask=PRRTE_NS_CMP_ALL | PRRTE_NS_CMP_WILD;

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s received IOF from proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(sender)));

    /* unpack the stream first as this may be flow control info */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &stream, &count, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    if (PRRTE_IOF_XON & stream) {
        /* re-start the stdin read event */
        if (NULL != prrte_iof_hnp_component.stdinev &&
            !prrte_job_term_ordered &&
            !prrte_iof_hnp_component.stdinev->active) {
            PRRTE_IOF_READ_ACTIVATE(prrte_iof_hnp_component.stdinev);
        }
        goto CLEAN_RETURN;
    } else if (PRRTE_IOF_XOFF & stream) {
        /* stop the stdin read event */
        if (NULL != prrte_iof_hnp_component.stdinev &&
            !prrte_iof_hnp_component.stdinev->active) {
            prrte_event_del(prrte_iof_hnp_component.stdinev->ev);
            prrte_iof_hnp_component.stdinev->active = false;
        }
        goto CLEAN_RETURN;
    }

    /* get name of the process whose io we are discussing */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &origin, &count, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s received IOF cmd for source %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(&origin)));

    /* check to see if a tool has requested something */
    if (PRRTE_IOF_PULL & stream) {
        /* get name of the process wishing to be the sink */
        count = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &requestor, &count, PRRTE_NAME))) {
            PRRTE_ERROR_LOG(rc);
            goto CLEAN_RETURN;
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s received pull cmd from remote tool %s for proc %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&requestor),
                             PRRTE_NAME_PRINT(&origin)));

        if (PRRTE_IOF_EXCLUSIVE & stream) {
            exclusive = true;
        } else {
            exclusive = false;
        }
        /* do we already have this process in our list? */
        PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
            if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, &origin)) {
                /* found it */
                goto PROCESS;
            }
        }
        /* if we get here, then we don't yet have this proc in our list */
        proct = PRRTE_NEW(prrte_iof_proc_t);
        proct->name.jobid = origin.jobid;
        proct->name.vpid = origin.vpid;
        prrte_list_append(&prrte_iof_hnp_component.procs, &proct->super);

      PROCESS:
        /* a tool is requesting that we send it a copy of the specified stream(s)
         * from the specified process(es), so create a sink for it
         */
        if (NULL == proct->subscribers) {
            proct->subscribers = PRRTE_NEW(prrte_list_t);
        }
        if (PRRTE_IOF_STDOUT & stream) {
            PRRTE_IOF_SINK_DEFINE(&sink, &origin, -1, PRRTE_IOF_STDOUT, NULL);
            sink->daemon.jobid = requestor.jobid;
            sink->daemon.vpid = requestor.vpid;
            sink->exclusive = exclusive;
            prrte_list_append(proct->subscribers, &sink->super);
        }
        if (PRRTE_IOF_STDERR & stream) {
            PRRTE_IOF_SINK_DEFINE(&sink, &origin, -1, PRRTE_IOF_STDERR, NULL);
            sink->daemon.jobid = requestor.jobid;
            sink->daemon.vpid = requestor.vpid;
            sink->exclusive = exclusive;
            prrte_list_append(proct->subscribers, &sink->super);
        }
        if (PRRTE_IOF_STDDIAG & stream) {
            PRRTE_IOF_SINK_DEFINE(&sink, &origin, -1, PRRTE_IOF_STDDIAG, NULL);
            sink->daemon.jobid = requestor.jobid;
            sink->daemon.vpid = requestor.vpid;
            sink->exclusive = exclusive;
            prrte_list_append(proct->subscribers, &sink->super);
        }
        goto CLEAN_RETURN;
    }

    if (PRRTE_IOF_CLOSE & stream) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s received close cmd from remote tool %s for proc %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(sender),
                             PRRTE_NAME_PRINT(&origin)));
        /* a tool is requesting that we no longer forward a copy of the
         * specified stream(s) from the specified process(es) - remove the sink
         */
        PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
            if (PRRTE_EQUAL != prrte_util_compare_name_fields(mask, &proct->name, &origin)) {
                continue;
            }
            PRRTE_LIST_FOREACH_SAFE(sink, next, proct->subscribers, prrte_iof_sink_t) {
                 /* if the target isn't set, then this sink is for another purpose - ignore it */
                if (PRRTE_JOBID_INVALID == sink->daemon.jobid) {
                    continue;
                }
                /* if this sink is the designated one, then remove it from list */
                if ((stream & sink->tag) &&
                    sink->name.jobid == origin.jobid &&
                    (PRRTE_VPID_WILDCARD == sink->name.vpid ||
                     PRRTE_VPID_WILDCARD == origin.vpid ||
                     sink->name.vpid == origin.vpid)) {
                    /* send an ack message to the requestor - this ensures that the RML has
                     * completed sending anything to that requestor before it exits
                     */
                    prrte_iof_hnp_send_data_to_endpoint(&sink->daemon, &origin, PRRTE_IOF_CLOSE, NULL, 0);
                    prrte_list_remove_item(proct->subscribers, &sink->super);
                    PRRTE_RELEASE(sink);
                }
            }
        }
        goto CLEAN_RETURN;
    }

    /* this must have come from a daemon forwarding output - unpack the data */
    numbytes=PRRTE_IOF_BASE_MSG_MAX;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, data, &numbytes, PRRTE_BYTE))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }
    /* numbytes will contain the actual #bytes that were sent */

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s unpacked %d bytes from remote proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                         PRRTE_NAME_PRINT(&origin)));

    /* do we already have this process in our list? */
    PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, &origin)) {
            /* found it */
            goto NSTEP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = PRRTE_NEW(prrte_iof_proc_t);
    proct->name.jobid = origin.jobid;
    proct->name.vpid = origin.vpid;
    prrte_list_append(&prrte_iof_hnp_component.procs, &proct->super);

  NSTEP:
    /* cycle through the endpoints to see if someone else wants a copy */
    exclusive = false;
    if (NULL != proct->subscribers) {
        PRRTE_LIST_FOREACH(sink, proct->subscribers, prrte_iof_sink_t) {
            /* if the target isn't set, then this sink is for another purpose - ignore it */
            if (PRRTE_JOBID_INVALID == sink->daemon.jobid) {
                continue;
            }
            if ((stream & sink->tag) &&
                sink->name.jobid == origin.jobid &&
                (PRRTE_VPID_WILDCARD == sink->name.vpid ||
                 PRRTE_VPID_WILDCARD == origin.vpid ||
                 sink->name.vpid == origin.vpid)) {
                /* send the data to the tool */
                    /* don't pass along zero byte blobs */
                if (0 < numbytes) {
                    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                         "%s sending data from proc %s of size %d via PMIx to tool %s",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         PRRTE_NAME_PRINT(&origin), (int)numbytes,
                                         PRRTE_NAME_PRINT(&sink->daemon)));
                    pmix_proc_t source;
                    pmix_byte_object_t bo;
                    pmix_iof_channel_t pchan;
                    prrte_pmix_lock_t lock;
                    pmix_status_t prc;
                    PRRTE_PMIX_CONVERT_NAME(&source, &origin);
                    pchan = 0;
                    if (PRRTE_IOF_STDIN & stream) {
                        pchan |= PMIX_FWD_STDIN_CHANNEL;
                    }
                    if (PRRTE_IOF_STDOUT & stream) {
                        pchan |= PMIX_FWD_STDOUT_CHANNEL;
                    }
                    if (PRRTE_IOF_STDERR & stream) {
                        pchan |= PMIX_FWD_STDERR_CHANNEL;
                    }
                    if (PRRTE_IOF_STDDIAG & stream) {
                        pchan |= PMIX_FWD_STDDIAG_CHANNEL;
                    }
                    /* setup the byte object */
                    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
                    bo.bytes = (char*)data;
                    bo.size = numbytes;
                    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
                    prc = PMIx_server_IOF_deliver(&source, pchan, &bo, NULL, 0, lkcbfunc, (void*)&lock);
                    if (PMIX_SUCCESS != prc) {
                        PMIX_ERROR_LOG(prc);
                    } else {
                        /* wait for completion */
                        PRRTE_PMIX_WAIT_THREAD(&lock);
                    }
                    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
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
        if (PRRTE_IOF_STDOUT & stream || prrte_xml_output) {
            prrte_iof_base_write_output(&origin, stream, data, numbytes, prrte_iof_base.iof_write_stdout->wev);
        } else {
            prrte_iof_base_write_output(&origin, stream, data, numbytes, prrte_iof_base.iof_write_stderr->wev);
        }
    }

 CLEAN_RETURN:
    return;
}
