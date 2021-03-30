/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#include <time.h>

#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"

int prte_iof_base_write_output(const pmix_proc_t *name, prte_iof_tag_t stream,
                               const unsigned char *data, int numbytes,
                               prte_iof_write_event_t *channel)
{
    char starttag[PRTE_IOF_BASE_TAG_MAX], endtag[PRTE_IOF_BASE_TAG_MAX], *suffix;
    prte_iof_write_output_t *output;
    int i, j, k, starttaglen, endtaglen, num_buffered;
    bool endtagged;
    char qprint[10];
    prte_job_t *jdata;
    bool prte_xml_output;
    bool prte_timestamp_output;
    bool prte_tag_output;

    PRTE_OUTPUT_VERBOSE(
        (1, prte_iof_base_framework.framework_output,
         "%s write:output setting up to write %d bytes to %s for %s on fd %d",
         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
         (PRTE_IOF_STDIN & stream)
             ? "stdin"
             : ((PRTE_IOF_STDOUT & stream) ? "stdout"
                                           : ((PRTE_IOF_STDERR & stream) ? "stderr" : "stddiag")),
         PRTE_NAME_PRINT(name), (NULL == channel) ? -1 : channel->fd));

    /* setup output object */
    output = PRTE_NEW(prte_iof_write_output_t);

    /* get the job object for this process */
    jdata = prte_get_job_data_object(name->nspace);
    prte_timestamp_output = prte_get_attribute(&jdata->attributes, PRTE_JOB_TIMESTAMP_OUTPUT, NULL,
                                               PMIX_BOOL);
    prte_tag_output = prte_get_attribute(&jdata->attributes, PRTE_JOB_TAG_OUTPUT, NULL, PMIX_BOOL);
    prte_xml_output = prte_get_attribute(&jdata->attributes, PRTE_JOB_XML_OUTPUT, NULL, PMIX_BOOL);

    /* write output data to the corresponding tag */
    if (PRTE_IOF_STDIN & stream) {
        /* copy over the data to be written */
        if (0 < numbytes) {
            /* don't copy 0 bytes - we just need to pass
             * the zero bytes so the fd can be closed
             * after it writes everything out
             */
            memcpy(output->data, data, numbytes);
        }
        output->numbytes = numbytes;
        goto process;
    } else if (PRTE_IOF_STDOUT & stream) {
        /* write the bytes to stdout */
        suffix = "stdout";
    } else if (PRTE_IOF_STDERR & stream) {
        /* write the bytes to stderr */
        suffix = "stderr";
    } else if (PRTE_IOF_STDDIAG & stream) {
        /* write the bytes to stderr */
        suffix = "stddiag";
    } else {
        /* error - this should never happen */
        PRTE_ERROR_LOG(PRTE_ERR_VALUE_OUT_OF_BOUNDS);
        PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output, "%s stream %0x",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), stream));
        return PRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    /* if this is to be xml tagged, create a tag with the correct syntax - we do not allow
     * timestamping of xml output
     */
    if (prte_xml_output) {
        snprintf(starttag, PRTE_IOF_BASE_TAG_MAX, "<%s rank=\"%s\">", suffix,
                 PRTE_VPID_PRINT(name->rank));
        snprintf(endtag, PRTE_IOF_BASE_TAG_MAX, "</%s>", suffix);
        goto construct;
    }

    /* if we are to timestamp output, start the tag with that */
    if (prte_timestamp_output) {
        time_t mytime;
        char *cptr;
        /* get the timestamp */
        time(&mytime);
        cptr = ctime(&mytime);
        cptr[strlen(cptr) - 1] = '\0'; /* remove trailing newline */

        if (prte_tag_output) {
            /* if we want it tagged as well, use both */
            snprintf(starttag, PRTE_IOF_BASE_TAG_MAX, "%s[%s,%s]<%s>:", cptr,
                     PRTE_LOCAL_JOBID_PRINT(name->nspace), PRTE_VPID_PRINT(name->rank), suffix);
        } else {
            /* only use timestamp */
            snprintf(starttag, PRTE_IOF_BASE_TAG_MAX, "%s<%s>:", cptr, suffix);
        }
        /* no endtag for this option */
        memset(endtag, '\0', PRTE_IOF_BASE_TAG_MAX);
        goto construct;
    }

    if (prte_tag_output) {
        snprintf(starttag, PRTE_IOF_BASE_TAG_MAX,
                 "[%s,%s]<%s>:", PRTE_LOCAL_JOBID_PRINT(name->nspace), PRTE_VPID_PRINT(name->rank),
                 suffix);
        /* no endtag for this option */
        memset(endtag, '\0', PRTE_IOF_BASE_TAG_MAX);
        goto construct;
    }

    /* if we get here, then the data is not to be tagged - just copy it
     * and move on to processing
     */
    if (0 < numbytes) {
        /* don't copy 0 bytes - we just need to pass
         * the zero bytes so the fd can be closed
         * after it writes everything out
         */
        memcpy(output->data, data, numbytes);
    }
    output->numbytes = numbytes;
    goto process;

construct:
    starttaglen = strlen(starttag);
    endtaglen = strlen(endtag);
    endtagged = false;
    /* start with the tag */
    for (j = 0, k = 0; j < starttaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
        output->data[k++] = starttag[j];
    }
    /* cycle through the data looking for <cr>
     * and replace those with the tag
     */
    for (i = 0; i < numbytes && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; i++) {
        if (prte_xml_output) {
            if ('&' == data[i]) {
                if (k + 5 >= PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&amp;");
                for (j = 0; j < (int) strlen(qprint) && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if ('<' == data[i]) {
                if (k + 4 >= PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&lt;");
                for (j = 0; j < (int) strlen(qprint) && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if ('>' == data[i]) {
                if (k + 4 >= PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&gt;");
                for (j = 0; j < (int) strlen(qprint) && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if (data[i] < 32 || data[i] > 127) {
                /* this is a non-printable character, so escape it too */
                if (k + 7 >= PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&#%03d;", (int) data[i]);
                for (j = 0; j < (int) strlen(qprint) && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
                /* if this was a \n, then we also need to break the line with the end tag */
                if ('\n' == data[i] && (k + endtaglen + 1) < PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    /* we need to break the line with the end tag */
                    for (j = 0; j < endtaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX - 1; j++) {
                        output->data[k++] = endtag[j];
                    }
                    /* move the <cr> over */
                    output->data[k++] = '\n';
                    /* if this isn't the end of the data buffer, add a new start tag */
                    if (i < numbytes - 1 && (k + starttaglen) < PRTE_IOF_BASE_TAGGED_OUT_MAX) {
                        for (j = 0; j < starttaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                            output->data[k++] = starttag[j];
                            endtagged = false;
                        }
                    } else {
                        endtagged = true;
                    }
                }
            } else {
                output->data[k++] = data[i];
            }
        } else {
            if ('\n' == data[i]) {
                /* we need to break the line with the end tag */
                for (j = 0; j < endtaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX - 1; j++) {
                    output->data[k++] = endtag[j];
                }
                /* move the <cr> over */
                output->data[k++] = '\n';
                /* if this isn't the end of the data buffer, add a new start tag */
                if (i < numbytes - 1) {
                    for (j = 0; j < starttaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                        output->data[k++] = starttag[j];
                        endtagged = false;
                    }
                } else {
                    endtagged = true;
                }
            } else {
                output->data[k++] = data[i];
            }
        }
    }
    if (!endtagged && k < PRTE_IOF_BASE_TAGGED_OUT_MAX) {
        /* need to add an endtag */
        for (j = 0; j < endtaglen && k < PRTE_IOF_BASE_TAGGED_OUT_MAX - 1; j++) {
            output->data[k++] = endtag[j];
        }
        output->data[k] = '\n';
    }
    output->numbytes = k;

process:
    /* add this data to the write list for this fd */
    prte_list_append(&channel->outputs, &output->super);

    /* record how big the buffer is */
    num_buffered = prte_list_get_size(&channel->outputs);

    /* is the write event issued? */
    if (!channel->pending) {
        /* issue it */
        PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                             "%s write:output adding write event",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_IOF_SINK_ACTIVATE(channel);
    }

    return num_buffered;
}

void prte_iof_base_static_dump_output(prte_iof_read_event_t *rev)
{
    bool dump;
    int num_written;
    prte_iof_write_event_t *wev;
    prte_iof_write_output_t *output;

    if (NULL != rev->sink) {
        wev = rev->sink->wev;
        if (NULL != wev && !prte_list_is_empty(&wev->outputs)) {
            dump = false;
            /* make one last attempt to write this out */
            while (
                NULL
                != (output = (prte_iof_write_output_t *) prte_list_remove_first(&wev->outputs))) {
                if (!dump) {
                    num_written = write(wev->fd, output->data, output->numbytes);
                    if (num_written < output->numbytes) {
                        /* don't retry - just cleanout the list and dump it */
                        dump = true;
                    }
                }
                PRTE_RELEASE(output);
            }
        }
    }
}

void prte_iof_base_write_handler(int _fd, short event, void *cbdata)
{
    prte_iof_sink_t *sink = (prte_iof_sink_t *) cbdata;
    prte_iof_write_event_t *wev = sink->wev;
    prte_list_item_t *item;
    prte_iof_write_output_t *output;
    int num_written, total_written = 0;

    PRTE_ACQUIRE_OBJECT(sink);

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s write:handler writing data to %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         wev->fd));

    while (NULL != (item = prte_list_remove_first(&wev->outputs))) {
        output = (prte_iof_write_output_t *) item;
        if (0 == output->numbytes) {
            /* indicates we are to close this stream */
            PRTE_RELEASE(sink);
            return;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                prte_list_prepend(&wev->outputs, item);
                /* if the list is getting too large, abort */
                if (prte_iof_base.output_limit < prte_list_get_size(&wev->outputs)) {
                    prte_output(0, "IO Forwarding is running too far behind - something is "
                                   "blocking us from writing");
                    PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                    goto ABORT;
                }
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                goto NEXT_CALL;
            }
            /* otherwise, something bad happened so all we can do is abort
             * this attempt
             */
            PRTE_RELEASE(output);
            goto ABORT;
        } else if (num_written < output->numbytes) {
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* adjust the number of bytes remaining to be written */
            output->numbytes -= num_written;
            /* push this item back on the front of the list */
            prte_list_prepend(&wev->outputs, item);
            /* if the list is getting too large, abort */
            if (prte_iof_base.output_limit < prte_list_get_size(&wev->outputs)) {
                prte_output(0, "IO Forwarding is running too far behind - something is blocking us "
                               "from writing");
                PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_FORCED_EXIT);
                goto ABORT;
            }
            /* leave the write event running so it will call us again
             * when the fd is ready
             */
            goto NEXT_CALL;
        }
        PRTE_RELEASE(output);

        total_written += num_written;
        if (wev->always_writable && (PRTE_IOF_SINK_BLOCKSIZE <= total_written)) {
            /* If this is a regular file it will never tell us it will block
             * Write no more than PRTE_IOF_REGULARF_BLOCK at a time allowing
             * other fds to progress
             */
            goto NEXT_CALL;
        }
    }
ABORT:
    wev->pending = false;
    PRTE_POST_OBJECT(wev);
    return;
NEXT_CALL:
    PRTE_IOF_SINK_ACTIVATE(wev);
}
