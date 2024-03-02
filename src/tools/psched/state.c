/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_quit.h"

#include "src/mca/state/base/base.h"
#include "psched.h"

/* global variables */
pmix_list_t prte_psched_states = PMIX_LIST_STATIC_INIT;

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

/* local functions */
static void alloc_complete(int fd, short args, void *cbata);

/* define job state machine sequence - only required to
 * allow integration to main PRRTE code for detecting
 * base allocation */
static prte_job_state_t launch_states[] = {
    PRTE_JOB_STATE_ALLOCATE,
    PRTE_JOB_STATE_ALLOCATION_COMPLETE
};

static prte_state_cbfunc_t launch_callbacks[] = {
    prte_ras_base_allocate,
    alloc_complete
};

/* define scheduler state machine sequence for walking
 * thru an allocation lifecycle */
static prte_sched_state_t sched_states[] = {
    PSCHED_STATE_INIT,
    PSCHED_STATE_QUEUE,
    PSCHED_STATE_SESSION_COMPLETE
};

static prte_state_cbfunc_t sched_callbacks[] = {
    psched_request_init,
    psched_request_queue,
    psched_session_complete
};


static void force_quit(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;

    prte_event_base_active = false;
    PMIX_RELEASE(caddy);
}

static int add_psched_state(prte_sched_state_t state,
                            prte_state_cbfunc_t cbfunc);
static void psched_print_state_machine(void);

prte_state_base_module_t psched_state_module = {
    .init = init,
    .finalize = finalize,
    .activate_job_state = prte_state_base_activate_job_state,
    .add_job_state = prte_state_base_add_job_state,
    .set_job_state_callback = prte_state_base_set_job_state_callback,
    .remove_job_state = prte_state_base_remove_job_state,
    .activate_proc_state = prte_state_base_activate_proc_state,
    .add_proc_state = prte_state_base_add_proc_state,
    .set_proc_state_callback = prte_state_base_set_proc_state_callback,
    .remove_proc_state = prte_state_base_remove_proc_state
};

static int state_base_verbose = -1;
void psched_state_init(void)
{
    pmix_output_stream_t lds;

    pmix_mca_base_var_register("prte", "state", "base", "verbose",
                               "Verbosity for debugging state machine",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &state_base_verbose);
    if (0 <= state_base_verbose) {
        PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
        lds.lds_want_stdout = true;
        prte_state_base_framework.framework_output = pmix_output_open(&lds);
        PMIX_DESTRUCT(&lds);
        pmix_output_set_verbosity(prte_state_base_framework.framework_output, state_base_verbose);
    }

    prte_state = psched_state_module;
    psched_state_module.init();
}

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int i, rc;
    int num_states;

    pmix_output_verbose(2, prte_state_base_framework.framework_output,
                        "%s state:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* setup the state machines */
    PMIX_CONSTRUCT(&prte_job_states, pmix_list_t);
    PMIX_CONSTRUCT(&prte_proc_states, pmix_list_t);
    PMIX_CONSTRUCT(&prte_psched_states, pmix_list_t);

    /* setup the job state machine */
    num_states = sizeof(launch_states) / sizeof(prte_job_state_t);
    for (i = 0; i < num_states; i++) {
        if (PRTE_SUCCESS
            != (rc = prte_state.add_job_state(launch_states[i], launch_callbacks[i]))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    /* add the termination response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_DAEMONS_TERMINATED, prte_quit);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add a default error response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_FORCED_EXIT, force_quit);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add callback to report progress, if requested */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_REPORT_PROGRESS,
                                  prte_state_base_report_progress);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    if (5 < pmix_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_job_state_machine();
    }

    /* setup the scheduler state machine */
    num_states = sizeof(sched_states) / sizeof(prte_sched_state_t);
    for (i = 0; i < num_states; i++) {
        rc = add_psched_state(sched_states[i], sched_callbacks[i]);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
        }
    }
    if (4 < pmix_output_get_verbosity(psched_globals.output)) {
        psched_print_state_machine();
    }

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    /* cleanup the state machines */
    PMIX_LIST_DESTRUCT(&prte_proc_states);
    PMIX_LIST_DESTRUCT(&prte_job_states);
    PMIX_LIST_DESTRUCT(&prte_psched_states);

    return PRTE_SUCCESS;
}

static void alloc_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_RELEASE(caddy);
}

void psched_activate_sched_state(psched_req_t *req, prte_sched_state_t state)
{
    psched_state_t *s, *any = NULL, *error = NULL;

    /* check for uniqueness */
    PMIX_LIST_FOREACH(s, &prte_psched_states, psched_state_t) {
        if (s->sched_state == PSCHED_STATE_ANY) {
            /* save this place */
            any = s;
        }
        if (s->sched_state == PSCHED_STATE_ERROR) {
            error = s;
        }
        if (s->sched_state == state) {
            PRTE_REACHING_SCHED_STATE(req, state);
            if (NULL == s->cbfunc) {
                pmix_output_verbose(1, psched_globals.output,
                                    "%s NULL CBFUNC FOR SCHED %s STATE %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    (NULL == req->user_refid) ? "N/A" : req->user_refid,
                                    prte_sched_state_to_str(state));
                return;
            }
            PSCHED_THREADSHIFT(req, s->cbfunc);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PSCHED_STATE_ERROR < state && NULL != error) {
        s = (psched_state_t *) error;
    } else if (NULL != any) {
        s = (psched_state_t *) any;
    } else {
        pmix_output_verbose(1, psched_globals.output,
                            "ACTIVATE: SCHED STATE %s NOT REGISTERED",
                            prte_sched_state_to_str(state));
        return;
    }
    if (NULL == s->cbfunc) {
        pmix_output_verbose(1, psched_globals.output,
                            "ACTIVATE: ANY STATE HANDLER NOT DEFINED");
        return;
    }
    PRTE_REACHING_SCHED_STATE(req, state);
    PSCHED_THREADSHIFT(req, s->cbfunc);
}

static int add_psched_state(prte_sched_state_t state,
                            prte_state_cbfunc_t cbfunc)
{
    psched_state_t *st;

    /* check for uniqueness */
    PMIX_LIST_FOREACH(st, &prte_psched_states, psched_state_t) {
        if (st->sched_state == state) {
            pmix_output_verbose(1, psched_globals.output,
                                "DUPLICATE STATE DEFINED: %s", prte_sched_state_to_str(state));
            return PRTE_ERR_BAD_PARAM;
        }
    }

    st = PMIX_NEW(psched_state_t);
    st->sched_state = state;
    st->cbfunc = cbfunc;
    pmix_list_append(&prte_psched_states, &(st->super));

    return PRTE_SUCCESS;
}

const char* prte_sched_state_to_str(prte_sched_state_t s)
{
    switch (s) {
        case PSCHED_STATE_UNDEF:
            return "UNDEFINED";
        case PSCHED_STATE_INIT:
            return "INIT";
        case PSCHED_STATE_QUEUE:
            return "QUEUE";
        case PSCHED_STATE_SESSION_COMPLETE:
            return "SESSION COMPLETE";
        default:
            return "UNKNOWN";
    }
}

static void psched_print_state_machine(void)
{
    psched_state_t *st;

    pmix_output(0, "SCHEDULER STATE MACHINE:");
    PMIX_LIST_FOREACH (st, &prte_psched_states, psched_state_t) {
        pmix_output(0, "\tState: %s cbfunc: %s",
                    prte_sched_state_to_str(st->sched_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}


static void state_con(psched_state_t *p)
{
    p->sched_state = PSCHED_STATE_UNDEF;
    p->cbfunc = NULL;
}
PMIX_CLASS_INSTANCE(psched_state_t,
                    pmix_list_item_t,
                    state_con, NULL);

static void req_con(psched_req_t *p)
{
    PMIx_Load_procid(&p->requestor, NULL, PMIX_RANK_INVALID);
    p->copy = false;  // data is not a local copy
    p->data = NULL;
    p->ndata = 0;
    p->user_refid = NULL;
    p->alloc_refid = NULL;
    p->num_nodes = 0;
    p->nlist = NULL;
    p->exclude = NULL;
    p->num_cpus = 0;
    p->ncpulist = NULL;
    p->cpulist = NULL;
    p->memsize = 0.0;
    p->time = NULL;
    p->queue = NULL;
    p->preemptible = false;
    p->lend = NULL;
    p->image = NULL;
    p->waitall = false;
    p->share = false;
    p->noshell = false;
    p->dependency = NULL;
    p->begintime = NULL;
    p->state = PSCHED_STATE_UNDEF;
    p->sessionID = UINT32_MAX;
}
static void req_des(psched_req_t *p)
{
    if (NULL != p->data && p->copy) {
        PMIx_Info_free(p->data, p->ndata);
    }
    if (NULL != p->user_refid) {
        free(p->user_refid);
    }
    if (NULL != p->alloc_refid) {
        free(p->alloc_refid);
    }
    if (NULL != p->nlist) {
        free(p->nlist);
    }
    if (NULL != p->exclude) {
        free(p->exclude);
    }
    if (NULL != p->ncpulist) {
        free(p->ncpulist);
    }
    if (NULL != p->cpulist) {
        free(p->cpulist);
    }
    if (NULL != p->time) {
        free(p->time);
    }
    if (NULL != p->queue) {
        free(p->queue);
    }
    if (NULL != p->lend) {
        free(p->lend);
    }
    if (NULL != p->image) {
        free(p->image);
    }
    if (NULL != p->dependency) {
        free(p->dependency);
    }
    if (NULL != p->begintime) {
        free(p->begintime);
    }
}
PMIX_CLASS_INSTANCE(psched_req_t,
                    pmix_list_item_t,
                    req_con, req_des);
