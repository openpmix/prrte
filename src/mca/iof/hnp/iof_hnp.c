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
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "prrte_config.h"
#include "src/util/output.h"
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

#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/rml/rml.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/mca/odls/odls_types.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "iof_hnp.h"

/* LOCAL FUNCTIONS */
static void stdin_write_handler(int fd, short event, void *cbdata);

/* API FUNCTIONS */
static int init(void);

static int hnp_push(const prrte_process_name_t* dst_name, prrte_iof_tag_t src_tag, int fd);

static int hnp_pull(const prrte_process_name_t* src_name,
                prrte_iof_tag_t src_tag,
                int fd);

static int hnp_close(const prrte_process_name_t* peer,
                 prrte_iof_tag_t source_tag);

static int hnp_output(const prrte_process_name_t* peer,
                      prrte_iof_tag_t source_tag,
                      const char *msg);

static void hnp_complete(const prrte_job_t *jdata);

static int finalize(void);

static int hnp_ft_event(int state);

static int push_stdin(const prrte_process_name_t* dst_name,
                      uint8_t *data, size_t sz);

/* The API's in this module are solely used to support LOCAL
 * procs - i.e., procs that are co-located to the HNP. Remote
 * procs interact with the HNP's IOF via the HNP's receive function,
 * which operates independently and is in the iof_hnp_receive.c file
 */

prrte_iof_base_module_t prrte_iof_hnp_module = {
    .init = init,
    .push = hnp_push,
    .pull = hnp_pull,
    .close = hnp_close,
    .output = hnp_output,
    .complete = hnp_complete,
    .finalize = finalize,
    .ft_event = hnp_ft_event,
    .push_stdin = push_stdin
};

/* Initialize the module */
static int init(void)
{
    /* post non-blocking recv to catch forwarded IO from
     * the orteds
     */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_IOF_HNP,
                            PRRTE_RML_PERSISTENT,
                            prrte_iof_hnp_recv,
                            NULL);

    PRRTE_CONSTRUCT(&prrte_iof_hnp_component.procs, prrte_list_t);
    prrte_iof_hnp_component.stdinev = NULL;

    return PRRTE_SUCCESS;
}

/* Setup to read local data. If the tag is other than STDIN,
 * then this is output being pushed from one of my child processes
 * and I'll write the data out myself. If the tag is STDIN,
 * then I need to setup to read from my stdin, and send anything
 * I get to the specified dst_name. The dst_name in this case tells
 * us which procs are to get stdin - only two options are supported:
 *
 * (a) a specific name, usually vpid=0; or
 *
 * (b) all procs, specified by vpid=PRRTE_VPID_WILDCARD
 *
 * The prrte_plm_base_launch_apps function calls iof.push after
 * the procs are launched and tells us how to distribute stdin. This
 * ensures that the procs are started -before- we begin reading stdin
 * and attempting to send it to remote procs
 */
static int hnp_push(const prrte_process_name_t* dst_name, prrte_iof_tag_t src_tag, int fd)
{
    prrte_job_t *jdata;
    prrte_iof_proc_t *proct, *pptr;
    int flags, rc;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;

    /* don't do this if the dst vpid is invalid or the fd is negative! */
    if (PRRTE_VPID_INVALID == dst_name->vpid || fd < 0) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:hnp pushing fd %d for process %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         fd, PRRTE_NAME_PRINT(dst_name)));

    /* do we already have this process in our list? */
    PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = PRRTE_NEW(prrte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    prrte_list_append(&prrte_iof_hnp_component.procs, &proct->super);

  SETUP:
    /* set the file descriptor to non-blocking - do this before we setup
     * and activate the read event in case it fires right away
     */
    if((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        prrte_output(prrte_iof_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                    __FILE__, __LINE__, errno);
    } else {
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }
    /* get the local jobdata for this proc */
    if (NULL == (jdata = prrte_get_job_data_object(proct->name.jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    /* define a read event and activate it */
    if (src_tag & PRRTE_IOF_STDOUT) {
        PRRTE_IOF_READ_EVENT(&proct->revstdout, proct, fd, PRRTE_IOF_STDOUT,
                            prrte_iof_hnp_read_local_handler, false);
    } else if (src_tag & PRRTE_IOF_STDERR) {
        PRRTE_IOF_READ_EVENT(&proct->revstderr, proct, fd, PRRTE_IOF_STDERR,
                            prrte_iof_hnp_read_local_handler, false);
    }
    /* setup any requested output files */
    if (PRRTE_SUCCESS != (rc = prrte_iof_base_setup_output_files(dst_name, jdata, proct))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* if -all- of the readevents for this proc have been defined, then
     * activate them. Otherwise, we can think that the proc is complete
     * because one of the readevents fires -prior- to all of them having
     * been defined!
     */
    if (NULL != proct->revstdout &&
        (prrte_iof_base.redirect_app_stderr_to_stdout || NULL != proct->revstderr)) {
        if (proct->copy) {
            /* see if there are any wildcard subscribers out there that
             * apply to us */
            PRRTE_LIST_FOREACH(pptr, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
                if (dst_name->jobid == pptr->name.jobid &&
                    PRRTE_VPID_WILDCARD == pptr->name.vpid &&
                    NULL != pptr->subscribers) {
                    PRRTE_RETAIN(pptr->subscribers);
                    proct->subscribers = pptr->subscribers;
                    break;
                }
            }
        }
        PRRTE_IOF_READ_ACTIVATE(proct->revstdout);
        if (!prrte_iof_base.redirect_app_stderr_to_stdout) {
            PRRTE_IOF_READ_ACTIVATE(proct->revstderr);
        }
    }
    return PRRTE_SUCCESS;
}

/* Push data to stdin of a client process
 *
 * (a) a specific name, usually vpid=0; or
 *
 * (b) all procs, specified by vpid=PRRTE_VPID_WILDCARD
 *
 */
static int push_stdin(const prrte_process_name_t* dst_name,
                      uint8_t *data, size_t sz)
{
    prrte_iof_proc_t *proct, *pptr;
    int rc;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;

    /* don't do this if the dst vpid is invalid */
    if (PRRTE_VPID_INVALID == dst_name->vpid) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                          "%s iof:hnp pushing stdin for process %s (size %zu)",
                          PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                          PRRTE_NAME_PRINT(dst_name),
                          sz));

    /* do we already have this process in our list? */
    proct = NULL;
    PRRTE_LIST_FOREACH(pptr, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &pptr->name, dst_name)) {
            /* found it */
            proct = pptr;
        }
    }
    if (NULL == proct) {
        return PRRTE_ERR_NOT_FOUND;
    }

    /* pass the data to the sink */

    /* if the daemon is me, then this is a local sink */
    if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, PRRTE_PROC_MY_NAME, &proct->stdinev->daemon)) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s read %d bytes from stdin - writing to %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)sz,
                             PRRTE_NAME_PRINT(&proct->name)));
        /* send the bytes down the pipe - we even send 0 byte events
         * down the pipe so it forces out any preceding data before
         * closing the output stream
         */
        if (NULL != proct->stdinev->wev) {
            if (PRRTE_IOF_MAX_INPUT_BUFFERS < prrte_iof_base_write_output(&proct->name, PRRTE_IOF_STDIN, data, sz, proct->stdinev->wev)) {
                /* getting too backed up - stop the read event for now if it is still active */

                PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                     "buffer backed up - holding"));
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
        }
    } else {
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s sending %d bytes from stdinev to daemon %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)sz,
                             PRRTE_NAME_PRINT(&proct->stdinev->daemon)));

        /* send the data to the daemon so it can
         * write it to the proc's fd - in this case,
         * we pass sink->name to indicate who is to
         * receive the data. If the connection closed,
         * numbytes will be zero so zero bytes will be
         * sent - this will tell the daemon to close
         * the fd for stdin to that proc
         */
        if( PRRTE_SUCCESS != (rc = prrte_iof_hnp_send_data_to_endpoint(&proct->stdinev->daemon, &proct->stdinev->name, PRRTE_IOF_STDIN, data, sz))) {
            /* if the addressee is unknown, remove the sink from the list */
            if( PRRTE_ERR_ADDRESSEE_UNKNOWN == rc ) {
                PRRTE_RELEASE(proct->stdinev);
            }
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * Since we are the HNP, the only "pull" call comes from a local
 * process so we can record the file descriptor for its stdin.
 */

static int hnp_pull(const prrte_process_name_t* dst_name,
                    prrte_iof_tag_t src_tag,
                    int fd)
{
    prrte_iof_proc_t *proct;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;
    int flags;

    /* this is a local call - only stdin is supported */
    if (PRRTE_IOF_STDIN != src_tag) {
        return PRRTE_ERR_NOT_SUPPORTED;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:hnp pulling fd %d for process %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         fd, PRRTE_NAME_PRINT(dst_name)));

    /* set the file descriptor to non-blocking - do this before we setup
     * the sink in case it fires right away
     */
    if((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        prrte_output(prrte_iof_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                    __FILE__, __LINE__, errno);
    } else {
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }

    /* do we already have this process in our list? */
    PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = PRRTE_NEW(prrte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    prrte_list_append(&prrte_iof_hnp_component.procs, &proct->super);

  SETUP:
    PRRTE_IOF_SINK_DEFINE(&proct->stdinev, dst_name, fd, PRRTE_IOF_STDIN,
                         stdin_write_handler);
    proct->stdinev->daemon.jobid = PRRTE_PROC_MY_NAME->jobid;
    proct->stdinev->daemon.vpid = PRRTE_PROC_MY_NAME->vpid;

    return PRRTE_SUCCESS;
}

/*
 * One of our local procs wants us to close the specifed
 * stream(s), thus terminating any potential io to/from it.
 */
static int hnp_close(const prrte_process_name_t* peer,
                     prrte_iof_tag_t source_tag)
{
    prrte_iof_proc_t* proct;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                          "%s iof:hnp closing connection to process %s",
                          PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                          PRRTE_NAME_PRINT(peer)));

    PRRTE_LIST_FOREACH(proct, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, peer)) {
            if (PRRTE_IOF_STDIN & source_tag) {
                if (NULL != proct->stdinev) {
                    PRRTE_RELEASE(proct->stdinev);
                }
                proct->stdinev = NULL;
            }
            if ((PRRTE_IOF_STDOUT & source_tag) ||
                (PRRTE_IOF_STDMERGE & source_tag)) {
                if (NULL != proct->revstdout) {
                    prrte_iof_base_static_dump_output(proct->revstdout);
                    PRRTE_RELEASE(proct->revstdout);
                }
                proct->revstdout = NULL;
            }
            if (PRRTE_IOF_STDERR & source_tag) {
                if (NULL != proct->revstderr) {
                    prrte_iof_base_static_dump_output(proct->revstderr);
                    PRRTE_RELEASE(proct->revstderr);
                }
                proct->revstderr = NULL;
            }
            /* if we closed them all, then remove this proc */
            if (NULL == proct->stdinev &&
                NULL == proct->revstdout &&
                NULL == proct->revstderr) {
                prrte_list_remove_item(&prrte_iof_hnp_component.procs, &proct->super);
                PRRTE_RELEASE(proct);
            }
            break;
        }
    }
    return PRRTE_SUCCESS;
}

static void hnp_complete(const prrte_job_t *jdata)
{
    prrte_iof_proc_t *proct, *next;

    /* cleanout any lingering sinks */
    PRRTE_LIST_FOREACH_SAFE(proct, next, &prrte_iof_hnp_component.procs, prrte_iof_proc_t) {
        if (jdata->jobid == proct->name.jobid) {
            prrte_list_remove_item(&prrte_iof_hnp_component.procs, &proct->super);
            if (NULL != proct->revstdout) {
                prrte_iof_base_static_dump_output(proct->revstdout);
                PRRTE_RELEASE(proct->revstdout);
            }
            proct->revstdout = NULL;
            if (NULL != proct->revstderr) {
                prrte_iof_base_static_dump_output(proct->revstderr);
                PRRTE_RELEASE(proct->revstderr);
            }
            proct->revstderr = NULL;
            PRRTE_RELEASE(proct);
        }
    }
}

static int finalize(void)
{
    prrte_iof_write_event_t *wev;
    prrte_iof_proc_t *proct;
    bool dump;
    prrte_iof_write_output_t *output;
    int num_written;

    /* check if anything is still trying to be written out */
    wev = prrte_iof_base.iof_write_stdout->wev;
    if (!prrte_list_is_empty(&wev->outputs)) {
        dump = false;
        /* make one last attempt to write this out */
        while (NULL != (output = (prrte_iof_write_output_t*)prrte_list_remove_first(&wev->outputs))) {
            if (!dump) {
                num_written = write(wev->fd, output->data, output->numbytes);
                if (num_written < output->numbytes) {
                    /* don't retry - just cleanout the list and dump it */
                    dump = true;
                }
            }
            PRRTE_RELEASE(output);
        }
    }
    if (!prrte_xml_output) {
        /* we only opened stderr channel if we are NOT doing xml output */
        wev = prrte_iof_base.iof_write_stderr->wev;
        if (!prrte_list_is_empty(&wev->outputs)) {
            dump = false;
            /* make one last attempt to write this out */
            while (NULL != (output = (prrte_iof_write_output_t*)prrte_list_remove_first(&wev->outputs))) {
                if (!dump) {
                    num_written = write(wev->fd, output->data, output->numbytes);
                    if (num_written < output->numbytes) {
                        /* don't retry - just cleanout the list and dump it */
                        dump = true;
                    }
                }
                PRRTE_RELEASE(output);
            }
        }
    }

    /* cycle thru the procs and ensure all their output was delivered
     * if they were writing to files */
    while (NULL != (proct = (prrte_iof_proc_t*)prrte_list_remove_first(&prrte_iof_hnp_component.procs))) {
        if (NULL != proct->revstdout) {
            prrte_iof_base_static_dump_output(proct->revstdout);
        }
        if (NULL != proct->revstderr) {
            prrte_iof_base_static_dump_output(proct->revstderr);
        }
        PRRTE_RELEASE(proct);
    }
    PRRTE_DESTRUCT(&prrte_iof_hnp_component.procs);

    return PRRTE_SUCCESS;
}

int hnp_ft_event(int state) {
    /*
     * Replica doesn't need to do anything for a checkpoint
     */
    return PRRTE_SUCCESS;
}


/* this function is called by the event library and thus
 * can access information global to the state machine
 */
static void stdin_write_handler(int fd, short event, void *cbdata)
{
    prrte_iof_sink_t *sink = (prrte_iof_sink_t*)cbdata;
    prrte_iof_write_event_t *wev = sink->wev;
    prrte_list_item_t *item;
    prrte_iof_write_output_t *output;
    int num_written, total_written = 0;

    PRRTE_ACQUIRE_OBJECT(sink);

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s hnp:stdin:write:handler writing data to %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         wev->fd));

    wev->pending = false;

    while (NULL != (item = prrte_list_remove_first(&wev->outputs))) {
        output = (prrte_iof_write_output_t*)item;
        /* if an abnormal termination has occurred, just dump
         * this data as we are aborting
         */
        if (prrte_abnormal_term_ordered) {
            PRRTE_RELEASE(output);
            continue;
        }
        if (0 == output->numbytes) {
            /* this indicates we are to close the fd - there is
             * nothing to write
             */
            PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                                 "%s iof:hnp closing fd %d on write event due to zero bytes output",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wev->fd));
            goto finish;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s hnp:stdin:write:handler wrote %d bytes",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             num_written));
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                prrte_list_prepend(&wev->outputs, item);
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                goto re_enter;
            }
            /* otherwise, something bad happened so all we can do is declare an
             * error and abort
             */
            PRRTE_RELEASE(output);
            PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                                 "%s iof:hnp closing fd %d on write event due to negative bytes written",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wev->fd));
            goto finish;
        } else if (num_written < output->numbytes) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                 "%s hnp:stdin:write:handler incomplete write %d - adjusting data",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), num_written));
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* push this item back on the front of the list */
            prrte_list_prepend(&wev->outputs, item);
            /* leave the write event running so it will call us again
             * when the fd is ready.
             */
            goto re_enter;
        }
        PRRTE_RELEASE(output);

        total_written += num_written;
        if ((PRRTE_IOF_SINK_BLOCKSIZE <= total_written) && wev->always_writable) {
            goto re_enter;
        }
    }
    goto check;

  re_enter:
    PRRTE_IOF_SINK_ACTIVATE(wev);

  check:
    if (sink->closed && 0 == prrte_list_get_size(&wev->outputs)) {
        /* the sink has already been closed and everything was written, time to release it */
        PRRTE_RELEASE(sink);
    }
    return;

  finish:
    PRRTE_RELEASE(wev);
    sink->wev = NULL;
    return;
}

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lk = (prrte_pmix_lock_t*)cbdata;

    PRRTE_POST_OBJECT(lk);
    lk->status = prrte_pmix_convert_status(status);
    PRRTE_PMIX_WAKEUP_THREAD(lk);
}

static int hnp_output(const prrte_process_name_t* peer,
                      prrte_iof_tag_t source_tag,
                      const char *msg)
{
    pmix_iof_channel_t pchan;
    pmix_proc_t source;
    pmix_byte_object_t bo;
    prrte_pmix_lock_t lock;
    pmix_status_t rc;
    int ret;

    if (PRRTE_PROC_IS_MASTER) {
        PRRTE_PMIX_CONVERT_NAME(&source, peer);
        pchan = 0;
        if (PRRTE_IOF_STDIN & source_tag) {
            pchan |= PMIX_FWD_STDIN_CHANNEL;
        }
        if (PRRTE_IOF_STDOUT & source_tag) {
            pchan |= PMIX_FWD_STDOUT_CHANNEL;
        }
        if (PRRTE_IOF_STDERR & source_tag) {
            pchan |= PMIX_FWD_STDERR_CHANNEL;
        }
        if (PRRTE_IOF_STDDIAG & source_tag) {
            pchan |= PMIX_FWD_STDDIAG_CHANNEL;
        }
        /* setup the byte object */
        PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
        if (NULL != msg) {
            bo.bytes = (char*)msg;
            bo.size = strlen(msg)+1;
        }
        PRRTE_PMIX_CONSTRUCT_LOCK(&lock);
        rc = PMIx_server_IOF_deliver(&source, pchan, &bo, NULL, 0, lkcbfunc, (void*)&lock);
        if (PMIX_SUCCESS != rc) {
            ret = prrte_pmix_convert_status(rc);
        } else {
            /* wait for completion */
            PRRTE_PMIX_WAIT_THREAD(&lock);
            ret = lock.status;
        }
        PRRTE_PMIX_DESTRUCT_LOCK(&lock);
        return ret;
    } else {
        /* output this to our local output */
        if (PRRTE_IOF_STDOUT & source_tag || prrte_xml_output) {
            prrte_iof_base_write_output(peer, source_tag, (const unsigned char*)msg, strlen(msg), prrte_iof_base.iof_write_stdout->wev);
        } else {
            prrte_iof_base_write_output(peer, source_tag, (const unsigned char*)msg, strlen(msg), prrte_iof_base.iof_write_stderr->wev);
        }
    }
    return PRRTE_SUCCESS;
}
