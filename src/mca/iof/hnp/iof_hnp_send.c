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
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>

#include "src/dss/dss.h"

#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

#include "iof_hnp.h"

int prte_iof_hnp_send_data_to_endpoint(prte_process_name_t *host,
                                       prte_process_name_t *target,
                                       prte_iof_tag_t tag,
                                       unsigned char *data, int numbytes)
{
    prte_buffer_t *buf;
    int rc;
    prte_grpcomm_signature_t *sig;

    /* if the host is a daemon and we are in the process of aborting,
     * then ignore this request. We leave it alone if the host is not
     * a daemon because it might be a tool that wants to watch the
     * output from an abort procedure
     */
    if (PRTE_JOB_FAMILY(host->jobid) == PRTE_JOB_FAMILY(PRTE_PROC_MY_NAME->jobid)
        && prte_job_term_ordered) {
        return PRTE_SUCCESS;
    }

    buf = PRTE_NEW(prte_buffer_t);

    /* pack the tag - we do this first so that flow control messages can
     * consist solely of the tag
     */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(buf, &tag, 1, PRTE_IOF_TAG))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return rc;
    }
    /* pack the name of the target - this is either the intended
     * recipient (if the tag is stdin and we are sending to a daemon),
     * or the source (if we are sending to anyone else)
     */
    if (PRTE_SUCCESS != (rc = prte_dss.pack(buf, target, 1, PRTE_NAME))) {
        PRTE_ERROR_LOG(rc);
        PRTE_RELEASE(buf);
        return rc;
    }

    /* if data is NULL, then we are done */
    if (NULL != data) {
        /* pack the data - if numbytes is zero, we will pack zero bytes */
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buf, data, numbytes, PRTE_BYTE))) {
            PRTE_ERROR_LOG(rc);
            PRTE_RELEASE(buf);
            return rc;
        }
    }

    /* if the target is wildcard, then this needs to go to everyone - xcast it */
    if (PRTE_PROC_MY_NAME->jobid == host->jobid &&
        PRTE_VPID_WILDCARD == host->vpid) {
        /* xcast this to everyone - the local daemons will know how to handle it */
        sig = PRTE_NEW(prte_grpcomm_signature_t);
        sig->signature = (prte_process_name_t*)malloc(sizeof(prte_process_name_t));
        sig->signature[0].jobid = PRTE_PROC_MY_NAME->jobid;
        sig->signature[0].vpid = PRTE_VPID_WILDCARD;
        (void)prte_grpcomm.xcast(sig, PRTE_RML_TAG_IOF_PROXY, buf);
        PRTE_RELEASE(buf);
        PRTE_RELEASE(sig);
        return PRTE_SUCCESS;
    }

    /* send the buffer to the host - this is either a daemon or
     * a tool that requested IOF
     */
    if (0 > (rc = prte_rml.send_buffer_nb(host, buf, PRTE_RML_TAG_IOF_PROXY,
                                          prte_rml_send_callback, NULL))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}
