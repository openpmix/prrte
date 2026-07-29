/*
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
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
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/schizo/base/base.h"
#include "src/util/pmix_show_help.h"

/* Process the session control directive from the scheduler.
 *
 * Returns a pmix_status_t - NOT a PRRTE return code.  The caller
 * (pass_request) hands the result straight to req->infocbfunc, which is a
 * PMIx callback, so every exit from here must already be in PMIx's numbering.
 * This function must also never invoke that callback itself: pass_request
 * falls through to its "callback:" tail on return, so answering here too
 * would deliver the response twice and release the request twice. */
static pmix_status_t process_directive(prte_pmix_server_req_t *req)
{
    char *user_refid = NULL, *alloc_refid = NULL;
    pmix_info_t *personality = NULL, *iptr;
    pmix_proc_t *requestor = NULL;
    char *hosts = NULL;
    prte_session_t *session = NULL;
    prte_job_t *jdata = NULL;
    prte_app_context_t *app;
    pmix_app_t *apps;
    size_t napps;
    size_t n, i;
    int j, prc;
    pmix_status_t rc;
    bool terminate = false;
    bool pause = false;
    bool resume = false;
    bool preempt = false;
    bool restore = false;
    bool signal = false;
    bool extend = false;
    int sigvalue = 0, tval = 0;

    /* cycle across the directives to see what we are being asked to do */
    for (n = 0; n < req->ninfo; n++) {

        if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_INSTANTIATE)) {
            // check to see if we already have this session
            session = prte_get_session_object(req->sessionID);
            if (NULL != session) {
                // this is an error - cannot instantiate a preexisting
                // session
                return PMIX_ERR_BAD_PARAM;
            }
            /* create a new session object */
            session = PMIX_NEW(prte_session_t);
            if (NULL == session) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                return PMIX_ERR_NOMEM;
            }
            session->session_id = req->sessionID;
            prc = prte_set_session_object(session);
            if (PRTE_SUCCESS != prc) {
                PMIX_RELEASE(session);
                return prte_pmix_convert_rc(prc);
            }
            // if a job has already been described, add it here
            if (NULL != jdata) {
                pmix_pointer_array_add(session->jobs, jdata);
            }
            if (NULL != alloc_refid) {
                session->alloc_refid = strdup(alloc_refid);
                alloc_refid = NULL;
            }
            if (NULL != user_refid) {
                session->user_refid = strdup(user_refid);
                user_refid = NULL;
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_JOB)) {
            if (NULL == jdata) {
                jdata = PMIX_NEW(prte_job_t);
                jdata->map = PMIX_NEW(prte_job_map_t);
                /* default to the requestor as the originator */
                if (NULL != requestor) {
                    PMIX_LOAD_PROCID(&jdata->originator, requestor->nspace, requestor->rank);
                }
            }
            // transfer the job description across
            iptr = (pmix_info_t*)req->info[n].value.data.darray->array;
            i = req->info[n].value.data.darray->size;
            prc = prte_pmix_xfer_job_info(jdata, iptr, i);
            if (PRTE_SUCCESS != prc) {
                PRTE_ERROR_LOG(prc);
                return prte_pmix_convert_rc(prc);
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_APP)) {
            // the apps that are to be started in the session
            if (NULL == jdata) {
                jdata = PMIX_NEW(prte_job_t);
                jdata->map = PMIX_NEW(prte_job_map_t);
                /* default to the requestor as the originator */
                if (NULL != requestor) {
                    PMIX_LOAD_PROCID(&jdata->originator, requestor->nspace, requestor->rank);
                }
            }
            apps = (pmix_app_t*)req->info[n].value.data.darray->array;
            napps = req->info[n].value.data.darray->size;
            for (i=0; i < napps; i++) {
                prc = prte_pmix_xfer_app(jdata, &apps[i]);
                if (PRTE_SUCCESS != prc) {
                    PRTE_ERROR_LOG(prc);
                    return prte_pmix_convert_rc(prc);
                }
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_PERSONALITY)) {
            personality = &req->info[n];
            if (NULL != jdata && NULL == jdata->personality) {
                jdata->personality = PMIx_Argv_split(personality->value.data.string, ',');
                jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(personality->value.data.string);
                pmix_server_cache_job_info(jdata, personality);
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_REQUESTOR)) {
            requestor = req->info[n].value.data.proc;
            if (NULL != jdata) {
                PMIX_LOAD_PROCID(&jdata->originator, requestor->nspace, requestor->rank);
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_PROVISION_NODES) ||
                   PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_PROVISION_IMAGE)) {
            // we don't support these directives
            return PMIX_ERR_NOT_SUPPORTED;

        } else if(PMIX_CHECK_KEY(&req->info[n], PMIX_ALLOC_ID)) {
            if (NULL != session) {
                session->alloc_refid = strdup(req->info[n].value.data.string);
            } else {
                alloc_refid = req->info[n].value.data.string;
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_ALLOC_REQ_ID)) {
            if (NULL != session) {
                session->user_refid = strdup(req->info[n].value.data.string);
            } else {
                user_refid = req->info[n].value.data.string;
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_ALLOC_NODE_LIST)) {
            hosts = req->info[n].value.data.string;

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_ALLOC_NUM_CPU_LIST)) {
            // we don't support this directive
            return PMIX_ERR_NOT_SUPPORTED;

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_PAUSE)) {
            pause = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_RESUME)) {
            resume = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_TERMINATE)) {
            terminate = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_PREEMPT)) {
            preempt = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_RESTORE)) {
            restore = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_SIGNAL)) {
            signal = true;
            PMIX_VALUE_GET_NUMBER(rc, &req->info[n].value, sigvalue, int);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_BAD_PARAM;
            }

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_SESSION_EXTEND)) {
            extend = PMIX_INFO_TRUE(&req->info[n]);

        } else if (PMIX_CHECK_KEY(&req->info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &req->info[n].value, tval, int);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }  // for loop


    if (NULL == session) {
        // this operation is referring to a previously instantiated session
        session = prte_get_session_object(req->sessionID);
        if (NULL == session) {
            // we don't know about it
            return PMIX_ERR_NOT_FOUND;
        }
        // TODO: execute the specified operation on this session
        if (terminate || pause || resume || restore || preempt ||
            signal || extend || 0 < tval || 0 < sigvalue) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        return PMIX_SUCCESS;
    }

    // continue working on a new instantiation
    if (NULL != jdata) {
        /* find the personality being passed - we need this info to direct
         * option parsing */
        if (NULL == personality) {
            // do a quick search for it
            for (i=0; i < req->ninfo; i++) {
                if (PMIX_CHECK_KEY(&req->info[i], PMIX_PERSONALITY)) {
                    personality = &req->info[i];
                    break;
                }
            }
        }
        if (NULL == personality) {
            /* use the default */
            jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(NULL);
        } else if (NULL == jdata->personality) {
            /* only resolve it once - the info scan above may already have
             * done so, and repeating the split leaks the first argv and
             * caches the same job info twice */
            jdata->personality = PMIx_Argv_split(personality->value.data.string, ',');
            jdata->schizo = (struct prte_schizo_base_module_t*)prte_schizo_base_detect_proxy(personality->value.data.string);
            pmix_server_cache_job_info(jdata, personality);
        }
        if (NULL == jdata->schizo) {
            /* no component claims the requested personality - refuse the
             * request instead of leaving the job with a NULL personality
             * for someone downstream to dereference */
            pmix_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename,
                           (NULL == personality) ? "NULL" : personality->value.data.string);
            return PMIX_ERR_NOT_FOUND;
        }
    }

    if (NULL != hosts) {
        /* add the designation to the apps in the job, if one was provided. These
         * will be added to the global pool when the job is setup for launch */
        if (NULL != jdata) {
            for (j=0; j < jdata->apps->size; j++) {
                app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, j);
                if (NULL == app) {
                    continue;
                }
                // the add_host attribute will be removed after processing
                prte_set_attribute(&app->attributes, PRTE_APP_ADD_HOST, PRTE_ATTR_GLOBAL,
                                   hosts, PMIX_STRING);
                /* also provide it as dash-host (if not already specified) so the
                 * job will use those nodes */
                if (!prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, NULL, PMIX_STRING)) {
                    prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST, PRTE_ATTR_GLOBAL,
                                       hosts, PMIX_STRING);
                }
            }
        }
    }
    return PMIX_SUCCESS;
}

/* Callbacks to process a session control answer from the scheduler
 * and pass on any results to the requesting client
 */
static void passthru(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL != req->infocbfunc) {
        // call the requestor's callback with the returned info
        req->infocbfunc(req->status, req->info, req->ninfo, req->cbdata, req->rlcbfunc, req->rlcbdata);
    } else if (NULL != req->rlcbfunc) {
        // no one to answer, but the result array still has to go back
        req->rlcbfunc(req->rlcbdata);
    }
    // cleanup our request
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* Caddy carrying a session-control result from the PMIx progress thread over
 * to the PRRTE progress thread - the same shape, and for the same reason, as
 * prte_alloc_resp_caddy_t in pmix_server.c.  A prte_pmix_server_req_t is a
 * PRRTE object owned by the PRRTE progress thread, so the PMIx-thread callback
 * must only capture the result; recording it on the request - and above all
 * freeing the array the request was carrying - has to happen in the shifted
 * handler. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_pmix_server_req_t *req;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
    pmix_release_cbfunc_t rel;
    void *relcbdata;
} prte_sessctrl_resp_caddy_t;
static void srcon(prte_sessctrl_resp_caddy_t *p)
{
    p->req = NULL;
    p->status = PMIX_SUCCESS;
    p->info = NULL;
    p->ninfo = 0;
    p->rel = NULL;
    p->relcbdata = NULL;
}
static void srdes(prte_sessctrl_resp_caddy_t *p)
{
    if (NULL != p->req) {
        PMIX_RELEASE(p->req);
    }
}
static PMIX_CLASS_INSTANCE(prte_sessctrl_resp_caddy_t, pmix_object_t, srcon, srdes);

/* record the result on the request and answer - runs on the PRRTE
 * progress thread */
static void _sessctrl_resp(int sd, short args, void *cbdata)
{
    prte_sessctrl_resp_caddy_t *cd = (prte_sessctrl_resp_caddy_t*)cbdata;
    prte_pmix_server_req_t *req = cd->req;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* now safe to touch the request. Beware that a caller may hand the
     * request's OWN info array back as the result - do not free it in
     * that case */
    req->status = cd->status;
    if (req->copy && NULL != req->info && cd->info != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
        req->copy = false;
    }
    req->info = cd->info;
    req->ninfo = cd->ninfo;
    req->rlcbfunc = cd->rel;
    req->rlcbdata = cd->relcbdata;

    passthru(0, 0, req);
    /* passthru released the request's array-slot reference; drop ours */
    PMIX_RELEASE(cd);
}

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_sessctrl_resp_caddy_t *cd;

    /* GOLDEN RULE: this can be invoked on the PMIx progress thread, so
     * capture the result and post it - touch no PRRTE state here */
    cd = PMIX_NEW(prte_sessctrl_resp_caddy_t);
    PMIX_RETAIN(req);
    cd->req = req;
    cd->status = status;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->rel = rel;
    cd->relcbdata = relcbdata;

    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, _sessctrl_resp);
}

static void pass_request(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    pmix_status_t rc;
    size_t n;
    pmix_info_t *xfer;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    if (!PRTE_PROC_IS_MASTER) {
        /* if we are not the DVM master, then we have to send
         * this request to the master for processing */
        rc = prte_server_send_request(PRTE_PMIX_SESSION_CTRL, req);
        if (PRTE_SUCCESS != rc) {
            goto callback;
        }
        return;
    }

    /* if we are the DVM master, then handle this ourselves - start
     * by ensuring the scheduler is connected to us */
    rc = prte_pmix_set_scheduler();
    if (PMIX_SUCCESS != rc) {
        goto callback;
    }

    /* if the requestor is the scheduler, then this is a directive
     * to the DVM - e.g., to instantiate or terminate a session.
     * In that case, we process it here */
    if (PMIX_CHECK_PROCID(&prte_pmix_server_globals.scheduler, &req->tproc)) {
        rc = process_directive(req);
    } else {
        // we need to pass the request on to the scheduler
        // need to add the requestor's ID to the info array
        PMIX_INFO_CREATE(xfer, req->ninfo + 1);
        for (n=0; n < req->ninfo; n++) {
            PMIX_INFO_XFER(&xfer[n], &req->info[n]);
        }
        PMIX_INFO_LOAD(&xfer[req->ninfo], PMIX_REQUESTOR, &req->tproc, PMIX_PROC);
        // the current req object points to the caller's info array, so leave it alone
        req->copy = true;
        req->info = xfer;
        req->ninfo++;
        rc = PMIx_Session_control(req->sessionID, req->info, req->ninfo,
                                  infocbfunc, req);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto callback;
        }
        return;
    }

callback:
    /* this section gets executed solely upon an error or if this was a
     * directive to us from the scheduler */
    if (NULL != req->infocbfunc) {
        req->infocbfunc(rc, req->info, req->ninfo, req->cbdata, prte_pmix_server_req_release, req);
        return;
    }
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* this is the upcall from the PMIx server for the session
 * control support. Since we are going to touch global structures
 * (e.g., the session tracker pointer array), we have to threadshift
 * this request into our own internal progress thread. Note that the
 * session control directive could have come to this host from the
 * scheduler, or a tool, or even an application process. */
pmix_status_t pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                                          uint32_t sessionID,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl upcalled on behalf of proc %s:%u with %" PRIsize_t " directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), requestor->nspace, requestor->rank, ndirs);

    /* create a request tracker for this operation */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "SESSIONCTRL: %u", sessionID);
    PMIX_PROC_LOAD(&req->tproc, requestor->nspace, requestor->rank);
    req->sessionID = sessionID;
    req->info = (pmix_info_t *) directives;
    req->ninfo = ndirs;
    req->infocbfunc = cbfunc;
    req->cbdata = cbdata;

    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, pass_request, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}
