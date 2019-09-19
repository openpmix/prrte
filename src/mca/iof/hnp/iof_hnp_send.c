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
 * Copyright (c) 2012      Los Alamos National Security, LLC
 *                         All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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
#include "src/runtime/prrte_globals.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/util/name_fns.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

#include "iof_hnp.h"

int prrte_iof_hnp_send_data_to_endpoint(prrte_process_name_t *host,
                                       prrte_process_name_t *target,
                                       prrte_iof_tag_t tag,
                                       unsigned char *data, int numbytes)
{
    prrte_buffer_t *buf;
    int rc;
    prrte_grpcomm_signature_t *sig;

    /* if the host is a daemon and we are in the process of aborting,
     * then ignore this request. We leave it alone if the host is not
     * a daemon because it might be a tool that wants to watch the
     * output from an abort procedure
     */
    if (PRRTE_JOB_FAMILY(host->jobid) == PRRTE_JOB_FAMILY(PRRTE_PROC_MY_NAME->jobid)
        && prrte_job_term_ordered) {
        return PRRTE_SUCCESS;
    }

    buf = PRRTE_NEW(prrte_buffer_t);

    /* pack the tag - we do this first so that flow control messages can
     * consist solely of the tag
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &tag, 1, PRRTE_IOF_TAG))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return rc;
    }
    /* pack the name of the target - this is either the intended
     * recipient (if the tag is stdin and we are sending to a daemon),
     * or the source (if we are sending to anyone else)
     */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, target, 1, PRRTE_NAME))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return rc;
    }

    /* if data is NULL, then we are done */
    if (NULL != data) {
        /* pack the data - if numbytes is zero, we will pack zero bytes */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, data, numbytes, PRRTE_BYTE))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(buf);
            return rc;
        }
    }

    /* if the target is wildcard, then this needs to go to everyone - xcast it */
    if (PRRTE_PROC_MY_NAME->jobid == host->jobid &&
        PRRTE_VPID_WILDCARD == host->vpid) {
        /* xcast this to everyone - the local daemons will know how to handle it */
        sig = PRRTE_NEW(prrte_grpcomm_signature_t);
        sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
        sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
        sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
        (void)prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_IOF_PROXY, buf);
        PRRTE_RELEASE(buf);
        PRRTE_RELEASE(sig);
        return PRRTE_SUCCESS;
    }

    /* send the buffer to the host - this is either a daemon or
     * a tool that requested IOF
     */
    if (0 > (rc = prrte_rml.send_buffer_nb(host, buf, PRRTE_RML_TAG_IOF_PROXY,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}
