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
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
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

#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <time.h>
#include <errno.h>

#include "src/util/output.h"

#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/state/state.h"

#include "src/mca/iof/base/base.h"

int prrte_iof_base_write_output(const prrte_process_name_t *name, prrte_iof_tag_t stream,
                               const unsigned char *data, int numbytes,
                               prrte_iof_write_event_t *channel)
{
    char starttag[PRRTE_IOF_BASE_TAG_MAX], endtag[PRRTE_IOF_BASE_TAG_MAX], *suffix;
    prrte_iof_write_output_t *output;
    int i, j, k, starttaglen, endtaglen, num_buffered;
    bool endtagged;
    char qprint[10];

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s write:output setting up to write %d bytes to %s for %s on fd %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                         (PRRTE_IOF_STDIN & stream) ? "stdin" : ((PRRTE_IOF_STDOUT & stream) ? "stdout" : ((PRRTE_IOF_STDERR & stream) ? "stderr" : "stddiag")),
                         PRRTE_NAME_PRINT(name),
                         (NULL == channel) ? -1 : channel->fd));

    /* setup output object */
    output = PRRTE_NEW(prrte_iof_write_output_t);

    /* write output data to the corresponding tag */
    if (PRRTE_IOF_STDIN & stream) {
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
    } else if (PRRTE_IOF_STDOUT & stream) {
        /* write the bytes to stdout */
        suffix = "stdout";
    } else if (PRRTE_IOF_STDERR & stream) {
        /* write the bytes to stderr */
        suffix = "stderr";
    } else if (PRRTE_IOF_STDDIAG & stream) {
        /* write the bytes to stderr */
        suffix = "stddiag";
    } else {
        /* error - this should never happen */
        PRRTE_ERROR_LOG(PRRTE_ERR_VALUE_OUT_OF_BOUNDS);
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s stream %0x", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), stream));
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    /* if this is to be xml tagged, create a tag with the correct syntax - we do not allow
     * timestamping of xml output
     */
    if (prrte_xml_output) {
        snprintf(starttag, PRRTE_IOF_BASE_TAG_MAX, "<%s rank=\"%s\">", suffix, PRRTE_VPID_PRINT(name->vpid));
        snprintf(endtag, PRRTE_IOF_BASE_TAG_MAX, "</%s>", suffix);
        goto construct;
    }

    /* if we are to timestamp output, start the tag with that */
    if (prrte_timestamp_output) {
        time_t mytime;
        char *cptr;
        /* get the timestamp */
        time(&mytime);
        cptr = ctime(&mytime);
        cptr[strlen(cptr)-1] = '\0';  /* remove trailing newline */

        if (prrte_tag_output) {
            /* if we want it tagged as well, use both */
            snprintf(starttag, PRRTE_IOF_BASE_TAG_MAX, "%s[%s,%s]<%s>:",
                     cptr, PRRTE_LOCAL_JOBID_PRINT(name->jobid),
                     PRRTE_VPID_PRINT(name->vpid), suffix);
        } else {
            /* only use timestamp */
            snprintf(starttag, PRRTE_IOF_BASE_TAG_MAX, "%s<%s>:", cptr, suffix);
        }
        /* no endtag for this option */
        memset(endtag, '\0', PRRTE_IOF_BASE_TAG_MAX);
        goto construct;
    }

    if (prrte_tag_output) {
        snprintf(starttag, PRRTE_IOF_BASE_TAG_MAX, "[%s,%s]<%s>:",
                 PRRTE_LOCAL_JOBID_PRINT(name->jobid),
                 PRRTE_VPID_PRINT(name->vpid), suffix);
        /* no endtag for this option */
        memset(endtag, '\0', PRRTE_IOF_BASE_TAG_MAX);
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
    for (j=0, k=0; j < starttaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
        output->data[k++] = starttag[j];
    }
    /* cycle through the data looking for <cr>
     * and replace those with the tag
     */
    for (i=0; i < numbytes && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; i++) {
        if (prrte_xml_output) {
            if ('&' == data[i]) {
                if (k+5 >= PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&amp;");
                for (j=0; j < (int)strlen(qprint) && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if ('<' == data[i]) {
                if (k+4 >= PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&lt;");
                for (j=0; j < (int)strlen(qprint) && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if ('>' == data[i]) {
                if (k+4 >= PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&gt;");
                for (j=0; j < (int)strlen(qprint) && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
            } else if (data[i] < 32 || data[i] > 127) {
                /* this is a non-printable character, so escape it too */
                if (k+7 >= PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                    goto process;
                }
                snprintf(qprint, 10, "&#%03d;", (int)data[i]);
                for (j=0; j < (int)strlen(qprint) && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
                    output->data[k++] = qprint[j];
                }
                /* if this was a \n, then we also need to break the line with the end tag */
                if ('\n' == data[i] && (k+endtaglen+1) < PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                    /* we need to break the line with the end tag */
                    for (j=0; j < endtaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX-1; j++) {
                        output->data[k++] = endtag[j];
                    }
                    /* move the <cr> over */
                    output->data[k++] = '\n';
                    /* if this isn't the end of the data buffer, add a new start tag */
                    if (i < numbytes-1 && (k+starttaglen) < PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
                        for (j=0; j < starttaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
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
                for (j=0; j < endtaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX-1; j++) {
                    output->data[k++] = endtag[j];
                }
                /* move the <cr> over */
                output->data[k++] = '\n';
                /* if this isn't the end of the data buffer, add a new start tag */
                if (i < numbytes-1) {
                    for (j=0; j < starttaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX; j++) {
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
    if (!endtagged && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX) {
        /* need to add an endtag */
        for (j=0; j < endtaglen && k < PRRTE_IOF_BASE_TAGGED_OUT_MAX-1; j++) {
            output->data[k++] = endtag[j];
        }
        output->data[k] = '\n';
    }
    output->numbytes = k;

  process:
    /* add this data to the write list for this fd */
    prrte_list_append(&channel->outputs, &output->super);

    /* record how big the buffer is */
    num_buffered = prrte_list_get_size(&channel->outputs);

    /* is the write event issued? */
    if (!channel->pending) {
        /* issue it */
        PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                             "%s write:output adding write event",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_IOF_SINK_ACTIVATE(channel);
    }

    return num_buffered;
}

void prrte_iof_base_static_dump_output(prrte_iof_read_event_t *rev)
{
    bool dump;
    int num_written;
    prrte_iof_write_event_t *wev;
    prrte_iof_write_output_t *output;

    if (NULL != rev->sink) {
        wev = rev->sink->wev;
        if (NULL != wev && !prrte_list_is_empty(&wev->outputs)) {
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
}

void prrte_iof_base_write_handler(int _fd, short event, void *cbdata)
{
    prrte_iof_sink_t *sink = (prrte_iof_sink_t*)cbdata;
    prrte_iof_write_event_t *wev = sink->wev;
    prrte_list_item_t *item;
    prrte_iof_write_output_t *output;
    int num_written, total_written = 0;

    PRRTE_ACQUIRE_OBJECT(sink);

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s write:handler writing data to %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         wev->fd));

    while (NULL != (item = prrte_list_remove_first(&wev->outputs))) {
        output = (prrte_iof_write_output_t*)item;
        if (0 == output->numbytes) {
            /* indicates we are to close this stream */
            PRRTE_RELEASE(sink);
            return;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                prrte_list_prepend(&wev->outputs, item);
                /* if the list is getting too large, abort */
                if (prrte_iof_base.output_limit < prrte_list_get_size(&wev->outputs)) {
                    prrte_output(0, "IO Forwarding is running too far behind - something is blocking us from writing");
                    PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
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
            PRRTE_RELEASE(output);
            goto ABORT;
        } else if (num_written < output->numbytes) {
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* adjust the number of bytes remaining to be written */
            output->numbytes -= num_written;
            /* push this item back on the front of the list */
            prrte_list_prepend(&wev->outputs, item);
            /* if the list is getting too large, abort */
            if (prrte_iof_base.output_limit < prrte_list_get_size(&wev->outputs)) {
                prrte_output(0, "IO Forwarding is running too far behind - something is blocking us from writing");
                PRRTE_FORCED_TERMINATE(PRRTE_ERROR_DEFAULT_EXIT_CODE);
                goto ABORT;
            }
            /* leave the write event running so it will call us again
             * when the fd is ready
             */
            goto NEXT_CALL;
        }
        PRRTE_RELEASE(output);

        total_written += num_written;
        if(wev->always_writable && (PRRTE_IOF_SINK_BLOCKSIZE <= total_written)){
            /* If this is a regular file it will never tell us it will block
             * Write no more than PRRTE_IOF_REGULARF_BLOCK at a time allowing
             * other fds to progress
             */
            goto NEXT_CALL;
        }
    }
  ABORT:
    wev->pending = false;
    PRRTE_POST_OBJECT(wev);
    return;
NEXT_CALL:
    PRRTE_IOF_SINK_ACTIVATE(wev);
}
