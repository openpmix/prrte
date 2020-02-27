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
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif  /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <time.h>
#include <signal.h>
#include <pmix.h>
#include <pmix_server.h>

#include "prrte_stdint.h"
#include "src/util/prrte_environ.h"
#include "src/util/argv.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/sys_limits.h"
#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/pstat/pstat.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rtc/rtc.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/mca/filem/filem.h"

#include "src/util/context_fns.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/session_dir.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_wait.h"
#include "src/prted/prted.h"
#include "src/prted/pmix/pmix_server.h"

#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"

typedef struct {
    prrte_job_t *jdata;
    pmix_info_t *info;
    size_t ninfo;
    prrte_pmix_lock_t lock;
} prrte_odls_jcaddy_t;


static void setup_cbfunc(pmix_status_t status,
                         pmix_info_t info[], size_t ninfo,
                         void *provided_cbdata,
                         pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    prrte_odls_jcaddy_t *cd = (prrte_odls_jcaddy_t*)provided_cbdata;
    prrte_job_t *jdata = cd->jdata;
    prrte_buffer_t cache, *bptr;
    pmix_data_buffer_t pbuf;
    pmix_byte_object_t pbo;
    prrte_byte_object_t bo, *boptr;
    int rc = PRRTE_SUCCESS;

    /* release any info */
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }

    PRRTE_CONSTRUCT(&cache, prrte_buffer_t);
    if (NULL != info) {
        PMIX_DATA_BUFFER_CONSTRUCT(&pbuf);
        /* pack the provided info */
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &pbuf, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            PRRTE_DESTRUCT(&cache);
            goto done;
        }
        if (PMIX_SUCCESS != (rc = PMIx_Data_pack(NULL, &pbuf, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            PRRTE_DESTRUCT(&cache);
            goto done;
        }
        /* unload it */
        PMIX_DATA_BUFFER_UNLOAD(&pbuf, pbo.bytes, pbo.size);
        /* pass it along */
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;
        boptr = &bo;
        prrte_dss.pack(&cache, &boptr, 1, PRRTE_BYTE_OBJECT);
        free(pbo.bytes);
    }
    /* add the results */
    bptr = &cache;
    prrte_dss.pack(&jdata->launch_msg, &bptr, 1, PRRTE_BUFFER);
    PRRTE_DESTRUCT(&cache);

  done:
    /* release our caller */
    if (NULL != cbfunc) {
        cbfunc(rc, cbdata);
    }

    /* move to next stage */
    PRRTE_ACTIVATE_JOB_STATE(jdata, PRRTE_JOB_STATE_SEND_LAUNCH_MSG);

    /* release the original thread */
    PRRTE_PMIX_WAKEUP_THREAD(&cd->lock);

}

/* IT IS CRITICAL THAT ANY CHANGE IN THE ORDER OF THE INFO PACKED IN
 * THIS FUNCTION BE REFLECTED IN THE CONSTRUCT_CHILD_LIST PARSER BELOW
*/
int prrte_odls_base_default_get_add_procs_data(prrte_buffer_t *buffer,
                                              prrte_jobid_t job)
{
    int rc, n;
    prrte_job_t *jdata=NULL, *jptr;
    prrte_job_map_t *map=NULL;
    prrte_buffer_t *wireup, jobdata, priorjob;
    int8_t flag;
    void *nptr;
    uint32_t key;
    prrte_proc_t *proc;
    pmix_info_t *info;
    pmix_proc_t pproc;
    pmix_status_t ret;
    prrte_node_t *node;
    int i, k;
    char **list, **procs, **micro, *tmp, *regex;
    prrte_odls_jcaddy_t cd = {0};
    prrte_proc_t *pptr;

    /* get the job data pointer */
    if (NULL == (jdata = prrte_get_job_data_object(job))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }

    /* get a pointer to the job map */
    map = jdata->map;
    /* if there is no map, just return */
    if (NULL == map) {
        return PRRTE_SUCCESS;
    }

    /* setup the daemon job */
    PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, PRRTE_PROC_MY_NAME->jobid);

    /* we need to ensure that any new daemons get a complete
     * copy of all active jobs so the grpcomm collectives can
     * properly work should a proc from one of the other jobs
     * interact with this one */
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_LAUNCHED_DAEMONS, NULL, PRRTE_BOOL)) {
        flag = 1;
        prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8);
        PRRTE_CONSTRUCT(&jobdata, prrte_buffer_t);
        rc = prrte_hash_table_get_first_key_uint32(prrte_job_data, &key, (void **)&jptr, &nptr);
        while (PRRTE_SUCCESS == rc) {
            /* skip the one we are launching now */
            if (NULL != jptr && jptr != jdata &&
                PRRTE_PROC_MY_NAME->jobid != jptr->jobid) {
                PRRTE_CONSTRUCT(&priorjob, prrte_buffer_t);
                /* pack the job struct */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&priorjob, &jptr, 1, PRRTE_JOB))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_DESTRUCT(&jobdata);
                    PRRTE_DESTRUCT(&priorjob);
                    return rc;
                }
                /* pack the location of each proc */
                for (n=0; n < jptr->procs->size; n++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jptr->procs, n))) {
                        continue;
                    }
                    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&priorjob, &proc->parent, 1, PRRTE_VPID))) {
                        PRRTE_ERROR_LOG(rc);
                        PRRTE_DESTRUCT(&jobdata);
                        PRRTE_DESTRUCT(&priorjob);
                        return rc;
                    }
                }
                /* pack the jobdata buffer */
                wireup = &priorjob;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&jobdata, &wireup, 1, PRRTE_BUFFER))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_DESTRUCT(&jobdata);
                    PRRTE_DESTRUCT(&priorjob);
                    return rc;
                }
                PRRTE_DESTRUCT(&priorjob);
            }
            rc = prrte_hash_table_get_next_key_uint32(prrte_job_data, &key, (void **)&jptr, nptr, &nptr);
        }
        /* pack the jobdata buffer */
        wireup = &jobdata;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &wireup, 1, PRRTE_BUFFER))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_DESTRUCT(&jobdata);
            return rc;
        }
        PRRTE_DESTRUCT(&jobdata);
    } else {
        flag = 0;
        prrte_dss.pack(buffer, &flag, 1, PRRTE_INT8);
    }

    /* pack the job struct */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &jdata, 1, PRRTE_JOB))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
        /* compute and pack the ppn */
        if (PRRTE_SUCCESS != (rc = prrte_util_generate_ppn(jdata, buffer))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* compute the ranks and add the proc objects
     * to the jdata->procs array */
    if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_vpids(jdata))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* assemble the node and proc map info */
    list = NULL;
    procs = NULL;
    cd.ninfo = 3;
    PMIX_INFO_CREATE(cd.info, cd.ninfo);
    for (i=0; i < map->nodes->size; i++) {
        micro = NULL;
        if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(map->nodes, i))) {
            prrte_argv_append_nosize(&list, node->name);
            /* assemble all the ranks for this job that are on this node */
            for (k=0; k < node->procs->size; k++) {
                if (NULL != (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(node->procs, k))) {
                    if (jdata->jobid == pptr->name.jobid) {
                        prrte_argv_append_nosize(&micro, PRRTE_VPID_PRINT(pptr->name.vpid));
                    }
                }
            }
            /* assemble the rank/node map */
            if (NULL != micro) {
                tmp = prrte_argv_join(micro, ',');
                prrte_argv_free(micro);
                prrte_argv_append_nosize(&procs, tmp);
                free(tmp);
            }
        }
    }

    /* let the PMIx server generate the nodemap regex */
    if (NULL != list) {
        tmp = prrte_argv_join(list, ',');
        prrte_argv_free(list);
        list = NULL;
        if (PMIX_SUCCESS != (ret = PMIx_generate_regex(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_FREE(cd.info, cd.ninfo);
            return prrte_pmix_convert_status(ret);
        }
        free(tmp);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&cd.info[0], PMIX_NODE_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&cd.info[0], PMIX_NODE_MAP, regex, PMIX_STRING);
#endif
        free(regex);
    }

    /* let the PMIx server generate the procmap regex */
    if (NULL != procs) {
        tmp = prrte_argv_join(procs, ';');
        prrte_argv_free(procs);
        procs = NULL;
        if (PMIX_SUCCESS != (ret = PMIx_generate_ppn(tmp, &regex))) {
            PMIX_ERROR_LOG(ret);
            free(tmp);
            PMIX_INFO_FREE(cd.info, cd.ninfo);
            return prrte_pmix_convert_status(ret);
        }
        free(tmp);
#ifdef PMIX_REGEX
        PMIX_INFO_LOAD(&cd.info[1], PMIX_PROC_MAP, regex, PMIX_REGEX);
#else
        PMIX_INFO_LOAD(&cd.info[0], PMIX_PROC_MAP, regex, PMIX_STRING);
#endif
        free(regex);
    }

    /* construct the actual request - we just let them pick the
     * default transport for now. Someday, we will add to prun
     * the ability for transport specifications */
    PRRTE_PMIX_CONVERT_JOBID(pproc.nspace, jdata->jobid);
    (void)strncpy(cd.info[2].key, PMIX_ALLOC_NETWORK, PMIX_MAX_KEYLEN);
    cd.info[2].value.type = PMIX_DATA_ARRAY;
#if PMIX_NUMERIC_VERSION < 0x00020203
    PMIX_INFO_CREATE(info, 3);
    cd.info[2].value.data.darray = (pmix_data_array_t*)malloc(sizeof(pmix_data_array_t));
    cd.info[2].value.data.darray->array = info;
    cd.info[2].value.data.darray->size = 3;
#else
    PMIX_DATA_ARRAY_CREATE(cd.info[2].value.data.darray, 3, PMIX_INFO);
    info = (pmix_info_t*)cd.info[2].value.data.darray->array;
#endif
    asprintf(&tmp, "%s.net", pproc.nspace);
    PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NETWORK_ID, tmp, PMIX_STRING);
    free(tmp);
    PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_NETWORK_SEC_KEY, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_SETUP_APP_ENVARS, NULL, PMIX_BOOL);

    /* we don't want to block here because it could
     * take some indeterminate time to get the info */
    rc = PRRTE_SUCCESS;
    cd.jdata = jdata;
    PRRTE_PMIX_CONSTRUCT_LOCK(&cd.lock);
    if (PMIX_SUCCESS != (ret = PMIx_server_setup_application(pproc.nspace, cd.info, cd.ninfo,
                                                             setup_cbfunc, &cd))) {
        prrte_output(0, "[%s:%d] PMIx_server_setup_application failed: %s", __FILE__, __LINE__, PMIx_Error_string(ret));
        rc = PRRTE_ERROR;
    } else {
        PRRTE_PMIX_WAIT_THREAD(&cd.lock);
    }
    PRRTE_PMIX_DESTRUCT_LOCK(&cd.lock);
    return rc;
}

static void ls_cbunc(pmix_status_t status, void *cbdata)
{
    prrte_pmix_lock_t *lock = (prrte_pmix_lock_t*)cbdata;
    PRRTE_PMIX_WAKEUP_THREAD(lock);
}

int prrte_odls_base_default_construct_child_list(prrte_buffer_t *buffer,
                                                prrte_jobid_t *job)
{
    int rc;
    prrte_std_cntr_t cnt;
    prrte_job_t *jdata=NULL, *daemons;
    prrte_node_t *node;
    prrte_vpid_t dmnvpid, v;
    int32_t n;
    prrte_buffer_t *bptr, *jptr;
    prrte_proc_t *pptr, *dmn;
    prrte_app_context_t *app;
    int8_t flag;
    prrte_pmix_lock_t lock;
    pmix_info_t *info = NULL;
    size_t ninfo=0;
    pmix_status_t ret;
    pmix_proc_t pproc;
    pmix_data_buffer_t pbuf;
    prrte_byte_object_t *bo;
    size_t m;
    prrte_envar_t envt;

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls:constructing child list",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* set a default response */
    *job = PRRTE_JOBID_INVALID;
    /* get the daemon job object */
    daemons = prrte_get_job_data_object(PRRTE_PROC_MY_NAME->jobid);
    PRRTE_PMIX_CONSTRUCT_LOCK(&lock);

    /* unpack the flag to see if new daemons were launched */
    cnt=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &flag, &cnt, PRRTE_INT8))) {
        PRRTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }

    if (0 != flag) {
        /* unpack the buffer containing the info */
        cnt=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &bptr, &cnt, PRRTE_BUFFER))) {
            *job = PRRTE_JOBID_INVALID;
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(bptr);
            goto REPORT_ERROR;
        }
        cnt=1;
        while (PRRTE_SUCCESS == (rc = prrte_dss.unpack(bptr, &jptr, &cnt, PRRTE_BUFFER))) {
            /* unpack each job and add it to the local prrte_job_data array */
            cnt=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(jptr, &jdata, &cnt, PRRTE_JOB))) {
                *job = PRRTE_JOBID_INVALID;
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(bptr);
                PRRTE_RELEASE(jptr);
                goto REPORT_ERROR;
            }
            /* check to see if we already have this one */
            if (NULL == prrte_get_job_data_object(jdata->jobid)) {
                /* nope - add it */
                prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, jdata);
            } else {
                /* yep - so we can drop this copy */
                jdata->jobid = PRRTE_JOBID_INVALID;
                PRRTE_RELEASE(jdata);
                PRRTE_RELEASE(jptr);
                cnt=1;
                continue;
            }
            /* unpack the location of each proc in this job */
            for (v=0; v < jdata->num_procs; v++) {
                if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, v))) {
                    pptr = PRRTE_NEW(prrte_proc_t);
                    pptr->name.jobid = jdata->jobid;
                    pptr->name.vpid = v;
                    prrte_pointer_array_set_item(jdata->procs, v, pptr);
                }
                cnt=1;
                if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(jptr, &dmnvpid, &cnt, PRRTE_VPID))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(jptr);
                    PRRTE_RELEASE(bptr);
                    goto REPORT_ERROR;
                }
                /* lookup the daemon */
                if (NULL == (dmn = (prrte_proc_t*)prrte_pointer_array_get_item(daemons->procs, dmnvpid))) {
                    PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                    rc = PRRTE_ERR_NOT_FOUND;
                    PRRTE_RELEASE(jptr);
                    PRRTE_RELEASE(bptr);
                    goto REPORT_ERROR;
                }
                /* connect the two */
                PRRTE_RETAIN(dmn->node);
                pptr->node = dmn->node;
            }
            /* release the buffer */
            PRRTE_RELEASE(jptr);
            cnt = 1;
        }
        PRRTE_RELEASE(bptr);
    }

    /* unpack the job we are to launch */
    cnt=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &jdata, &cnt, PRRTE_JOB))) {
        *job = PRRTE_JOBID_INVALID;
        PRRTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    if (PRRTE_JOBID_INVALID == jdata->jobid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        rc = PRRTE_ERR_BAD_PARAM;
        goto REPORT_ERROR;
    }
    *job = jdata->jobid;

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls:construct_child_list unpacking data to launch job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), PRRTE_JOBID_PRINT(*job)));

    /* if we are the HNP, we don't need to unpack this buffer - we already
     * have all the required info in our local job array. So just build the
     * array of local children
     */
    if (PRRTE_PROC_IS_MASTER) {
        /* we don't want/need the extra copy of the prrte_job_t, but
         * we can't just release it as that will NULL the location in
         * the prrte_job_data array. So set the jobid to INVALID to
         * protect the array, and then release the object to free
         * the storage */
        jdata->jobid = PRRTE_JOBID_INVALID;
        PRRTE_RELEASE(jdata);
        /* get the correct job object - it will be completely filled out */
        if (NULL == (jdata = prrte_get_job_data_object(*job))) {
            PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
            rc = PRRTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
    } else {
        prrte_hash_table_set_value_uint32(prrte_job_data, jdata->jobid, jdata);

        /* ensure the map object is present */
        if (NULL == jdata->map) {
            jdata->map = PRRTE_NEW(prrte_job_map_t);
        }
    }

    /* if the job is fully described, then mpirun will have computed
     * and sent us the complete array of procs in the prrte_job_t, so we
     * don't need to do anything more here */
    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
        /* load the ppn info into the job and node arrays - the
         * function will ignore the data on the HNP as it already
         * has the info */
        if (PRRTE_SUCCESS != (rc = prrte_util_decode_ppn(jdata, buffer))) {
            PRRTE_ERROR_LOG(rc);
            goto REPORT_ERROR;
        }

        if (!PRRTE_PROC_IS_MASTER) {
            /* assign locations to the procs */
            if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_assign_locations(jdata))) {
                PRRTE_ERROR_LOG(rc);
                goto REPORT_ERROR;
            }

            /* compute the ranks and add the proc objects
             * to the jdata->procs array */
            if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_vpids(jdata))) {
                PRRTE_ERROR_LOG(rc);
                goto REPORT_ERROR;
            }
        }

        /* and finally, compute the local and node ranks */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_local_ranks(jdata))) {
            PRRTE_ERROR_LOG(rc);
            goto REPORT_ERROR;
        }
    }

    /* unpack the buffer containing any application setup info - there
     * might not be any, so it isn't an error if we don't find things */
    cnt=1;
    rc = prrte_dss.unpack(buffer, &bptr, &cnt, PRRTE_BUFFER);
    if (PRRTE_SUCCESS != rc) {
        PRRTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }

    cnt=1;
    rc = prrte_dss.unpack(bptr, &bo, &cnt, PRRTE_BYTE_OBJECT);
    PRRTE_RELEASE(bptr);
    if (PRRTE_SUCCESS == rc) {
        /* there was setup data - process it */
        PMIX_DATA_BUFFER_LOAD(&pbuf, bo->bytes, bo->size);
        bo->bytes = NULL;
        bo->size = 0;
        free(bo);
        /* setup the daemon job */
        PRRTE_PMIX_CONVERT_NAME(&pproc, PRRTE_PROC_MY_NAME);
        /* unpack the number of info structs */
        cnt = 1;
        ret = PMIx_Data_unpack(&pproc, &pbuf, &ninfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            rc = PRRTE_ERROR;
            goto REPORT_ERROR;
        }
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        ret = PMIx_Data_unpack(&pproc, &pbuf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
            rc = PRRTE_ERROR;
            goto REPORT_ERROR;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbuf);
        /* add any cache'd values to the front of the job attributes  */
        for (m=0; m < ninfo; m++) {
            if (0 == strcmp(info[m].key, PMIX_SET_ENVAR)) {
                envt.envar = strdup(info[m].value.data.envar.envar);
                envt.value = strdup(info[m].value.data.envar.value);
                envt.separator = info[m].value.data.envar.separator;
                prrte_prepend_attribute(&jdata->attributes, PRRTE_JOB_SET_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envt, PRRTE_ENVAR);
            } else if (0 == strcmp(info[m].key, PMIX_ADD_ENVAR)) {
                envt.envar = info[m].value.data.envar.envar;
                envt.value = info[m].value.data.envar.value;
                envt.separator = info[m].value.data.envar.separator;
                prrte_prepend_attribute(&jdata->attributes, PRRTE_JOB_ADD_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envt, PRRTE_ENVAR);
            } else if (0 == strcmp(info[m].key, PMIX_UNSET_ENVAR)) {
                prrte_prepend_attribute(&jdata->attributes, PRRTE_JOB_UNSET_ENVAR,
                                       PRRTE_ATTR_GLOBAL, info[m].value.data.string, PRRTE_STRING);
            } else if (0 == strcmp(info[m].key, PMIX_PREPEND_ENVAR)) {
                envt.envar = info[m].value.data.envar.envar;
                envt.value = info[m].value.data.envar.value;
                envt.separator = info[m].value.data.envar.separator;
                prrte_prepend_attribute(&jdata->attributes, PRRTE_JOB_PREPEND_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envt, PRRTE_ENVAR);
            } else if (0 == strcmp(info[m].key, PMIX_APPEND_ENVAR)) {
                envt.envar = info[m].value.data.envar.envar;
                envt.value = info[m].value.data.envar.value;
                envt.separator = info[m].value.data.envar.separator;
                prrte_prepend_attribute(&jdata->attributes, PRRTE_JOB_APPEND_ENVAR,
                                       PRRTE_ATTR_GLOBAL, &envt, PRRTE_ENVAR);
            }
        }
    }

    /* now that the node array in the job map and jdata are completely filled out,.
     * we need to "wireup" the procs to their nodes so other utilities can
     * locate them */
    for (n=0; n < jdata->procs->size; n++) {
        if (NULL == (pptr = (prrte_proc_t*)prrte_pointer_array_get_item(jdata->procs, n))) {
            continue;
        }
        if (PRRTE_PROC_STATE_UNDEF == pptr->state) {
            /* not ready for use yet */
            continue;
        }
        if (!PRRTE_PROC_IS_MASTER &&
            prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
            /* the parser will have already made the connection, but the fully described
             * case won't have done it, so connect the proc to its node here */
            prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                                "%s GETTING DAEMON FOR PROC %s WITH PARENT %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&pptr->name),
                                PRRTE_VPID_PRINT(pptr->parent));
            if (PRRTE_VPID_INVALID == pptr->parent) {
                PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                rc = PRRTE_ERR_BAD_PARAM;
                goto REPORT_ERROR;
            }
            /* connect the proc to its node object */
            if (NULL == (dmn = (prrte_proc_t*)prrte_pointer_array_get_item(daemons->procs, pptr->parent))) {
                PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
                rc = PRRTE_ERR_NOT_FOUND;
                goto REPORT_ERROR;
            }
            PRRTE_RETAIN(dmn->node);
            pptr->node = dmn->node;
            /* add the node to the job map, if needed */
            if (!PRRTE_FLAG_TEST(pptr->node, PRRTE_NODE_FLAG_MAPPED)) {
                PRRTE_RETAIN(pptr->node);
                prrte_pointer_array_add(jdata->map->nodes, pptr->node);
                jdata->map->num_nodes++;
                PRRTE_FLAG_SET(pptr->node, PRRTE_NODE_FLAG_MAPPED);
            }
            /* add this proc to that node */
            PRRTE_RETAIN(pptr);
            prrte_pointer_array_add(pptr->node->procs, pptr);
            pptr->node->num_procs++;
        }
        /* see if it belongs to us */
        if (pptr->parent == PRRTE_PROC_MY_NAME->vpid) {
            /* is this child on our current list of children */
            if (!PRRTE_FLAG_TEST(pptr, PRRTE_PROC_FLAG_LOCAL)) {
                /* not on the local list */
                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s[%s:%d] adding proc %s to my local list",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     __FILE__, __LINE__,
                                     PRRTE_NAME_PRINT(&pptr->name)));
                /* keep tabs of the number of local procs */
                jdata->num_local_procs++;
                /* add this proc to our child list */
                PRRTE_RETAIN(pptr);
                PRRTE_FLAG_SET(pptr, PRRTE_PROC_FLAG_LOCAL);
                prrte_pointer_array_add(prrte_local_children, pptr);
            }

            /* if the job is in restart mode, the child must not barrier when launched */
            if (PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_RESTART)) {
                prrte_set_attribute(&pptr->attributes, PRRTE_PROC_NOBARRIER, PRRTE_ATTR_LOCAL, NULL, PRRTE_BOOL);
            }
            /* mark that this app_context is being used on this node */
            app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, pptr->app_idx);
            PRRTE_FLAG_SET(app, PRRTE_APP_FLAG_USED_ON_NODE);
        }
    }
    if (prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
        /* reset the mapped flags */
        for (n=0; n < jdata->map->nodes->size; n++) {
            if (NULL != (node = (prrte_node_t*)prrte_pointer_array_get_item(jdata->map->nodes, n))) {
                PRRTE_FLAG_UNSET(node, PRRTE_NODE_FLAG_MAPPED);
            }
        }
    }

    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
        /* compute and save bindings of local children */
        if (PRRTE_SUCCESS != (rc = prrte_rmaps_base_compute_bindings(jdata))) {
            PRRTE_ERROR_LOG(rc);
            goto REPORT_ERROR;
        }
    }

    /* if we wanted to see the map, now is the time to display it */
    if (jdata->map->display_map) {
        prrte_rmaps_base_display_map(jdata);
    }

    /* register this job with the PMIx server - need to wait until after we
     * have computed the #local_procs before calling the function */
    if (PRRTE_SUCCESS != (rc = prrte_pmix_server_register_nspace(jdata))) {
        PRRTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }

    /* if we have local support setup info, then execute it here - we
     * have to do so AFTER we register the nspace so the PMIx server
     * has the nspace info it needs */
    if (0 < ninfo) {
        (void)prrte_snprintf_jobid(pproc.nspace, PMIX_MAX_NSLEN, jdata->jobid);
        if (PMIX_SUCCESS != (ret = PMIx_server_setup_local_support(pproc.nspace, info, ninfo,
                                                                   ls_cbunc, &lock))) {
            PMIX_ERROR_LOG(ret);
            rc = PRRTE_ERROR;
            goto REPORT_ERROR;
        }
    } else {
        lock.active = false;  // we won't get a callback
    }

    /* load any controls into the job */
    prrte_rtc.assign(jdata);

    /* spin up the spawn threads */
    prrte_odls_base_start_threads(jdata);

    /* to save memory, purge the job map of all procs other than
     * our own - for daemons, this will completely release the
     * proc structures. For the HNP, the proc structs will
     * remain in the prrte_job_t array */

    /* wait here until the local support has been setup */
    PRRTE_PMIX_WAIT_THREAD(&lock);
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    return PRRTE_SUCCESS;

  REPORT_ERROR:
    PRRTE_PMIX_DESTRUCT_LOCK(&lock);
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    /* we have to report an error back to the HNP so we don't just
     * hang. Although there shouldn't be any errors once this is
     * all debugged, it is still good practice to have a way
     * for it to happen - especially so developers don't have to
     * deal with the hang!
     */
    PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_NEVER_LAUNCHED);
    return rc;
}

static int setup_path(prrte_app_context_t *app, char **wdir)
{
    int rc=PRRTE_SUCCESS;
    char dir[MAXPATHLEN];

    if (!prrte_get_attribute(&app->attributes, PRRTE_APP_SSNDIR_CWD, NULL, PRRTE_BOOL)) {
        /* Try to change to the app's cwd and check that the app
           exists and is executable The function will
           take care of outputting a pretty error message, if required
        */
        if (PRRTE_SUCCESS != (rc = prrte_util_check_context_cwd(app, true))) {
            /* do not ERROR_LOG - it will be reported elsewhere */
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
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        *wdir = strdup(dir);
        prrte_setenv("PWD", dir, true, &app->env);
        /* update the initial wdir value too */
        prrte_setenv("OMPI_MCA_initial_wdir", dir, true, &app->env);
    } else {
        *wdir = NULL;
    }

 CLEANUP:
    return rc;
}


/* define a timer release point so that we can wait for
 * file descriptors to come available, if necessary
 */
static void timer_cb(int fd, short event, void *cbdata)
{
    prrte_timer_t *tm = (prrte_timer_t*)cbdata;
    prrte_odls_launch_local_t *ll = (prrte_odls_launch_local_t*)tm->payload;

    PRRTE_ACQUIRE_OBJECT(tm);

    /* increment the number of retries */
    ll->retries++;

    /* re-attempt the launch */
    prrte_event_active(ll->ev, PRRTE_EV_WRITE, 1);

    /* release the timer event */
    PRRTE_RELEASE(tm);
}

static int compute_num_procs_alive(prrte_jobid_t job)
{
    int i;
    prrte_proc_t *child;
    int num_procs_alive = 0;

    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        if (!PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
            continue;
        }
        /* do not include members of the specified job as they
         * will be added later, if required
         */
        if (job == child->name.jobid) {
            continue;
        }
        num_procs_alive++;
    }
    return num_procs_alive;
}

void prrte_odls_base_spawn_proc(int fd, short sd, void *cbdata)
{
    prrte_odls_spawn_caddy_t *cd = (prrte_odls_spawn_caddy_t*)cbdata;
    prrte_job_t *jobdat = cd->jdata;
    prrte_app_context_t *app = cd->app;
    prrte_proc_t *child = cd->child;
    int rc, i;
    bool found;
    prrte_proc_state_t state;
    pmix_proc_t pproc;
    pmix_status_t ret;
    char *ptr;

    PRRTE_ACQUIRE_OBJECT(cd);

    /* thread-protect common values */
    cd->env = prrte_argv_copy(prrte_launch_environ);
    if (NULL != app->env) {
        for (i=0; NULL != app->env[i]; i++) {
            /* find the '=' sign.
             * strdup the env string to a tmp variable,
             * since it is shared among apps.
             */
            char *tmp = strdup(app->env[i]);
            ptr = strchr(tmp, '=');
            *ptr = '\0';
            ++ptr;
            prrte_setenv(tmp, ptr, true, &cd->env);
            free(tmp);
        }
    }

    /* ensure we clear any prior info regarding state or exit status in
     * case this is a restart
     */
    child->exit_code = 0;
    PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_WAITPID);

    /* setup the pmix environment */
    (void)prrte_snprintf_jobid(pproc.nspace, PMIX_MAX_NSLEN, child->name.jobid);
    pproc.rank = child->name.vpid;
    if (PMIX_SUCCESS != (ret = PMIx_server_setup_fork(&pproc, &cd->env))) {
        PMIX_ERROR_LOG(ret);
        rc = PRRTE_ERROR;
        state = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
        goto errorout;
    }

    /* if we are not forwarding output for this job, then
     * flag iof as complete
     */
    if (PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_FORWARD_OUTPUT)) {
        PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_IOF_COMPLETE);
    } else {
        PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_IOF_COMPLETE);
    }
    child->pid = 0;
    if (NULL != child->rml_uri) {
        free(child->rml_uri);
        child->rml_uri = NULL;
    }

    /* setup the rest of the environment with the proc-specific items - these
     * will be overwritten for each child
     */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.setup_child(jobdat, child, app, &cd->env))) {
        PRRTE_ERROR_LOG(rc);
        state = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
        goto errorout;
    }

    /* did the user request we display output in xterms? */
    if (NULL != prrte_xterm && !PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        prrte_list_item_t *nmitem;
        prrte_namelist_t *nm;
        /* see if this rank is one of those requested */
        found = false;
        for (nmitem = prrte_list_get_first(&prrte_odls_globals.xterm_ranks);
             nmitem != prrte_list_get_end(&prrte_odls_globals.xterm_ranks);
             nmitem = prrte_list_get_next(nmitem)) {
            nm = (prrte_namelist_t*)nmitem;
            if (PRRTE_VPID_WILDCARD == nm->name.vpid ||
                child->name.vpid == nm->name.vpid) {
                /* we want this one - modify the app's command to include
                 * the prrte xterm cmd that starts with the xtermcmd */
                cd->argv = prrte_argv_copy(prrte_odls_globals.xtermcmd);
                /* insert the rank into the correct place as a window title */
                free(cd->argv[2]);
                prrte_asprintf(&cd->argv[2], "Rank %s", PRRTE_VPID_PRINT(child->name.vpid));
                /* add in the argv from the app */
                for (i=0; NULL != app->argv[i]; i++) {
                    prrte_argv_append_nosize(&cd->argv, app->argv[i]);
                }
                /* use the xterm cmd as the app string */
                cd->cmd = strdup(prrte_odls_globals.xtermcmd[0]);
                found = true;
                break;
            } else if (jobdat->num_procs <= nm->name.vpid) {  /* check for bozo case */
                /* can't be done! */
                prrte_show_help("help-prrte-odls-base.txt",
                               "prrte-odls-base:xterm-rank-out-of-bounds",
                               true, prrte_process_info.nodename,
                               nm->name.vpid, jobdat->num_procs);
                state = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
                goto errorout;
            }
        }
        if (!found) {
            cd->cmd = strdup(app->app);
            cd->argv = prrte_argv_copy(app->argv);
        }
    } else if (NULL != prrte_fork_agent) {
        /* we were given a fork agent - use it */
        cd->argv = prrte_argv_copy(prrte_fork_agent);
        /* add in the argv from the app */
        for (i=0; NULL != app->argv[i]; i++) {
            prrte_argv_append_nosize(&cd->argv, app->argv[i]);
        }
        cd->cmd = prrte_path_findv(prrte_fork_agent[0], X_OK, prrte_launch_environ, NULL);
        if (NULL == cd->cmd) {
            prrte_show_help("help-prrte-odls-base.txt",
                           "prrte-odls-base:fork-agent-not-found",
                           true, prrte_process_info.nodename, prrte_fork_agent[0]);
            state = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
            goto errorout;
        }
    } else {
        cd->cmd = strdup(app->app);
        cd->argv = prrte_argv_copy(app->argv);
    }

    /* if we are indexing the argv by rank, do so now */
    if (cd->index_argv && !PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        char *param;
        prrte_asprintf(&param, "%s-%d", cd->argv[0], (int)child->name.vpid);
        free(cd->argv[0]);
        cd->argv[0] = param;
    }

    prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                        "%s odls:launch spawning child %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&child->name));

    if (15 < prrte_output_get_verbosity(prrte_odls_base_framework.framework_output)) {
        /* dump what is going to be exec'd */
        prrte_dss.dump(prrte_odls_base_framework.framework_output, app, PRRTE_APP_CONTEXT);
    }

    if (PRRTE_SUCCESS != (rc = cd->fork_local(cd))) {
        /* error message already output */
        state = PRRTE_PROC_STATE_FAILED_TO_START;
        goto errorout;
    }

    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_RUNNING);
    PRRTE_RELEASE(cd);
    return;

  errorout:
    PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_ALIVE);
    child->exit_code = rc;
    PRRTE_ACTIVATE_PROC_STATE(&child->name, state);
    PRRTE_RELEASE(cd);
}

void prrte_odls_base_default_launch_local(int fd, short sd, void *cbdata)
{
    prrte_app_context_t *app;
    prrte_proc_t *child=NULL;
    int rc=PRRTE_SUCCESS;
    char basedir[MAXPATHLEN];
    int j, idx;
    int total_num_local_procs = 0;
    prrte_odls_launch_local_t *caddy = (prrte_odls_launch_local_t*)cbdata;
    prrte_job_t *jobdat;
    prrte_jobid_t job = caddy->job;
    prrte_odls_base_fork_local_proc_fn_t fork_local = caddy->fork_local;
    bool index_argv;
    char *msg;
    prrte_odls_spawn_caddy_t *cd;
    prrte_event_base_t *evb;
    char *effective_dir = NULL;
    char **argvptr;
    char *pathenv = NULL, *mpiexec_pathenv = NULL;
    char *full_search;

    PRRTE_ACQUIRE_OBJECT(caddy);

    prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                        "%s local:launch",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* establish our baseline working directory - we will be potentially
     * bouncing around as we execute various apps, but we will always return
     * to this place as our default directory
     */
    if (NULL == getcwd(basedir, sizeof(basedir))) {
        PRRTE_ACTIVATE_JOB_STATE(NULL, PRRTE_JOB_STATE_FAILED_TO_LAUNCH);
        goto ERROR_OUT;
    }
    /* find the jobdat for this job */
    if (NULL == (jobdat = prrte_get_job_data_object(job))) {
        /* not much we can do here - the most likely explanation
         * is that a job that didn't involve us already completed
         * and was removed. This isn't an error so just move along */
        goto ERROR_OUT;
    }

    /* do we have any local procs to launch? */
    if (0 == jobdat->num_local_procs) {
        /* indicate that we are done trying to launch them */
        prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                            "%s local:launch no local procs",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        goto GETOUT;
    }

    /* track if we are indexing argvs so we don't check every time */
    index_argv = prrte_get_attribute(&jobdat->attributes, PRRTE_JOB_INDEX_ARGV, NULL, PRRTE_BOOL);

    /* compute the total number of local procs currently alive and about to be launched */
    total_num_local_procs = compute_num_procs_alive(job) + jobdat->num_local_procs;

    /* check the system limits - if we are at our max allowed children, then
     * we won't be allowed to do this anyway, so we may as well abort now.
     * According to the documentation, num_procs = 0 is equivalent to
     * no limit, so treat it as unlimited here.
     */
    if (0 < prrte_sys_limits.num_procs) {
        PRRTE_OUTPUT_VERBOSE((10,  prrte_odls_base_framework.framework_output,
                             "%s checking limit on num procs %d #children needed %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             prrte_sys_limits.num_procs, total_num_local_procs));
        if (prrte_sys_limits.num_procs < total_num_local_procs) {
            if (2 < caddy->retries) {
                /* if we have already tried too many times, then just give up */
                PRRTE_ACTIVATE_JOB_STATE(jobdat, PRRTE_JOB_STATE_FAILED_TO_LAUNCH);
                goto ERROR_OUT;
            }
            /* set a timer event so we can retry later - this
             * gives the system a chance to let other procs
             * terminate, thus creating room for new ones
             */
            PRRTE_DETECT_TIMEOUT(1000, 1000, -1, timer_cb, caddy);
            return;
        }
    }

    /* check to see if we have enough available file descriptors
     * to launch these children - if not, then let's wait a little
     * while to see if some come free. This can happen if we are
     * in a tight loop over comm_spawn
     */
    if (0 < prrte_sys_limits.num_files) {
        int limit;
        limit = 4*total_num_local_procs + 6*jobdat->num_local_procs;
        PRRTE_OUTPUT_VERBOSE((10,  prrte_odls_base_framework.framework_output,
                             "%s checking limit on file descriptors %d need %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             prrte_sys_limits.num_files, limit));
        if (prrte_sys_limits.num_files < limit) {
            if (2 < caddy->retries) {
                /* tried enough - give up */
                for (idx=0; idx < prrte_local_children->size; idx++) {
                    if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                        continue;
                    }
                    if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID)) {
                        child->exit_code = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
                        PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                    }
                }
                goto ERROR_OUT;
            }
            /* don't have enough - wait a little time */
            PRRTE_DETECT_TIMEOUT(1000, 1000, -1, timer_cb, caddy);
            return;
        }
    }

    for (j=0; j < jobdat->apps->size; j++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jobdat->apps, j))) {
            continue;
        }

        /* if this app isn't being used on our node, skip it */
        if (!PRRTE_FLAG_TEST(app, PRRTE_APP_FLAG_USED_ON_NODE)) {
            prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                                "%s app %d not used on node",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
            continue;
        }

        /* setup the environment for this app */
        if (PRRTE_SUCCESS != (rc = prrte_schizo.setup_fork(jobdat, app))) {

            PRRTE_OUTPUT_VERBOSE((10, prrte_odls_base_framework.framework_output,
                                 "%s odls:launch:setup_fork failed with error %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_ERROR_NAME(rc)));

            /* do not ERROR_LOG this failure - it will be reported
             * elsewhere. The launch is going to fail. Since we could have
             * multiple app_contexts, we need to ensure that we flag only
             * the correct one that caused this operation to fail. We then have
             * to flag all the other procs from the app_context as having "not failed"
             * so we can report things out correctly
             */
            /* cycle through children to find those for this jobid */
            for (idx=0; idx < prrte_local_children->size; idx++) {
                if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                    continue;
                }
                if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID) &&
                    j == (int)child->app_idx) {
                    child->exit_code = PRRTE_PROC_STATE_FAILED_TO_LAUNCH;
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                }
            }
            goto GETOUT;
        }

        /* setup the working directory for this app - will jump us
         * to that directory
         */
        if (PRRTE_SUCCESS != (rc = setup_path(app, &effective_dir))) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s odls:launch:setup_path failed with error %s(%d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_ERROR_NAME(rc), rc));
            /* do not ERROR_LOG this failure - it will be reported
             * elsewhere. The launch is going to fail. Since we could have
             * multiple app_contexts, we need to ensure that we flag only
             * the correct one that caused this operation to fail. We then have
             * to flag all the other procs from the app_context as having "not failed"
             * so we can report things out correctly
             */
            /* cycle through children to find those for this jobid */
            for (idx=0; idx < prrte_local_children->size; idx++) {
                if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                    continue;
                }
                if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID) &&
                    j == (int)child->app_idx) {
                    child->exit_code = rc;
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                }
            }
            goto GETOUT;
        }

        /* setup any local files that were prepositioned for us */
        if (PRRTE_SUCCESS != (rc = prrte_filem.link_local_files(jobdat, app))) {
            /* cycle through children to find those for this jobid */
            for (idx=0; idx < prrte_local_children->size; idx++) {
                if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                    continue;
                }
                if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID) &&
                    j == (int)child->app_idx) {
                    child->exit_code = rc;
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                }
            }
            goto GETOUT;
        }

        /* Search for the OMPI_exec_path and PATH settings in the environment. */
        for (argvptr = app->env; *argvptr != NULL; argvptr++) {
            if (0 == strncmp("OMPI_exec_path=", *argvptr, 15)) {
                mpiexec_pathenv = *argvptr + 15;
            }
            if (0 == strncmp("PATH=", *argvptr, 5)) {
                pathenv = *argvptr + 5;
            }
        }

        /* If OMPI_exec_path is set (meaning --path was used), then create a
           temporary environment to be used in the search for the executable.
           The PATH setting in this temporary environment is a combination of
           the OMPI_exec_path and PATH values.  If OMPI_exec_path is not set,
           then just use existing environment with PATH in it.  */
        if (NULL != mpiexec_pathenv) {
            argvptr = NULL;
            if (pathenv != NULL) {
                prrte_asprintf(&full_search, "%s:%s", mpiexec_pathenv, pathenv);
            } else {
                prrte_asprintf(&full_search, "%s", mpiexec_pathenv);
            }
            prrte_setenv("PATH", full_search, true, &argvptr);
            free(full_search);
        } else {
            argvptr = app->env;
        }

        rc = prrte_util_check_context_app(app, argvptr);
        /* do not ERROR_LOG - it will be reported elsewhere */
        if (NULL != mpiexec_pathenv) {
            prrte_argv_free(argvptr);
        }
        if (PRRTE_SUCCESS != rc) {
            /* cycle through children to find those for this jobid */
            for (idx=0; idx < prrte_local_children->size; idx++) {
                if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                    continue;
                }
                if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID) &&
                    j == (int)child->app_idx) {
                    child->exit_code = rc;
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                }
            }
            goto GETOUT;
        }


        /* tell all children that they are being launched via PRRTE */
        prrte_setenv("OMPI_MCA_prrte_launch", "1", true, &app->env);

        /* if the user requested it, set the system resource limits */
        if (PRRTE_SUCCESS != (rc = prrte_util_init_sys_limits(&msg))) {
            prrte_show_help("help-prrte-odls-default.txt", "set limit", true,
                           prrte_process_info.nodename, app,
                           __FILE__, __LINE__, msg);
            /* cycle through children to find those for this jobid */
            for (idx=0; idx < prrte_local_children->size; idx++) {
                if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                    continue;
                }
                if (PRRTE_EQUAL == prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID) &&
                    j == (int)child->app_idx) {
                    child->exit_code = rc;
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                }
            }
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
            PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
            goto GETOUT;
        }

        /* okay, now let's launch all the local procs for this app using the provided fork_local fn */
        for (idx=0; idx < prrte_local_children->size; idx++) {
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, idx))) {
                continue;
            }
            /* does this child belong to this app? */
            if (j != (int)child->app_idx) {
                continue;
            }

            /* is this child already alive? This can happen if
             * we are asked to launch additional processes.
             * If it has been launched, then do nothing
             */
            if (PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {

                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:launch child %s has already been launched",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name)));

                continue;
            }
            /* is this child a candidate to start? it may not be alive
             * because it already executed
             */
            if (PRRTE_PROC_STATE_INIT != child->state &&
                PRRTE_PROC_STATE_RESTART != child->state) {
                continue;
            }
            /* do we have a child from the specified job. Because the
             * job could be given as a WILDCARD value, we must use
             * the dss.compare function to check for equality.
             */
            if (PRRTE_EQUAL != prrte_dss.compare(&job, &(child->name.jobid), PRRTE_JOBID)) {

                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:launch child %s is not in job %s being launched",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name),
                                     PRRTE_JOBID_PRINT(job)));

                continue;
            }

            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s odls:launch working child %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name)));

            /* determine the thread that will handle this child */
            ++prrte_odls_globals.next_base;
            if (prrte_odls_globals.num_threads <= prrte_odls_globals.next_base) {
                prrte_odls_globals.next_base = 0;
            }
            evb = prrte_odls_globals.ev_bases[prrte_odls_globals.next_base];

            /* set the waitpid callback here for thread protection and
             * to ensure we can capture the callback on shortlived apps */
            PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_ALIVE);
            prrte_wait_cb(child, prrte_odls_base_default_wait_local_proc, evb, NULL);

            /* dispatch this child to the next available launch thread */
            cd = PRRTE_NEW(prrte_odls_spawn_caddy_t);
            if (NULL != effective_dir) {
                cd->wdir = strdup(effective_dir);
            }
            cd->jdata = jobdat;
            cd->app = app;
            cd->child = child;
            cd->fork_local = fork_local;
            cd->index_argv = index_argv;
            /* setup any IOF */
            cd->opts.usepty = PRRTE_ENABLE_PTY_SUPPORT;

            /* do we want to setup stdin? */
            if (jobdat->stdin_target == PRRTE_VPID_WILDCARD ||
                 child->name.vpid == jobdat->stdin_target) {
                cd->opts.connect_stdin = true;
            } else {
                cd->opts.connect_stdin = false;
            }
            if (PRRTE_SUCCESS != (rc = prrte_iof_base_setup_prefork(&cd->opts))) {
                PRRTE_ERROR_LOG(rc);
                child->exit_code = rc;
                PRRTE_RELEASE(cd);
                PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                goto GETOUT;
            }
            if (PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_FORWARD_OUTPUT)) {
                /* connect endpoints IOF */
                rc = prrte_iof_base_setup_parent(&child->name, &cd->opts);
                if (PRRTE_SUCCESS != rc) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(cd);
                    PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
                    goto GETOUT;
                }
            }
            prrte_output_verbose(1, prrte_odls_base_framework.framework_output,
                                "%s odls:dispatch %s to thread %d",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&child->name),
                                prrte_odls_globals.next_base);
            prrte_event_set(evb, &cd->ev, -1,
                           PRRTE_EV_WRITE, prrte_odls_base_spawn_proc, cd);
            prrte_event_set_priority(&cd->ev, PRRTE_MSG_PRI);
            prrte_event_active(&cd->ev, PRRTE_EV_WRITE, 1);

        }
        if (NULL != effective_dir) {
            free(effective_dir);
            effective_dir = NULL;
        }
    }

  GETOUT:
    if (NULL != effective_dir) {
        free(effective_dir);
        effective_dir = NULL;
    }

  ERROR_OUT:
    /* ensure we reset our working directory back to our default location  */
    if (0 != chdir(basedir)) {
        PRRTE_ERROR_LOG(PRRTE_ERROR);
    }
    /* release the event */
    PRRTE_RELEASE(caddy);
}

/**
*  Pass a signal to my local procs
 */

int prrte_odls_base_default_signal_local_procs(const prrte_process_name_t *proc, int32_t signal,
                                              prrte_odls_base_signal_local_fn_t signal_local)
{
    int rc, i;
    prrte_proc_t *child;

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls: signaling proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (NULL == proc) ? "NULL" : PRRTE_NAME_PRINT(proc)));

    /* if procs is NULL, then we want to signal all
     * of the local procs, so just do that case
     */
    if (NULL == proc) {
        rc = PRRTE_SUCCESS;  /* pre-set this as an empty list causes us to drop to bottom */
        for (i=0; i < prrte_local_children->size; i++) {
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                continue;
            }
            if (0 == child->pid || !PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE)) {
                /* skip this one as the child isn't alive */
                continue;
            }
            if (PRRTE_SUCCESS != (rc = signal_local(child->pid, (int)signal))) {
                PRRTE_ERROR_LOG(rc);
            }
        }
        return rc;
    }

    /* we want it sent to some specified process, so find it */
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        if (PRRTE_EQUAL == prrte_dss.compare(&(child->name), (prrte_process_name_t*)proc, PRRTE_NAME)) {
            if (PRRTE_SUCCESS != (rc = signal_local(child->pid, (int)signal))) {
                PRRTE_ERROR_LOG(rc);
            }
            return rc;
        }
    }

    /* only way to get here is if we couldn't find the specified proc.
     * report that as an error and return it
     */
    PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
    return PRRTE_ERR_NOT_FOUND;
}

/*
 *  Wait for a callback indicating the child has completed.
 */

void prrte_odls_base_default_wait_local_proc(int fd, short sd, void *cbdata)
{
    prrte_wait_tracker_t *t2 = (prrte_wait_tracker_t*)cbdata;
    prrte_proc_t *proc = t2->child;
    int i;
    prrte_job_t *jobdat;
    prrte_proc_state_t state=PRRTE_PROC_STATE_WAITPID_FIRED;
    prrte_proc_t *cptr;

    prrte_output_verbose(5, prrte_odls_base_framework.framework_output,
                        "%s odls:wait_local_proc child process %s pid %ld terminated",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&proc->name), (long)proc->pid);

    /* if the child was previously flagged as dead, then just
     * update its exit status and
     * ensure that its exit state gets reported to avoid hanging
     * don't forget to check if the process was signaled.
     */
    if (!PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_ALIVE)) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s was already dead exit code %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name),proc->exit_code));
        if (WIFEXITED(proc->exit_code)) {
            proc->exit_code = WEXITSTATUS(proc->exit_code);
            if (0 != proc->exit_code) {
                state = PRRTE_PROC_STATE_TERM_NON_ZERO;
            }
        } else {
            if (WIFSIGNALED(proc->exit_code)) {
                state = PRRTE_PROC_STATE_ABORTED_BY_SIG;
                proc->exit_code = WTERMSIG(proc->exit_code) + 128;
            }
        }
        goto MOVEON;
    }

    /* if the proc called "abort", then we just need to flag that it
     * came thru here */
    if (PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_ABORT)) {
        /* even though the process exited "normally", it happened
         * via an prrte_abort call
         */
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s died by call to abort",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name)));
        state = PRRTE_PROC_STATE_CALLED_ABORT;
        /* regardless of our eventual code path, we need to
         * flag that this proc has had its waitpid fired */
        PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_WAITPID);
        goto MOVEON;
    }

    /* get the jobdat for this child */
    if (NULL == (jobdat = prrte_get_job_data_object(proc->name.jobid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        goto MOVEON;
    }

    /* if this is a debugger daemon, then just report the state
     * and return as we aren't monitoring it
     */
    if (PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_DEBUGGER_DAEMON))  {
        goto MOVEON;
    }

    /* if this child was ordered to die, then just pass that along
     * so we don't hang
     */
    if (PRRTE_PROC_STATE_KILLED_BY_CMD == proc->state) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s was ordered to die",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name)));
        /* regardless of our eventual code path, we need to
         * flag that this proc has had its waitpid fired */
        PRRTE_FLAG_SET(proc, PRRTE_PROC_FLAG_WAITPID);
        goto MOVEON;
    }

    /* determine the state of this process */
    if (WIFEXITED(proc->exit_code)) {

        /* set the exit status appropriately */
        proc->exit_code = WEXITSTATUS(proc->exit_code);

        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child %s exit code %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name), proc->exit_code));

        /* provide a default state */
        state = PRRTE_PROC_STATE_WAITPID_FIRED;

        /* check to see if a sync was required and if it was received */
        if (PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_REG)) {
            if (PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_HAS_DEREG) ||
                prrte_allowed_exit_without_sync || 0 != proc->exit_code) {
                /* if we did recv a finalize sync, or one is not required,
                 * then declare it normally terminated
                 * unless it returned with a non-zero status indicating the code
                 * felt it was non-normal - in this latter case, we do not
                 * require that the proc deregister before terminating
                 */
                if (0 != proc->exit_code && prrte_abort_non_zero_exit) {
                    state = PRRTE_PROC_STATE_TERM_NON_ZERO;
                    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                         "%s odls:waitpid_fired child process %s terminated normally "
                                         "but with a non-zero exit status - it "
                                         "will be treated as an abnormal termination",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         PRRTE_NAME_PRINT(&proc->name)));
                } else {
                    /* indicate the waitpid fired */
                    state = PRRTE_PROC_STATE_WAITPID_FIRED;
                }
            } else {
                /* we required a finalizing sync and didn't get it, so this
                 * is considered an abnormal termination and treated accordingly
                 */
                state = PRRTE_PROC_STATE_TERM_WO_SYNC;
                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:waitpid_fired child process %s terminated normally "
                                     "but did not provide a required finalize sync - it "
                                     "will be treated as an abnormal termination",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&proc->name)));
            }
        } else {
            /* has any child in this job already registered? */
            for (i=0; i < prrte_local_children->size; i++) {
                if (NULL == (cptr = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
                    continue;
                }
                if (cptr->name.jobid != proc->name.jobid) {
                    continue;
                }
                if (PRRTE_FLAG_TEST(cptr, PRRTE_PROC_FLAG_REG) && !prrte_allowed_exit_without_sync) {
                    /* someone has registered, and we didn't before
                     * terminating - this is an abnormal termination unless
                     * the allowed_exit_without_sync flag is set
                     */
                    if (0 != proc->exit_code) {
                        state = PRRTE_PROC_STATE_TERM_NON_ZERO;
                        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                             "%s odls:waitpid_fired child process %s terminated normally "
                                             "but with a non-zero exit status - it "
                                             "will be treated as an abnormal termination",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             PRRTE_NAME_PRINT(&proc->name)));
                    } else {
                        state = PRRTE_PROC_STATE_TERM_WO_SYNC;
                        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                             "%s odls:waitpid_fired child process %s terminated normally "
                                             "but did not provide a required init sync - it "
                                             "will be treated as an abnormal termination",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             PRRTE_NAME_PRINT(&proc->name)));
                    }
                    goto MOVEON;
                }
            }
            /* if no child has registered, then it is possible that
             * none of them will. This is considered acceptable. Still
             * flag it as abnormal if the exit code was non-zero
             */
            if (0 != proc->exit_code && prrte_abort_non_zero_exit) {
                state = PRRTE_PROC_STATE_TERM_NON_ZERO;
            } else {
                state = PRRTE_PROC_STATE_WAITPID_FIRED;
            }
        }

        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name),
                             (0 == proc->exit_code) ? "normally" : "with non-zero status"));
    } else {
        /* the process was terminated with a signal! That's definitely
         * abnormal, so indicate that condition
         */
        state = PRRTE_PROC_STATE_ABORTED_BY_SIG;
        /* If a process was killed by a signal, then make the
         * exit code of prun be "signo + 128" so that "prog"
         * and "prun prog" will both yield the same exit code.
         *
         * This is actually what the shell does for you when
         * a process dies by signal, so this makes prun treat
         * the termination code to exit status translation the
         * same way
         */
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:waitpid_fired child process %s terminated with signal %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name), strsignal(WTERMSIG(proc->exit_code))));
        proc->exit_code = WTERMSIG(proc->exit_code) + 128;

        /* Do not decrement the number of local procs here. That is handled in the errmgr */
    }

 MOVEON:
    /* cancel the wait as this proc has already terminated */
    prrte_wait_cb_cancel(proc);
    PRRTE_ACTIVATE_PROC_STATE(&proc->name, state);
    /* cleanup the tracker */
    PRRTE_RELEASE(t2);
}

typedef struct {
    prrte_list_item_t super;
    prrte_proc_t *child;
} prrte_odls_quick_caddy_t;
static void qcdcon(prrte_odls_quick_caddy_t *p)
{
    p->child = NULL;
}
static void qcddes(prrte_odls_quick_caddy_t *p)
{
    if (NULL != p->child) {
        PRRTE_RELEASE(p->child);
    }
}
PRRTE_CLASS_INSTANCE(prrte_odls_quick_caddy_t,
                   prrte_list_item_t,
                   qcdcon, qcddes);

int prrte_odls_base_default_kill_local_procs(prrte_pointer_array_t *procs,
                                            prrte_odls_base_kill_local_fn_t kill_local)
{
    prrte_proc_t *child;
    prrte_list_t procs_killed;
    prrte_proc_t *proc, proctmp;
    int i, j, ret;
    prrte_pointer_array_t procarray, *procptr;
    bool do_cleanup;
    prrte_odls_quick_caddy_t *cd;

    PRRTE_CONSTRUCT(&procs_killed, prrte_list_t);

    /* if the pointer array is NULL, then just kill everything */
    if (NULL == procs) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:kill_local_proc working on WILDCARD",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        PRRTE_CONSTRUCT(&procarray, prrte_pointer_array_t);
        prrte_pointer_array_init(&procarray, 1, 1, 1);
        PRRTE_CONSTRUCT(&proctmp, prrte_proc_t);
        proctmp.name.jobid = PRRTE_JOBID_WILDCARD;
        proctmp.name.vpid = PRRTE_VPID_WILDCARD;
        prrte_pointer_array_add(&procarray, &proctmp);
        procptr = &procarray;
        do_cleanup = true;
    } else {
        PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                             "%s odls:kill_local_proc working on provided array",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        procptr = procs;
        do_cleanup = false;
    }

    /* cycle through the provided array of processes to kill */
    for (i=0; i < procptr->size; i++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(procptr, i))) {
            continue;
        }
        for (j=0; j < prrte_local_children->size; j++) {
            if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, j))) {
                continue;
            }

            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s odls:kill_local_proc checking child process %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name)));

            /* do we have a child from the specified job? Because the
             *  job could be given as a WILDCARD value, we must
             *  check for that as well as for equality.
             */
            if (PRRTE_JOBID_WILDCARD != proc->name.jobid &&
                proc->name.jobid != child->name.jobid) {

                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not part of job %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name),
                                     PRRTE_JOBID_PRINT(proc->name.jobid)));
                continue;
            }

            /* see if this is the specified proc - could be a WILDCARD again, so check
             * appropriately
             */
            if (PRRTE_VPID_WILDCARD != proc->name.vpid &&
                proc->name.vpid != child->name.vpid) {

                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not covered by rank %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name),
                                     PRRTE_VPID_PRINT(proc->name.vpid)));
                continue;
            }

            /* is this process alive? if not, then nothing for us
             * to do to it
             */
            if (!PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_ALIVE) || 0 == child->pid) {

                PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                     "%s odls:kill_local_proc child %s is not alive",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     PRRTE_NAME_PRINT(&child->name)));

                /* ensure, though, that the state is terminated so we don't lockup if
                 * the proc never started
                 */
                if (PRRTE_PROC_STATE_UNDEF == child->state ||
                    PRRTE_PROC_STATE_INIT == child->state ||
                    PRRTE_PROC_STATE_RUNNING == child->state) {
                    /* we can't be sure what happened, but make sure we
                     * at least have a value that will let us eventually wakeup
                     */
                    child->state = PRRTE_PROC_STATE_TERMINATED;
                    /* ensure we realize that the waitpid will never come, if
                     * it already hasn't
                     */
                    PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_WAITPID);
                    child->pid = 0;
                    goto CLEANUP;
                } else {
                    continue;
                }
            }

            /* ensure the stdin IOF channel for this child is closed. The other
             * channels will automatically close when the proc is killed
             */
            if (NULL != prrte_iof.close) {
                prrte_iof.close(&child->name, PRRTE_IOF_STDIN);
            }

            /* cancel the waitpid callback as this induces unmanageable race
             * conditions when we are deliberately killing the process
             */
            prrte_wait_cb_cancel(child);

            /* First send a SIGCONT in case the process is in stopped state.
               If it is in a stopped state and we do not first change it to
               running, then SIGTERM will not get delivered.  Ignore return
               value. */
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s SENDING SIGCONT TO %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&child->name)));
            cd = PRRTE_NEW(prrte_odls_quick_caddy_t);
            PRRTE_RETAIN(child);
            cd->child = child;
            prrte_list_append(&procs_killed, &cd->super);
            kill_local(child->pid, SIGCONT);
            continue;

        CLEANUP:
            /* ensure the child's session directory is cleaned up */
            prrte_session_dir_finalize(&child->name);
            /* check for everything complete - this will remove
             * the child object from our local list
             */
            if (!prrte_finalizing &&
                PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
                PRRTE_FLAG_TEST(child, PRRTE_PROC_FLAG_WAITPID)) {
                PRRTE_ACTIVATE_PROC_STATE(&child->name, child->state);
            }
        }
    }

    /* if we are issuing signals, then we need to wait a little
     * and send the next in sequence */
    if (0 < prrte_list_get_size(&procs_killed)) {
        /* Wait a little. Do so in a loop since sleep() can be interrupted by a
         * signal. Most likely SIGCHLD in this case */
        ret = prrte_odls_globals.timeout_before_sigkill;
        while (ret > 0) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s Sleep %d sec (total = %d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 ret, prrte_odls_globals.timeout_before_sigkill));
            ret = sleep(ret);
        }
        /* issue a SIGTERM to all */
        PRRTE_LIST_FOREACH(cd, &procs_killed, prrte_odls_quick_caddy_t) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s SENDING SIGTERM TO %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&cd->child->name)));
            kill_local(cd->child->pid, SIGTERM);
        }
        /* Wait a little. Do so in a loop since sleep() can be interrupted by a
         * signal. Most likely SIGCHLD in this case */
        ret = prrte_odls_globals.timeout_before_sigkill;
        while (ret > 0) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s Sleep %d sec (total = %d)",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 ret, prrte_odls_globals.timeout_before_sigkill));
            ret = sleep(ret);
        }

        /* issue a SIGKILL to all */
        PRRTE_LIST_FOREACH(cd, &procs_killed, prrte_odls_quick_caddy_t) {
            PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                                 "%s SENDING SIGKILL TO %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&cd->child->name)));
            kill_local(cd->child->pid, SIGKILL);
            /* indicate the waitpid fired as this is effectively what
             * has happened
             */
            PRRTE_FLAG_SET(cd->child, PRRTE_PROC_FLAG_WAITPID);

            /* Since we are not going to wait for this process, make sure
             * we mark it as not-alive so that we don't wait for it
             * in orted_cmd
             */
            PRRTE_FLAG_UNSET(cd->child, PRRTE_PROC_FLAG_ALIVE);
            cd->child->pid = 0;

            /* mark the child as "killed" */
            cd->child->state = PRRTE_PROC_STATE_KILLED_BY_CMD;  /* we ordered it to die */

            /* ensure the child's session directory is cleaned up */
            prrte_session_dir_finalize(&cd->child->name);
            /* check for everything complete - this will remove
             * the child object from our local list
             */
            if (!prrte_finalizing &&
                PRRTE_FLAG_TEST(cd->child, PRRTE_PROC_FLAG_IOF_COMPLETE) &&
                PRRTE_FLAG_TEST(cd->child, PRRTE_PROC_FLAG_WAITPID)) {
                PRRTE_ACTIVATE_PROC_STATE(&cd->child->name, cd->child->state);
            }
        }
    }
    PRRTE_LIST_DESTRUCT(&procs_killed);

    /* cleanup arrays, if required */
    if (do_cleanup) {
        PRRTE_DESTRUCT(&procarray);
        PRRTE_DESTRUCT(&proctmp);
    }

    return PRRTE_SUCCESS;
}

int prrte_odls_base_get_proc_stats(prrte_buffer_t *answer,
                                  prrte_process_name_t *proc)
{
    int rc;
    prrte_proc_t *child;
    prrte_pstats_t stats, *statsptr;
    int i, j;

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls:get_proc_stats for proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(proc)));

    /* find this child */
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (child = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }

        if (proc->jobid == child->name.jobid &&
            (proc->vpid == child->name.vpid ||
             PRRTE_VPID_WILDCARD == proc->vpid)) { /* found it */

            PRRTE_CONSTRUCT(&stats, prrte_pstats_t);
            /* record node up to first '.' */
            for (j=0; j < (int)strlen(prrte_process_info.nodename) &&
                 j < PRRTE_PSTAT_MAX_STRING_LEN-1 &&
                 prrte_process_info.nodename[j] != '.'; j++) {
                stats.node[j] = prrte_process_info.nodename[j];
            }
            /* record rank */
            stats.rank = child->name.vpid;
            /* get stats */
            rc = prrte_pstat.query(child->pid, &stats, NULL);
            if (PRRTE_SUCCESS != rc) {
                PRRTE_DESTRUCT(&stats);
                return rc;
            }
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, proc, 1, PRRTE_NAME))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&stats);
                return rc;
            }
            statsptr = &stats;
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &statsptr, 1, PRRTE_PSTAT))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_DESTRUCT(&stats);
                return rc;
            }
            PRRTE_DESTRUCT(&stats);
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_odls_base_default_restart_proc(prrte_proc_t *child,
                                        prrte_odls_base_fork_local_proc_fn_t fork_local)
{
    int rc;
    prrte_app_context_t *app;
    prrte_job_t *jobdat;
    char basedir[MAXPATHLEN];
    char *wdir = NULL;
    prrte_odls_spawn_caddy_t *cd;
    prrte_event_base_t *evb;

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls:restart_proc for proc %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(&child->name)));

    /* establish our baseline working directory - we will be potentially
     * bouncing around as we execute this app, but we will always return
     * to this place as our default directory
     */
    if (NULL == getcwd(basedir, sizeof(basedir))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* find this child's jobdat */
    if (NULL == (jobdat = prrte_get_job_data_object(child->name.jobid))) {
        /* not found */
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }

    /* CHECK THE NUMBER OF TIMES THIS CHILD HAS BEEN RESTARTED
     * AGAINST MAX_RESTARTS */

    child->state = PRRTE_PROC_STATE_FAILED_TO_START;
    child->exit_code = 0;
    PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_WAITPID);
    PRRTE_FLAG_UNSET(child, PRRTE_PROC_FLAG_IOF_COMPLETE);
    child->pid = 0;
    if (NULL != child->rml_uri) {
        free(child->rml_uri);
        child->rml_uri = NULL;
    }
    app = (prrte_app_context_t*)prrte_pointer_array_get_item(jobdat->apps, child->app_idx);

    /* reset envars to match this child */
    if (PRRTE_SUCCESS != (rc = prrte_schizo.setup_child(jobdat, child, app, &app->env))) {
        PRRTE_ERROR_LOG(rc);
        goto CLEANUP;
    }

    /* setup the path */
    if (PRRTE_SUCCESS != (rc = setup_path(app, &wdir))) {
        PRRTE_ERROR_LOG(rc);
        if (NULL != wdir) {
            free(wdir);
        }
        goto CLEANUP;
    }

    /* NEED TO UPDATE THE REINCARNATION NUMBER IN PMIX */

    /* dispatch this child to the next available launch thread */
    cd = PRRTE_NEW(prrte_odls_spawn_caddy_t);
    if (NULL != wdir) {
        cd->wdir = strdup(wdir);
        free(wdir);
    }
    cd->jdata = jobdat;
    cd->app = app;
    cd->child = child;
    cd->fork_local = fork_local;
    /* setup any IOF */
    cd->opts.usepty = PRRTE_ENABLE_PTY_SUPPORT;

    /* do we want to setup stdin? */
    if (jobdat->stdin_target == PRRTE_VPID_WILDCARD ||
         child->name.vpid == jobdat->stdin_target) {
        cd->opts.connect_stdin = true;
    } else {
        cd->opts.connect_stdin = false;
    }
    if (PRRTE_SUCCESS != (rc = prrte_iof_base_setup_prefork(&cd->opts))) {
        PRRTE_ERROR_LOG(rc);
        child->exit_code = rc;
        PRRTE_RELEASE(cd);
        PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
        goto CLEANUP;
    }
    if (PRRTE_FLAG_TEST(jobdat, PRRTE_JOB_FLAG_FORWARD_OUTPUT)) {
        /* connect endpoints IOF */
        rc = prrte_iof_base_setup_parent(&child->name, &cd->opts);
        if (PRRTE_SUCCESS != rc) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(cd);
            PRRTE_ACTIVATE_PROC_STATE(&child->name, PRRTE_PROC_STATE_FAILED_TO_LAUNCH);
            goto CLEANUP;
        }
    }
    ++prrte_odls_globals.next_base;
    if (prrte_odls_globals.num_threads <= prrte_odls_globals.next_base) {
        prrte_odls_globals.next_base = 0;
    }
    evb = prrte_odls_globals.ev_bases[prrte_odls_globals.next_base];
    prrte_wait_cb(child, prrte_odls_base_default_wait_local_proc, evb, NULL);

    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s restarting app %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), app->app));

    prrte_event_set(evb, &cd->ev, -1,
                   PRRTE_EV_WRITE, prrte_odls_base_spawn_proc, cd);
    prrte_event_set_priority(&cd->ev, PRRTE_MSG_PRI);
    prrte_event_active(&cd->ev, PRRTE_EV_WRITE, 1);

  CLEANUP:
    PRRTE_OUTPUT_VERBOSE((5, prrte_odls_base_framework.framework_output,
                         "%s odls:restart of proc %s %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(&child->name),
                         (PRRTE_SUCCESS == rc) ? "succeeded" : "failed"));

    /* reset our working directory back to our default location - if we
     * don't do this, then we will be looking for relative paths starting
     * from the last wdir option specified by the user. Thus, we would
     * be requiring that the user keep track on the cmd line of where
     * each app was located relative to the prior app, instead of relative
     * to their current location
     */
    if (0 != chdir(basedir)) {
        PRRTE_ERROR_LOG(PRRTE_ERROR);
    }

    return rc;
}
