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
    pmix_byte_object_t bo;
    pmix_iof_channel_t pchan;
    prte_pmix_lock_t lock;
    pmix_status_t prc;

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

   /* this must be output from one of my local procs */
    pchan = 0;
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

    PRTE_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output, "%s read %d bytes from %s of %s",
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
            PRTE_RELEASE(proct->revstdout);
            proct->revstdout = NULL;
        } else if (rev->tag & PRTE_IOF_STDERR) {
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

    /* re-add the event */
    PRTE_IOF_READ_ACTIVATE(rev);
    return;
}
