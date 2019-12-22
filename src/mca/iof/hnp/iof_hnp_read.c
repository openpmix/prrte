/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
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

#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/odls_types.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

#include "iof_hnp.h"

static void restart_stdin(int fd, short event, void *cbdata)
{
    prrte_timer_t *tm = (prrte_timer_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(tm);

    if (NULL != prrte_iof_hnp_component.stdinev &&
        !prrte_job_term_ordered &&
        !prrte_iof_hnp_component.stdinev->active) {
        PRRTE_IOF_READ_ACTIVATE(prrte_iof_hnp_component.stdinev);
    }

    /* if this was a timer callback, then release the timer */
    if (NULL != tm) {
        PRRTE_RELEASE(tm);
    }
}

/* return true if we should read stdin from fd, false otherwise */
bool prrte_iof_hnp_stdin_check(int fd)
{
#if defined(HAVE_TCGETPGRP)
    if( isatty(fd) && (getpgrp() != tcgetpgrp(fd)) ) {
        return false;
    }
#endif
    return true;
}

void prrte_iof_hnp_stdin_cb(int fd, short event, void *cbdata)
{
    bool should_process;

    PRRTE_ACQUIRE_OBJECT(prrte_iof_hnp_component.stdinev);

    should_process = prrte_iof_hnp_stdin_check(0);

    if (should_process) {
        PRRTE_IOF_READ_ACTIVATE(prrte_iof_hnp_component.stdinev);
    } else {

        prrte_event_del(prrte_iof_hnp_component.stdinev->ev);
        prrte_iof_hnp_component.stdinev->active = false;
    }
}

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_POST_OBJECT(lk);
    lk->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lk);
}

/* this is the read handler for my own child procs. In this case,
 * the data is going nowhere - I just output it myself
 */
void prrte_iof_hnp_read_local_handler(int fd, short event, void *cbdata)
{
    prrte_iof_read_event_t *rev = (prrte_iof_read_event_t*)cbdata;
    unsigned char data[PRRTE_IOF_BASE_MSG_MAX];
    int32_t numbytes;
    prrte_iof_proc_t *proct = (prrte_iof_proc_t*)rev->proc;
    int rc;
    prrte_ns_cmp_bitmask_t mask=PRRTE_NS_CMP_ALL;
    bool exclusive;
    prrte_iof_sink_t *sink;

    PRRTE_ACQUIRE_OBJECT(rev);

    /* As we may use timer events, fd can be bogus (-1)
     * use the right one here
     */
    fd = rev->fd;

    /* read up to the fragment size */
    memset(data, 0, PRRTE_IOF_BASE_MSG_MAX);
    numbytes = read(fd, data, sizeof(data));

    if (NULL == proct) {
        /* this is an error - nothing we can do */
        PRRTE_ERROR_LOG(PRRTE_ERR_ADDRESSEE_UNKNOWN);
        return;
    }

    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */

        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            PRRTE_IOF_READ_ACTIVATE(rev);
            return;
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s iof:hnp:read handler %s Error on connection:%d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proct->name), fd));
        /* Un-recoverable error. Allow the code to flow as usual in order to
         * to send the zero bytes message up the stream, and then close the
         * file descriptor and delete the event.
         */
        numbytes = 0;
    }

    /* is this read from our stdin? */
    if (PRRTE_IOF_STDIN & rev->tag) {
        /* The event has fired, so it's no longer active until we
           re-add it */
        rev->active = false;
        if (NULL == proct->stdinev) {
            /* nothing further to do */
            return;
        }

        /* if job termination has been ordered, just ignore the
         * data and delete the read event
         */
        if (prrte_job_term_ordered) {
            PRRTE_RELEASE(rev);
            return;
        }
        /* if the daemon is me, then this is a local sink */
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, PRRTE_PROC_MY_NAME, &proct->stdinev->daemon)) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                 "%s read %d bytes from stdin - writing to %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                                 PRRTE_NAME_PRINT(&proct->name)));
            /* send the bytes down the pipe - we even send 0 byte events
             * down the pipe so it forces out any preceding data before
             * closing the output stream
             */
            if (NULL != proct->stdinev->wev) {
                if (PRRTE_IOF_MAX_INPUT_BUFFERS < prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, proct->stdinev->wev)) {
                    /* getting too backed up - stop the read event for now if it is still active */

                    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                         "buffer backed up - holding"));
                    return;
                }
            }
        } else {
            PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                 "%s sending %d bytes from stdinev to daemon %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                                 PRRTE_NAME_PRINT(&proct->stdinev->daemon)));

            /* send the data to the daemon so it can
             * write it to the proc's fd - in this case,
             * we pass sink->name to indicate who is to
             * receive the data. If the connection closed,
             * numbytes will be zero so zero bytes will be
             * sent - this will tell the daemon to close
             * the fd for stdin to that proc
             */
            if( PRRTE_SUCCESS != (rc = prrte_iof_hnp_send_data_to_endpoint(&proct->stdinev->daemon, &proct->stdinev->name, PRRTE_IOF_STDIN, data, numbytes))) {
                /* if the addressee is unknown, remove the sink from the list */
                if( PRRTE_ERR_ADDRESSEE_UNKNOWN == rc ) {
                    PRRTE_RELEASE(rev->sink);
                }
            }
        }

        /* if num_bytes was zero, or we read the last piece of the file, then we need to terminate the event */
        if (0 == numbytes) {
            if (0 != prrte_list_get_size(&proct->stdinev->wev->outputs)) {
                /* some stuff has yet to be written, so delay the release of proct->stdinev */
                proct->stdinev->closed = true;
            } else {
                /* this will also close our stdin file descriptor */
                PRRTE_RELEASE(proct->stdinev);
            }
        } else {
            /* if we are looking at a tty, then we just go ahead and restart the
             * read event assuming we are not backgrounded
             */
            if (prrte_iof_hnp_stdin_check(fd)) {
                restart_stdin(fd, 0, NULL);
            } else {
                /* delay for awhile and then restart */
                PRRTE_TIMER_EVENT(0, 10000, restart_stdin, PRRTE_INFO_PRI);
            }
        }
        /* nothing more to do */
        return;
    }

    /* this must be output from one of my local procs - see
     * if anyone else has requested a copy of this info. If
     * we were directed to put it into a file, then
     */
    exclusive = false;
    if (NULL != proct->subscribers) {
        PRRTE_LIST_FOREACH(sink, proct->subscribers, prrte_iof_sink_t) {
            /* if the target isn't set, then this sink is for another purpose - ignore it */
            if (PRRTE_JOBID_INVALID == sink->daemon.jobid) {
                continue;
            }
            if ((sink->tag & rev->tag) &&
                sink->name.jobid == proct->name.jobid &&
                (PRRTE_VPID_WILDCARD == sink->name.vpid || sink->name.vpid == proct->name.vpid)) {
                /* need to send the data to the remote endpoint - if
                 * the connection closed, numbytes will be zero, so
                 * the remote endpoint will know to close its local fd.
                 * In this case, we pass rev->name to indicate who the
                 * data came from.
                 */
                PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                     "%s sending data from proc %s of size %d via PMIx to tool %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proct->name), (int)numbytes,
                                     PRRTE_NAME_PRINT(&sink->daemon)));
                /* don't pass down zero byte blobs */
                if (0 < numbytes) {
                    pmix_proc_t source;
                    pmix_byte_object_t bo;
                    pmix_iof_channel_t pchan;
                    prrte_pmix_lock_t lock;
                    pmix_status_t prc;
                    PRRTE_PMIX_CONVERT_NAME(&source, &proct->name);
                    pchan = 0;
                    if (PRRTE_IOF_STDIN & rev->tag) {
                        pchan |= PMIX_FWD_STDIN_CHANNEL;
                    }
                    if (PRRTE_IOF_STDOUT & rev->tag) {
                        pchan |= PMIX_FWD_STDOUT_CHANNEL;
                    }
                    if (PRRTE_IOF_STDERR & rev->tag) {
                        pchan |= PMIX_FWD_STDERR_CHANNEL;
                    }
                    if (PRRTE_IOF_STDDIAG & rev->tag) {
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

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s read %d bytes from %s of %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                         (PRRTE_IOF_STDOUT & rev->tag) ? "stdout" : ((PRRTE_IOF_STDERR & rev->tag) ? "stderr" : "stddiag"),
                         PRRTE_NAME_PRINT(&proct->name)));

    if (0 == numbytes) {
        /* if we read 0 bytes from the stdout/err/diag, there is
         * nothing to output - release the appropriate event.
         * This will delete the read event and close the file descriptor */
        /* make sure we don't do recursive delete on the proct */
        PRRTE_RETAIN(proct);
        if (rev->tag & PRRTE_IOF_STDOUT) {
            prrte_iof_base_static_dump_output(proct->revstdout);
            PRRTE_RELEASE(proct->revstdout);
            proct->revstdout = NULL;
        } else if (rev->tag & PRRTE_IOF_STDERR) {
            prrte_iof_base_static_dump_output(proct->revstderr);
            PRRTE_RELEASE(proct->revstderr);
            proct->revstderr = NULL;
        }
        /* check to see if they are all done */
        if (NULL == proct->revstdout &&
            NULL == proct->revstderr) {
            /* this proc's iof is complete */
            PRRTE_ACTIVATE_PROC_STATE(&proct->name, PRRTE_PROC_STATE_IOF_COMPLETE);
        }
        PRRTE_RELEASE(proct);
        return;
    }

    if (proct->copy) {
        if (NULL != proct->subscribers) {
            if (!exclusive) {
                /* output this to our local output */
                if (PRRTE_IOF_STDOUT & rev->tag || prrte_xml_output) {
                    prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, prrte_iof_base.iof_write_stdout->wev);
                } else {
                    prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, prrte_iof_base.iof_write_stderr->wev);
                }
            }
        } else {
            /* output this to our local output */
            if (PRRTE_IOF_STDOUT & rev->tag || prrte_xml_output) {
                prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, prrte_iof_base.iof_write_stdout->wev);
            } else {
                prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, prrte_iof_base.iof_write_stderr->wev);
            }
        }
    }
    /* see if the user wanted the output directed to files */
    if (NULL != rev->sink && !(PRRTE_IOF_STDIN & rev->sink->tag)) {
        /* output to the corresponding file */
        prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, rev->sink->wev);
    }

    /* re-add the event */
    PRRTE_IOF_READ_ACTIVATE(rev);
    return;
}
