/*
 * Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/ras/base/ras_private.h"

static void localrelease(void *cbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    if(0 < req->ninfo && NULL != req->info){
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* Callback to process the allocation request answer from the scheduler
 * and pass on any results to the local client
 */
static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t rel, void *relcbdata)
{
    pmix_server_req_t *req = (pmix_server_req_t*)cbdata;
    pmix_alloc_directive_t directive;
    uint32_t session_id = UINT32_MAX;
    char *endptr, *hosts, *tmp;
    char **dir, **host_argv = NULL, **alloc_nlist = NULL, **alloc_clist = NULL;
    pmix_list_t nodes;
    prte_node_t *nptr;
    prte_session_t *session;
    prte_job_t *jdata;
    size_t len, n, i;
    pmix_status_t rc;

    /* track this allocation so it can be referenced in e.g. a call to PMIx_Spawn */
    if(PRTE_PROC_IS_MASTER && status == PMIX_SUCCESS){
        for(int n = 0; n < ninfo; n++){
            if(PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_ID)){
                session_id = strtoul(info[n].value.data.string, &endptr, 10); 
            }else if(PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NODE_LIST)){
                alloc_nlist = PMIX_ARGV_SPLIT_COMPAT(info[n].value.data.string, ',');
            }else if(PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NUM_CPU_LIST)){
                alloc_clist = PMIX_ARGV_SPLIT_COMPAT(info[n].value.data.string, ',');
            }
        }
        /* They did not provide an alloc id and nodelist. This is an error! */
        if(session_id == UINT32_MAX || alloc_nlist == NULL){
            status = PMIX_ERR_BAD_PARAM;
            goto ANSWER;
        }

        dir = PMIX_ARGV_SPLIT_COMPAT(req->operation, ' ');
        directive = strtoul(dir[1], &endptr, 10);
        switch(directive){
            case PMIX_ALLOC_RELEASE:
                if(NULL == (session = prte_get_session_object(session_id))){
                    status = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(status);
                    goto ANSWER;
                }
                /* Remove specified nodes from session object */
                len = PMIX_ARGV_COUNT_COMPAT(alloc_nlist);
                for(n = 0; n < session->nodes->size; n++){
                    if(NULL == (nptr = (prte_node_t *) pmix_pointer_array_get_item(session->nodes, n))){
                        continue;
                    }
                    for(i = 0; i < len; i++){
                        if(0 == strcmp(nptr->name, alloc_nlist[i])){
                            pmix_pointer_array_set_item(session->nodes, n, NULL);
                        }
                    }
                }
                PMIX_ARGV_FREE_COMPAT(alloc_nlist);
                /* all done */
                goto ANSWER;
            case PMIX_ALLOC_NEW:
                /* if we already have a session with this id that's an error */
                if(NULL != (session = prte_get_session_object(session_id))){
                    status = PRTE_ERR_BAD_PARAM;
                    PRTE_ERROR_LOG(status);
                    goto ANSWER;
                }
                /* create a new session object */
                session = PMIX_NEW(prte_session_t);
                if(NULL == session){
                    status = PRTE_ERR_OUT_OF_RESOURCE;
                    PRTE_ERROR_LOG(status);
                    goto ANSWER;
                }
                session->session_id = session_id;
                prte_set_session_object(session);

                /* Track the session as a child session of session the requestor is running in */
                jdata = prte_get_job_data_object(req->tproc.nspace);
                if(NULL != jdata){
                    pmix_pointer_array_add(jdata->session->children, session);
                }else{
                    /* default to our default session */
                    pmix_pointer_array_add(prte_default_session->children, session);
                }
                break;
            case PMIX_ALLOC_EXTEND:
                /* Get the session object to extend */
                if(NULL == (session = prte_get_session_object(session_id))){
                    status = PRTE_ERR_BAD_PARAM;;
                    PRTE_ERROR_LOG(status);
                    goto ANSWER;
                }
                break;
            case PMIX_ALLOC_REAQUIRE:
                /* TODO: Not sure how to handle this */
                goto ANSWER;
        }

        /* If we get here, we need to add specified nodes to the nodepool */
        if(NULL != alloc_nlist && NULL == alloc_clist){
            hosts = PMIX_ARGV_JOIN_COMPAT(alloc_nlist, ',');
            PMIX_ARGV_FREE_COMPAT(alloc_nlist);
        }else if (NULL != alloc_nlist && NULL != alloc_clist){
            len = PMIX_ARGV_COUNT_COMPAT(alloc_nlist);
            for(n = 0; n < len; n++){
                tmp = malloc(strlen(alloc_nlist[n]) + strlen(alloc_clist[n]) + 2);
                sprintf(tmp, "%s:%s", alloc_nlist[n], alloc_clist[n]);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&host_argv, tmp);
                free(tmp);
            }
            hosts = PMIX_ARGV_JOIN_COMPAT(host_argv, ',');
            PMIX_ARGV_FREE_COMPAT(host_argv);
            PMIX_ARGV_FREE_COMPAT(alloc_nlist);
            PMIX_ARGV_FREE_COMPAT(alloc_clist);
        }

        /* Create a list of nodes from the hostlist */
        PMIX_CONSTRUCT(&nodes, pmix_list_t);
        if (PRTE_SUCCESS != (rc = prte_util_add_dash_host_nodes(&nodes, hosts, true))) {
            PRTE_ERROR_LOG(rc);
            status = rc;
            goto ANSWER;
        }
        free(hosts);

        /* If we got something back add it to */
        if (!pmix_list_is_empty(&nodes)) {
            /* Mark that this node was added */
            PMIX_LIST_FOREACH(nptr, &nodes, prte_node_t){
                nptr->state = PRTE_NODE_STATE_ADDED;
                if(session != prte_default_session){
                    pmix_pointer_array_add(session->nodes, nptr);
                }
            }
            /* store the results in the global resource pool - this removes the
             * list items
             */
            if (PRTE_SUCCESS != (rc = prte_ras_base_node_insert(&nodes, NULL))) {
                PRTE_ERROR_LOG(rc);
                status = rc;
                goto ANSWER;
            }

            /* mark that an updated nidmap must be communicated to existing daemons */
            prte_nidmap_communicated = false;
        }
        for(n = 0; n < prte_node_pool->size; n++){
            if(NULL == (nptr = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n))){
                continue;
            }
        }
    }
ANSWER:
    if (NULL != req->infocbfunc) {
        req->infocbfunc(status, info, ninfo, req->cbdata, rel, relcbdata);
    }else{
        if (NULL != rel) {
            rel(relcbdata);
        }
    }
    /* Release our local request */
    localrelease(req);
    return;
}

void pmix_server_alloc_request_resp(int status, pmix_proc_t *sender,
                                                pmix_data_buffer_t *buffer, prte_rml_tag_t tg,
                                                void *cbdata)
{

    int req_index, cnt;
    pmix_status_t ret, rc;
    pmix_server_req_t *req;

    PRTE_HIDE_UNUSED_PARAMS(status, sender, tg, cbdata);

    /* unpack the status - this is already a PMIx value */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
    }

    /* we let the above errors fall thru in the vain hope that the req number can
     * be successfully unpacked, thus allowing us to respond to the requestor */

    /* unpack our tracking room number */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &req_index, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we are hosed */
        return;
    }

    req = pmix_pointer_array_get_item(&prte_pmix_server_globals.local_reqs, req_index);

    /* Report the error */
    if(ret != PMIX_SUCCESS){
        goto ANSWER;
    }
    
    rc = PMIx_Data_unpack(NULL, buffer, &req->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = prte_pmix_convert_rc(rc);
        goto ANSWER;
    }

    if(0 < req->ninfo){
        PMIX_INFO_CREATE(req->info, req->ninfo);
        
        cnt = req->ninfo;  
        rc = PMIx_Data_unpack(NULL, buffer, req->info, &cnt, PMIX_INFO);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            ret = prte_pmix_convert_rc(rc);
            req->ninfo = 0;
            goto ANSWER;
        }
    }

ANSWER:
    infocbfunc(ret, req->info, req->ninfo, req, NULL, NULL);
}

static void pass_request(int sd, short args, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd = (prte_pmix_server_op_caddy_t*)cbdata;
    prte_job_t *client_job;
    pmix_server_req_t *req;
    pmix_data_buffer_t *buf;
    size_t n, ninfo, need_id;
    uint8_t command;
    pmix_status_t rc;
    pmix_info_t info[2];
    char alloc_id[64];

    /* create a request tracker for this operation */
    req = PMIX_NEW(pmix_server_req_t);
    if (0 < cd->allocdir) {
        pmix_asprintf(&req->operation, "ALLOCATE: %u", cd->allocdir);
        command = 0;
    } else {
        pmix_asprintf(&req->operation, "SESSIONCTRL: %u", cd->sessionID);
        command = 1;
    }
    req->infocbfunc = cd->infocbfunc;
    req->cbdata = cd->cbdata;
    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    /* if we are the DVM master, then handle this ourselves */
    if (PRTE_PROC_IS_MASTER) {
        if (!prte_pmix_server_globals.scheduler_connected) {
            /* the scheduler has not attached to us - see if we
             * can attach to it, make it optional so we don't
             * hang if there is no scheduler available */
            PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SCHEDULER, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&info[1], PMIX_TOOL_CONNECT_OPTIONAL, NULL, PMIX_BOOL);
            rc = PMIx_tool_attach_to_server(NULL, &prte_pmix_server_globals.scheduler,
                                            info, 2);
            PMIX_INFO_DESTRUCT(&info[0]);
            PMIX_INFO_DESTRUCT(&info[1]);
            if (PMIX_SUCCESS != rc) {
                goto callback;
            }
            prte_pmix_server_globals.scheduler_set_as_server = true;
        }

        /* if we have not yet set the scheduler as our server, do so */
        if (!prte_pmix_server_globals.scheduler_set_as_server) {
            rc = PMIx_tool_set_server(&prte_pmix_server_globals.scheduler, NULL, 0);
            if (PMIX_SUCCESS != rc) {
                goto callback;
            }
            prte_pmix_server_globals.scheduler_set_as_server = true;
        }

        if (0 == command) {
            /* check if they specified an alloc id - otherwise default to requestors session */
            need_id = 1;
            for(n = 0; n < cd->ninfo; n++){
                if(PMIX_CHECK_KEY(&cd->info[n], PMIX_ALLOC_ID)){
                    need_id = 0;
                }
            }
            req->ninfo = cd->ninfo + need_id;
            PMIX_INFO_CREATE(req->info, req->ninfo);
            for(n = 0; n < cd->ninfo; n++){
                PMIX_INFO_XFER(&req->info[n], &cd->info[n]);
            }
            if(need_id){
                client_job = prte_get_job_data_object(cd->proc.nspace);
                if(NULL != client_job){
                    sprintf(alloc_id, "%u", client_job->session->session_id);
                }else{
                    /* default to the default session */
                    sprintf(alloc_id, "%u", 0);
                }
                PMIX_INFO_LOAD(&req->info[cd->ninfo], PMIX_ALLOC_ID, alloc_id, PMIX_STRING);
            }
            PMIX_PROC_LOAD(&req->tproc, cd->proc.nspace, cd->proc.rank);

            rc = PMIx_Allocation_request_nb(cd->allocdir, req->info, req->ninfo,
                                            infocbfunc, req);
        } else {
#if PMIX_NUMERIC_VERSION < 0x00050000
            rc = PMIX_ERR_NOT_SUPPORTED;
#else
            rc = PMIx_Session_control(cd->sessionID, cd->info, cd->ninfo,
                                      infocbfunc, req);
#endif
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto callback;
        }
        PMIX_RELEASE(cd);
        return;
    }

    PMIX_DATA_BUFFER_CREATE(buf);

    /* construct a request message for the command */
    rc = PMIx_Data_pack(NULL, buf, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        goto callback;
    }

    /* pack the local requestor ID */
    rc = PMIx_Data_pack(NULL, buf, &req->local_index, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        goto callback;
    }

    /* pack the requestor */
    rc = PMIx_Data_pack(NULL, buf, &cd->proc, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        goto callback;
    }

    if (0 == command) {
        /* pack the allocation directive */
        rc = PMIx_Data_pack(NULL, buf, &cd->allocdir, 1, PMIX_ALLOC_DIRECTIVE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            goto callback;
        }
    } else {
        /* pack the sessionID */
        rc = PMIx_Data_pack(NULL, buf, &cd->sessionID, 1, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            goto callback;
        }
    }

    /* pack the number of info */
    rc = PMIx_Data_pack(NULL, buf, &cd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        goto callback;
    }
    if (0 < cd->ninfo) {
        /* pack the info */
        rc = PMIx_Data_pack(NULL, buf, cd->info, cd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(buf);
            pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
            goto callback;
        }
    }

    /* send this request to the DVM controller */
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_SCHED);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
        PMIX_DATA_BUFFER_RELEASE(buf);
        goto callback;
    }
    PMIX_RELEASE(cd);
    return;

callback:
    PMIX_RELEASE(cd);
    /* this section gets executed solely upon an error */
    if (NULL != req->infocbfunc) {
        req->infocbfunc(rc, req->info, req->ninfo, req->cbdata, localrelease, req);
        return;
    }
    PMIX_RELEASE(req);
}

pmix_status_t pmix_server_alloc_fn(const pmix_proc_t *client,
                                   pmix_alloc_directive_t directive,
                                   const pmix_info_t data[], size_t ndata,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;


    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s allocate upcalled on behalf of proc %s:%u with %" PRIsize_t " infos",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), client->nspace, client->rank, ndata);

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    PMIX_LOAD_PROCID(&cd->proc, client->nspace, client->rank);
    cd->allocdir = directive;
    cd->info = (pmix_info_t *) data;
    cd->ninfo = ndata;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, pass_request, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

#if PMIX_NUMERIC_VERSION >= 0x00050000

pmix_status_t pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                                          uint32_t sessionID,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_op_caddy_t *cd;


    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl upcalled on behalf of proc %s:%u with %" PRIsize_t " directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), requestor->nspace, requestor->rank, ndirs);

    cd = PMIX_NEW(prte_pmix_server_op_caddy_t);
    PMIX_LOAD_PROCID(&cd->proc, requestor->nspace, requestor->rank);
    cd->sessionID = sessionID;
    cd->info = (pmix_info_t *) directives;
    cd->ninfo = ndirs;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, pass_request, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

#endif
