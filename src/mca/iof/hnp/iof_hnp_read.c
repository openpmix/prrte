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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
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

#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"

#include "iof_hnp.h"

static void restart_stdin(int fd, short event, void *cbdata)
{
    prte_timer_t *tm = (prte_timer_t *) cbdata;

    PRTE_ACQUIRE_OBJECT(tm);

    if (NULL != prte_iof_hnp_component.stdinev && !prte_job_term_ordered
        && !prte_iof_hnp_component.stdinev->active) {
        PRTE_IOF_READ_ACTIVATE(prte_iof_hnp_component.stdinev);
    }

    /* if this was a timer callback, then release the timer */
    if (NULL != tm) {
        PRTE_RELEASE(tm);
    }
}

/* return true if we should read stdin from fd, false otherwise */
bool prte_iof_hnp_stdin_check(int fd)
{
#if defined(HAVE_TCGETPGRP)
    if (isatty(fd) && (getpgrp() != tcgetpgrp(fd))) {
        return false;
    }
#endif
    return true;
}

void prte_iof_hnp_stdin_cb(int fd, short event, void *cbdata)
{
    bool should_process;

    PRTE_ACQUIRE_OBJECT(prte_iof_hnp_component.stdinev);

    should_process = prte_iof_hnp_stdin_check(0);

    if (should_process) {
        PRTE_IOF_READ_ACTIVATE(prte_iof_hnp_component.stdinev);
    } else {

        prte_event_del(prte_iof_hnp_component.stdinev->ev);
        prte_iof_hnp_component.stdinev->active = false;
    }
}

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lk = (prte_pmix_lock_t *) cbdata;

    PRTE_POST_OBJECT(lk);
    lk->status = prte_pmix_convert_status(status);
    PRTE_PMIX_WAKEUP_THREAD(lk);
}

/* this is the read handler for my own child procs. In this case,
 * the data is going nowhere - I just output it myself
 */
void prte_iof_hnp_read_local_handler(int fd, short event, void *cbdata)
{
    prte_iof_read_event_t *rev = (prte_iof_read_event_t *) cbdata;
    unsigned char data[PRTE_IOF_BASE_MSG_MAX];
    int32_t numbytes;
    prte_iof_proc_t *proct = (prte_iof_proc_t *) rev->proc;
    int rc;
    bool exclusive;
    prte_iof_sink_t *sink;

    PRTE_ACQUIRE_OBJECT(rev);

    /* As we may use timer events, fd can be bogus (-1)
     * use the right one here
     */
    fd = rev->fd;

    /* read up to the fragment size */
    memset(data, 0, PRTE_IOF_BASE_MSG_MAX);
    numbytes = read(fd, data, sizeof(data));

    if (NULL == proct) {
        /* this is an error - nothing we can do */
        PRTE_ERROR_LOG(PRTE_ERR_ADDRESSEE_UNKNOWN);
        return;
    }

    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */

        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            PRTE_IOF_READ_ACTIVATE(rev);
            return;
        }

        PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                             "%s iof:hnp:read handler %s Error on connection:%d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proct->name),
                             fd));
        /* Un-recoverable error. Allow the code to flow as usual in order to
         * to send the zero bytes message up the stream, and then close the
         * file descriptor and delete the event.
         */
        numbytes = 0;
    }

    /* is this read from our stdin? */
    if (PRTE_IOF_STDIN & rev->tag) {
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
        if (prte_job_term_ordered) {
            PRTE_RELEASE(rev);
            return;
        }
        /* if the daemon is me, then this is a local sink */
        if (PMIX_CHECK_PROCID(PRTE_PROC_MY_NAME, &proct->stdinev->daemon)) {
            PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                 "%s read %d bytes from stdin - writing to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
                                 PRTE_NAME_PRINT(&proct->name)));
            /* send the bytes down the pipe - we even send 0 byte events
             * down the pipe so it forces out any preceding data before
             * closing the output stream
             */
            if (NULL != proct->stdinev->wev) {
                if (PRTE_IOF_MAX_INPUT_BUFFERS < prte_iof_base_write_output(&proct->name, rev->tag,
                                                                            data, numbytes,
                                                                            proct->stdinev->wev)) {
                    /* getting too backed up - stop the read event for now if it is still active */

                    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                         "buffer backed up - holding"));
                    return;
                }
            }
        } else {
            PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                 "%s sending %d bytes from stdinev to daemon %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
                                 PRTE_NAME_PRINT(&proct->stdinev->daemon)));

            /* send the data to the daemon so it can
             * write it to the proc's fd - in this case,
             * we pass sink->name to indicate who is to
             * receive the data. If the connection closed,
             * numbytes will be zero so zero bytes will be
             * sent - this will tell the daemon to close
             * the fd for stdin to that proc
             */
            if (PRTE_SUCCESS
                != (rc = prte_iof_hnp_send_data_to_endpoint(&proct->stdinev->daemon,
                                                            &proct->stdinev->name, PRTE_IOF_STDIN,
                                                            data, numbytes))) {
                /* if the addressee is unknown, remove the sink from the list */
                if (PRTE_ERR_ADDRESSEE_UNKNOWN == rc) {
                    PRTE_RELEASE(rev->sink);
                }
            }
        }

        /* if num_bytes was zero, or we read the last piece of the file, then we need to terminate
         * the event */
        if (0 == numbytes) {
            if (0 != prte_list_get_size(&proct->stdinev->wev->outputs)) {
                /* some stuff has yet to be written, so delay the release of proct->stdinev */
                proct->stdinev->closed = true;
            } else {
                /* this will also close our stdin file descriptor */
                PRTE_RELEASE(proct->stdinev);
            }
        } else {
            /* if we are looking at a tty, then we just go ahead and restart the
             * read event assuming we are not backgrounded
             */
            if (prte_iof_hnp_stdin_check(fd)) {
                restart_stdin(fd, 0, NULL);
            } else {
                /* delay for awhile and then restart */
                PRTE_TIMER_EVENT(0, 10000, restart_stdin, PRTE_INFO_PRI);
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
        PRTE_LIST_FOREACH(sink, proct->subscribers, prte_iof_sink_t)
        {
            /* if the target isn't set, then this sink is for another purpose - ignore it */
            if (PMIX_NSPACE_INVALID(sink->daemon.nspace)) {
                continue;
            }
            if ((sink->tag & rev->tag) && PMIX_CHECK_NSPACE(sink->name.nspace, proct->name.nspace)
                && PMIX_CHECK_RANK(sink->name.rank, proct->name.rank)) {
                /* need to send the data to the remote endpoint - if
                 * the connection closed, numbytes will be zero, so
                 * the remote endpoint will know to close its local fd.
                 * In this case, we pass rev->name to indicate who the
                 * data came from.
                 */
                PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                     "%s sending data from proc %s of size %d via PMIx to tool %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proct->name), (int) numbytes,
                                     PRTE_NAME_PRINT(&sink->daemon)));
                /* don't pass down zero byte blobs */
                if (0 < numbytes) {
                    pmix_byte_object_t bo;
                    pmix_iof_channel_t pchan;
                    prte_pmix_lock_t lock;
                    pmix_status_t prc;
                    pchan = 0;
                    if (PRTE_IOF_STDIN & rev->tag) {
                        pchan |= PMIX_FWD_STDIN_CHANNEL;
                    }
                    if (PRTE_IOF_STDOUT & rev->tag) {
                        pchan |= PMIX_FWD_STDOUT_CHANNEL;
                    }
                    if (PRTE_IOF_STDERR & rev->tag) {
                        pchan |= PMIX_FWD_STDERR_CHANNEL;
                    }
                    if (PRTE_IOF_STDDIAG & rev->tag) {
                        pchan |= PMIX_FWD_STDDIAG_CHANNEL;
                    }
                    /* setup the byte object */
                    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
                    bo.bytes = (char *) data;
                    bo.size = numbytes;
                    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
                    prc = PMIx_server_IOF_deliver(&proct->name, pchan, &bo, NULL, 0, lkcbfunc,
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

    PRTE_OUTPUT_VERBOSE(
        (1, prte_iof_base_framework.framework_output, "%s read %d bytes from %s of %s",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
         (PRTE_IOF_STDOUT & rev->tag) ? "stdout"
                                      : ((PRTE_IOF_STDERR & rev->tag) ? "stderr" : "stddiag"),
         PRTE_NAME_PRINT(&proct->name)));

    if (0 == numbytes) {
        /* if we read 0 bytes from the stdout/err/diag, there is
         * nothing to output - release the appropriate event.
         * This will delete the read event and close the file descriptor */
        /* make sure we don't do recursive delete on the proct */
        PRTE_RETAIN(proct);
        if (rev->tag & PRTE_IOF_STDOUT) {
            prte_iof_base_static_dump_output(proct->revstdout);
            PRTE_RELEASE(proct->revstdout);
            proct->revstdout = NULL;
        } else if (rev->tag & PRTE_IOF_STDERR) {
            prte_iof_base_static_dump_output(proct->revstderr);
            PRTE_RELEASE(proct->revstderr);
            proct->revstderr = NULL;
        }
        /* check to see if they are all done */
        if (NULL == proct->revstdout && NULL == proct->revstderr) {
            /* this proc's iof is complete */
            PRTE_ACTIVATE_PROC_STATE(&proct->name, PRTE_PROC_STATE_IOF_COMPLETE);
        }
        PRTE_RELEASE(proct);
        return;
    }
    if (proct->copy) {
        if (NULL != proct->subscribers) {
            if (!exclusive) {
                /* output this to our local output */
                if (PRTE_IOF_STDOUT & rev->tag) {
                    prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes,
                                               prte_iof_base.iof_write_stdout->wev);
                } else {
                    prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes,
                                               prte_iof_base.iof_write_stderr->wev);
                }
            }
        } else {
            /* output this to our local output */
            if (PRTE_IOF_STDOUT & rev->tag) {
                prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes,
                                           prte_iof_base.iof_write_stdout->wev);
            } else {
                prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes,
                                           prte_iof_base.iof_write_stderr->wev);
            }
        }
    }
    /* see if the user wanted the output directed to files */
    if (NULL != rev->sink && !(PRTE_IOF_STDIN & rev->sink->tag)) {
        /* output to the corresponding file */
        prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, rev->sink->wev);
    }

    /* re-add the event */
    PRTE_IOF_READ_ACTIVATE(rev);
    return;
}
