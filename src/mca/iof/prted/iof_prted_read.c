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
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
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
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"

#include "iof_prted.h"

void prte_iof_prted_read_handler(int fd, short event, void *cbdata)
{
    prte_iof_read_event_t *rev = (prte_iof_read_event_t *) cbdata;
    unsigned char data[PRTE_IOF_BASE_MSG_MAX];
    pmix_data_buffer_t *buf = NULL;
    int rc;
    int32_t numbytes;
    prte_iof_proc_t *proct = (prte_iof_proc_t *) rev->proc;

    PRTE_ACQUIRE_OBJECT(rev);

    /* As we may use timer events, fd can be bogus (-1)
     * use the right one here
     */
    fd = rev->fd;

    /* read up to the fragment size */
    numbytes = read(fd, data, sizeof(data));

    if (NULL == proct) {
        /* nothing we can do */
        PRTE_ERROR_LOG(PRTE_ERR_ADDRESSEE_UNKNOWN);
        return;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s iof:prted:read handler read %d bytes from %s, fd %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
                         PRTE_NAME_PRINT(&proct->name), fd));

    if (numbytes <= 0) {
        if (0 > numbytes) {
            /* either we have a connection error or it was a non-blocking read */
            if (EAGAIN == errno || EINTR == errno) {
                /* non-blocking, retry */
                PRTE_IOF_READ_ACTIVATE(rev);
                return;
            }

            PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                 "%s iof:prted:read handler %s Error on connection:%d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proct->name),
                                 fd));
        }
        /* numbytes must have been zero, so go down and close the fd etc */
        goto CLEAN_RETURN;
    }

    /* see if the user wanted the output directed to files */
    if (NULL != rev->sink) {
        /* output to the corresponding file */
        prte_iof_base_write_output(&proct->name, rev->tag, data, numbytes, rev->sink->wev);
    }
    if (!proct->copy) {
        /* re-add the event */
        PRTE_IOF_READ_ACTIVATE(rev);
        return;
    }

    /* prep the buffer */
    PMIX_DATA_BUFFER_CREATE(buf);

    /* pack the stream first - we do this so that flow control messages can
     * consist solely of the tag
     */
    rc = PMIx_Data_pack(NULL, buf, &rev->tag, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* pack name of process that gave us this data */
    rc = PMIx_Data_pack(NULL, buf, &proct->name, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* pack the data - only pack the #bytes we read! */
    rc = PMIx_Data_pack(NULL, buf, &data, numbytes, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }

    /* start non-blocking RML call to forward received data */
    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s iof:prted:read handler sending %d bytes to HNP",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes));

    prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf, PRTE_RML_TAG_IOF_HNP, prte_rml_send_callback,
                            NULL);

    /* re-add the event */
    PRTE_IOF_READ_ACTIVATE(rev);

    return;

CLEAN_RETURN:
    /* must be an error, or zero bytes were read indicating that the
     * proc terminated this IOF channel - either way, release the
     * corresponding event. This deletes the read event and closes
     * the file descriptor */
    if (rev->tag & PRTE_IOF_STDOUT) {
        if (NULL != proct->revstdout) {
            prte_iof_base_static_dump_output(proct->revstdout);
            PRTE_RELEASE(proct->revstdout);
        }
    } else if (rev->tag & PRTE_IOF_STDERR) {
        if (NULL != proct->revstderr) {
            prte_iof_base_static_dump_output(proct->revstderr);
            PRTE_RELEASE(proct->revstderr);
        }
    }
    /* check to see if they are all done */
    if (NULL == proct->revstdout && NULL == proct->revstderr) {
        /* this proc's iof is complete */
        PRTE_ACTIVATE_PROC_STATE(&proct->name, PRTE_PROC_STATE_IOF_COMPLETE);
    }
    if (NULL != buf) {
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
    return;
}
