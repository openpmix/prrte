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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#include <stdlib.h>
#include <string.h>

#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof_types.h"

#include "iof_prted.h"

void prte_iof_prted_send_xonxoff(prte_iof_tag_t tag)
{
    pmix_data_buffer_t *buf;
    int rc;

    PMIX_DATA_BUFFER_CREATE(buf);

    /* pack the tag - we do this first so that flow control messages can
     * consist solely of the tag
     */
    rc = PMIx_Data_pack(NULL, buf, &tag, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output, "%s sending %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (PRTE_IOF_XON == tag) ? "xon" : "xoff"));

    /* send the buffer to the HNP */
    PRTE_RML_RELIABLE_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_IOF_HNP);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

#if PRTE_PMIX_IOF_DELIVER_LOCAL
static void lkcbfunc(pmix_status_t status, void *cbdata)
{
    prte_iof_deliver_t *p = (prte_iof_deliver_t *) cbdata;

    /* nothing to do here - we use this solely to
     * ensure that IOF_deliver doesn't block */
    if (PMIX_SUCCESS != status) {
        PMIX_ERROR_LOG(status);
    }
    PMIX_RELEASE(p);
}
#endif

/*
 * Output from a process on another node, relayed to us by the master because
 * a tool attached to US asked for it.
 *
 * A tool need not attach to the DVM master, and PMIx registers its IOF
 * request with whichever server it did attach to - ours. Our own processes'
 * output already reaches that tool, because the read handler hands every
 * chunk to our PMIx server as well as forwarding it. Output from processes
 * we do not host never passed through here at all, which is why such a tool
 * used to see only the part of its job that happened to land on this node.
 *
 * The one thing we must NOT do is emit it. The daemon hosting those processes
 * has already written whatever the job's output directives called for, and
 * this server holds the same directives - it registered the same namespace.
 * Emitting here would duplicate the terminal copy and, with --output file=,
 * would have two daemons writing one file. PMIX_IOF_LOCAL_OUTPUT=false on the
 * delivery says "deliver this to your registered requestors, but it is not
 * yours to write."
 */
static void deliver_relayed_output(prte_iof_tag_t stream,
                                   pmix_data_buffer_t *buffer)
{
#if PRTE_PMIX_IOF_DELIVER_LOCAL
    pmix_proc_t source;
    int32_t count, numbytes;
    pmix_iof_channel_t pchan;
    prte_iof_deliver_t *p;
    pmix_status_t prc;
    static pmix_info_t relay_directive;
    static bool directive_ready = false;
    bool flag = false;
    int rc;

    /* unpack the process the output came from */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &source, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &numbytes, &count, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 >= numbytes) {
        /* nothing to deliver - a negative count is a corrupted message */
        if (0 > numbytes) {
            PRTE_ERROR_LOG(PRTE_ERR_COMM_FAILURE);
        }
        return;
    }

    p = PMIX_NEW(prte_iof_deliver_t);
    PMIX_XFER_PROCID(&p->source, &source);
    p->bo.bytes = (char *) malloc(numbytes);
    if (NULL == p->bo.bytes) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PMIX_RELEASE(p);
        return;
    }
    count = numbytes;
    rc = PMIx_Data_unpack(NULL, buffer, p->bo.bytes, &count, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(p);
        return;
    }
    p->bo.size = count;

    PMIX_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s delivering %d relayed bytes from %s to our tool",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) p->bo.size,
                         PRTE_NAME_PRINT(&source)));

    pchan = 0;
    if (PRTE_IOF_STDOUT & stream) {
        pchan |= PMIX_FWD_STDOUT_CHANNEL;
    }
    if (PRTE_IOF_STDERR & stream) {
        pchan |= PMIX_FWD_STDERR_CHANNEL;
    }
    if (PRTE_IOF_STDDIAG & stream) {
        pchan |= PMIX_FWD_STDDIAG_CHANNEL;
    }

    /* The delivery is non-blocking, so PMIx reads the directive on its own
     * progress thread after we have returned - the array has to outlive this
     * function. It never varies and holds nothing allocated, so one static
     * built on first use serves every delivery and never needs releasing. */
    if (!directive_ready) {
        PMIX_INFO_LOAD(&relay_directive, PMIX_IOF_LOCAL_OUTPUT, &flag, PMIX_BOOL);
        directive_ready = true;
    }
    prc = PMIx_server_IOF_deliver(&p->source, pchan, &p->bo, &relay_directive, 1,
                                  lkcbfunc, (void *) p);
    if (PMIX_SUCCESS != prc) {
        PMIX_ERROR_LOG(prc);
        PMIX_RELEASE(p);
    }
#else
    /* the master does not relay output without a PMIx that can be told not to
     * emit it, so nothing should ever arrive here */
    PRTE_HIDE_UNUSED_PARAMS(stream, buffer);
    PRTE_ERROR_LOG(PRTE_ERR_NOT_SUPPORTED);
#endif
}

/*
 * The messages coming to a prted on this tag are:
 *
 * (a) stdin, which is to be copied to whichever local
 *     procs "pull'd" a copy
 *
 * (b) output from processes on OTHER nodes, which the master is relaying
 *     to us because a tool attached to us asked for it - see
 *     deliver_relayed_output() below
 *
 * (c) flow control messages
 *
 * The leading stream tag says which.
 */
void prte_iof_prted_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                         prte_rml_tag_t tag, void *cbdata)
{
    unsigned char *data = NULL;
    prte_iof_tag_t stream;
    int32_t count, numbytes;
    pmix_proc_t target;
    prte_iof_proc_t *proct;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    /* see what stream generated this data */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stream, &count, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* output being relayed to us for a tool of ours */
    if (PRTE_IOF_STDOUTALL & stream) {
        deliver_relayed_output(stream, buffer);
        return;
    }

    /* anything else on this tag has to be stdin */
    if (PRTE_IOF_STDIN != stream) {
        PRTE_ERROR_LOG(PRTE_ERR_COMM_FAILURE);
        return;
    }

    /* unpack the intended target */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &target, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the number of bytes that follow - the HNP sends this ahead
     * of the data itself as a stdin fragment can be of any size, and we
     * must not silently truncate it
     */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &numbytes, &count, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 > numbytes) {
        /* corrupted message */
        PRTE_ERROR_LOG(PRTE_ERR_COMM_FAILURE);
        return;
    }

    /* unpack the data itself - a zero count means we are to close
     * the target's stdin, so there is nothing to unpack
     */
    if (0 < numbytes) {
        data = (unsigned char *) malloc(numbytes);
        if (NULL == data) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return;
        }
        count = numbytes;
        rc = PMIx_Data_unpack(NULL, buffer, data, &count, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            free(data);
            return;
        }
        /* count contains the actual #bytes that were sent */
        numbytes = count;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s unpacked %d bytes for local proc %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes, PRTE_NAME_PRINT(&target)));

    /* cycle through our list of procs */
    PMIX_LIST_FOREACH(proct, &prte_mca_iof_prted_component.procs, prte_iof_proc_t)
    {
        /* is this intended for this jobid? */
        if (PMIX_CHECK_NSPACE(target.nspace, proct->name.nspace)) {
            /* yes - is this intended for all vpids or this vpid? */
            if (PMIX_CHECK_RANK(target.rank, proct->name.rank)) {
                PMIX_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                                     "%s writing data to local proc %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proct->name)));
                if (NULL == proct->stdinev) {
                    continue;
                }
                /* send the bytes down the pipe - we even send 0 byte events
                 * down the pipe so it forces out any preceding data before
                 * closing the output stream
                 */
                if (PRTE_IOF_MAX_INPUT_BUFFERS < prte_iof_base_write_output(&target, stream, data,
                                                                            numbytes,
                                                                            proct->stdinev->wev)) {
                    /* getting too backed up - tell the HNP to hold off any more input if we
                     * haven't already told it
                     */
                    if (!prte_mca_iof_prted_component.xoff) {
                        prte_mca_iof_prted_component.xoff = true;
                        prte_iof_prted_send_xonxoff(PRTE_IOF_XOFF);
                    }
                }
            }
        }
    }

    if (NULL != data) {
        free(data);
    }
}
