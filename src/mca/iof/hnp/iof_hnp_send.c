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
 * Copyright (c) 2012      Los Alamos National Security, LLC
 *                         All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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

#include "src/mca/errmgr/errmgr.h"
#include "src/grpcomm/grpcomm.h"
#include "src/rml/rml.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"

#include "iof_hnp.h"

int prte_iof_hnp_send_data_to_endpoint(const pmix_proc_t *host,
                                       const pmix_proc_t *target,
                                       prte_iof_tag_t tag,
                                       unsigned char *data, int numbytes)
{
    pmix_data_buffer_t *buf;
    int32_t nbytes;
    int rc;

    /* if the host is a daemon and we are in the process of aborting,
     * then ignore this request. We leave it alone if the host is not
     * a daemon because it might be a tool that wants to watch the
     * output from an abort procedure
     */
    if (PMIX_CHECK_NSPACE(PRTE_JOB_FAMILY_PRINT(host->nspace),
                          PRTE_JOB_FAMILY_PRINT(PRTE_PROC_MY_NAME->nspace))
        && prte_dvm_abort_ordered) {
        return PRTE_SUCCESS;
    }

    PMIX_DATA_BUFFER_CREATE(buf);

    /* pack the tag - we do this first so that flow control messages can
     * consist solely of the tag
     */
    rc = PMIx_Data_pack(NULL, buf, &tag, 1, PMIX_UINT16);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }
    /* pack the name of the target - this is either the intended
     * recipient (if the tag is stdin and we are sending to a daemon),
     * or the source (if we are sending to anyone else)
     */
    rc = PMIx_Data_pack(NULL, buf, (void*)target, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* pack the number of bytes being sent so the receiver can
     * allocate storage of the correct size for them - a count
     * of zero tells the receiver to close the target's stdin
     */
    nbytes = (0 < numbytes) ? numbytes : 0;
    rc = PMIx_Data_pack(NULL, buf, &nbytes, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* pack the data itself */
    if (0 < nbytes) {
        rc = PMIx_Data_pack(NULL, buf, data, nbytes, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }

    /* if the target is wildcard, then this needs to go to everyone - xcast it */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, host->nspace)
        && PMIX_RANK_WILDCARD == host->rank) {
        /* xcast this to everyone - the local daemons will know how to handle it.
         * The xcast copies what it needs, so the buffer is ours either way. A
         * failure here loses the fragment outright - nothing retries stdin -
         * so report it rather than returning success over a silent drop
         */
        rc = prte_grpcomm_xcast(PRTE_RML_TAG_IOF_PROXY, buf);
        PMIX_DATA_BUFFER_RELEASE(buf);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
        }
        return rc;
    }

    /* send the buffer to the host - this is either a daemon or
     * a tool that requested IOF
     */
    PRTE_RML_RELIABLE_SEND(rc, host->rank, buf, PRTE_RML_TAG_IOF_PROXY);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    return PRTE_SUCCESS;
}

/*
 * Relay a job's output to the daemon hosting the tool that launched it.
 *
 * A tool need not attach to the DVM master. A prun started on a compute node
 * of a persistent DVM finds its LOCAL daemon's rendezvous file and attaches
 * there, and PMIx registers its IOF request with THAT daemon's server - the
 * only server that can deliver to it. Output, though, converges here: a
 * daemon hands its own processes' output to its own PMIx server (which is why
 * a tool already sees the processes its own daemon happens to host) and
 * forwards it to us, and we are the only process that sees all of it.
 *
 * So we send a copy back out to the tool's daemon, which hands it to its PMIx
 * server. Two things decide whether a copy is owed:
 *
 *   - the requestor has to be reachable through a daemon at all. On the
 *     master, jdata->originator is the daemon that relayed the spawn request
 *     (plm_base_receive overwrites the tool's own procID with the sender), so
 *     an originator in the daemon namespace names that daemon. An originator
 *     in any other namespace is a tool that spawned through US, and it is a
 *     client of our own PMIx server - already served.
 *   - somebody must not have delivered it already. The daemon that hosts the
 *     process delivered to its own server before forwarding to us, so if that
 *     is also the tool's daemon the tool has its copy and a second one would
 *     duplicate it. Same for output from our own children when the tool is
 *     attached to us.
 */
#if PRTE_PMIX_IOF_DELIVER_LOCAL
/* Relay one job's output to one daemon, skipping the two destinations
 * that would duplicate what has already been written: ourselves, and
 * whichever daemon the caller says already delivered it. */
static void relay_one(pmix_rank_t dmn,
                      const pmix_proc_t *source,
                      prte_iof_tag_t stream,
                      unsigned char *data, int numbytes,
                      pmix_rank_t already_delivered)
{
    pmix_proc_t host;
    int rc;

    if (dmn == PRTE_PROC_MY_NAME->rank || dmn == already_delivered ||
        PMIX_RANK_INVALID == dmn) {
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_iof_base_framework.framework_output,
                         "%s iof:hnp relaying %d bytes of %s output to the tool on daemon %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), numbytes,
                         PRTE_NAME_PRINT(source), PMIX_RANK_PRINT(dmn)));

    PMIX_LOAD_PROCID(&host, PRTE_PROC_MY_NAME->nspace, dmn);
    rc = prte_iof_hnp_send_data_to_endpoint(&host, source, stream, data, numbytes);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
}
#endif

void prte_iof_hnp_relay_to_tool(const pmix_proc_t *source,
                                prte_iof_tag_t stream,
                                unsigned char *data, int numbytes,
                                pmix_rank_t already_delivered)
{
#if PRTE_PMIX_IOF_DELIVER_LOCAL
    prte_job_t *jdata;
    pmix_rank_t dmn;
    int n, last;

    jdata = prte_get_job_data_object(source->nspace);
    if (NULL == jdata) {
        return;
    }

    /* The daemon hosting the tool that LAUNCHED this job, which is what
     * jdata->originator names once the HNP has resolved it. Kept as the
     * seed of the set below rather than as a second mechanism: a job that
     * nobody has since subscribed to has exactly this one destination,
     * which is the behavior this function has always had. */
    /* STRICT: an unset originator - a job nobody is waiting on - must not
     * be read as the daemon namespace, which PMIX_CHECK_NSPACE's wildcard
     * would do.  An originator outside the daemon namespace is a tool
     * attached to us, so there is nothing to relay anywhere. */
    if (PMIX_CHECK_NSPACE_STRICT(jdata->originator.nspace,
                                 PRTE_PROC_MY_NAME->nspace)) {
        relay_one(jdata->originator.rank, source, stream, data, numbytes,
                  already_delivered);
    }

    /* ...and every daemon that has since acquired an interest in this
     * job's output: a tool that attached to it and called PMIx_IOF_pull,
     * or - for a spawned job - whatever its parent had. Relaying to a
     * daemon is what puts the bytes in front of the PMIx server that
     * holds that tool's subscription; no other server can deliver to it. */
    last = pmix_bitmap_size(&jdata->iof_daemons);
    for (n = 0; n < last; n++) {
        if (!pmix_bitmap_is_set_bit(&jdata->iof_daemons, n)) {
            continue;
        }
        dmn = (pmix_rank_t) n;
        if (PMIX_CHECK_NSPACE_STRICT(jdata->originator.nspace,
                                     PRTE_PROC_MY_NAME->nspace) &&
            dmn == jdata->originator.rank) {
            continue;   // already served as the seed above
        }
        relay_one(dmn, source, stream, data, numbytes, already_delivered);
    }
#else
    /* without a PMIx that can be told not to emit a delivery, relaying would
     * make the tool's daemon write its own copy of every output file the
     * hosting daemon already wrote - see config/prte_setup_pmix.m4 */
    PRTE_HIDE_UNUSED_PARAMS(source, stream, data, numbytes, already_delivered);
#endif
}
