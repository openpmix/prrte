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
 * Copyright (c) 2007-2011 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2017-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <pmix.h>
#include <pmix_server.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_SYS_PTRACE_H
#    include <sys/ptrace.h>
#endif

#include "prte_stdint.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"
#include "src/util/sys_limits.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/filem/filem.h"
#include "src/grpcomm/grpcomm.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml_contact.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/base/base.h"
#include "src/mca/state/base/base.h"
#include "src/mca/state/state.h"

#include "src/prted/pmix/pmix_server.h"
#include "src/prted/prted.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_context_fns.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/odls/base/base.h"

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    prte_job_t *jdata;
    pmix_info_t *info;
    size_t ninfo;
    pmix_byte_object_t pbo;
    prte_odls_base_fork_local_proc_fn_t fork_local;
    int pending;
    pmix_status_t rstatus;
} prte_odls_jcaddy_t;
static void jccon(prte_odls_jcaddy_t *p)
{
    p->jdata = NULL;
    p->info = NULL;
    p->ninfo = 0;
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->pbo);
    p->fork_local = NULL;
    p->pending = 0;
    p->rstatus = PMIX_SUCCESS;
}
static void jcdes(prte_odls_jcaddy_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&p->pbo);
}
static PMIX_CLASS_INSTANCE(prte_odls_jcaddy_t, pmix_object_t, jccon, jcdes);

static void _setup_complete(int sd, short args, void *cbdata)
{
    prte_odls_jcaddy_t *cd = (prte_odls_jcaddy_t *) cbdata;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* Add the results to the launch msg.
     *
     * NB: this appends to jdata->launch_msg, not to the "buffer" argument
     * get_add_procs_data() was handed - by the time we run, that call has
     * long since returned and its argument is out of scope. The two are
     * the same object: the only caller (prte_plm_base_launch_apps) passes
     * &jdata->launch_msg, which is also what the xcast in
     * SEND_LAUNCH_MSG sends. Passing anything else would silently drop the
     * setup blob into a buffer nobody transmits. */
    rc = PMIx_Data_pack(NULL, &cd->jdata->launch_msg, &cd->pbo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }

    PMIX_OUTPUT_VERBOSE((2, prte_odls_base_framework.framework_output,
                         "%s odls:launch_msg setup blob %lu bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (unsigned long) cd->pbo.size));

    /* move to next stage */
    PRTE_ACTIVATE_JOB_STATE(cd->jdata, PRTE_JOB_STATE_SEND_LAUNCH_MSG);

    /* releasing the caddy also releases the app-setup info
     * we passed to the PMIx server */
    PMIX_RELEASE(cd);
}

static void setup_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                         void *provided_cbdata, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prte_odls_jcaddy_t *cd = (prte_odls_jcaddy_t *) provided_cbdata;
    pmix_data_buffer_t pbuf;
    int rc = PRTE_SUCCESS;
    PRTE_HIDE_UNUSED_PARAMS(status);

    /* this callback executes on the PMIx progress thread, so we
     * cannot touch the job object here - capture the returned
     * info by serializing it into a byte object, then shift to
     * our progress thread */
    if (NULL != info) {
        PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
        /* pack the provided info */
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &pbuf, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            goto done;
        }
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &pbuf, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            goto done;
        }
        /* unload it */
        rc = PMIx_Data_unload(&pbuf, &cd->pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

done:
    /* release our caller - we are done with the provided info */
    if (NULL != cbfunc) {
        cbfunc(rc, cbdata);
    }

    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, _setup_complete, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
}

/* IT IS CRITICAL THAT ANY CHANGE IN THE ORDER OF THE INFO PACKED IN
 * THIS FUNCTION BE REFLECTED IN THE CONSTRUCT_CHILD_LIST PARSER BELOW
 */
int prte_odls_base_default_get_add_procs_data(pmix_data_buffer_t *buffer, pmix_nspace_t job)
{
    int rc;
    prte_job_t *jdata = NULL;
    prte_job_map_t *map = NULL;
    pmix_status_t ret;
    prte_node_t *node;
    int i, k;
    char **list, **procs, **micro, *tmp;
#if !PRTE_PMIX_HAVE_REGEX2
    char *regex;
#endif
    prte_odls_jcaddy_t *cd;
    prte_proc_t *pptr;
    uint32_t uid;
    uint32_t gid;
    void *ilist, *mlist;
    pmix_data_array_t darray;

    /* get the job data pointer */
    if (NULL == (jdata = prte_get_job_data_object(job))) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }

    /* get a pointer to the job map */
    map = jdata->map;
    /* if there is no map, just return */
    if (NULL == map) {
        return PRTE_SUCCESS;
    }

    /* Note what is NOT here: the other jobs already running in this DVM.
     * They used to be packed in front of the job being launched, whenever
     * this launch brought new daemons in, so that a fresh daemon could
     * resolve their namespaces.  That tied the size of the launch message to
     * the number of jobs resident in the DVM and sent them to every daemon
     * rather than the ones that needed them.  They now ride in the nidmap
     * message the master sends at VM_READY - see prte_util_pack_job_catchup.
     */

    /* Pack the job struct.
     *
     * Without the cpusets, if we are scattering them: this buffer is
     * broadcast to every daemon, and a proc's binding is read only by the
     * daemon that forks it.  Each daemon's own bindings follow separately
     * from prte_odls_base_send_cpuset_slices() below. */
    rc = prte_job_pack(buffer, jdata,
                       prte_odls_globals.scatter_cpusets ? PRTE_JOB_PACK_NO_CPUSETS
                                                         : PRTE_JOB_PACK_ALL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    PMIX_OUTPUT_VERBOSE((2, prte_odls_base_framework.framework_output,
                         "%s odls:launch_msg jobdata %lu bytes (%u procs)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (unsigned long) buffer->bytes_used, jdata->num_procs));

    /* assemble the node and proc map info */
    list = NULL;
    procs = NULL;
    PMIX_INFO_LIST_START(ilist);
    for (i = 0; i < map->nodes->size; i++) {
        micro = NULL;
        if (NULL != (node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, i))) {
            PMIx_Argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k = 0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, k))) {
                    if (PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                        PMIx_Argv_append_nosize(&micro, PRTE_VPID_PRINT(pptr->name.rank));
                    }
                }
            }
            /* assemble the rank/node map */
            if (NULL != micro) {
                tmp = PMIx_Argv_join(micro, ',');
                PMIx_Argv_free(micro);
                PMIx_Argv_append_nosize(&procs, tmp);
                free(tmp);
            }
        }
    }

    /* let the PMIx server generate the nodemap regex */
    if (NULL != list) {
        tmp = PMIx_Argv_join(list, ',');
        PMIx_Argv_free(list);
        list = NULL;
#if PRTE_PMIX_HAVE_REGEX2
        pmix_regex2_t nregex = PMIX_REGEX2_STATIC_INIT;
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex2(tmp, NULL, 0, &nregex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            if (NULL != procs) {
                PMIx_Argv_free(procs);
            }
            PMIX_INFO_LIST_RELEASE(ilist);
            return prte_pmix_convert_status(ret);
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_NODE_MAP, &nregex, PMIX_REGEX2);
        PMIx_Regex2_destruct(&nregex);
#else
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            if (NULL != procs) {
                PMIx_Argv_free(procs);
            }
            PMIX_INFO_LIST_RELEASE(ilist);
            return prte_pmix_convert_status(ret);
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_NODE_MAP, regex, PMIX_REGEX);
        free(regex);
#endif
    }

    /* let the PMIx server generate the procmap regex */
    if (NULL != procs) {
        tmp = PMIx_Argv_join(procs, ';');
        PMIx_Argv_free(procs);
        procs = NULL;
#if PRTE_PMIX_HAVE_REGEX2
        /* the proc map goes through the same generator: PMIx_generate_ppn is
         * deprecated with no replacement, because the per-node rank mapping
         * is now carried by an ordinary pmix_regex2_t.  The receiving side
         * (gds/hash) splits it on ';' rather than ',' - which is a property
         * of the string, not of the encoding */
        pmix_regex2_t pregex = PMIX_REGEX2_STATIC_INIT;
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex2(tmp, NULL, 0, &pregex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            /* list and procs were already freed above */
            PMIX_INFO_LIST_RELEASE(ilist);
            return prte_pmix_convert_status(ret);
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_PROC_MAP, &pregex, PMIX_REGEX2);
        PMIx_Regex2_destruct(&pregex);
#else
        if (PMIX_SUCCESS != (ret = PMIx_generate_ppn(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            /* list and procs were already freed above */
            PMIX_INFO_LIST_RELEASE(ilist);
            return prte_pmix_convert_status(ret);
        }
        free(tmp);
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_PROC_MAP, regex, PMIX_REGEX);
        free(regex);
#endif
    }

    /* add in the personality */
    if (NULL != jdata->personality) {
        tmp = PMIx_Argv_join(jdata->personality, ',');
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_PERSONALITY, tmp, PMIX_STRING);
        free(tmp);
    }

    /* construct the actual request - we just let them pick the
     * default transport for now. Someday, we will add to prun
     * the ability for transport specifications */
    PMIX_INFO_LIST_START(mlist);

    pmix_asprintf(&tmp, "%s.net", jdata->nspace);
    PMIX_INFO_LIST_ADD(ret, mlist, PMIX_ALLOC_NETWORK_ID, tmp, PMIX_STRING);
    free(tmp);
    PMIX_INFO_LIST_ADD(ret, mlist, PMIX_ALLOC_NETWORK_SEC_KEY, NULL, PMIX_BOOL);
    PMIX_INFO_LIST_CONVERT(ret, mlist, &darray);
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_ALLOC_NETWORK, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    PMIX_INFO_LIST_RELEASE(mlist);

    /* add in the user's uid and gid */
    uid = geteuid();
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_USERID, &uid, PMIX_UINT32);
    gid = getegid();
    PMIX_INFO_LIST_ADD(ret, ilist, PMIX_GRPID, &gid, PMIX_UINT32);

    /* if they haven't harvested envars, do so now */
    if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_ENVARS_HARVESTED, NULL, PMIX_BOOL)) {
        PMIX_INFO_LIST_ADD(ret, ilist, PMIX_SETUP_APP_ENVARS, NULL, PMIX_BOOL);
    }
    /* convert the job info into an array */
    PMIX_INFO_LIST_CONVERT(ret, ilist, &darray);
    PMIX_INFO_LIST_RELEASE(ilist);

    /* do not block waiting for the answer as it could take some
     * indeterminate time to get the info - the callback will
     * thread-shift, complete the launch message, and activate
     * the next state */
    cd = PMIX_NEW(prte_odls_jcaddy_t);
    cd->jdata = jdata;
    cd->info = (pmix_info_t *) darray.array;
    cd->ninfo = darray.size;
    ret = PMIx_server_setup_application(jdata->nspace, cd->info, cd->ninfo,
                                        setup_cbfunc, cd);
    if (PMIX_SUCCESS != ret) {
        pmix_output(0, "[%s:%d] PMIx_server_setup_application failed: %s", __FILE__, __LINE__,
                    PMIx_Error_string(ret));
        /* the callback will never fire - releasing the caddy
         * also releases the info array */
        PMIX_RELEASE(cd);
        return PRTE_ERROR;
    }
    return PRTE_SUCCESS;
}

static void _local_support_complete(int sd, short args, void *cbdata)
{
    prte_odls_jcaddy_t *cd = (prte_odls_jcaddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* the local support is in place - we can now launch the
     * local procs */
    PRTE_ACTIVATE_LOCAL_LAUNCH(cd->jdata->nspace, cd->fork_local);

    /* releasing the caddy also releases the setup info */
    PMIX_RELEASE(cd);
}

static void ls_cbunc(pmix_status_t status, void *cbdata)
{
    prte_odls_jcaddy_t *cd = (prte_odls_jcaddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    /* this callback executes on the PMIx progress thread - shift
     * to our progress thread before proceeding with the launch */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, _local_support_complete, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
}

/* join point for the nspace registrations - once they have all
 * reported, proceed with the local support setup and launch.
 * Executes on the PRRTE progress thread */
static void job_reg_join(prte_odls_jcaddy_t *cd)
{
    pmix_status_t ret;

    cd->pending--;
    if (0 < cd->pending) {
        return;
    }

    /* all registrations have reported */
    if (PMIX_SUCCESS != cd->rstatus) {
        /* report an error back to the HNP so we don't just hang
         * while it waits to hear that our local procs launched */
        PRTE_ACTIVATE_JOB_STATE(cd->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(cd);
        return;
    }

    /* if we have local support setup info, then execute it here - we
     * have to do so AFTER we register the nspace so the PMIx server
     * has the nspace info it needs */
    if (0 < cd->ninfo) {
        /* do not block waiting for the answer - the callback will
         * thread-shift and activate the local launch once the
         * support is in place */
        ret = PMIx_server_setup_local_support(cd->jdata->nspace, cd->info, cd->ninfo,
                                              ls_cbunc, cd);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PRTE_ACTIVATE_JOB_STATE(cd->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
            PMIX_RELEASE(cd);
            return;
        }
        /* spin up the spawn threads */
        prte_odls_base_start_threads(cd->jdata);
        return;
    }

    /* spin up the spawn threads */
    prte_odls_base_start_threads(cd->jdata);

    /* no local support setup is required, so we can proceed
     * directly to launching the local procs */
    PRTE_ACTIVATE_LOCAL_LAUNCH(cd->jdata->nspace, cd->fork_local);
    PMIX_RELEASE(cd);
}

static void _job_reg_complete(pmix_status_t status, void *cbdata)
{
    prte_odls_jcaddy_t *cd = (prte_odls_jcaddy_t *) cbdata;

    if (PMIX_SUCCESS != status) {
        PMIX_ERROR_LOG(status);
        cd->rstatus = status;
    }
    job_reg_join(cd);
}

/* Register the nspace and start the launch.  Reached either straight from
 * construct_child_list or, when this daemon's cpuset slice had not arrived
 * by then, from the receiver that completes it. */
static void start_registration(prte_odls_jcaddy_t *cd)
{
    int rc;

    /* register this job with the PMIx server. We have to wait until
     * after we computed the #local_procs before registering it. The
     * registration completes asynchronously, and the launch proceeds
     * once it has reported. Hold a sentinel on the join so it cannot
     * fire before we finish initiating the registration */
    cd->pending = 1;
    rc = prte_pmix_server_register_nspace(cd->jdata, _job_reg_complete, cd);
    if (PRTE_SUCCESS == rc) {
        cd->pending++;
    } else {
        PRTE_ERROR_LOG(rc);
        cd->rstatus = PMIX_ERROR;
    }
    /* release our sentinel - if everything already reported,
     * this progresses the launch */
    job_reg_join(cd);
}

/*
 * The launch message's cpuset slice.
 *
 * A proc's binding is read by exactly one daemon - the one that forks it -
 * and it is the largest of the three things a proc costs the launch
 * message.  Broadcasting it put every daemon's bindings on every link of
 * the tree; sending each daemon its own costs the tree the sum of the
 * slices instead of the sum times the number of daemons they pass.
 *
 * The two halves are independent messages, so they arrive in either order,
 * and neither order is the rare one: the broadcast has more hops but the
 * slices are sent one at a time from the master.  Whichever comes second
 * finds the other waiting on this list and completes the launch.  A daemon
 * that hosts none of the job's procs is sent no slice and waits for none.
 */
typedef struct {
    pmix_list_item_t super;
    pmix_nspace_t nspace;
    /* exactly one of these is set: the slice arrived first, or the
     * launch did */
    pmix_data_buffer_t *slice;
    prte_odls_jcaddy_t *cd;
} prte_odls_slice_t;
static void slcon(prte_odls_slice_t *p)
{
    PMIX_LOAD_NSPACE(p->nspace, NULL);
    p->slice = NULL;
    p->cd = NULL;
}
static void sldes(prte_odls_slice_t *p)
{
    if (NULL != p->slice) {
        PMIX_DATA_BUFFER_RELEASE(p->slice);
    }
}
static PMIX_CLASS_INSTANCE(prte_odls_slice_t, pmix_list_item_t, slcon, sldes);

static prte_odls_slice_t *slice_find(const pmix_nspace_t nspace)
{
    prte_odls_slice_t *sl;

    PMIX_LIST_FOREACH(sl, &prte_odls_globals.pending_slices, prte_odls_slice_t)
    {
        if (PMIX_CHECK_NSPACE(sl->nspace, nspace)) {
            return sl;
        }
    }
    return NULL;
}

/* drop anything parked for a job we are not going to launch after all */
static void slice_discard(const pmix_nspace_t nspace)
{
    prte_odls_slice_t *sl;

    sl = slice_find(nspace);
    if (NULL != sl) {
        pmix_list_remove_item(&prte_odls_globals.pending_slices, &sl->super);
        PMIX_RELEASE(sl);
    }
}

/* read one slice into the job's procs */
static int apply_cpuset_slice(prte_job_t *jdata, pmix_data_buffer_t *buffer)
{
    pmix_status_t rc;
    int32_t cnt, n, nprocs;
    pmix_rank_t rank;
    char *cpuset;
    prte_proc_t *proc;

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nprocs, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    for (n = 0; n < nprocs; n++) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &rank, &cnt, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        cpuset = NULL;
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &cpuset, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
        proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, (int) rank);
        if (NULL == proc) {
            /* the slice and the job it belongs to disagree about which
             * procs exist - the two are packed from the same job object on
             * the same daemon, so this cannot be a routine race */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            if (NULL != cpuset) {
                free(cpuset);
            }
            return PRTE_ERR_NOT_FOUND;
        }
        if (NULL != proc->cpuset) {
            free(proc->cpuset);
        }
        proc->cpuset = cpuset; // the proc owns it now
    }

    return PRTE_SUCCESS;
}

int prte_odls_base_send_cpuset_slices(prte_job_t *jdata)
{
    prte_node_t *node;
    prte_proc_t *pptr;
    pmix_data_buffer_t *buf;
    pmix_status_t rc;
    int i, k;
    int32_t nprocs;

    if (NULL == jdata->map) {
        return PRTE_SUCCESS;
    }

    for (i = 0; i < jdata->map->nodes->size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, i);
        if (NULL == node) {
            continue;
        }
        if (NULL == node->daemon) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return PRTE_ERR_NOT_FOUND;
        }
        if (node->daemon->name.rank == PRTE_PROC_MY_NAME->rank) {
            /* our own procs are already bound in our own job object -
             * construct_child_list keeps that copy and discards the
             * unpacked one */
            continue;
        }

        /* count first: the receiver reads a count rather than unpacking
         * until the buffer runs out, because it has no way to tell the end
         * of the slice from a short read */
        nprocs = 0;
        for (k = 0; k < node->procs->size; k++) {
            pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, k);
            if (NULL == pptr || !PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                continue;
            }
            ++nprocs;
        }

        PMIX_DATA_BUFFER_CREATE(buf);
        rc = PMIx_Data_pack(NULL, buf, &jdata->nspace, 1, PMIX_PROC_NSPACE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return prte_pmix_convert_status(rc);
        }
        rc = PMIx_Data_pack(NULL, buf, &nprocs, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return prte_pmix_convert_status(rc);
        }
        for (k = 0; k < node->procs->size; k++) {
            pptr = (prte_proc_t *) pmix_pointer_array_get_item(node->procs, k);
            if (NULL == pptr || !PMIX_CHECK_NSPACE(jdata->nspace, pptr->name.nspace)) {
                continue;
            }
            /* the rank is carried rather than implied by position: the
             * receiver would otherwise have to reproduce the order this
             * loop happens to walk in, and getting that wrong would bind
             * procs to each other's cpus without failing anywhere */
            rc = PMIx_Data_pack(NULL, buf, &pptr->name.rank, 1, PMIX_PROC_RANK);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                return prte_pmix_convert_status(rc);
            }
            rc = PMIx_Data_pack(NULL, buf, &pptr->cpuset, 1, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(buf);
                return prte_pmix_convert_status(rc);
            }
        }

        PMIX_OUTPUT_VERBOSE((2, prte_odls_base_framework.framework_output,
                             "%s odls:launch_msg slice %lu bytes (%d procs) to %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned long) buf->bytes_used, nprocs,
                             PRTE_NAME_PRINT(&node->daemon->name)));

        PRTE_RML_SEND(rc, node->daemon->name.rank, buf, PRTE_RML_TAG_LAUNCH_SLICE);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

void prte_odls_base_recv_cpuset_slice(int status, pmix_proc_t *sender,
                                      pmix_data_buffer_t *buffer,
                                      prte_rml_tag_t tag, void *cbdata)
{
    pmix_nspace_t nspace;
    pmix_status_t rc;
    int32_t cnt;
    prte_odls_slice_t *sl;
    prte_odls_jcaddy_t *cd;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &cnt, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    sl = slice_find(nspace);
    if (NULL == sl || NULL == sl->cd) {
        /* we are first - park the payload for the launch to collect.
         * PMIx_Data_copy_payload takes what has not been unpacked yet,
         * which is the slice proper */
        if (NULL == sl) {
            sl = PMIX_NEW(prte_odls_slice_t);
            PMIX_LOAD_NSPACE(sl->nspace, nspace);
            pmix_list_append(&prte_odls_globals.pending_slices, &sl->super);
        } else {
            /* a second slice for a job whose first is still parked - the
             * master sends one per daemon per job, so this is a bug
             * somewhere, not a race.  Keep the first. */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return;
        }
        PMIX_DATA_BUFFER_CREATE(sl->slice);
        rc = PMIx_Data_copy_payload(sl->slice, buffer);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            pmix_list_remove_item(&prte_odls_globals.pending_slices, &sl->super);
            PMIX_RELEASE(sl);
        }
        return;
    }

    /* the launch got here first and is parked on this entry */
    cd = sl->cd;
    pmix_list_remove_item(&prte_odls_globals.pending_slices, &sl->super);
    PMIX_RELEASE(sl);

    rc = apply_cpuset_slice(cd->jdata, buffer);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        /* the procs would launch unbound, or bound to somebody else's
         * cpus - report rather than proceed */
        PRTE_ACTIVATE_JOB_STATE(cd->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(cd);
        return;
    }
    start_registration(cd);
}

bool prte_odls_base_awaiting_cpusets(const pmix_nspace_t nspace)
{
    prte_odls_slice_t *sl;

    sl = slice_find(nspace);
    /* an entry holding a parked launch is one waiting for its slice; an
     * entry holding a slice is the other way round, and that daemon has no
     * job object yet, so nobody can be asking it anything */
    return (NULL != sl && NULL != sl->cd);
}

/* Returns true if this daemon's bindings are in hand and the launch can go
 * on, false if the caddy has been parked to await them. */
static bool slice_rendezvous(prte_odls_jcaddy_t *cd)
{
    prte_odls_slice_t *sl;
    int rc;

    sl = slice_find(cd->jdata->nspace);
    if (NULL == sl) {
        /* we are first */
        sl = PMIX_NEW(prte_odls_slice_t);
        PMIX_LOAD_NSPACE(sl->nspace, cd->jdata->nspace);
        sl->cd = cd;
        pmix_list_append(&prte_odls_globals.pending_slices, &sl->super);
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:construct_child_list awaiting cpuset slice for %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_JOBID_PRINT(cd->jdata->nspace)));
        return false;
    }

    /* the slice beat us here - unless what is parked is another launch of
     * this same job, which the master does not do and which would leave us
     * dereferencing a slice that was never stored */
    pmix_list_remove_item(&prte_odls_globals.pending_slices, &sl->super);
    if (NULL == sl->slice) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        rc = PRTE_ERR_BAD_PARAM;
    } else {
        rc = apply_cpuset_slice(cd->jdata, sl->slice);
    }
    PMIX_RELEASE(sl);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PRTE_ACTIVATE_JOB_STATE(cd->jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
        PMIX_RELEASE(cd);
        return false;
    }
    return true;
}

int prte_odls_base_default_construct_child_list(pmix_data_buffer_t *buffer, pmix_nspace_t *job,
                                                prte_odls_base_fork_local_proc_fn_t fork_local)
{
    int rc;
    int32_t cnt;
    prte_job_t *jdata = NULL, *daemons;
    prte_node_t *node;
    int32_t n;
    prte_proc_t *pptr, *dmn;
    prte_app_context_t *app;
    prte_odls_jcaddy_t *cd;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    pmix_status_t ret;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t bo;
    size_t m;
    pmix_envar_t envt;
    char *tmp;
    prte_job_pack_mode_t mode = PRTE_JOB_PACK_ALL;

    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                         "%s odls:constructing child list", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* set a default response */
    PMIX_LOAD_NSPACE(*job, NULL);

    /* get the daemon job object */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);

    /* Note what we do NOT unpack here: the other jobs already running in this
     * DVM.  They used to lead this message so a newly-launched daemon could
     * learn about them; they now arrive with the nidmap at VM_READY, which is
     * both earlier and only when the DVM's membership actually changed.  See
     * prte_util_decode_job_catchup. */

    /* unpack the job we are to launch */
    rc = prte_job_unpack(buffer, &jdata, &mode);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    if (PMIX_NSPACE_INVALID(jdata->nspace)) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        rc = PRTE_ERR_BAD_PARAM;
        goto REPORT_ERROR;
    }
    PMIX_LOAD_NSPACE(*job, jdata->nspace);

    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                         "%s odls:construct_child_list unpacking data to launch job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(*job)));

    /* if we are the HNP, we don't need to unpack this buffer - we already
     * have all the required info in our local job array. So just build the
     * array of local children
     */
    if (PRTE_PROC_IS_MASTER) {
        /* we don't want/need the extra copy of the prte_job_t, but
         * we can't just release it as that will NULL the location in
         * the prte_job_data array. So set its index to -1 to
         * protect the array, and then release the object to free
         * the storage */
        jdata->index = -1;
        PMIX_RELEASE(jdata);
        /* get the correct job object - it will be completely filled out */
        if (NULL == (jdata = prte_get_job_data_object(*job))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
        if (NULL == jdata->schizo) {
            prte_show_help("help-schizo-base.txt", "no-proxy", true,
                           prte_tool_basename, "NULL");
            rc = PRTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
    } else {
        prte_set_job_data_object(jdata);

        /* ensure the map object is present */
        if (NULL == jdata->map) {
            jdata->map = PMIX_NEW(prte_job_map_t);
        }
        /* get the associated schizo module */
        if (NULL != jdata->personality) {
            tmp = PMIx_Argv_join(jdata->personality, ',');
        } else {
            tmp = NULL;
        }
        jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(tmp);
        if (NULL == jdata->schizo) {
            prte_show_help("help-schizo-base.txt", "no-proxy", true,
                           prte_tool_basename, (NULL == tmp) ? "NULL" : tmp);
            if (NULL != tmp) {
                free(tmp);
            }
            rc = PRTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
        if (NULL != tmp) {
            free(tmp);
        }
    }

    /* unpack the byte object containing any application setup info - there
     * might not be any, so it isn't an error if we don't find things */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PRTE_SUCCESS == rc && 0 < bo.size) {
        /* there was setup data - process it */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
        rc = PMIx_Data_load(&pbuf, &bo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto REPORT_ERROR;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        /* unpack the number of info structs */
        cnt = 1;
        ret = PMIx_Data_unpack(NULL, &pbuf, &ninfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            rc = PRTE_ERROR;
            goto REPORT_ERROR;
        }
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        ret = PMIx_Data_unpack(NULL, &pbuf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            rc = PRTE_ERROR;
            goto REPORT_ERROR;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
        /* Add any cache'd values to the FRONT of the job attributes: these
         * come from PMIx_server_setup_application, and the user's own
         * directives - which arrive on the job already - have to be applied
         * after them so the user's wins.
         *
         * Walk the array in REVERSE to do it. Prepending a block one entry
         * at a time reverses that block, and these are envar directives:
         * PREPEND/APPEND edit an existing value, so the order they are
         * applied in is the value the process ends up with. */
        for (m = ninfo; m > 0; m--) {
            size_t idx = m - 1;
            pmix_info_t *iptr = &info[idx];
            if (0 == strcmp(iptr->key, PMIX_SET_ENVAR)) {
                envt.envar = iptr->value.data.envar.envar;
                envt.value = iptr->value.data.envar.value;
                envt.separator = iptr->value.data.envar.separator;
                prte_prepend_attribute(&jdata->attributes, PRTE_JOB_SET_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envt, PMIX_ENVAR);
            } else if (0 == strcmp(iptr->key, PMIX_ADD_ENVAR)) {
                envt.envar = iptr->value.data.envar.envar;
                envt.value = iptr->value.data.envar.value;
                envt.separator = iptr->value.data.envar.separator;
                prte_prepend_attribute(&jdata->attributes, PRTE_JOB_ADD_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envt, PMIX_ENVAR);
            } else if (0 == strcmp(iptr->key, PMIX_UNSET_ENVAR)) {
                prte_prepend_attribute(&jdata->attributes, PRTE_JOB_UNSET_ENVAR, PRTE_ATTR_GLOBAL,
                                       iptr->value.data.string, PMIX_STRING);
            } else if (0 == strcmp(iptr->key, PMIX_PREPEND_ENVAR)) {
                envt.envar = iptr->value.data.envar.envar;
                envt.value = iptr->value.data.envar.value;
                envt.separator = iptr->value.data.envar.separator;
                prte_prepend_attribute(&jdata->attributes, PRTE_JOB_PREPEND_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envt, PMIX_ENVAR);
            } else if (0 == strcmp(iptr->key, PMIX_APPEND_ENVAR)) {
                envt.envar = iptr->value.data.envar.envar;
                envt.value = iptr->value.data.envar.value;
                envt.separator = iptr->value.data.envar.separator;
                prte_prepend_attribute(&jdata->attributes, PRTE_JOB_APPEND_ENVAR, PRTE_ATTR_GLOBAL,
                                       &envt, PMIX_ENVAR);
            }
        }
    }

    /* now that the node array in the job map and jdata are completely filled out,.
     * we need to "wireup" the procs to their nodes so other utilities can
     * locate them */
    for (n = 0; n < jdata->procs->size; n++) {
        if (NULL == (pptr = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, n))) {
            continue;
        }
        if (PRTE_PROC_STATE_UNDEF == pptr->state) {
            /* not ready for use yet */
            continue;
        }
        if (!PRTE_PROC_IS_MASTER) {
            /* connect the proc to its node here */
            pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                                "%s GETTING DAEMON FOR PROC %s WITH PARENT %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pptr->name),
                                PRTE_VPID_PRINT(pptr->parent));
            if (PMIX_RANK_INVALID == pptr->parent) {
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                rc = PRTE_ERR_BAD_PARAM;
                goto REPORT_ERROR;
            }
            /* connect the proc to its node object */
            dmn = (prte_proc_t *) pmix_pointer_array_get_item(daemons->procs, pptr->parent);
            if (NULL == dmn) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto REPORT_ERROR;
            }
            /* the node backpointer is borrowed, not retained */
            pptr->node = dmn->node;
            /* add the node to the job map, if needed */
            if (!PRTE_FLAG_TEST(pptr->node, PRTE_NODE_FLAG_MAPPED)) {
                PMIX_RETAIN(pptr->node);
                pmix_pointer_array_add(jdata->map->nodes, pptr->node);
                jdata->map->num_nodes++;
                PRTE_FLAG_SET(pptr->node, PRTE_NODE_FLAG_MAPPED);
            }
            /* add this proc to that node */
            PMIX_RETAIN(pptr);
            pmix_pointer_array_add(pptr->node->procs, pptr);
            pptr->node->num_procs++;
        }
        /* see if it belongs to us */
        if (pptr->parent == PRTE_PROC_MY_NAME->rank) {
            /* is this child on our current list of children */
            if (!PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_LOCAL)) {
                /* not on the local list */
                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s[%s:%d] adding proc %s to my local list",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), __FILE__, __LINE__,
                                     PRTE_NAME_PRINT(&pptr->name)));
                /* keep tabs of the number of local procs */
                jdata->num_local_procs++;
                /* add this proc to our child list */
                PMIX_RETAIN(pptr);
                PRTE_FLAG_SET(pptr, PRTE_PROC_FLAG_LOCAL);
                pmix_pointer_array_add(prte_local_children, pptr);
            }

            /* if the job is in restart mode, the child must not barrier when launched */
            if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
                prte_set_attribute(&pptr->attributes, PRTE_PROC_NOBARRIER, PRTE_ATTR_LOCAL, NULL,
                                   PMIX_BOOL);
            }
            /* mark that this app_context is being used on this node */
            app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, pptr->app_idx);
            if (NULL == app) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto REPORT_ERROR;
            }
            PRTE_FLAG_SET(app, PRTE_APP_FLAG_USED_ON_NODE);
        }
    }

    /* reset the mapped flags */
    for (n = 0; n < jdata->map->nodes->size; n++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(jdata->map->nodes, n);
        if (NULL != node) {
            PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        }
    }

    /* create the caddy that carries the launch through the
     * asynchronous registrations and local support setup */
    cd = PMIX_NEW(prte_odls_jcaddy_t);
    cd->jdata = jdata;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->fork_local = fork_local;
    info = NULL; // the caddy now owns the info array

    /* Our procs' bindings came separately, and may not be here yet.
     *
     * The master is never sent one - it kept its own fully-populated job
     * object above - and neither is a daemon hosting none of this job's
     * procs, which has nothing to bind and nothing to wait for.  Anything
     * parked for such a daemon is a slice for a job it will not launch;
     * drop it rather than leave it on the list. */
    if (PRTE_JOB_PACK_NO_CPUSETS == mode && !PRTE_PROC_IS_MASTER) {
        if (0 < jdata->num_local_procs) {
            if (!slice_rendezvous(cd)) {
                /* parked - the slice's receiver takes it from here, and
                 * has released the caddy if it failed */
                return PRTE_SUCCESS;
            }
        } else {
            slice_discard(jdata->nspace);
        }
    }

    start_registration(cd);
    return PRTE_SUCCESS;

REPORT_ERROR:
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (NULL != jdata) {
        /* nobody is going to collect a slice for a job we are failing */
        slice_discard(jdata->nspace);
    }
    /* NB: "jdata" is either NULL (we failed before unpacking the job we
     * were told to launch) or that job - never one of the prior jobs
     * decoded above, which use their own variable. A NULL here is
     * survivable: the prted errmgr falls back to the daemon job. */
    /* We have to report an error back to the HNP so we don't just hang.
     * Activating the job state is not enough on its own: what the errmgr
     * sends the HNP is the state of this daemon's local children of that
     * job, and a child list we failed to build has none - or has them in
     * whatever state they were unpacked in, which the HNP reads as "nothing
     * wrong here". The job then never completes and the tool that asked for
     * it waits forever with nothing logged anywhere (that is what made
     * #2616 present as a silent hang). So first fail every proc that was
     * ours to launch, which is what gives the HNP something to act on.
     * The master reports to nobody and already holds the authoritative
     * job object, so this is for the daemons only. */
    if (!PRTE_PROC_IS_MASTER && NULL != jdata && NULL != jdata->procs) {
        int32_t nlocal = 0;
        for (n = 0; n < jdata->procs->size; n++) {
            pptr = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, n);
            if (NULL == pptr || pptr->parent != PRTE_PROC_MY_NAME->rank) {
                continue;
            }
            pptr->state = PRTE_PROC_STATE_FAILED_TO_START;
            pptr->exit_code = rc;
            /* nothing was forked and no stdio was ever opened for it, so the
             * two things the lifecycle waits on are complete by definition */
            PRTE_FLAG_SET(pptr, PRTE_PROC_FLAG_IOF_COMPLETE);
            PRTE_FLAG_SET(pptr, PRTE_PROC_FLAG_WAITPID);
            if (!PRTE_FLAG_TEST(pptr, PRTE_PROC_FLAG_LOCAL)) {
                /* the report walks prte_local_children, so a proc we never
                 * got as far as adding would otherwise go unmentioned */
                PMIX_RETAIN(pptr);
                PRTE_FLAG_SET(pptr, PRTE_PROC_FLAG_LOCAL);
                pmix_pointer_array_add(prte_local_children, pptr);
            }
            ++nlocal;
        }
        /* keep the count that the termination accounting compares against in
         * step with the list we just completed - we abandoned the loop that
         * normally maintains it */
        jdata->num_local_procs = nlocal;
    }
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NEVER_LAUNCHED);
    return rc;
}

static int setup_path(prte_job_t *job, prte_app_context_t *app, char **wdir)
{
    int rc = PRTE_SUCCESS;
    char dir[PRTE_PATH_MAX];
    char *session_dir;
    bool usercwd = false;

    if (prte_get_attribute(&app->attributes, PRTE_APP_SSNDIR_CWD, NULL, PMIX_BOOL)) {
        /* move us to that location. Use the job handed in by our caller: the
         * app->job back-pointer is a raw pointer that is never serialized into
         * the launch message, so it is NULL on any daemon that unpacked this
         * job - dereferencing it segfaults (it is only set on the HNP).
         */
        if (NULL == job) {
            return PRTE_ERROR;
        }
        session_dir = job->session_dir;
        if (NULL == session_dir) {
            // cannot do it
            return PRTE_ERROR;
        }
        if (0 != chdir(session_dir)) {
            return PRTE_ERROR;
        }
        /* NOTE: if a user's program does a chdir(), then $PWD will once
         * again not match getcwd! This is beyond our control - we are only
         * ensuring they start out matching.
         */
        if (NULL == getcwd(dir, sizeof(dir))) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* free any prior value before overwriting - our caller may pass
         * &app->cwd, which already holds an owned string */
        if (NULL != *wdir) {
            free(*wdir);
        }
        *wdir = strdup(dir);
        PMIx_Setenv("PWD", dir, true, &app->env);
    } else {
        /* Try to change to the app's cwd and check that the app
           exists and is executable The function will
           take care of outputting a pretty error message, if required
        */
        if (prte_get_attribute(&app->attributes, PRTE_APP_USER_CWD, NULL, PMIX_BOOL)) {
            usercwd = true;
        }
        rc = pmix_util_check_context_cwd(&app->cwd, true, usercwd);
        if (PMIX_SUCCESS != rc) {
            /* Do not ERROR_LOG, and do NOT convert. This status ends up as
             * the proc's exit_code, and prte_render_launch_failure()
             * (src/runtime/prte_quit.c) switches on that value to pick the
             * diagnostic - it has arms for the PMIx codes this returns
             * (PMIX_ERR_JOB_WDIR_NOT_FOUND / _NOT_ACCESSIBLE) alongside
             * arms for PRRTE codes. Converting collapses them to a generic
             * PRTE_ERROR, and the user gets "Error code: -3001, Error
             * name: Error" instead of being told which directory. The
             * exit_code field is deliberately a union of both numbering
             * schemes; see the note in this framework's AGENTS.md. */
            goto CLEANUP;
        }

        /* The prior function will have done a chdir() to jump us to
         * wherever the app is to be executed. This could be either where
         * the user specified (via -wdir), or to the user's home directory
         * on this node if nothing was provided. It seems that chdir doesn't
         * adjust the $PWD enviro variable when it changes the directory. This
         * can cause a user to get a different response when doing getcwd vs
         * looking at the enviro variable. To keep this consistent, we explicitly
         * ensure that the PWD enviro variable matches the CWD we moved to.
         *
         * NOTE: if a user's program does a chdir(), then $PWD will once
         * again not match getcwd! This is beyond our control - we are only
         * ensuring they start out matching.
         */
        if (NULL == getcwd(dir, sizeof(dir))) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* free any prior value before overwriting (see note above) */
        if (NULL != *wdir) {
            free(*wdir);
        }
        *wdir = strdup(dir);
        PMIx_Setenv("PWD", dir, true, &app->env);
    }

CLEANUP:
    return rc;
}

/* define a timer release point so that we can wait for
 * file descriptors to come available, if necessary
 */
static void timer_cb(int fd, short event, void *cbdata)
{
    prte_timer_t *tm = (prte_timer_t *) cbdata;
    prte_odls_launch_local_t *ll = (prte_odls_launch_local_t *) tm->payload;
    PRTE_HIDE_UNUSED_PARAMS(fd, event);

    PMIX_ACQUIRE_OBJECT(tm);

    /* increment the number of retries */
    ll->retries++;

    /* re-attempt the launch */
    prte_event_active(ll->ev, PRTE_EV_WRITE, 1);

    /* release the timer event */
    PMIX_RELEASE(tm);
}

static int compute_num_procs_alive(pmix_nspace_t job)
{
    int i;
    prte_proc_t *child;
    int num_procs_alive = 0;

    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL == (child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        if (!PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {
            continue;
        }
        /* do not include members of the specified job as they
         * will be added later, if required
         */
        if (PMIX_CHECK_NSPACE(job, child->name.nspace)) {
            continue;
        }
        num_procs_alive++;
    }
    return num_procs_alive;
}

void prte_odls_base_spawn_proc(int fd, short sd, void *cbdata)
{
    prte_odls_spawn_caddy_t *cd = (prte_odls_spawn_caddy_t *) cbdata;
    prte_job_t *jobdat = cd->jdata;
    prte_app_context_t *app = cd->app;
    prte_proc_t *child = cd->child;
    int rc = PRTE_SUCCESS;
    int i;
    bool found;
    prte_proc_state_t state;
    pmix_proc_t pproc;
    pmix_status_t ret;
    char *ptr;
    pmix_value_t pidval = PMIX_VALUE_STATIC_INIT;

    PRTE_HIDE_UNUSED_PARAMS(fd, sd);

    PMIX_ACQUIRE_OBJECT(cd);


    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_DO_NOT_SPAWN, NULL, PMIX_BOOL)) {
        // if we aren't spawning the apps, then just mark them as
        // terminated and return. Drop the waitpid registration our caller
        // made first: there will be no fork, so it can never fire, and the
        // tracker (plus its reference on the child) would otherwise live
        // until the daemon exits - once per proc of every mapping-only job.
        prte_wait_cb_cancel(child);
        PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_TERMINATED);
        PMIX_RELEASE(cd);
        return;
    }

    /* ensure we clear any prior info regarding state or exit status in
     * case this is a restart
     */
    child->exit_code = 0;
    PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_WAITPID);

    /* setup the pmix environment */
    cd->env = PMIx_Argv_copy(app->env);
    PMIX_LOAD_PROCID(&pproc, child->name.nspace, child->name.rank);
    if (PMIX_SUCCESS != (ret = PMIx_server_setup_fork(&pproc, &cd->env))) {
        PMIX_ERROR_LOG(ret);
        rc = PRTE_ERROR;
        state = PRTE_PROC_STATE_FAILED_TO_LAUNCH;
        goto errorout;
    }

    /* if we are not forwarding output for this job, then
     * flag iof as complete
     */
    if (PRTE_FLAG_TEST(jobdat, PRTE_JOB_FLAG_FORWARD_OUTPUT)) {
        PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_IOF_COMPLETE);
    } else {
        PRTE_FLAG_SET(child, PRTE_PROC_FLAG_IOF_COMPLETE);
    }
    child->pid = 0;
    if (NULL != child->rml_uri) {
        free(child->rml_uri);
        child->rml_uri = NULL;
    }

    /* did the user request we display output in xterms? */
    if (NULL != prte_xterm) {
        pmix_list_item_t *nmitem;
        prte_namelist_t *nm;
        /* see if this rank is one of those requested */
        found = false;
        for (nmitem = pmix_list_get_first(&prte_odls_globals.xterm_ranks);
             nmitem != pmix_list_get_end(&prte_odls_globals.xterm_ranks);
             nmitem = pmix_list_get_next(nmitem)) {
            nm = (prte_namelist_t *) nmitem;
            if (PMIX_RANK_WILDCARD == nm->name.rank || child->name.rank == nm->name.rank) {
                /* we want this one - modify the app's command to include
                 * the prte xterm cmd that starts with the xtermcmd */
                cd->argv = PMIx_Argv_copy(prte_odls_globals.xtermcmd);
                /* insert the rank into the correct place as a window title */
                free(cd->argv[2]);
                pmix_asprintf(&cd->argv[2], "Rank %s", PRTE_VPID_PRINT(child->name.rank));
                /* add in the argv from the app */
                for (i = 0; NULL != app->argv[i]; i++) {
                    PMIx_Argv_append_nosize(&cd->argv, app->argv[i]);
                }
                /* use the xterm cmd as the app string */
                cd->cmd = strdup(prte_odls_globals.xtermcmd[0]);
                found = true;
                break;
            } else if (jobdat->num_procs <= nm->name.rank) { /* check for bozo case */
                /* can't be done! */
                prte_show_help("help-prte-odls-base.txt", "prte-odls-base:xterm-rank-out-of-bounds",
                               true, prte_process_info.nodename, nm->name.rank, jobdat->num_procs);
                rc = PRTE_ERR_BAD_PARAM;
                state = PRTE_PROC_STATE_FAILED_TO_LAUNCH;
                goto errorout;
            }
        }
        if (!found) {
            cd->cmd = strdup(app->app);
            cd->argv = PMIx_Argv_copy(app->argv);
        }
    } else if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_EXEC_AGENT, (void**)&ptr, PMIX_STRING)) {
        /* we were given a fork agent - use it */
        cd->argv = PMIx_Argv_split(ptr, ' ');
        /* add in the argv from the app */
        for (i = 0; NULL != app->argv[i]; i++) {
            PMIx_Argv_append_nosize(&cd->argv, app->argv[i]);
        }
        cd->cmd = pmix_path_findv(cd->argv[0], X_OK, prte_launch_environ, NULL);
        if (NULL == cd->cmd) {
            prte_show_help("help-prte-odls-base.txt", "prte-odls-base:fork-agent-not-found", true,
                           prte_process_info.nodename, ptr);
            rc = PRTE_ERR_NOT_FOUND;
            state = PRTE_PROC_STATE_FAILED_TO_LAUNCH;
            free(ptr);
            goto errorout;
        }
        free(ptr);
    } else if (NULL != prte_odls_globals.exec_agent) {
        /* we were given a fork agent - use it */
        cd->argv = PMIx_Argv_split(prte_odls_globals.exec_agent, ' ');
        /* add in the argv from the app */
        for (i = 0; NULL != app->argv[i]; i++) {
            PMIx_Argv_append_nosize(&cd->argv, app->argv[i]);
        }
        cd->cmd = pmix_path_findv(cd->argv[0], X_OK, prte_launch_environ, NULL);
        if (NULL == cd->cmd) {
            prte_show_help("help-prte-odls-base.txt", "prte-odls-base:fork-agent-not-found", true,
                           prte_process_info.nodename, cd->argv[0]);
            rc = PRTE_ERR_NOT_FOUND;
            state = PRTE_PROC_STATE_FAILED_TO_LAUNCH;
            goto errorout;
        }
    } else {
        cd->cmd = strdup(app->app);
        cd->argv = PMIx_Argv_copy(app->argv);
    }

    /* An app may carry no argv at all - a PMIx_Spawn is allowed to name a
     * cmd and nothing else - so default it here, before anything reads
     * argv[0]. The index-argv rewrite immediately below does exactly that,
     * and used to dereference NULL for such an app. (The component defaults
     * it too, but only once it has the caddy in hand, which is too late.) */
    if (NULL == cd->argv) {
        PMIx_Argv_append_nosize(&cd->argv, cd->cmd);
    }

    /* if we are indexing the argv by rank, do so now */
    if (cd->index_argv) {
        char *param;
        pmix_asprintf(&param, "%s-%u", cd->argv[0], child->name.rank);
        free(cd->argv[0]);
        cd->argv[0] = param;
    }

    pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                        "%s odls:launch spawning child %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&child->name));

    if (15 < pmix_output_get_verbosity(prte_odls_base_framework.framework_output)) {
        /* dump what is going to be exec'd */
        char *output = NULL;
        prte_app_print(&output, jobdat, app);
        pmix_output(prte_odls_base_framework.framework_output, "%s", output);
        free(output);
    }

    /* compute this child's binding in the parent, before the fork, so the
       async-signal-safe child only has to issue the bind syscalls */
    prte_odls_base_prepare_binding(cd);

    if (PRTE_SUCCESS != (rc = cd->fork_local(cd))) {
        /* error message already output */
        state = PRTE_PROC_STATE_FAILED_TO_START;
        goto errorout;
    }
    if (PRTE_PROC_IS_MASTER) {
        /* locally store the pid */
        pidval.type = PMIX_PID;
        pidval.data.pid = child->pid;
        /* store the PID for later retrieval */
        rc = PMIx_Store_internal(&child->name, PMIX_PROC_PID, &pidval);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_RUNNING);
    PMIX_RELEASE(cd);
    return;

errorout:
    PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_ALIVE);
    if (0 >= child->pid) {
        /* No child process exists: either we never reached the fork
         * (child->pid is cleared above and only fork_local sets it) or the
         * fork itself failed, which stores -1. Either way the waitpid
         * registration our caller made can never fire, so drop it -
         * otherwise the tracker, and the reference it holds on the child,
         * live until the daemon exits. A failure *after* a successful fork
         * keeps its registration: that child exists, and its SIGCHLD is
         * still coming. */
        prte_wait_cb_cancel(child);
    }
    child->exit_code = rc;
    PRTE_ACTIVATE_PROC_STATE(&child->name, state);
    PMIX_RELEASE(cd);
}

/*
 * If this "NAME=value" environment entry is for exactly this variable,
 * return its value; otherwise NULL.
 *
 * The name has to be matched up to - and including - the '='.  A bare
 * strncmp() of the name's length matches any variable that merely STARTS
 * with it, so "--prepend-env PATH[:] /foo" would edit PATHEXT (or
 * PATH_SEPARATOR, or the user's own PATHOLOGY) instead of, or as well as,
 * PATH - whichever the environment happened to list first.
 */
static char *envar_value(char *entry, const char *name)
{
    size_t len = strlen(name);

    if (0 != strncmp(entry, name, len) || '=' != entry[len]) {
        return NULL;
    }
    return entry + len + 1;
}

/*
 * Remove a variable from the app's environment.  A trailing '*' makes the
 * name a prefix: every variable that starts with it goes.
 */
static void unset_envar(const char *name, prte_app_context_t *app)
{
    char *ptr, *tmp, *p2;
    size_t n;

    if (NULL == name) {
        return;
    }
    if (NULL == strchr(name, '*')) {
        pmix_unsetenv((char *) name, &app->env);
        return;
    }
    ptr = strdup(name);
    ptr[strlen(ptr) - 1] = '\0'; // trim off the '*'
    for (n = 0; NULL != app->env[n]; n++) {
        if (0 == strncmp(app->env[n], ptr, strlen(ptr))) {
            // find the '=' sign
            tmp = strdup(app->env[n]);
            p2 = strchr(tmp, '=');
            if (NULL != p2) {
                *p2 = '\0';
                pmix_unsetenv(tmp, &app->env);
            }
            free(tmp);
            /* the entry we just removed shifted the array down, so re-check
             * this index rather than stepping past the new occupant */
            --n;
        }
    }
    free(ptr);
}

/*
 * Apply the job's and then the app's envar directives to app->env.
 *
 * Exported (rather than file-static) so the unit tests can drive it: it is
 * a pure function of the two attribute lists and the environment, and the
 * semantics it implements - SET vs ADD, the front-to-back ordering, the
 * "match up to the '='" rule, and app-trumps-job - are all contracts a
 * silent regression would break with no visible symptom until a user's
 * PATH came out wrong.
 */
void prte_odls_base_process_envars(prte_job_t *jdata,
                                   prte_app_context_t *app)
{
    prte_attribute_t *attr;
    pmix_value_t *val;
    pmix_envar_t *envar;
    char *ptr, *tmp;
    size_t n;
    bool found;

    PMIX_LIST_FOREACH(attr, &jdata->attributes, prte_attribute_t) {
        /* UNSET names a variable, so it is carried as a STRING, not as a
         * pmix_envar_t - it has to be handled BEFORE the PMIX_ENVAR type
         * filter below, and read out of data.string.  Filtered out first
         * (and read as an envar), --unset-env quietly did nothing. */
        if (attr->key == PRTE_JOB_UNSET_ENVAR) {
            unset_envar(attr->data.data.string, app);
            continue;
        }
        if (PMIX_ENVAR != attr->data.type) {
            continue;
        }
        val = &attr->data;
        envar = &val->data.envar;
        if (attr->key == PRTE_JOB_SET_ENVAR) {
            PMIx_Setenv(envar->envar, envar->value, true, &app->env);

        } else if (attr->key == PRTE_JOB_ADD_ENVAR) {
            /* ADD is not SET: it means "add this envar, but do NOT override
             * a pre-existing one" (see attr.h, and PMIX_ADD_ENVAR from which
             * it is translated). Overwriting here silently discarded a value
             * the user - or the environment - had already established. */
            PMIx_Setenv(envar->envar, envar->value, false, &app->env);

        } else if (attr->key == PRTE_JOB_PREPEND_ENVAR) {
            // see if this envar exists
            ptr = NULL;
            found = false;
            for (n=0; NULL != app->env[n]; n++) {
                ptr = envar_value(app->env[n], envar->envar);
                if (NULL != ptr) {
                    // this is a copied env, so we can modify it
                    pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, envar->value, envar->separator, ptr);
                    free(app->env[n]);
                    app->env[n] = tmp;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // didn't find it
                PMIx_Setenv(envar->envar, envar->value, true, &app->env);
            }

        } else if (attr->key == PRTE_JOB_APPEND_ENVAR) {
            // see if this envar exists
            ptr = NULL;
            found = false;
            for (n=0; NULL != app->env[n]; n++) {
                ptr = envar_value(app->env[n], envar->envar);
                if (NULL != ptr) {
                    // this is a copied env, so we can modify it
                    pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, ptr, envar->separator, envar->value);
                    free(app->env[n]);
                    app->env[n] = tmp;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // didn't find it
                PMIx_Setenv(envar->envar, envar->value, true, &app->env);
            }
        }
    }

    // app trumps job, so do it after the job
    PMIX_LIST_FOREACH(attr, &app->attributes, prte_attribute_t) {
        /* see the note on UNSET in the job loop above */
        if (attr->key == PRTE_APP_UNSET_ENVAR) {
            unset_envar(attr->data.data.string, app);
            continue;
        }
        if (PMIX_ENVAR != attr->data.type) {
            continue;
        }
        val = &attr->data;
        envar = &val->data.envar;
        if (attr->key == PRTE_APP_SET_ENVAR) {
            PMIx_Setenv(envar->envar, envar->value, true, &app->env);

        } else if (attr->key == PRTE_APP_ADD_ENVAR) {
            /* see the note on ADD in the job loop above */
            PMIx_Setenv(envar->envar, envar->value, false, &app->env);

        } else if (attr->key == PRTE_APP_PREPEND_ENVAR) {
            // see if this envar exists
            ptr = NULL;
            found = false;
            for (n=0; NULL != app->env[n]; n++) {
                ptr = envar_value(app->env[n], envar->envar);
                if (NULL != ptr) {
                    // this is a copied env, so we can modify it
                    pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, envar->value, envar->separator, ptr);
                    free(app->env[n]);
                    app->env[n] = tmp;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // didn't find it
                PMIx_Setenv(envar->envar, envar->value, true, &app->env);
            }

        } else if (attr->key == PRTE_APP_APPEND_ENVAR) {
            // see if this envar exists
            ptr = NULL;
            found = false;
            for (n=0; NULL != app->env[n]; n++) {
                ptr = envar_value(app->env[n], envar->envar);
                if (NULL != ptr) {
                    // this is a copied env, so we can modify it
                    pmix_asprintf(&tmp, "%s=%s%c%s", envar->envar, ptr, envar->separator, envar->value);
                    free(app->env[n]);
                    app->env[n] = tmp;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // didn't find it
                PMIx_Setenv(envar->envar, envar->value, true, &app->env);
            }
        }
    }
}

void prte_odls_base_default_launch_local(int fd, short sd, void *cbdata)
{
    prte_app_context_t *app;
    prte_proc_t *child = NULL;
    int rc = PRTE_SUCCESS;
    char basedir[PRTE_PATH_MAX];
    int j, idx;
    int total_num_local_procs = 0;
    prte_odls_launch_local_t *caddy = (prte_odls_launch_local_t *) cbdata;
    prte_job_t *jobdat;
    pmix_nspace_t job;
    prte_odls_base_fork_local_proc_fn_t fork_local = caddy->fork_local;
    bool index_argv;
    char *msg, **xfer;
    prte_odls_spawn_caddy_t *cd;
    prte_event_base_t *evb;
    prte_schizo_base_module_t *schizo;
    PRTE_HIDE_UNUSED_PARAMS(fd, sd);

    PMIX_ACQUIRE_OBJECT(caddy);

    pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                        "%s local:launch",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_LOAD_NSPACE(job, caddy->job);

    /* establish our baseline working directory - we will be potentially
     * bouncing around as we execute various apps, but we will always return
     * to this place as our default directory
     */
    if (NULL == getcwd(basedir, sizeof(basedir))) {
        /* basedir is unusable, so do NOT fall through to ERROR_OUT (which
         * would chdir() to an uninitialized buffer). Fail the job we were
         * asked to launch and release the caddy directly. */
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        if (NULL != (jobdat = prte_get_job_data_object(job))) {
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
        }
        PMIX_RELEASE(caddy);
        return;
    }
    /* find the jobdat for this job */
    if (NULL == (jobdat = prte_get_job_data_object(job))) {
        /* not much we can do here - the most likely explanation
         * is that a job that didn't involve us already completed
         * and was removed. This isn't an error so just move along */
        goto ERROR_OUT;
    }
    schizo = (prte_schizo_base_module_t*)jobdat->schizo;

    /* do we have any local procs to launch? */
    if (0 == jobdat->num_local_procs) {
        /* indicate that we are done trying to launch them */
        pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                            "%s local:launch no local procs",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        goto GETOUT;
    }

    /* track if we are indexing argvs so we don't check every time */
    index_argv = prte_get_attribute(&jobdat->attributes, PRTE_JOB_INDEX_ARGV, NULL, PMIX_BOOL);

    /* compute the total number of local procs currently alive and about to be launched */
    total_num_local_procs = compute_num_procs_alive(job) + jobdat->num_local_procs;

    /* check the system limits - if we are at our max allowed children, then
     * we won't be allowed to do this anyway, so we may as well abort now.
     * According to the documentation, num_procs = 0 is equivalent to
     * no limit, so treat it as unlimited here.
     */
    if (0 < prte_sys_limits.num_procs) {
        PMIX_OUTPUT_VERBOSE((10, prte_odls_base_framework.framework_output,
                             "%s checking limit on num procs %d #children needed %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_sys_limits.num_procs,
                             total_num_local_procs));
        if (prte_sys_limits.num_procs < total_num_local_procs) {
            if (2 < caddy->retries) {
                /* if we have already tried too many times, then just give up */
                PRTE_ODLS_SET_ERROR(job, PMIX_ERR_SYS_LIMITS_CHILDREN, UINT_MAX);
                PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
                goto ERROR_OUT;
            }
            /* set a timer event so we can retry later - this
             * gives the system a chance to let other procs
             * terminate, thus creating room for new ones
             */
            PRTE_DETECT_TIMEOUT(1000, 1000, -1, timer_cb, caddy);
            return;
        }
    }

    /* check to see if we have enough available file descriptors
     * to launch these children - if not, then let's wait a little
     * while to see if some come free. This can happen if we are
     * in a tight loop over comm_spawn
     */
    if (0 < prte_sys_limits.num_files) {
        int limit;
        limit = 4 * total_num_local_procs + 6 * jobdat->num_local_procs;
        PMIX_OUTPUT_VERBOSE((10, prte_odls_base_framework.framework_output,
                             "%s checking limit on file descriptors %d need %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_sys_limits.num_files, limit));
        if (prte_sys_limits.num_files < limit) {
            if (2 < caddy->retries) {
                /* tried enough - give up */
                PRTE_ODLS_SET_ERROR(job, PMIX_ERR_SYS_LIMITS_FILES, UINT_MAX);
                PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
                goto ERROR_OUT;
            }
            /* don't have enough - wait a little time */
            PRTE_DETECT_TIMEOUT(1000, 1000, -1, timer_cb, caddy);
            return;
        }
    }

    for (j = 0; j < jobdat->apps->size; j++) {
        app = (prte_app_context_t *) pmix_pointer_array_get_item(jobdat->apps, j);
        if (NULL == app) {
            continue;
        }

        /* if this app isn't being used on our node, skip it */
        if (!PRTE_FLAG_TEST(app, PRTE_APP_FLAG_USED_ON_NODE)) {
            pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                                "%s app %d not used on node",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), j);
            continue;
        }

        /* setup the working directory for this app - will jump us
         * to that directory
         */
        if (PRTE_SUCCESS != (rc = setup_path(jobdat, app, &app->cwd))) {
            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s odls:launch:setup_path failed with error %s(%d)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_ERROR_NAME(rc), rc));
            /* do not ERROR_LOG this failure - it will be reported
             * elsewhere. The launch is going to fail. Since we could have
             * multiple app_contexts, we need to ensure that we flag only
             * the correct one that caused this operation to fail. We then have
             * to flag all the other procs from the app_context as having "not failed"
             * so we can report things out correctly
             */
            PRTE_ODLS_SET_ERROR(job, rc, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* setup the environment for this app */
        if (NULL == app->env) {
            app->env = PMIx_Argv_copy(prte_launch_environ);
        } else {
            xfer = pmix_environ_merge(prte_launch_environ, app->env);
            PMIx_Argv_free(app->env);
            app->env = xfer;
        }

        // process any provided env directives
        prte_odls_base_process_envars(jobdat, app);


        if (PRTE_SUCCESS != (rc = schizo->setup_fork(jobdat, app))) {

            PMIX_OUTPUT_VERBOSE((10, prte_odls_base_framework.framework_output,
                                 "%s odls:launch:setup_fork failed with error %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_ERROR_NAME(rc)));

            /* do not ERROR_LOG this failure - it will be reported elsewhere.
             * Flag this app's procs as failed (with the correct error code),
             * then drive the whole job down. Aborting the job as a whole -
             * rather than just failing this app's procs - is what keeps a
             * multi-app launch from hanging: any procs belonging to apps we
             * have not reached yet would otherwise be left in INIT forever,
             * and on a real daemon the errmgr only reports FAILED_TO_LAUNCH
             * once every local proc has been accounted for. */
            PRTE_ODLS_SET_ERROR(job, rc, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* setup any local files that were prepositioned for us */
        if (PRTE_SUCCESS != (rc = prte_filem.link_local_files(jobdat, app))) {
            /* flag this app's procs and abort the whole job (see setup_fork) */
            PRTE_ODLS_SET_ERROR(job, rc, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        rc = pmix_util_check_context_app(&app->app, app->cwd, app->env);
        /* do not ERROR_LOG - it will be reported elsewhere */
        if (PMIX_SUCCESS != rc) {
            /* Deliberately NOT converted - see the note in setup_path().
             * prte_render_launch_failure() keys on PMIX_ERR_JOB_EXE_NOT_FOUND
             * and PMIX_ERR_EXE_NOT_ACCESSIBLE, which is the only thing that
             * lets the tool name the executable it could not run. */
            /* flag this app's procs and abort the whole job (see setup_fork) */
            PRTE_ODLS_SET_ERROR(job, rc, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* if the user requested it, set the system resource limits */
        if (PRTE_SUCCESS != (rc = prte_util_init_sys_limits(&msg))) {
            /* the "Application name" field is a %s - pass the app's
             * executable, not the app_context object itself */
            prte_show_help("help-prte-odls-default.txt", "set limit", true,
                           prte_process_info.nodename, app->app, __FILE__, __LINE__, msg);
            /* flag this app's procs and abort the whole job (see setup_fork) */
            PRTE_ODLS_SET_ERROR(job, rc, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* reset our working directory back to our default location - if we
         * don't do this, then we will be looking for relative paths starting
         * from the last wdir option specified by the user. Thus, we would
         * be requiring that the user keep track on the cmd line of where
         * each app was located relative to the prior app, instead of relative
         * to their current location
         */
        if (0 != chdir(basedir)) {
            /* NOTE: we are still in the per-app loop, before the per-child
             * loop that assigns "child", so "child" may be NULL (first app)
             * or a stale proc from a prior app - do not dereference it. Fail
             * the job as a whole. */
            PRTE_ODLS_SET_ERROR(job, PRTE_ERR_NOT_FOUND, j);
            PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* okay, now let's launch all the local procs for this app using the provided fork_local fn
         */
        for (idx = 0; idx < prte_local_children->size; idx++) {
            child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, idx);
            if (NULL == child) {
                continue;
            }
            /* does this child belong to this app? */
            if (j != (int) child->app_idx) {
                continue;
            }

            /* is this child already alive? This can happen if
             * we are asked to launch additional processes.
             * If it has been launched, then do nothing
             */
            if (PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {

                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:launch child %s has already been launched",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&child->name)));

                continue;
            }
            /* is this child a candidate to start? it may not be alive
             * because it already executed
             */
            if (PRTE_PROC_STATE_INIT != child->state &&
                PRTE_PROC_STATE_RESTART != child->state) {
                continue;
            }
            /* do we have a child from the specified job. Because the
             * job could be given as a WILDCARD value, we must use
             * the dss.compare function to check for equality.
             */
            if (!PMIX_CHECK_NSPACE(job, child->name.nspace)) {

                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:launch child %s is not in job %s being launched",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&child->name), PRTE_JOBID_PRINT(job)));

                continue;
            }

            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s odls:launch working child %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&child->name)));

            /* determine the thread that will handle this child.
             *
             * STOP_ON_EXEC requires that the SAME thread which calls fork()
             * (and thereby becomes the ptrace(2) tracer of the child) also
             * performs the later ptrace(PTRACE_DETACH) call - ptrace control
             * operations are only permitted from the exact tracer thread,
             * even though waitpid() for the same status is visible to any
             * thread in the process.  Our STOP_ON_EXEC detach happens in
             * wait_local_proc(), which always runs on prte_event_base, so
             * force the fork onto that same base instead of handing it to
             * one of the odls worker threads - otherwise the PTRACE_DETACH
             * call fails (ESRCH) once the local proc count reaches the
             * worker-thread-pool cutoff. */
            if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_STOP_ON_EXEC, NULL, PMIX_BOOL)) {
                evb = prte_event_base;
            } else {
                ++prte_odls_globals.next_base;
                if (prte_odls_globals.num_threads <= prte_odls_globals.next_base) {
                    prte_odls_globals.next_base = 0;
                }
                evb = prte_odls_globals.ev_bases[prte_odls_globals.next_base];
            }

            /* set the waitpid callback here for thread protection and
             * to ensure we can capture the callback on shortlived apps */
            PRTE_FLAG_SET(child, PRTE_PROC_FLAG_ALIVE);
            prte_wait_cb(child, prte_odls_base_default_wait_local_proc, NULL);

            /* dispatch this child to the next available launch thread */
            cd = PMIX_NEW(prte_odls_spawn_caddy_t);
            cd->jdata = jobdat;
            cd->app = app;
            cd->wdir = strdup(app->cwd);
            cd->child = child;
            cd->fork_local = fork_local;
            cd->index_argv = index_argv;
            /* setup any IOF */
            cd->opts.usepty = PRTE_ENABLE_PTY_SUPPORT;

            /* do we want to setup stdin? */
            if (jobdat->stdin_target == PMIX_RANK_WILDCARD ||
                child->name.rank == jobdat->stdin_target) {
                cd->opts.connect_stdin = true;
            } else {
                cd->opts.connect_stdin = false;
            }
            if (PRTE_SUCCESS != (rc = prte_iof_base_setup_prefork(&cd->opts))) {
                PRTE_ERROR_LOG(rc);
                child->exit_code = rc;
                PMIX_RELEASE(cd);
                /* we flagged this child ALIVE and registered its waitpid
                 * above, in anticipation of a fork that is now never going
                 * to happen. Undo both, or the daemon keeps a wait tracker
                 * for a pid that will never exist and the child looks alive
                 * to every teardown path that scans prte_local_children. */
                PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_ALIVE);
                prte_wait_cb_cancel(child);
                PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_FAILED_TO_LAUNCH);
                /* abort the whole job (see setup_fork): other children of
                 * this app - and later apps - that we will now never launch
                 * must not be left to strand it */
                PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
                goto GETOUT;
            }
            if (PRTE_FLAG_TEST(jobdat, PRTE_JOB_FLAG_FORWARD_OUTPUT)) {
                /* connect endpoints IOF */
                rc = prte_iof_base_setup_parent(&child->name, &cd->opts);
                if (PRTE_SUCCESS != rc) {
                    PRTE_ERROR_LOG(rc);
                    child->exit_code = rc;
                    PMIX_RELEASE(cd);
                    /* see the note on the prefork failure above */
                    PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_ALIVE);
                    prte_wait_cb_cancel(child);
                    PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_FAILED_TO_LAUNCH);
                    /* abort the whole job (see setup_fork) */
                    PRTE_ACTIVATE_JOB_STATE(jobdat, PRTE_JOB_STATE_FAILED_TO_LAUNCH);
                    goto GETOUT;
                }
            }
            pmix_output_verbose(1, prte_odls_base_framework.framework_output,
                                "%s odls:dispatch %s to thread %d",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&child->name),
                                prte_odls_globals.next_base);
            prte_event_set(evb, &cd->ev, -1, PRTE_EV_WRITE, prte_odls_base_spawn_proc, cd);
            prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
        }
    }

GETOUT:

ERROR_OUT:
    /* ensure we reset our working directory back to our default location  */
    if (0 != chdir(basedir)) {
        PRTE_ERROR_LOG(PRTE_ERROR);
    }
    /* release the event */
    PMIX_RELEASE(caddy);
}

/**
 *  Pass a signal to my local procs
 */

int prte_odls_base_default_signal_local_procs(const pmix_proc_t *proc, int32_t signal,
                                              prte_odls_base_signal_local_fn_t signal_local)
{
    int rc, ret, i;
    prte_proc_t *child;

    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                         "%s odls: signaling proc %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == proc) ? "NULL" : PRTE_NAME_PRINT(proc)));

    /* if procs is NULL, then we want to signal all
     * of the local procs, so just do that case
     */
    if (NULL == proc) {
        rc = PRTE_SUCCESS; /* pre-set this as an empty list causes us to drop to bottom */
        for (i = 0; i < prte_local_children->size; i++) {
            child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
            if (NULL == child) {
                continue;
            }
            if (0 >= child->pid || !PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {
                /* skip this one as the child isn't alive */
                continue;
            }
            ret = signal_local(child->pid, (int) signal);
            if (PRTE_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                /* keep going - one unsignalable child must not stop the
                 * rest - but report the FIRST failure rather than whatever
                 * the last child happened to return */
                if (PRTE_SUCCESS == rc) {
                    rc = ret;
                }
            }
        }
        return rc;
    }

    /* we want it sent to some specified process, so find it */
    for (i = 0; i < prte_local_children->size; i++) {
        child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL == child) {
            continue;
        }
        if (PMIX_CHECK_PROCID(&child->name, proc)) {
            /* Apply the same aliveness gate the all-procs branch above
             * uses. Without it this signals a pid of 0 - and the component
             * primitive turns a pid into a process GROUP (-pid), so
             * kill(-0) / kill(0) is "every process in the DAEMON's own
             * group", i.e. the daemon signals itself and every other child
             * it owns. A child sits at pid 0 for a real window: before its
             * fork, after kill_local_procs cleared it, and for any proc
             * that failed to launch - and our caller in prted_comm.c walks
             * every local child of the named job and asks for each one by
             * name, so it hands us exactly those. */
            if (0 >= child->pid || !PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {
                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls: not signaling proc %s - it is not alive",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(proc)));
                return PRTE_SUCCESS;
            }
            if (PRTE_SUCCESS != (rc = signal_local(child->pid, (int) signal))) {
                PRTE_ERROR_LOG(rc);
            }
            return rc;
        }
    }

    /* only way to get here is if we couldn't find the specified proc.
     * report that as an error and return it
     */
    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
    return PRTE_ERR_NOT_FOUND;
}

/*
 *  Wait for a callback indicating the child has completed.
 */

void prte_odls_base_default_wait_local_proc(int fd, short sd, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_proc_t *proc = t2->child;
    int i;
    prte_job_t *jobdat;
    prte_proc_state_t state = PRTE_PROC_STATE_WAITPID_FIRED;
    prte_proc_t *cptr;
    bool flag = false;
    PRTE_HIDE_UNUSED_PARAMS(fd, sd);

    PMIX_ACQUIRE_OBJECT(t2);

    pmix_output_verbose(5, prte_odls_base_framework.framework_output,
                        "%s odls:wait_local_proc child process %s pid %ld terminated",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                        (long) proc->pid);

    /* if the child was previously flagged as dead, then just
     * update its exit status and
     * ensure that its exit state gets reported to avoid hanging
     * don't forget to check if the process was signaled.
     */
    if (!PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_ALIVE)) {
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s was already dead exit code %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             proc->exit_code));
        if (WIFEXITED(proc->exit_code)) {
            proc->exit_code = WEXITSTATUS(proc->exit_code);
            if (0 != proc->exit_code) {
                state = PRTE_PROC_STATE_TERM_NON_ZERO;
            }
        } else {
            if (WIFSIGNALED(proc->exit_code)) {
                state = PRTE_PROC_STATE_ABORTED_BY_SIG;
                proc->exit_code = WTERMSIG(proc->exit_code) + 128;
            }
        }
        goto MOVEON;
    }

    /* mark that the waitpid fired */
    PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_WAITPID);

    /* if the proc called "abort", then we just need to flag that it
     * came thru here */
    if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_ABORT)) {
        /* even though the process exited "normally", it happened
         * via an prte_abort call
         */
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s died by call to abort",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
        state = PRTE_PROC_STATE_CALLED_ABORT;
        goto MOVEON;
    }

    /* Get the jobdat for this child.  Not having it is an error worth
     * recording, but it is NOT a reason to stop: a process we forked has
     * died, and what it died of is knowable from the wait status alone.
     * Bailing out here jumped over the entire decode below, so the proc was
     * reported as a clean WAITPID_FIRED however it had actually gone - a
     * signal death, a non-zero exit - carrying the raw wait status as its
     * exit code (256 for exit(1)).  The only two things the job object is
     * read for below both have a defensible answer without it. */
    if (NULL == (jobdat = prte_get_job_data_object(proc->name.nspace))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
    }

    /* if this child was ordered to die, then just pass that along
     * so we don't hang
     */
    if (PRTE_PROC_STATE_KILLED_BY_CMD == proc->state) {
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s was ordered to die",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
        goto MOVEON;
    }

#if PRTE_HAVE_STOP_ON_EXEC
    /* If the child stopped due to the ptrace TRACEME+exec SIGTRAP, this is the
     * stop-on-exec debug-attach point.  The wait_signal_callback consumed the
     * ptrace-stop notification before do_parent could see it; handle it here
     * instead.  Detach with SIGSTOP injected so the child stays stopped for
     * the debugger, re-register for its eventual exit, and fire READY_FOR_DEBUG.
     * Do NOT fall through to the exit-handling logic below. */
    if (WIFSTOPPED(proc->exit_code) && WSTOPSIG(proc->exit_code) == SIGTRAP) {
        if (NULL != jobdat &&
            prte_get_attribute(&jobdat->attributes, PRTE_JOB_STOP_ON_EXEC, NULL, PMIX_BOOL)) {
            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s odls:waitpid_fired ptrace-stop for %s, detaching with SIGSTOP",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&proc->name)));
            errno = 0;
#    if PRTE_HAVE_LINUX_PTRACE
            ptrace(PRTE_DETACH, proc->pid, 0, (void *) SIGSTOP);
#    else
            ptrace(PRTE_DETACH, proc->pid, 0, SIGSTOP);
#    endif
            if (0 != errno) {
                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:waitpid_fired ptrace detach failed for %s: %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name), strerror(errno)));
                state = PRTE_PROC_STATE_FAILED_TO_START;
                PRTE_FLAG_UNSET(proc, PRTE_PROC_FLAG_ALIVE);
                goto MOVEON;
            }
            proc->state = PRTE_PROC_STATE_RUNNING;
            PRTE_FLAG_SET(proc, PRTE_PROC_FLAG_ALIVE);
            /* re-register to catch the child's eventual exit after the debugger releases it */
            prte_wait_cb(proc, prte_odls_base_default_wait_local_proc, NULL);
            PRTE_ACTIVATE_PROC_STATE(&proc->name, PRTE_PROC_STATE_READY_FOR_DEBUG);
            PMIX_RELEASE(t2);
            return;
        }
    }
#endif

    /* determine the state of this process */
    if (WIFEXITED(proc->exit_code)) {

        /* set the exit status appropriately */
        proc->exit_code = WEXITSTATUS(proc->exit_code);

        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s exit code %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             proc->exit_code));

        /* provide a default state */
        state = PRTE_PROC_STATE_WAITPID_FIRED;
        /* the job object is the authority on whether a non-zero exit is to be
         * treated as an error; with no job object, fall back to the framework
         * default that would have put the attribute there in the first place */
        if (NULL == jobdat) {
            flag = prte_state_base.error_non_zero_exit;
        } else {
            flag = prte_get_attribute(&jobdat->attributes, PRTE_JOB_ERROR_NONZERO_EXIT, NULL,
                                      PMIX_BOOL);
        }

        /* check to see if a sync was required and if it was received */
        if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_REG)) {
            if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_HAS_DEREG) ||
                prte_allowed_exit_without_sync ||
                0 != proc->exit_code) {
                /* if we did recv a finalize sync, or one is not required,
                 * then declare it normally terminated
                 * unless it returned with a non-zero status indicating the code
                 * felt it was non-normal - in this latter case, we do not
                 * require that the proc deregister before terminating
                 */
                if (0 != proc->exit_code && flag) {
                    PMIX_OUTPUT_VERBOSE(
                        (5, prte_odls_base_framework.framework_output,
                         "%s odls:waitpid_fired child process %s terminated normally "
                         "but with a non-zero exit status - it "
                         "will be treated as an abnormal termination",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
                    state = PRTE_PROC_STATE_TERM_NON_ZERO;
                } else {
                    /* indicate the waitpid fired */
                    state = PRTE_PROC_STATE_WAITPID_FIRED;
                }
            } else {
                /* we required a finalizing sync and didn't get it, so this
                 * is considered an abnormal termination and treated accordingly
                 */
                state = PRTE_PROC_STATE_TERM_WO_SYNC;
                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:waitpid_fired child process %s terminated normally "
                                     "but did not provide a required finalize sync - it "
                                     "will be treated as an abnormal termination",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name)));
            }
        } else {
            /* has any child in this job already registered? */
            for (i = 0; i < prte_local_children->size; i++) {
                cptr = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
                if (NULL == cptr) {
                    continue;
                }
                if (!PMIX_CHECK_NSPACE(cptr->name.nspace, proc->name.nspace)) {
                    continue;
                }
                if (PRTE_FLAG_TEST(cptr, PRTE_PROC_FLAG_REG) && !prte_allowed_exit_without_sync) {
                    /* someone has registered, and we didn't before
                     * terminating - this is an abnormal termination unless
                     * the allowed_exit_without_sync flag is set
                     */
                    if (0 != proc->exit_code) {
                        state = PRTE_PROC_STATE_TERM_NON_ZERO;
                        PMIX_OUTPUT_VERBOSE(
                            (5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated normally "
                             "but with a non-zero exit status - it "
                             "will be treated as an abnormal termination",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
                    } else {
                        state = PRTE_PROC_STATE_TERM_WO_SYNC;
                        PMIX_OUTPUT_VERBOSE(
                            (5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated normally "
                             "but did not provide a required init sync - it "
                             "will be treated as an abnormal termination",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name)));
                    }
                    goto MOVEON;
                }
            }
            /* if no child has registered, then it is possible that
             * none of them will. This is considered acceptable. Still
             * flag it as abnormal if the exit code was non-zero
             */
            if (0 != proc->exit_code && flag) {
                state = PRTE_PROC_STATE_TERM_NON_ZERO;
            } else {
                state = PRTE_PROC_STATE_WAITPID_FIRED;
            }
        }

        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             (0 == proc->exit_code) ? "normally" : "with non-zero status"));
    } else if (!WIFSIGNALED(proc->exit_code)) {
        /* Neither exited nor signaled - so this is a STOP status. The only
         * way one reaches us is a ptraced child (a plain waitpid without
         * WUNTRACED does not report stops), and the stop we expect - the
         * TRACEME+exec SIGTRAP - was handled above. Any other stop signal
         * would land here, and WTERMSIG() on a stop status is garbage:
         * reading it would report a nonsense "killed by signal N". Leave
         * the state at its WAITPID_FIRED default and say what happened. */
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s stopped on signal %d "
                             "- not treating it as a termination",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             WIFSTOPPED(proc->exit_code) ? WSTOPSIG(proc->exit_code) : 0));
        proc->exit_code = 0;
    } else {
        /* the process was terminated with a signal! That's definitely
         * abnormal, so indicate that condition
         */
        state = PRTE_PROC_STATE_ABORTED_BY_SIG;
        /* If a process was killed by a signal, then make the
         * exit code of prun be "signo + 128" so that "prog"
         * and "prun prog" will both yield the same exit code.
         *
         * This is actually what the shell does for you when
         * a process dies by signal, so this makes prun treat
         * the termination code to exit status translation the
         * same way
         */
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated with signal %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&proc->name),
                             strsignal(WTERMSIG(proc->exit_code))));
        proc->exit_code = WTERMSIG(proc->exit_code) + 128;

        /* Do not decrement the number of local procs here. That is handled in the errmgr */
    }

MOVEON:
    /* cancel the wait as this proc has already terminated */
    prte_wait_cb_cancel(proc);
    PRTE_ACTIVATE_PROC_STATE(&proc->name, state);
    /* cleanup the tracker */
    PMIX_RELEASE(t2);
}

typedef struct {
    pmix_list_item_t super;
    prte_proc_t *child;
} prte_odls_quick_caddy_t;
static void qcdcon(prte_odls_quick_caddy_t *p)
{
    p->child = NULL;
}
static void qcddes(prte_odls_quick_caddy_t *p)
{
    if (NULL != p->child) {
        PMIX_RELEASE(p->child);
    }
}
PMIX_CLASS_INSTANCE(prte_odls_quick_caddy_t, pmix_list_item_t, qcdcon, qcddes);

int prte_odls_base_default_kill_local_procs(pmix_pointer_array_t *procs,
                                            prte_odls_base_kill_local_fn_t kill_local)
{
    prte_proc_t *child;
    pmix_list_t procs_killed;
    prte_proc_t *proc, proctmp;
    int i, j;
    pmix_pointer_array_t procarray, *procptr;
    bool do_cleanup;
    prte_odls_quick_caddy_t *cd;
    struct timespec tp = {0, 250000000};

    PMIX_CONSTRUCT(&procs_killed, pmix_list_t);

    /* if the pointer array is NULL, then just kill everything */
    if (NULL == procs) {
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:kill_local_proc working on WILDCARD",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_CONSTRUCT(&procarray, pmix_pointer_array_t);
        pmix_pointer_array_init(&procarray, 1, 1, 1);
        PMIX_CONSTRUCT(&proctmp, prte_proc_t);
        PMIX_LOAD_PROCID(&proctmp.name, NULL, PMIX_RANK_WILDCARD);
        pmix_pointer_array_add(&procarray, &proctmp);
        procptr = &procarray;
        do_cleanup = true;
    } else {
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s odls:kill_local_proc working on provided array",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        procptr = procs;
        do_cleanup = false;
    }

    /* cycle through the provided array of processes to kill */
    for (i = 0; i < procptr->size; i++) {
        if (NULL == (proc = (prte_proc_t *) pmix_pointer_array_get_item(procptr, i))) {
            continue;
        }
        for (j = 0; j < prte_local_children->size; j++) {
            child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, j);
            if (NULL == child) {
                continue;
            }

            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s odls:kill_local_proc checking child process %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&child->name)));

            /* do we have a child from the specified job? Because the
             *  job could be given as a WILDCARD value, we must
             *  check for that as well as for equality.
             */
            if (!PMIX_NSPACE_INVALID(proc->name.nspace) &&
                !PMIX_CHECK_NSPACE(proc->name.nspace, child->name.nspace)) {

                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not part of job %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&child->name),
                                     PRTE_JOBID_PRINT(proc->name.nspace)));
                continue;
            }

            /* see if this is the specified proc - could be a WILDCARD again, so check
             * appropriately
             */
            if (PMIX_RANK_WILDCARD != proc->name.rank && proc->name.rank != child->name.rank) {

                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not covered by rank %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&child->name),
                                     PRTE_VPID_PRINT(proc->name.rank)));
                continue;
            }

            /* is this process alive? if not, then nothing for us
             * to do to it
             */
            if (!PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE) || 0 == child->pid) {

                PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not alive",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&child->name)));

                /* ensure, though, that the state is terminated so we don't lockup if
                 * the proc never started
                 */
                if (PRTE_PROC_STATE_UNDEF == child->state ||
                    PRTE_PROC_STATE_INIT == child->state ||
                    PRTE_PROC_STATE_RUNNING == child->state) {
                    /* we can't be sure what happened, but make sure we
                     * at least have a value that will let us eventually wakeup
                     */
                    child->state = PRTE_PROC_STATE_TERMINATED;
                    /* ensure we realize that the waitpid will never come, if
                     * it already hasn't
                     */
                    PRTE_FLAG_SET(child, PRTE_PROC_FLAG_WAITPID);
                    child->pid = 0;
                    goto CLEANUP;
                } else {
                    continue;
                }
            }

            /* ensure the stdin IOF channel for this child is closed. The other
             * channels will automatically close when the proc is killed
             */
            if (NULL != prte_iof.close) {
                prte_iof.close(&child->name, PRTE_IOF_STDIN);
            }

            /* cancel the waitpid callback as this induces unmanageable race
             * conditions when we are deliberately killing the process
             */
            prte_wait_cb_cancel(child);

            /* First send a SIGCONT in case the process is in stopped state.
               If it is in a stopped state and we do not first change it to
               running, then SIGTERM will not get delivered.  Ignore return
               value. */
            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s SENDING SIGCONT TO %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&child->name)));
            cd = PMIX_NEW(prte_odls_quick_caddy_t);
            PMIX_RETAIN(child);
            cd->child = child;
            pmix_list_append(&procs_killed, &cd->super);
            kill_local(child->pid, SIGCONT);
            continue;

        CLEANUP:
            /* check for everything complete - this will remove
             * the child object from our local list
             */
            if (!prte_finalizing && PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_IOF_COMPLETE)
                && PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_WAITPID)) {
                PRTE_ACTIVATE_PROC_STATE(&child->name, child->state);
            }
        }
    }

    /* if we are issuing signals, then we need to wait a little
     * and send the next in sequence */
    if (0 < pmix_list_get_size(&procs_killed)) {
        /* Wait a little. Do so in nanosleep() - can be interrupted by a
         * signal. Most likely SIGCHLD in this case */
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s Sleep %ld nsec",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (long)tp.tv_nsec));
        (void)nanosleep(&tp, NULL);
        /* issue a SIGTERM to all */
        PMIX_LIST_FOREACH(cd, &procs_killed, prte_odls_quick_caddy_t)
        {
            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s SENDING SIGTERM TO %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&cd->child->name)));
            kill_local(cd->child->pid, SIGTERM);
        }
        PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                             "%s Sleep %ld nsec",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (long)tp.tv_nsec));
        /* Wait a little. Do so in nanosleep() - can be interrupted by a
         * signal. Most likely SIGCHLD in this case */
        (void)nanosleep(&tp, NULL);

        /* issue a SIGKILL to all */
        PMIX_LIST_FOREACH(cd, &procs_killed, prte_odls_quick_caddy_t)
        {
            PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                                 "%s SENDING SIGKILL TO %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&cd->child->name)));
            kill_local(cd->child->pid, SIGKILL);
            /* indicate the waitpid fired as this is effectively what
             * has happened
             */
            PRTE_FLAG_SET(cd->child, PRTE_PROC_FLAG_WAITPID);

            /* Since we are not going to wait for this process, make sure
             * we mark it as not-alive so that we don't wait for it
             * in orted_cmd
             */
            PRTE_FLAG_UNSET(cd->child, PRTE_PROC_FLAG_ALIVE);
            cd->child->pid = 0;

            /* mark the child as "killed" */
            if (cd->child->state < PRTE_PROC_STATE_TERMINATED) {
                cd->child->state = PRTE_PROC_STATE_KILLED_BY_CMD; /* we ordered it to die */
            }

            /* check for everything complete - this will remove
             * the child object from our local list
             */
            if (!prte_finalizing &&
                PRTE_FLAG_TEST(cd->child, PRTE_PROC_FLAG_IOF_COMPLETE) &&
                PRTE_FLAG_TEST(cd->child, PRTE_PROC_FLAG_WAITPID)) {
                PRTE_ACTIVATE_PROC_STATE(&cd->child->name, cd->child->state);
            }
        }
    }
    PMIX_LIST_DESTRUCT(&procs_killed);

    /* cleanup arrays, if required */
    if (do_cleanup) {
        PMIX_DESTRUCT(&procarray);
        PMIX_DESTRUCT(&proctmp);
    }

    return PRTE_SUCCESS;
}

int prte_odls_base_default_restart_proc(prte_proc_t *child,
                                        prte_odls_base_fork_local_proc_fn_t fork_local)
{
    int rc;
    prte_app_context_t *app;
    prte_job_t *jobdat;
    char basedir[PRTE_PATH_MAX];
    char *wdir = NULL;
    prte_odls_spawn_caddy_t *cd;
    prte_event_base_t *evb;

    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                         "%s odls:restart_proc for proc %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(&child->name)));

    /* establish our baseline working directory - we will be potentially
     * bouncing around as we execute this app, but we will always return
     * to this place as our default directory
     */
    if (NULL == getcwd(basedir, sizeof(basedir))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* find this child's jobdat */
    if (NULL == (jobdat = prte_get_job_data_object(child->name.nspace))) {
        /* not found */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* CHECK THE NUMBER OF TIMES THIS CHILD HAS BEEN RESTARTED
     * AGAINST MAX_RESTARTS */

    child->state = PRTE_PROC_STATE_FAILED_TO_START;
    child->exit_code = 0;
    PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_WAITPID);
    PRTE_FLAG_UNSET(child, PRTE_PROC_FLAG_IOF_COMPLETE);
    child->pid = 0;
    if (NULL != child->rml_uri) {
        free(child->rml_uri);
        child->rml_uri = NULL;
    }
    app = (prte_app_context_t *) pmix_pointer_array_get_item(jobdat->apps, child->app_idx);
    if (NULL == app) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto CLEANUP;
    }

    /* setup the path */
    if (PRTE_SUCCESS != (rc = setup_path(jobdat, app, &wdir))) {
        PRTE_ERROR_LOG(rc);
        if (NULL != wdir) {
            free(wdir);
        }
        goto CLEANUP;
    }

    /* NEED TO UPDATE THE REINCARNATION NUMBER IN PMIX */

    /* dispatch this child to the next available launch thread */
    cd = PMIX_NEW(prte_odls_spawn_caddy_t);
    if (NULL != wdir) {
        cd->wdir = strdup(wdir);
        free(wdir);
    }
    cd->jdata = jobdat;
    cd->app = app;
    cd->child = child;
    cd->fork_local = fork_local;
    /* setup any IOF */
    cd->opts.usepty = PRTE_ENABLE_PTY_SUPPORT;

    /* do we want to setup stdin? */
    if (jobdat->stdin_target == PMIX_RANK_WILDCARD || child->name.rank == jobdat->stdin_target) {
        cd->opts.connect_stdin = true;
    } else {
        cd->opts.connect_stdin = false;
    }
    if (PRTE_SUCCESS != (rc = prte_iof_base_setup_prefork(&cd->opts))) {
        PRTE_ERROR_LOG(rc);
        child->exit_code = rc;
        PMIX_RELEASE(cd);
        PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_FAILED_TO_LAUNCH);
        goto CLEANUP;
    }
    if (PRTE_FLAG_TEST(jobdat, PRTE_JOB_FLAG_FORWARD_OUTPUT)) {
        /* connect endpoints IOF */
        rc = prte_iof_base_setup_parent(&child->name, &cd->opts);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
            PRTE_ACTIVATE_PROC_STATE(&child->name, PRTE_PROC_STATE_FAILED_TO_LAUNCH);
            goto CLEANUP;
        }
    }
    /* see comment in launch_local_procs() regarding STOP_ON_EXEC + ptrace
     * tracer-thread affinity */
    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_STOP_ON_EXEC, NULL, PMIX_BOOL)) {
        evb = prte_event_base;
    } else {
        ++prte_odls_globals.next_base;
        if (prte_odls_globals.num_threads <= prte_odls_globals.next_base) {
            prte_odls_globals.next_base = 0;
        }
        evb = prte_odls_globals.ev_bases[prte_odls_globals.next_base];
    }
    /* flag the proc alive BEFORE registering the waitpid, exactly as
     * launch_local does. prte_wait_cb short-circuits a registration for a
     * proc that is not flagged ALIVE (it fires the callback immediately as
     * an "already dead" notice), and wait_local_proc's first test is that
     * same flag - so a restarted proc whose eventual exit arrived here
     * would have been decoded down the already-dead path, skipping the
     * sync/exit-status interpretation entirely. */
    PRTE_FLAG_SET(child, PRTE_PROC_FLAG_ALIVE);
    prte_wait_cb(child, prte_odls_base_default_wait_local_proc, NULL);

    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output, "%s restarting app %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), app->app));

    prte_event_set(evb, &cd->ev, -1, PRTE_EV_WRITE, prte_odls_base_spawn_proc, cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);

CLEANUP:
    PMIX_OUTPUT_VERBOSE((5, prte_odls_base_framework.framework_output,
                         "%s odls:restart of proc %s %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(&child->name),
                         (PRTE_SUCCESS == rc) ? "succeeded" : "failed"));

    /* reset our working directory back to our default location - if we
     * don't do this, then we will be looking for relative paths starting
     * from the last wdir option specified by the user. Thus, we would
     * be requiring that the user keep track on the cmd line of where
     * each app was located relative to the prior app, instead of relative
     * to their current location
     */
    if (0 != chdir(basedir)) {
        PRTE_ERROR_LOG(PRTE_ERROR);
    }

    return rc;
}
