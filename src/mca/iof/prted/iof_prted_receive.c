/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
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

#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/iof/iof_types.h"
#include "src/mca/iof/base/base.h"

#include "iof_prted.h"

static void send_cb(int status, prrte_process_name_t *peer,
                    prrte_buffer_t *buf, prrte_rml_tag_t tag,
                    void *cbdata)
{
    /* nothing to do here - just release buffer and return */
    PRRTE_RELEASE(buf);
}

void prrte_iof_prted_send_xonxoff(prrte_iof_tag_t tag)
{
    prrte_buffer_t *buf;
    int rc;

    buf = PRRTE_NEW(prrte_buffer_t);

    /* pack the tag - we do this first so that flow control messages can
     * consist solely of the tag
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &tag, 1, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s sending %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (PRRTE_IOF_XON == tag) ? "xon" : "xoff"));

    /* send the buffer to the HNP */
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf, PRRTE_RML_TAG_IOF_HNP,
                                          send_cb, NULL))) {
        PRRTE_ERROR_LOG(rc);
    }
}

/*
 * The only messages coming to an prted are either:
 *
 * (a) stdin, which is to be copied to whichever local
 *     procs "pull'd" a copy
 *
 * (b) flow control messages
 */
void prrte_iof_prted_recv(int status, prrte_process_name_t* sender,
                         prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                         void* cbdata)
{
    unsigned char data[PRRTE_IOF_BASE_MSG_MAX];
    prrte_iof_tag_t stream;
    int32_t count, numbytes;
    prrte_process_name_t target;
    prrte_iof_proc_t *proct;
    int rc;

    /* see what stream generated this data */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &stream, &count, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* if this isn't stdin, then we have an error */
    if (PRRTE_IOF_STDIN != stream) {
        PRRTE_ERROR_LOG(PRRTE_ERR_COMM_FAILURE);
        return;
    }

    /* unpack the intended target */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &target, &count, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the data */
    numbytes=PRRTE_IOF_BASE_MSG_MAX;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, data, &numbytes, PRRTE_BYTE))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }
    /* numbytes will contain the actual #bytes that were sent */

    PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                         "%s unpacked %d bytes for local proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), numbytes,
                         PRRTE_NAME_PRINT(&target)));

    /* cycle through our list of procs */
    PRRTE_LIST_FOREACH(proct, &prrte_iof_prted_component.procs, prrte_iof_proc_t) {
        /* is this intended for this jobid? */
        if (target.jobid == proct->name.jobid) {
            /* yes - is this intended for all vpids or this vpid? */
            if (PRRTE_VPID_WILDCARD == target.vpid ||
                proct->name.vpid == target.vpid) {
                PRRTE_OUTPUT_VERBOSE((1, prrte_iof_base_framework.framework_output,
                                     "%s writing data to local proc %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proct->name)));
                if (NULL == proct->stdinev) {
                    continue;
                }
                /* send the bytes down the pipe - we even send 0 byte events
                 * down the pipe so it forces out any preceding data before
                 * closing the output stream
                 */
                if (PRRTE_IOF_MAX_INPUT_BUFFERS < prrte_iof_base_write_output(&target, stream, data, numbytes, proct->stdinev->wev)) {
                    /* getting too backed up - tell the HNP to hold off any more input if we
                     * haven't already told it
                     */
                    if (!prrte_iof_prted_component.xoff) {
                        prrte_iof_prted_component.xoff = true;
                        prrte_iof_prted_send_xonxoff(PRRTE_IOF_XOFF);
                    }
                }
            }
        }
    }
}
