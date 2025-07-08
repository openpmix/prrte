/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void _register_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* need to implement this */

    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

/* hook for the local PMIX server to pass event registrations
 * up to us - we will assume the responsibility for providing
 * notifications for registered events */
pmix_status_t pmix_server_register_events_fn(pmix_status_t *codes, size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _register_events, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void _deregister_events(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* need to implement this */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(PRTE_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}
/* hook for the local PMIX server to pass event deregistrations
 * up to us */
pmix_status_t pmix_server_deregister_events_fn(pmix_status_t *codes, size_t ncodes,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;

    /* need to thread-shift this request as we are going
     * to access our global list of registered events */
    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    cd->codes = codes;
    cd->ncodes = ncodes;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &(cd->ev), -1, PRTE_EV_WRITE, _deregister_events, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&(cd->ev), PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void _notify_release(int status, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

/* someone has sent us an event that we need to distribute
 * to our local clients */
void pmix_server_notify(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                        prte_rml_tag_t tg, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;
    int cnt, rc;
    pmix_proc_t source;
    pmix_data_range_t range = PMIX_RANGE_SESSION;
    pmix_status_t code, ret;
    size_t ninfo;
    pmix_rank_t vpid;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* unpack the daemon who broadcast the event */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &vpid, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s PRTE Notification received from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_RANK_PRINT(vpid));

    /* if I am the one who sent it, then discard it */
    if (vpid == PRTE_PROC_MY_NAME->rank) {
        return;
    }

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &code, &cnt, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the source */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &source, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* unpack the range */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &range, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    // transfer the source
    memcpy(&cd->proc, &source, sizeof(pmix_proc_t));

    /* unpack the #infos that were provided */
    cnt = 1;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, &cd->ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(cd);
        return;
    }
    /* reserve a spot for an additional flag */
    ninfo = cd->ninfo + 1;
    /* create the space */
    PMIX_INFO_CREATE(cd->info, ninfo);

    if (0 < cd->ninfo) {
        /* unpack into it */
        cnt = cd->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, cd->info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(cd->info, cd->ninfo);
            PMIX_RELEASE(cd);
            return;
        }
    }
    cd->ninfo = ninfo;

    /* protect against infinite loops by marking that this notification was
     * passed down to the server by me */
    PMIX_INFO_LOAD(&cd->info[ninfo - 1], "prte.notify.donotloop", NULL, PMIX_BOOL);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s NOTIFYING PMIX SERVER OF STATUS %s SOURCE %s RANGE %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PMIx_Error_string(code), source.nspace,
                        PMIx_Data_range_string(range));

    ret = PMIx_Notify_event(code, &cd->proc, range, cd->info, cd->ninfo, _notify_release, cd);
    if (PMIX_SUCCESS != ret) {
        if (PMIX_OPERATION_SUCCEEDED != ret) {
            PMIX_ERROR_LOG(ret);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
}

pmix_status_t pmix_server_notify_event(pmix_status_t code, const pmix_proc_t *source,
                                       pmix_data_range_t range, pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    pmix_data_buffer_t pbkt;
    pmix_status_t ret;
    size_t n;
    PRTE_HIDE_UNUSED_PARAMS(cbfunc, cbdata);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s local process %s generated event code %s range %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(source),
                        PMIx_Error_string(code), PMIx_Data_range_string(range));

    /* we can get events prior to completing prte_init as we have
     * to init PMIx early so that PRRTE components can use it */
    PMIX_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PMIX_RELEASE_THREAD(&prte_init_lock);
        goto done;
    }
    PMIX_RELEASE_THREAD(&prte_init_lock);

    /* check to see if this is one we sent down */
    for (n = 0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, "prte.notify.donotloop")) {
            /* yep - do not process */
            goto done;
        }
    }

    /* if this is notification of procs being ready for debug, then
     * we treat this as a state change */
    if (PMIX_READY_FOR_DEBUG == code) {
        PRTE_ACTIVATE_PROC_STATE((pmix_proc_t*)source, PRTE_PROC_STATE_READY_FOR_DEBUG);
        goto done;
    }

    /* a local process has generated an event - we need to xcast it
     * to all the daemons so it can be passed down to their local
     * procs */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* we need to add a flag indicating this came from us as we are going to get it echoed
     * back to us by the broadcast */
    if (PMIX_SUCCESS
        != (rc = PMIx_Data_pack(NULL, &pbkt, &PRTE_PROC_MY_NAME->rank, 1, PMIX_PROC_RANK))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return rc;
    }

    /* pack the status code */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the source */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, (pmix_proc_t *) source, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return ret;
    }
    if (0 < ninfo) {
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return ret;
        }
    }

    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_NOTIFICATION, &pbkt))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
        return PMIX_ERROR;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

done:
    /* we do not need to execute a callback as we did this atomically */
    return PMIX_OPERATION_SUCCEEDED;
}
