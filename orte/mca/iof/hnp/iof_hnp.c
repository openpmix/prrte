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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "orte_config.h"
#include "opal/util/output.h"
#include "orte/constants.h"

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

#include "opal/event/event-internal.h"
#include "opal/pmix/pmix-internal.h"

#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/rml/rml.h"
#include "orte/util/name_fns.h"
#include "orte/util/threads.h"
#include "orte/mca/odls/odls_types.h"

#include "orte/mca/iof/base/base.h"
#include "orte/mca/iof/base/iof_base_setup.h"
#include "iof_hnp.h"

/* LOCAL FUNCTIONS */
static void stdin_write_handler(int fd, short event, void *cbdata);

/* API FUNCTIONS */
static int init(void);

static int hnp_push(const orte_process_name_t* dst_name, orte_iof_tag_t src_tag, int fd);

static int hnp_pull(const orte_process_name_t* src_name,
                orte_iof_tag_t src_tag,
                int fd);

static int hnp_close(const orte_process_name_t* peer,
                 orte_iof_tag_t source_tag);

static int hnp_output(const orte_process_name_t* peer,
                      orte_iof_tag_t source_tag,
                      const char *msg);

static void hnp_complete(const orte_job_t *jdata);

static int finalize(void);

static int hnp_ft_event(int state);

static int push_stdin(const orte_process_name_t* dst_name,
                      uint8_t *data, size_t sz);

/* The API's in this module are solely used to support LOCAL
 * procs - i.e., procs that are co-located to the HNP. Remote
 * procs interact with the HNP's IOF via the HNP's receive function,
 * which operates independently and is in the iof_hnp_receive.c file
 */

orte_iof_base_module_t orte_iof_hnp_module = {
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
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                            ORTE_RML_TAG_IOF_HNP,
                            ORTE_RML_PERSISTENT,
                            orte_iof_hnp_recv,
                            NULL);

    OBJ_CONSTRUCT(&mca_iof_hnp_component.procs, opal_list_t);
    mca_iof_hnp_component.stdinev = NULL;

    return ORTE_SUCCESS;
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
 * (b) all procs, specified by vpid=ORTE_VPID_WILDCARD
 *
 * The orte_plm_base_launch_apps function calls iof.push after
 * the procs are launched and tells us how to distribute stdin. This
 * ensures that the procs are started -before- we begin reading stdin
 * and attempting to send it to remote procs
 */
static int hnp_push(const orte_process_name_t* dst_name, orte_iof_tag_t src_tag, int fd)
{
    orte_job_t *jdata;
    orte_iof_proc_t *proct, *pptr;
    int flags, rc;
    orte_ns_cmp_bitmask_t mask = ORTE_NS_CMP_ALL;

    /* don't do this if the dst vpid is invalid or the fd is negative! */
    if (ORTE_VPID_INVALID == dst_name->vpid || fd < 0) {
        return ORTE_SUCCESS;
    }

    OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                         "%s iof:hnp pushing fd %d for process %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         fd, ORTE_NAME_PRINT(dst_name)));

    /* do we already have this process in our list? */
    OPAL_LIST_FOREACH(proct, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
        if (OPAL_EQUAL == orte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = OBJ_NEW(orte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    opal_list_append(&mca_iof_hnp_component.procs, &proct->super);

  SETUP:
    /* set the file descriptor to non-blocking - do this before we setup
     * and activate the read event in case it fires right away
     */
    if((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        opal_output(orte_iof_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                    __FILE__, __LINE__, errno);
    } else {
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }
    /* get the local jobdata for this proc */
    if (NULL == (jdata = orte_get_job_data_object(proct->name.jobid))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_ERR_NOT_FOUND;
    }
    /* define a read event and activate it */
    if (src_tag & ORTE_IOF_STDOUT) {
        ORTE_IOF_READ_EVENT(&proct->revstdout, proct, fd, ORTE_IOF_STDOUT,
                            orte_iof_hnp_read_local_handler, false);
    } else if (src_tag & ORTE_IOF_STDERR) {
        ORTE_IOF_READ_EVENT(&proct->revstderr, proct, fd, ORTE_IOF_STDERR,
                            orte_iof_hnp_read_local_handler, false);
    }
    /* setup any requested output files */
    if (ORTE_SUCCESS != (rc = orte_iof_base_setup_output_files(dst_name, jdata, proct))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    /* if -all- of the readevents for this proc have been defined, then
     * activate them. Otherwise, we can think that the proc is complete
     * because one of the readevents fires -prior- to all of them having
     * been defined!
     */
    if (NULL != proct->revstdout &&
        (orte_iof_base.redirect_app_stderr_to_stdout || NULL != proct->revstderr)) {
        if (proct->copy) {
            /* see if there are any wildcard subscribers out there that
             * apply to us */
            OPAL_LIST_FOREACH(pptr, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
                if (dst_name->jobid == pptr->name.jobid &&
                    ORTE_VPID_WILDCARD == pptr->name.vpid &&
                    NULL != pptr->subscribers) {
                    OBJ_RETAIN(pptr->subscribers);
                    proct->subscribers = pptr->subscribers;
                    break;
                }
            }
        }
        ORTE_IOF_READ_ACTIVATE(proct->revstdout);
        if (!orte_iof_base.redirect_app_stderr_to_stdout) {
            ORTE_IOF_READ_ACTIVATE(proct->revstderr);
        }
    }
    return ORTE_SUCCESS;
}

/* Push data to stdin of a client process
 *
 * (a) a specific name, usually vpid=0; or
 *
 * (b) all procs, specified by vpid=ORTE_VPID_WILDCARD
 *
 */
static int push_stdin(const orte_process_name_t* dst_name,
                      uint8_t *data, size_t sz)
{
    orte_iof_proc_t *proct, *pptr;
    int rc;
    orte_ns_cmp_bitmask_t mask = ORTE_NS_CMP_ALL;

    /* don't do this if the dst vpid is invalid */
    if (ORTE_VPID_INVALID == dst_name->vpid) {
        return ORTE_SUCCESS;
    }

    OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                         "%s iof:hnp pushing stdin for process %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(dst_name)));

    /* do we already have this process in our list? */
    proct = NULL;
    OPAL_LIST_FOREACH(pptr, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
        if (OPAL_EQUAL == orte_util_compare_name_fields(mask, &pptr->name, dst_name)) {
            /* found it */
            proct = pptr;
        }
    }
    if (NULL == proct) {
        return ORTE_ERR_NOT_FOUND;
    }

    /* pass the data to the sink */

    /* if the daemon is me, then this is a local sink */
    if (OPAL_EQUAL == orte_util_compare_name_fields(mask, ORTE_PROC_MY_NAME, &proct->stdinev->daemon)) {
        OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                             "%s read %d bytes from stdin - writing to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)sz,
                             ORTE_NAME_PRINT(&proct->name)));
        /* send the bytes down the pipe - we even send 0 byte events
         * down the pipe so it forces out any preceding data before
         * closing the output stream
         */
        if (NULL != proct->stdinev->wev) {
            if (ORTE_IOF_MAX_INPUT_BUFFERS < orte_iof_base_write_output(&proct->name, ORTE_IOF_STDIN, data, sz, proct->stdinev->wev)) {
                /* getting too backed up - stop the read event for now if it is still active */

                OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                                     "buffer backed up - holding"));
                return ORTE_ERR_OUT_OF_RESOURCE;
            }
        }
    } else {
        OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                             "%s sending %d bytes from stdinev to daemon %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)sz,
                             ORTE_NAME_PRINT(&proct->stdinev->daemon)));

        /* send the data to the daemon so it can
         * write it to the proc's fd - in this case,
         * we pass sink->name to indicate who is to
         * receive the data. If the connection closed,
         * numbytes will be zero so zero bytes will be
         * sent - this will tell the daemon to close
         * the fd for stdin to that proc
         */
        if( ORTE_SUCCESS != (rc = orte_iof_hnp_send_data_to_endpoint(&proct->stdinev->daemon, &proct->stdinev->name, ORTE_IOF_STDIN, data, sz))) {
            /* if the addressee is unknown, remove the sink from the list */
            if( ORTE_ERR_ADDRESSEE_UNKNOWN == rc ) {
                OBJ_RELEASE(proct->stdinev);
            }
        }
    }

    return ORTE_SUCCESS;
}

/*
 * Since we are the HNP, the only "pull" call comes from a local
 * process so we can record the file descriptor for its stdin.
 */

static int hnp_pull(const orte_process_name_t* dst_name,
                    orte_iof_tag_t src_tag,
                    int fd)
{
    orte_iof_proc_t *proct;
    orte_ns_cmp_bitmask_t mask = ORTE_NS_CMP_ALL;
    int flags;

    /* this is a local call - only stdin is supported */
    if (ORTE_IOF_STDIN != src_tag) {
        return ORTE_ERR_NOT_SUPPORTED;
    }

    OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                         "%s iof:hnp pulling fd %d for process %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         fd, ORTE_NAME_PRINT(dst_name)));

    /* set the file descriptor to non-blocking - do this before we setup
     * the sink in case it fires right away
     */
    if((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        opal_output(orte_iof_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                    __FILE__, __LINE__, errno);
    } else {
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }

    /* do we already have this process in our list? */
    OPAL_LIST_FOREACH(proct, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
        if (OPAL_EQUAL == orte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = OBJ_NEW(orte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    opal_list_append(&mca_iof_hnp_component.procs, &proct->super);

  SETUP:
    ORTE_IOF_SINK_DEFINE(&proct->stdinev, dst_name, fd, ORTE_IOF_STDIN,
                         stdin_write_handler);
    proct->stdinev->daemon.jobid = ORTE_PROC_MY_NAME->jobid;
    proct->stdinev->daemon.vpid = ORTE_PROC_MY_NAME->vpid;

    return ORTE_SUCCESS;
}

/*
 * One of our local procs wants us to close the specifed
 * stream(s), thus terminating any potential io to/from it.
 */
static int hnp_close(const orte_process_name_t* peer,
                     orte_iof_tag_t source_tag)
{
    orte_iof_proc_t* proct;
    orte_ns_cmp_bitmask_t mask = ORTE_NS_CMP_ALL;

    OPAL_LIST_FOREACH(proct, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
        if (OPAL_EQUAL == orte_util_compare_name_fields(mask, &proct->name, peer)) {
            if (ORTE_IOF_STDIN & source_tag) {
                if (NULL != proct->stdinev) {
                    OBJ_RELEASE(proct->stdinev);
                }
                proct->stdinev = NULL;
            }
            if ((ORTE_IOF_STDOUT & source_tag) ||
                (ORTE_IOF_STDMERGE & source_tag)) {
                if (NULL != proct->revstdout) {
                    orte_iof_base_static_dump_output(proct->revstdout);
                    OBJ_RELEASE(proct->revstdout);
                }
                proct->revstdout = NULL;
            }
            if (ORTE_IOF_STDERR & source_tag) {
                if (NULL != proct->revstderr) {
                    orte_iof_base_static_dump_output(proct->revstderr);
                    OBJ_RELEASE(proct->revstderr);
                }
                proct->revstderr = NULL;
            }
            /* if we closed them all, then remove this proc */
            if (NULL == proct->stdinev &&
                NULL == proct->revstdout &&
                NULL == proct->revstderr) {
                opal_list_remove_item(&mca_iof_hnp_component.procs, &proct->super);
                OBJ_RELEASE(proct);
            }
            break;
        }
    }
    return ORTE_SUCCESS;
}

static void hnp_complete(const orte_job_t *jdata)
{
    orte_iof_proc_t *proct, *next;

    /* cleanout any lingering sinks */
    OPAL_LIST_FOREACH_SAFE(proct, next, &mca_iof_hnp_component.procs, orte_iof_proc_t) {
        if (jdata->jobid == proct->name.jobid) {
            opal_list_remove_item(&mca_iof_hnp_component.procs, &proct->super);
            if (NULL != proct->revstdout) {
                orte_iof_base_static_dump_output(proct->revstdout);
                OBJ_RELEASE(proct->revstdout);
            }
            proct->revstdout = NULL;
            if (NULL != proct->revstderr) {
                orte_iof_base_static_dump_output(proct->revstderr);
                OBJ_RELEASE(proct->revstderr);
            }
            proct->revstderr = NULL;
            OBJ_RELEASE(proct);
        }
    }
}

static int finalize(void)
{
    orte_iof_write_event_t *wev;
    orte_iof_proc_t *proct;
    bool dump;
    orte_iof_write_output_t *output;
    int num_written;

    /* check if anything is still trying to be written out */
    wev = orte_iof_base.iof_write_stdout->wev;
    if (!opal_list_is_empty(&wev->outputs)) {
        dump = false;
        /* make one last attempt to write this out */
        while (NULL != (output = (orte_iof_write_output_t*)opal_list_remove_first(&wev->outputs))) {
            if (!dump) {
                num_written = write(wev->fd, output->data, output->numbytes);
                if (num_written < output->numbytes) {
                    /* don't retry - just cleanout the list and dump it */
                    dump = true;
                }
            }
            OBJ_RELEASE(output);
        }
    }
    if (!orte_xml_output) {
        /* we only opened stderr channel if we are NOT doing xml output */
        wev = orte_iof_base.iof_write_stderr->wev;
        if (!opal_list_is_empty(&wev->outputs)) {
            dump = false;
            /* make one last attempt to write this out */
            while (NULL != (output = (orte_iof_write_output_t*)opal_list_remove_first(&wev->outputs))) {
                if (!dump) {
                    num_written = write(wev->fd, output->data, output->numbytes);
                    if (num_written < output->numbytes) {
                        /* don't retry - just cleanout the list and dump it */
                        dump = true;
                    }
                }
                OBJ_RELEASE(output);
            }
        }
    }

    /* cycle thru the procs and ensure all their output was delivered
     * if they were writing to files */
    while (NULL != (proct = (orte_iof_proc_t*)opal_list_remove_first(&mca_iof_hnp_component.procs))) {
        if (NULL != proct->revstdout) {
            orte_iof_base_static_dump_output(proct->revstdout);
        }
        if (NULL != proct->revstderr) {
            orte_iof_base_static_dump_output(proct->revstderr);
        }
        OBJ_RELEASE(proct);
    }
    OBJ_DESTRUCT(&mca_iof_hnp_component.procs);

    return ORTE_SUCCESS;
}

int hnp_ft_event(int state) {
    /*
     * Replica doesn't need to do anything for a checkpoint
     */
    return ORTE_SUCCESS;
}


/* this function is called by the event library and thus
 * can access information global to the state machine
 */
static void stdin_write_handler(int fd, short event, void *cbdata)
{
    orte_iof_sink_t *sink = (orte_iof_sink_t*)cbdata;
    orte_iof_write_event_t *wev = sink->wev;
    opal_list_item_t *item;
    orte_iof_write_output_t *output;
    int num_written, total_written = 0;

    ORTE_ACQUIRE_OBJECT(sink);

    OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                         "%s hnp:stdin:write:handler writing data to %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         wev->fd));

    wev->pending = false;

    while (NULL != (item = opal_list_remove_first(&wev->outputs))) {
        output = (orte_iof_write_output_t*)item;
        /* if an abnormal termination has occurred, just dump
         * this data as we are aborting
         */
        if (orte_abnormal_term_ordered) {
            OBJ_RELEASE(output);
            continue;
        }
        if (0 == output->numbytes) {
            /* this indicates we are to close the fd - there is
             * nothing to write
             */
            OPAL_OUTPUT_VERBOSE((20, orte_iof_base_framework.framework_output,
                                 "%s iof:hnp closing fd %d on write event due to zero bytes output",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), wev->fd));
            goto finish;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                             "%s hnp:stdin:write:handler wrote %d bytes",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             num_written));
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                opal_list_prepend(&wev->outputs, item);
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                goto re_enter;
            }
            /* otherwise, something bad happened so all we can do is declare an
             * error and abort
             */
            OBJ_RELEASE(output);
            OPAL_OUTPUT_VERBOSE((20, orte_iof_base_framework.framework_output,
                                 "%s iof:hnp closing fd %d on write event due to negative bytes written",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), wev->fd));
            goto finish;
        } else if (num_written < output->numbytes) {
            OPAL_OUTPUT_VERBOSE((1, orte_iof_base_framework.framework_output,
                                 "%s hnp:stdin:write:handler incomplete write %d - adjusting data",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), num_written));
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* push this item back on the front of the list */
            opal_list_prepend(&wev->outputs, item);
            /* leave the write event running so it will call us again
             * when the fd is ready.
             */
            goto re_enter;
        }
        OBJ_RELEASE(output);

        total_written += num_written;
        if ((ORTE_IOF_SINK_BLOCKSIZE <= total_written) && wev->always_writable) {
            goto re_enter;
        }
    }
    goto check;

  re_enter:
    ORTE_IOF_SINK_ACTIVATE(wev);

  check:
    if (sink->closed && 0 == opal_list_get_size(&wev->outputs)) {
        /* the sink has already been closed and everything was written, time to release it */
        OBJ_RELEASE(sink);
    }
    return;

  finish:
    OBJ_RELEASE(wev);
    sink->wev = NULL;
    return;
}

static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    opal_pmix_lock_t *lk = (opal_pmix_lock_t*)cbdata;

    OPAL_POST_OBJECT(lk);
    lk->status = opal_pmix_convert_status(status);
    OPAL_PMIX_WAKEUP_THREAD(lk);
}

static int hnp_output(const orte_process_name_t* peer,
                      orte_iof_tag_t source_tag,
                      const char *msg)
{
    pmix_iof_channel_t pchan;
    pmix_proc_t source;
    pmix_byte_object_t bo;
    opal_pmix_lock_t lock;
    pmix_status_t rc;
    int ret;

    if (ORTE_PROC_IS_MASTER) {
        OPAL_PMIX_CONVERT_NAME(&source, peer);
        pchan = 0;
        if (ORTE_IOF_STDIN & source_tag) {
            pchan |= PMIX_FWD_STDIN_CHANNEL;
        }
        if (ORTE_IOF_STDOUT & source_tag) {
            pchan |= PMIX_FWD_STDOUT_CHANNEL;
        }
        if (ORTE_IOF_STDERR & source_tag) {
            pchan |= PMIX_FWD_STDERR_CHANNEL;
        }
        if (ORTE_IOF_STDDIAG & source_tag) {
            pchan |= PMIX_FWD_STDDIAG_CHANNEL;
        }
        /* setup the byte object */
        PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
        if (NULL != msg) {
            bo.bytes = (char*)msg;
            bo.size = strlen(msg)+1;
        }
        OPAL_PMIX_CONSTRUCT_LOCK(&lock);
        rc = PMIx_server_IOF_deliver(&source, pchan, &bo, NULL, 0, lkcbfunc, (void*)&lock);
        if (PMIX_SUCCESS != rc) {
            ret = opal_pmix_convert_status(rc);
        } else {
            /* wait for completion */
            OPAL_PMIX_WAIT_THREAD(&lock);
            ret = lock.status;
        }
        OPAL_PMIX_DESTRUCT_LOCK(&lock);
        return ret;
    } else {
        /* output this to our local output */
        if (ORTE_IOF_STDOUT & source_tag || orte_xml_output) {
            orte_iof_base_write_output(peer, source_tag, (const unsigned char*)msg, strlen(msg), orte_iof_base.iof_write_stdout->wev);
        } else {
            orte_iof_base_write_output(peer, source_tag, (const unsigned char*)msg, strlen(msg), orte_iof_base.iof_write_stderr->wev);
        }
    }
    return ORTE_SUCCESS;
}
