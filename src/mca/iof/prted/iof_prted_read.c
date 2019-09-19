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

#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/odls_types.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

#include "iof_prted.h"

void prrte_iof_prted_read_handler(int fd, short event, void *cbdata)
{
    prrte_iof_read_event_t *rev = (prrte_iof_read_event_t*)cbdata;
    unsigned char data[PRRTE_IOF_BASE_MSG_MAX];
    prrte_buffer_t *buf=NULL;
    int rc;
    int32_t numbytes;
    prrte_iof_proc_t *proct = (prrte_iof_proc_t*)rev->proc;

    PRRTE_ACQUIRE_OBJECT(rev);

    /* As we may use timer events, fd can be bogus (-1)
     * use the right one here
     */
    fd = rev->fd;

    /* read up to the fragment size */
    numbytes = read(fd, data, sizeof(data));

    if (NULL == proct) {
        /* nothing we can do */
        PRRTE_ERROR_LOG(PRRTE_ERR_ADDRESSEE_UNKNOWN);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:prted:read handler read %d bytes from %s, fd %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         numbytes, PRRTE_NAME_PRINT(&proct->name), fd));

    if (numbytes <= 0) {
        if (0 > numbytes) {
            /* either we have a connection error or it was a non-blocking read */
            if (EAGAIN == errno || EINTR == errno) {
                /* non-blocking, retry */
                PRRTE_IOF_READ_ACTIVATE(rev);
                return;
            }

            PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                 "%s iof:prted:read handler %s Error on connection:%d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proct->name), fd));
        }
        /* numbytes must have been zero, so go down and close the fd etc */
        goto CLEAN_RETURN;
    }

    /* see if the user wanted the output directed to files */
    if (NULL != rev->sink) {
        /* output to the corresponding file */
        prrte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, rev->sink->wev);
    }
    if (!proct->copy) {
        /* re-add the event */
        PRRTE_IOF_READ_ACTIVATE(rev);
        return;
    }

    /* prep the buffer */
    buf = PRRTE_NEW(prrte_buffer_t);

    /* pack the stream first - we do this so that flow control messages can
     * consist solely of the tag
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &rev->tag, 1, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* pack name of process that gave us this data */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &proct->name, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* pack the data - only pack the #bytes we read! */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &data, numbytes, PRRTE_BYTE))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* start non-blocking RML call to forward received data */
    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s iof:prted:read handler sending %d bytes to HNP",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes));

    prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf, PRRTE_RML_TAG_IOF_HNP,
                            prrte_rml_send_callback, NULL);

    /* re-add the event */
    PRRTE_IOF_READ_ACTIVATE(rev);

    return;

 CLEAN_RETURN:
    /* must be an error, or zero bytes were read indicating that the
     * proc terminated this IOF channel - either way, release the
     * corresponding event. This deletes the read event and closes
     * the file descriptor */
    if (rev->tag & PRRTE_IOF_STDOUT) {
        if( NULL != proct->revstdout ) {
            prrte_iof_base_static_dump_output(proct->revstdout);
            PRRTE_RELEASE(proct->revstdout);
        }
    } else if (rev->tag & PRRTE_IOF_STDERR) {
        if( NULL != proct->revstderr ) {
            prrte_iof_base_static_dump_output(proct->revstderr);
            PRRTE_RELEASE(proct->revstderr);
        }
    }
    /* check to see if they are all done */
    if (NULL == proct->revstdout &&
        NULL == proct->revstderr) {
        /* this proc's iof is complete */
        PRRTE_ACTIVATE_PROC_STATE(&proct->name, PRRTE_PROC_STATE_IOF_COMPLETE);
    }
    if (NULL != buf) {
        PRRTE_RELEASE(buf);
    }
    return;
}
