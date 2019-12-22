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
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#include "src/util/os_dirpath.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/odls/odls_types.h"
#include "src/mca/rml/rml.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"

#include "iof_prted.h"


/* LOCAL FUNCTIONS */
static void stdin_write_handler(int fd, short event, void *cbdata);


/* API FUNCTIONS */
static int init(void);

static int prted_push(const prrte_process_name_t* dst_name, prrte_iof_tag_t src_tag, int fd);

static int prted_pull(const prrte_process_name_t* src_name,
                      prrte_iof_tag_t src_tag,
                      int fd);

static int prted_close(const prrte_process_name_t* peer,
                       prrte_iof_tag_t source_tag);

static int prted_output(const prrte_process_name_t* peer,
                        prrte_iof_tag_t source_tag,
                        const char *msg);

static void prted_complete(const prrte_job_t *jdata);

static int finalize(void);

static int prted_ft_event(int state);

/* The API's in this module are solely used to support LOCAL
 * procs - i.e., procs that are co-located to the daemon. Output
 * from local procs is automatically sent to the HNP for output
 * and possible forwarding to other requestors. The HNP automatically
 * determines and wires up the stdin configuration, so we don't
 * have to do anything here.
 */

prrte_iof_base_module_t prrte_iof_prted_module = {
    .init = init,
    .push = prted_push,
    .pull = prted_pull,
    .close = prted_close,
    .output = prted_output,
    .complete = prted_complete,
    .finalize = finalize,
    .ft_event = prted_ft_event
};

static int init(void)
{
    /* post a non-blocking RML receive to get messages
     from the HNP IOF component */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_IOF_PROXY,
                            PRRTE_RML_PERSISTENT,
                            prrte_iof_prted_recv,
                            NULL);

    /* setup the local global variables */
    PRRTE_CONSTRUCT(&prrte_iof_prted_component.procs, prrte_list_t);
    prrte_iof_prted_component.xoff = false;

    return PRRTE_SUCCESS;
}

/**
 * Push data from the specified file descriptor
 * to the HNP
 */

static int prted_push(const prrte_process_name_t* dst_name,
                      prrte_iof_tag_t src_tag, int fd)
{
    int flags;
    prrte_iof_proc_t *proct;
    int rc;
    prrte_job_t *jobdat=NULL;
    prrte_ns_cmp_bitmask_t mask;

   PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:prted pushing fd %d for process %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         fd, PRRTE_NAME_PRINT(dst_name)));

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

    /* do we already have this process in our list? */
    PRRTE_LIST_FOREACH(proct, &prrte_iof_prted_component.procs, prrte_iof_proc_t) {
        mask = PRRTE_NS_CMP_ALL;

        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = PRRTE_NEW(prrte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    prrte_list_append(&prrte_iof_prted_component.procs, &proct->super);

  SETUP:
    /* get the local jobdata for this proc */
    if (NULL == (jobdat = prrte_get_job_data_object(proct->name.jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    /* define a read event and activate it */
    if (src_tag & PRRTE_IOF_STDOUT) {
        PRRTE_IOF_READ_EVENT(&proct->revstdout, proct, fd, PRRTE_IOF_STDOUT,
                            prrte_iof_prted_read_handler, false);
    } else if (src_tag & PRRTE_IOF_STDERR) {
        PRRTE_IOF_READ_EVENT(&proct->revstderr, proct, fd, PRRTE_IOF_STDERR,
                            prrte_iof_prted_read_handler, false);
    }
    /* setup any requested output files */
    if (PRRTE_SUCCESS != (rc = prrte_iof_base_setup_output_files(dst_name, jobdat, proct))) {
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
        PRRTE_IOF_READ_ACTIVATE(proct->revstdout);
        if (!prrte_iof_base.redirect_app_stderr_to_stdout) {
            PRRTE_IOF_READ_ACTIVATE(proct->revstderr);
        }
    }
    return PRRTE_SUCCESS;
}


/**
 * Pull for a daemon tells
 * us that any info we receive from the HNP that is targeted
 * for stdin of the specified process should be fed down the
 * indicated file descriptor. Thus, all we need to do here
 * is define a local endpoint so we know where to feed anything
 * that comes to us
 */

static int prted_pull(const prrte_process_name_t* dst_name,
                      prrte_iof_tag_t src_tag,
                      int fd)
{
    prrte_iof_proc_t *proct;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;
    int flags;

    /* this is a local call - only stdin is suppprted */
    if (PRRTE_IOF_STDIN != src_tag) {
        return PRRTE_ERR_NOT_SUPPORTED;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:prted pulling fd %d for process %s",
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
    PRRTE_LIST_FOREACH(proct, &prrte_iof_prted_component.procs, prrte_iof_proc_t) {
        if (PRRTE_EQUAL == prrte_util_compare_name_fields(mask, &proct->name, dst_name)) {
            /* found it */
            goto SETUP;
        }
    }
    /* if we get here, then we don't yet have this proc in our list */
    proct = PRRTE_NEW(prrte_iof_proc_t);
    proct->name.jobid = dst_name->jobid;
    proct->name.vpid = dst_name->vpid;
    prrte_list_append(&prrte_iof_prted_component.procs, &proct->super);

  SETUP:
    PRRTE_IOF_SINK_DEFINE(&proct->stdinev, dst_name, fd, PRRTE_IOF_STDIN,
                         stdin_write_handler);

    return PRRTE_SUCCESS;
}


/*
 * One of our local procs wants us to close the specifed
 * stream(s), thus terminating any potential io to/from it.
 * For the prted, this just means closing the local fd
 */
static int prted_close(const prrte_process_name_t* peer,
                       prrte_iof_tag_t source_tag)
{
    prrte_iof_proc_t* proct;
    prrte_ns_cmp_bitmask_t mask = PRRTE_NS_CMP_ALL;

    PRRTE_LIST_FOREACH(proct, &prrte_iof_prted_component.procs, prrte_iof_proc_t) {
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
                prrte_list_remove_item(&prrte_iof_prted_component.procs, &proct->super);
                PRRTE_RELEASE(proct);
            }
            break;
        }
    }

    return PRRTE_SUCCESS;
}

static void prted_complete(const prrte_job_t *jdata)
{
    prrte_iof_proc_t *proct, *next;

    /* cleanout any lingering sinks */
    PRRTE_LIST_FOREACH_SAFE(proct, next, &prrte_iof_prted_component.procs, prrte_iof_proc_t) {
        if (jdata->jobid == proct->name.jobid) {
            prrte_list_remove_item(&prrte_iof_prted_component.procs, &proct->super);
            PRRTE_RELEASE(proct);
        }
    }
}

static int finalize(void)
{
    prrte_iof_proc_t *proct;

    /* cycle thru the procs and ensure all their output was delivered
     * if they were writing to files */
    while (NULL != (proct = (prrte_iof_proc_t*)prrte_list_remove_first(&prrte_iof_prted_component.procs))) {
        if (NULL != proct->revstdout) {
            prrte_iof_base_static_dump_output(proct->revstdout);
        }
        if (NULL != proct->revstderr) {
            prrte_iof_base_static_dump_output(proct->revstderr);
        }
        PRRTE_RELEASE(proct);
    }
    PRRTE_DESTRUCT(&prrte_iof_prted_component.procs);

    /* Cancel the RML receive */
    prrte_rml.recv_cancel(PRRTE_NAME_WILDCARD, PRRTE_RML_TAG_IOF_PROXY);
    return PRRTE_SUCCESS;
}

/*
 * FT event
 */

static int prted_ft_event(int state)
{
    return PRRTE_ERR_NOT_IMPLEMENTED;
}

static void stdin_write_handler(int _fd, short event, void *cbdata)
{
    prrte_iof_sink_t *sink = (prrte_iof_sink_t*)cbdata;
    prrte_iof_write_event_t *wev = sink->wev;
    prrte_list_item_t *item;
    prrte_iof_write_output_t *output;
    int num_written;

    PRRTE_ACQUIRE_OBJECT(sink);

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s prted:stdin:write:handler writing data to %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         wev->fd));

    wev->pending = false;

    while (NULL != (item = prrte_list_remove_first(&wev->outputs))) {
        output = (prrte_iof_write_output_t*)item;
        if (0 == output->numbytes) {
            /* this indicates we are to close the fd - there is
             * nothing to write
             */
            PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                                 "%s iof:prted closing fd %d on write event due to zero bytes output",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wev->fd));
            PRRTE_RELEASE(wev);
            sink->wev = NULL;
            return;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s prted:stdin:write:handler wrote %d bytes",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             num_written));
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                prrte_list_prepend(&wev->outputs, item);
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                PRRTE_IOF_SINK_ACTIVATE(wev);
                goto CHECK;
            }
            /* otherwise, something bad happened so all we can do is declare an
             * error and abort
             */
            PRRTE_RELEASE(output);
            PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                                 "%s iof:prted closing fd %d on write event due to negative bytes written",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wev->fd));
            PRRTE_RELEASE(wev);
            sink->wev = NULL;
            /* tell the HNP to stop sending us stuff */
            if (!prrte_iof_prted_component.xoff) {
                prrte_iof_prted_component.xoff = true;
                prrte_iof_prted_send_xonxoff(PRRTE_IOF_XOFF);
            }
            return;
        } else if (num_written < output->numbytes) {
            PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                 "%s prted:stdin:write:handler incomplete write %d - adjusting data",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), num_written));
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* push this item back on the front of the list */
            prrte_list_prepend(&wev->outputs, item);
            /* leave the write event running so it will call us again
             * when the fd is ready.
             */
            PRRTE_IOF_SINK_ACTIVATE(wev);
            goto CHECK;
        }
        PRRTE_RELEASE(output);
    }

CHECK:
    if (prrte_iof_prted_component.xoff) {
        /* if we have told the HNP to stop reading stdin, see if
         * the proc has absorbed enough to justify restart
         *
         * RHC: Note that when multiple procs want stdin, we
         * can get into a fight between a proc turnin stdin
         * back "on" and other procs turning it "off". There
         * is no clear way to resolve this as different procs
         * may take input at different rates.
         */
        if (prrte_list_get_size(&wev->outputs) < PRRTE_IOF_MAX_INPUT_BUFFERS) {
            /* restart the read */
            prrte_iof_prted_component.xoff = false;
            prrte_iof_prted_send_xonxoff(PRRTE_IOF_XON);
        }
    }
}

static int prted_output(const prrte_process_name_t* peer,
                        prrte_iof_tag_t source_tag,
                        const char *msg)
{
    prrte_buffer_t *buf;
    int rc;

    /* prep the buffer */
    buf = PRRTE_NEW(prrte_buffer_t);

    /* pack the stream first - we do this so that flow control messages can
     * consist solely of the tag
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &source_tag, 1, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* pack name of process that gave us this data */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, peer, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* pack the data - for compatibility, we have to pack this as PRRTE_BYTE,
     * so ensure we include the NULL string terminator */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, msg, strlen(msg)+1, PRRTE_BYTE))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* start non-blocking RML call to forward received data */
    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:prted:output sending %d bytes to HNP",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)strlen(msg)+1));

    prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf, PRRTE_RML_TAG_IOF_HNP,
                            prrte_rml_send_callback, NULL);

    return PRRTE_SUCCESS;
}
