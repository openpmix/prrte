/*
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#if HAVE_UNISTD_H
#    include <unistd.h>
#endif
#if HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/base/base.h"
#include "src/rml/rml.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/prte_show_help.h"

#include "src/mca/state/base/base.h"

void prte_state_base_activate_job_state(prte_job_t *jdata, prte_job_state_t state)
{
    pmix_list_item_t *itm, *any = NULL, *error = NULL;
    prte_state_t *s;
    prte_state_caddy_t *caddy;

    /* The state log records the moment a transition was ORDERED, which is
     * here - the dispatch below only queues the handler.  It is deliberately
     * ahead of everything else, so that a state nothing is registered for
     * still appears in the record: that a transition was ordered and then
     * dropped is exactly what the log exists to show. */
    if (prte_state_base.log_jobstate) {
        prte_state_base_log_job(jdata, state);
    }

    for (itm = pmix_list_get_first(&prte_job_states); itm != pmix_list_get_end(&prte_job_states);
         itm = pmix_list_get_next(itm)) {
        s = (prte_state_t *) itm;
        if (s->job_state == PRTE_JOB_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->job_state == PRTE_JOB_STATE_ERROR) {
            error = itm;
        }
        if (s->job_state == state) {
            PRTE_REACHING_JOB_STATE(jdata, state);
            if (NULL == s->cbfunc) {
                PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR JOB %s STATE %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     (NULL == jdata) ? "ALL" : PRTE_JOBID_PRINT(jdata->nspace),
                                     prte_job_state_to_str(state)));
                return;
            }
            caddy = PMIX_NEW(prte_state_caddy_t);
            /* the state is always recorded, even when no job accompanies it -
             * handlers reached via the ERROR/ANY fallback read it to decide
             * what happened */
            caddy->job_state = state;
            if (NULL != jdata) {
                caddy->jdata = jdata;
                PMIX_RETAIN(jdata);
            }
            PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, s->cbfunc);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRTE_JOB_STATE_ERROR < state && NULL != error) {
        s = (prte_state_t *) error;
    } else if (NULL != any) {
        s = (prte_state_t *) any;
    } else {
        PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: JOB STATE %s NOT REGISTERED",
                             prte_job_state_to_str(state)));
        return;
    }
    if (NULL == s->cbfunc) {
        PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PMIX_NEW(prte_state_caddy_t);
    /* record the state that actually fired, not the fallback we matched -
     * this is the only way the ERROR/ANY handler can tell what happened */
    caddy->job_state = state;
    if (NULL != jdata) {
        caddy->jdata = jdata;
        PMIX_RETAIN(jdata);
    }
    PRTE_REACHING_JOB_STATE(jdata, state);
    PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, s->cbfunc);
}

int prte_state_base_add_job_state(prte_job_state_t state, prte_state_cbfunc_t cbfunc)
{
    prte_state_t *st;

    /* check for uniqueness */
    PMIX_LIST_FOREACH(st, &prte_job_states, prte_state_t) {
        if (st->job_state == state) {
            PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s", prte_job_state_to_str(state)));
            return PRTE_ERR_BAD_PARAM;
        }
    }

    st = PMIX_NEW(prte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    pmix_list_append(&prte_job_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_set_job_state_callback(prte_job_state_t state, prte_state_cbfunc_t cbfunc)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    for (item = pmix_list_get_first(&prte_job_states); item != pmix_list_get_end(&prte_job_states);
         item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        if (st->job_state == state) {
            st->cbfunc = cbfunc;
            return PRTE_SUCCESS;
        }
    }

    /* if not found, assume SYS priority and install it */
    st = PMIX_NEW(prte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    pmix_list_append(&prte_job_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_remove_job_state(prte_job_state_t state)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    for (item = pmix_list_get_first(&prte_job_states); item != pmix_list_get_end(&prte_job_states);
         item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        if (st->job_state == state) {
            pmix_list_remove_item(&prte_job_states, item);
            PMIX_RELEASE(item);
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

void prte_state_base_print_job_state_machine(void)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    pmix_output(0, "PRTE_JOB_STATE_MACHINE:");
    for (item = pmix_list_get_first(&prte_job_states); item != pmix_list_get_end(&prte_job_states);
         item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        pmix_output(0, "\tState: %s cbfunc: %s", prte_job_state_to_str(st->job_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}

/****    PROC STATE MACHINE    ****/
void prte_state_base_activate_proc_state(pmix_proc_t *proc, prte_proc_state_t state)
{
    pmix_list_item_t *itm, *any = NULL, *error = NULL;
    prte_state_t *s;
    prte_state_caddy_t *caddy;

    /* as above: log the order, not the dispatch */
    if (prte_state_base.log_procstate) {
        prte_state_base_log_proc(proc, state);
    }

    /* the proc machine is keyed entirely on the name - there is nothing
     * to dispatch without one */
    if (NULL == proc) {
        PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: NULL PROC FOR STATE %s",
                             prte_proc_state_to_str(state)));
        return;
    }

    for (itm = pmix_list_get_first(&prte_proc_states); itm != pmix_list_get_end(&prte_proc_states);
         itm = pmix_list_get_next(itm)) {
        s = (prte_state_t *) itm;
        if (s->proc_state == PRTE_PROC_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->proc_state == PRTE_PROC_STATE_ERROR) {
            error = itm;
        }
        if (s->proc_state == state) {
            PRTE_REACHING_PROC_STATE(proc, state);
            if (NULL == s->cbfunc) {
                PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR PROC %s STATE %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                                     prte_proc_state_to_str(state)));
                return;
            }
            caddy = PMIX_NEW(prte_state_caddy_t);
            caddy->name = *proc;
            caddy->proc_state = state;
            PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, s->cbfunc);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRTE_PROC_STATE_ERROR < state && NULL != error) {
        s = (prte_state_t *) error;
    } else if (NULL != any) {
        s = (prte_state_t *) any;
    } else {
        PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "INCREMENT: ANY STATE NOT FOUND"));
        return;
    }
    if (NULL == s->cbfunc) {
        PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PMIX_NEW(prte_state_caddy_t);
    caddy->name = *proc;
    caddy->proc_state = state;
    PRTE_REACHING_PROC_STATE(proc, state);
    PRTE_PMIX_THREADSHIFT(caddy, prte_event_base, s->cbfunc);
}

int prte_state_base_add_proc_state(prte_proc_state_t state, prte_state_cbfunc_t cbfunc)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    /* check for uniqueness */
    for (item = pmix_list_get_first(&prte_proc_states);
         item != pmix_list_get_end(&prte_proc_states); item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        if (st->proc_state == state) {
            PMIX_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s", prte_proc_state_to_str(state)));
            return PRTE_ERR_BAD_PARAM;
        }
    }

    st = PMIX_NEW(prte_state_t);
    st->proc_state = state;
    st->cbfunc = cbfunc;
    pmix_list_append(&prte_proc_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_set_proc_state_callback(prte_proc_state_t state, prte_state_cbfunc_t cbfunc)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    for (item = pmix_list_get_first(&prte_proc_states);
         item != pmix_list_get_end(&prte_proc_states); item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        if (st->proc_state == state) {
            st->cbfunc = cbfunc;
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_state_base_remove_proc_state(prte_proc_state_t state)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    for (item = pmix_list_get_first(&prte_proc_states);
         item != pmix_list_get_end(&prte_proc_states); item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        if (st->proc_state == state) {
            pmix_list_remove_item(&prte_proc_states, item);
            PMIX_RELEASE(item);
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

void prte_state_base_print_proc_state_machine(void)
{
    pmix_list_item_t *item;
    prte_state_t *st;

    pmix_output(0, "PRTE_PROC_STATE_MACHINE:");
    for (item = pmix_list_get_first(&prte_proc_states);
         item != pmix_list_get_end(&prte_proc_states); item = pmix_list_get_next(item)) {
        st = (prte_state_t *) item;
        pmix_output(0, "\tState: %s cbfunc: %s", prte_proc_state_to_str(st->proc_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}

void prte_state_base_local_launch_complete(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    bool found = false;
    PRTE_HIDE_UNUSED_PARAMS(fd, argc);

    PMIX_ACQUIRE_OBJECT(state);
    jdata = state->jdata;
    if (NULL == jdata) {
        PMIX_RELEASE(state);
        return;
    }

    found = prte_get_attribute(&jdata->attributes, PRTE_JOB_SHOW_PROGRESS, NULL, PMIX_BOOL);
    if (found) {
        if (0 == jdata->num_daemons_reported % 100 ||
            jdata->num_daemons_reported == prte_process_info.num_daemons) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_REPORT_PROGRESS);
        }
    }
    PMIX_RELEASE(state);
}

void prte_state_base_report_progress(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    prte_job_t *jdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, argc);

    PMIX_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;
    if (NULL == jdata) {
        PMIX_RELEASE(caddy);
        return;
    }

    pmix_output(prte_clean_output,
                "App launch reported: %d (out of %d) daemons - %d (out of %d) procs",
                (int) jdata->num_daemons_reported, (int) prte_process_info.num_daemons,
                (int) jdata->num_launched, (int) jdata->num_procs);
    PMIX_RELEASE(caddy);
}

/* Tell the data server that a job has ended, so it can drop the published
 * data that was not to outlive it.
 *
 * The lifetime that ended is named in the message: PMIX_PERSIST_APP, since
 * the target is a whole namespace.  Without it the server cannot tell this
 * from an explicit "remove everything I published" and would take data the
 * publisher asked to keep for the session.
 *
 * PMIX_PERSIST_PROC data is therefore reclaimed at job granularity rather
 * than when its individual publisher exits - later than the Standard's
 * "until the publishing process terminates", but a message per terminating
 * process is not a cost this path can carry at scale. */
static void send_purge(pmix_rank_t dest, pmix_proc_t *target)
{
    pmix_data_buffer_t *buf;
    pmix_info_t horizon;
    pmix_persistence_t persist = PMIX_PERSIST_APP;
    int rc, room = -1;
    uint8_t cmd = PRTE_PMIX_PURGE_PROC_CMD;
    size_t ninfo;

    PMIX_DATA_BUFFER_CREATE(buf);

    /* pack the room number */
    rc = PMIx_Data_pack(NULL, buf, &room, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* load the command */
    rc = PMIx_Data_pack(NULL, buf, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* provide the target */
    rc = PMIx_Data_pack(NULL, buf, target, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* one directive: the lifetime that just ended */
    ninfo = 1;
    rc = PMIx_Data_pack(NULL, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    PMIX_INFO_LOAD(&horizon, PMIX_PERSISTENCE, &persist, PMIX_PERSIST);
    rc = PMIx_Data_pack(NULL, buf, &horizon, 1, PMIX_INFO);
    PMIX_INFO_DESTRUCT(&horizon);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    PRTE_RML_RELIABLE_SEND(rc, dest, buf, PRTE_RML_TAG_DATA_SERVER);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

void prte_state_base_notify_data_server(pmix_proc_t *target)
{
    pmix_rank_t global;

    /* if nobody local to us published anything, then we can ignore this */
    if (PMIX_NSPACE_INVALID(prte_pmix_server_globals.server.nspace)) {
        return;
    }

    /* The global store.  An external data server is not addressable over
     * the RML - only the master holds the PMIx connection to it - so the
     * request goes to the master, which relays it. */
    global = (NULL == prte_data_server_uri) ? prte_pmix_server_globals.server.rank
                                            : PRTE_PROC_MY_HNP->rank;
    send_purge(global, target);

    /* ...and OUR OWN store, which is a different one on every daemon but
     * the master.  A PMIX_RANGE_LOCAL publish never leaves the daemon that
     * relayed it - pmix_server_pub.c routes it to PRTE_PROC_MY_NAME - and
     * every daemon runs prte_data_server_init(), so what a local-range
     * publish leaves behind is reclaimable only from here.  Purging just
     * the global store reclaimed everything EXCEPT local-range data, which
     * a single-node run cannot show: there the two stores are one object.
     *
     * Gated on there being no external data server because in that case a
     * daemon's prte_data_server() relays what it receives rather than
     * serving it, so a request addressed to ourselves would not reach our
     * store at all.  (Local-range publish has the same problem in that
     * configuration, and is broken there for the same reason - a separate
     * defect, not one to paper over from here.) */
    if (NULL == prte_data_server_uri && global != PRTE_PROC_MY_NAME->rank) {
        send_purge(PRTE_PROC_MY_NAME->rank, target);
    }
}

/* A proc reported a state and we hold no job object to account it against.
 *
 * Both track_procs implementations used to drop such a report with a bare
 * "goto cleanup", and the silence is what turned a lost job object into a
 * hang.  Every conclusion about a job is derived HERE, from the job object:
 * the per-proc counting, the TERMINATED activation it rolls up into, and -
 * on a one-shot DVM - the "is anything still alive?" scan that only ever
 * runs when a job reaches TERMINATED.  With no job object none of that
 * happens, so the DVM waits forever on a job it can no longer see, having
 * said nothing about why.  (Seen when a spawned job's object was released
 * one reference early: prterun never exited, and the only output was an
 * odls "Not found" naming a file and a line.)
 *
 * The job itself cannot be recovered - it is gone.  What is still owed is a
 * report saying so, and a conclusion: a runtime that has lost a job must
 * stop waiting for it rather than hang.  Nothing here concludes while any
 * local child is still running, so a straggler is not torn down under.
 */
void prte_state_base_orphaned_proc(pmix_proc_t *proc, prte_proc_state_t state)
{
    prte_proc_t *proct;
    prte_job_t *jptr;
    int i;

    /* Only a report that the process is really gone matters.  The callers are
     * also reached by the running states (RUNNING, REGISTERED, IOF_COMPLETE,
     * ...), and those have nothing to account for - complaining about each of
     * them would bury the one report that does.  Everything above
     * PRTE_PROC_STATE_ERROR is a way of having died, and with the job gone
     * none of them is recoverable, so they all count. */
    if (PRTE_PROC_STATE_WAITPID_FIRED != state &&
        PRTE_PROC_STATE_TERMINATED != state &&
        PRTE_PROC_STATE_ERROR >= state) {
        return;
    }

    /* A teardown in progress releases job objects as it goes, so procs
     * reported during one are expected to find no job and there is nothing
     * left to conclude - the conclusion has already been reached. */
    if (prte_finalizing || prte_abnormal_term_ordered) {
        return;
    }

    prte_show_help("help-state-base.txt", "orphaned-proc", true,
                   PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc));

    /* Retire the proc ourselves.  Clearing PRTE_PROC_FLAG_ALIVE and dropping
     * the PMIx client are done by the accounted TERMINATED path below, which
     * reaches the proc THROUGH the job object - so with no job object the
     * proc stays marked alive forever, and every "is anything still running?"
     * test in the runtime (including the one right below) answers yes.  The
     * proc object itself is still perfectly good; only the job that owned it
     * is gone, and prte_local_children holds the daemon's own reference. */
    for (i = 0; i < prte_local_children->size; i++) {
        proct = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL == proct || !PMIX_CHECK_PROCID(&proct->name, proc)) {
            continue;
        }
        if (PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_ALIVE)) {
            PRTE_FLAG_UNSET(proct, PRTE_PROC_FLAG_ALIVE);
            PMIx_server_deregister_client(proc, NULL, NULL);
        }
        if (proct->state < PRTE_PROC_STATE_TERMINATED) {
            proct->state = PRTE_PROC_STATE_TERMINATED;
        }
        break;
    }

    /* nothing concludes while a local child is still running - this is the
     * same guard the accounted termination paths apply before exiting */
    for (i = 0; i < prte_local_children->size; i++) {
        proct = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL != proct && PRTE_FLAG_TEST(proct, PRTE_PROC_FLAG_ALIVE)) {
            pmix_output_verbose(5, prte_state_base_framework.framework_output,
                                "%s state:base orphaned proc %s, but %s is still alive",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                                PRTE_NAME_PRINT(&proct->name));
            return;
        }
    }

    if (!PRTE_PROC_IS_MASTER) {
        /* a daemon concludes only when it has been told to go and its routes
         * are gone - the identical condition its accounted path checks */
        if (prte_prteds_term_ordered && 0 == prte_rml_base.n_children) {
            pmix_output_verbose(5, prte_state_base_framework.framework_output,
                                "%s state:base orphaned proc, all routes and children gone"
                                " - exiting",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
        }
        return;
    }

    /* On the master: a persistent DVM outlives its jobs and must stay up
     * whatever it has lost - a later job is still perfectly launchable.  A
     * one-shot DVM exists only to run these jobs, so once none of the ones
     * it CAN still see is running, it has to end.  prte_prteds_term_ordered
     * says the teardown is already under way (terminate_orteds sets it), so
     * this stays a one-shot however many orphans arrive. */
    if (prte_persistent || prte_prteds_term_ordered) {
        return;
    }
    for (i = 0; i < prte_job_data->size; i++) {
        jptr = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, i);
        if (NULL == jptr ||
            PMIX_CHECK_NSPACE(jptr->nspace, PRTE_PROC_MY_NAME->nspace)) {
            continue;
        }
        if (jptr->state < PRTE_JOB_STATE_TERMINATED) {
            /* something we can still account for is running */
            return;
        }
    }

    pmix_output_verbose(5, prte_state_base_framework.framework_output,
                        "%s state:base orphaned proc and no job left running - terminating",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
    /* the run did not do what was asked of it - do not report success */
    PRTE_UPDATE_EXIT_STATUS(1);
    prte_plm.terminate_orteds();
}

void prte_state_base_track_procs(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    pmix_proc_t *proc;
    prte_proc_state_t state;
    prte_job_t *jdata;
    prte_proc_t *pdata;
    int i;
    pmix_proc_t target;
    pmix_rank_t threshold;
    PRTE_HIDE_UNUSED_PARAMS(fd, argc);

    PMIX_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    pmix_output_verbose(5, prte_state_base_framework.framework_output,
                        "%s state:base:track_procs called for proc %s state %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                        prte_proc_state_to_str(state));

    /* get the job object for this proc */
    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        prte_state_base_orphaned_proc(proc, state);
        goto cleanup;
    }
    if (PRTE_PROC_STATE_READY_FOR_DEBUG == state) {
        if (prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC, NULL, PMIX_BOOL) ||
            prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT, NULL, PMIX_BOOL) ||
            prte_get_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, NULL, PMIX_BOOL)) {
            if (PRTE_PROC_IS_MASTER) {
                threshold = jdata->num_procs;
            } else {
                threshold = jdata->num_local_procs;
            }
            if (PMIX_RANK_LOCAL_PEERS == proc->rank) {
                jdata->num_ready_for_debug += jdata->num_local_procs;
            } else {
                jdata->num_ready_for_debug++;
            }
            if (jdata->num_ready_for_debug < threshold) {
                goto cleanup;
            }
            PMIX_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:base all local %s procs on node %s ready for debug",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 proc->nspace, prte_process_info.nodename));
            /* let the DVM master know we are ready */
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_READY_FOR_DEBUG);
        }
        goto cleanup;
    }

    pdata = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, proc->rank);
    if (NULL == pdata) {
        goto cleanup;
    }

    if (PRTE_PROC_STATE_RUNNING == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_launched++;
        if (1 == jdata->num_launched) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_STARTED);
        }
        if (jdata->num_launched == jdata->num_procs) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_RUNNING);
        }
    } else if (PRTE_PROC_STATE_REGISTERED == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_reported++;
        if (jdata->num_reported == jdata->num_procs) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_REGISTERED);
        }
    } else if (PRTE_PROC_STATE_IOF_COMPLETE == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        /* Release the IOF file descriptors */
        if (NULL != prte_iof.close) {
            prte_iof.close(proc, PRTE_IOF_STDALL);
        }
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_IOF_COMPLETE);
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_WAITPID)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_WAITPID_FIRED == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_WAITPID);
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_IOF_COMPLETE)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_TERMINATED == state) {
        /* Have we already counted this proc?  Ask the flag, not the state
         * word.  Testing "pdata->state == state" looks equivalent and is not,
         * for exactly the procs this matters most for: every failure state is
         * PRTE_PROC_STATE_ERROR (50) + n, i.e. GREATER than
         * PRTE_PROC_STATE_TERMINATED (20), so the assignment below - guarded
         * by "< TERMINATED" so a terminal diagnosis is never overwritten by
         * the generic one - deliberately leaves such a proc's state above
         * TERMINATED.  The comparison therefore never matches for a proc that
         * failed, and every repeat activation counted it again.
         *
         * A repeat is not hypothetical on the HNP: for a *remote* failed proc
         * errmgr/dvm force-marks WAITPID_FIRED and IOF_COMPLETE ("we won't
         * hear anything more about it"), and its daemon then relays the real
         * pair as well - two completions, two counts, for one proc.
         * num_terminated then reaches num_procs while a survivor is still
         * running, the job is declared NORMALLY TERMINATED, and the DVM is
         * torn down under that survivor: its final output is discarded and it
         * never terminates properly.  Seen as an intermittently missing last
         * line from a random rank whenever a job had a failure in it.
         *
         * state/prted has always used the flag here; this is the same test. */
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_RECORDED)) {
            pmix_output_verbose(5, prte_state_base_framework.framework_output,
                                "%s state:base:track_procs proc %s already recorded as %s. Skip transition.",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(proc),
                                prte_proc_state_to_str(state));
            goto cleanup;
        }
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_RECORDED);

        /* If this proc was connected to others, they are owed an event: the
         * PMIx definition of "connected" is that a member which departs
         * without first calling PMIx_Disconnect is a reportable event for
         * the rest of the assemblage.  This is the one place that sees every
         * proc in the DVM stop, whether it exited, failed, or was on a node
         * whose daemon died, and the RECORDED flag just set above is what
         * makes it exactly once per proc. */
        prte_pmix_server_connection_terminated(pdata);

        /* update the proc state */
        PRTE_FLAG_UNSET(pdata, PRTE_PROC_FLAG_ALIVE);
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_LOCAL)) {
            PMIx_server_deregister_client(proc, NULL, NULL);
        }
        /* if we are trying to terminate and our routes are
         * gone, then terminate ourselves IF no local procs
         * remain (might be some from another job)
         */
        if (prte_prteds_term_ordered && 0 == prte_rml_base.n_children) {
            for (i = 0; i < prte_local_children->size; i++) {
                pdata = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
                if (NULL != pdata &&
                    PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    goto cleanup;
                }
            }
            /* call our appropriate exit procedure */
            pmix_output_verbose(5, prte_state_base_framework.framework_output,
                                 "%s state:base all routes and children gone - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            goto cleanup;
        }
        /* track job status */
        jdata->num_terminated++;
        if (jdata->num_terminated == jdata->num_procs) {
            /* if requested, check fd status for leaks */
            if (prte_state_base.run_fdcheck) {
                prte_state_base_check_fds(jdata);
            }
            /* tell the data server the job is over, so it can drop what
             * this namespace published that was not to outlive it.  This
             * used to be done only when an external data server was
             * configured, which left the built-in one - the usual case -
             * never told a job had ended, so nothing was ever reclaimed
             * from it short of the DVM shutting down */
            PMIX_LOAD_PROCID(&target, jdata->nspace, PMIX_RANK_WILDCARD);
            prte_state_base_notify_data_server(&target);
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
        }
    }

cleanup:
    PMIX_RELEASE(caddy);
}

void prte_state_base_check_fds(prte_job_t *jdata)
{
    int nfds, i, fdflags, flflags;
    char path[1024], info[256], **list = NULL, *status, *result, *r2;
    ssize_t rc;
    struct flock fl;
    bool flk;
    int cnt = 0;

    /* get the number of available file descriptors
     * for this daemon */
    nfds = getdtablesize();
    result = NULL;
    /* loop over them and get their info */
    for (i = 0; i < nfds; i++) {
        fdflags = fcntl(i, F_GETFD);
        if (-1 == fdflags) {
            /* no open fd in that slot */
            continue;
        }
        flflags = fcntl(i, F_GETFL);
        if (-1 == flflags) {
            /* no open fd in that slot */
            continue;
        }
        snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
        memset(info, 0, sizeof(info));
        /* read the info about this fd.  readlink() does not NUL-terminate,
         * so leave room for the terminator the memset above supplied */
        rc = readlink(path, info, sizeof(info) - 1);
        if (-1 == rc) {
            /* this fd is unavailable */
            continue;
        }
        /* get any file locking status */
        fl.l_type = F_WRLCK;
        fl.l_whence = 0;
        fl.l_start = 0;
        fl.l_len = 0;
        if (-1 == fcntl(i, F_GETLK, &fl)) {
            flk = false;
        } else {
            flk = true;
        }
        /* construct the list of capabilities */
        if (fdflags & FD_CLOEXEC) {
            PMIx_Argv_append_nosize(&list, "cloexec");
        }
        if (flflags & O_APPEND) {
            PMIx_Argv_append_nosize(&list, "append");
        }
        if (flflags & O_NONBLOCK) {
            PMIx_Argv_append_nosize(&list, "nonblock");
        }
        /* from the man page:
         *  Unlike the other values that can be specified in flags,
         * the access mode values O_RDONLY, O_WRONLY, and O_RDWR,
         * do not specify individual bits.  Rather, they define
         * the low order two bits of flags, and defined respectively
         * as 0, 1, and 2. */
        if (O_RDONLY == (flflags & 3)) {
            PMIx_Argv_append_nosize(&list, "rdonly");
        } else if (O_WRONLY == (flflags & 3)) {
            PMIx_Argv_append_nosize(&list, "wronly");
        } else {
            PMIx_Argv_append_nosize(&list, "rdwr");
        }
        if (flk && F_UNLCK != fl.l_type) {
            if (F_WRLCK == fl.l_type) {
                PMIx_Argv_append_nosize(&list, "wrlock");
            } else {
                PMIx_Argv_append_nosize(&list, "rdlock");
            }
        }
        if (NULL != list) {
            status = PMIx_Argv_join(list, ' ');
            PMIx_Argv_free(list);
            list = NULL;
            if (NULL == result) {
                pmix_asprintf(&result, "    %d\t(%s)\t%s\n", i, info, status);
            } else {
                pmix_asprintf(&r2, "%s    %d\t(%s)\t%s\n", result, i, info, status);
                free(result);
                result = r2;
            }
            free(status);
        }
        ++cnt;
    }
    /* on a platform with no /proc (e.g. macOS) every readlink above fails and
     * result is still NULL - do not hand that to a "%s" conversion */
    pmix_asprintf(&r2, "%s: %d open file descriptors after job %d completed\n%s",
                  PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cnt, PRTE_LOCAL_JOBID(jdata->nspace),
                  (NULL == result) ? "" : result);
    pmix_output(0, "%s", r2);
    if (NULL != result) {
        free(result);
    }
    free(r2);
}

void prte_state_base_recover_resources(prte_job_t *jdata, prte_proc_t *pptr)
{
    prte_node_t *node, *nptr;
    prte_job_map_t *map;
    prte_proc_t *p;
    int n, node_idx, proc_idx, rc;
    bool takeall, empty;
    hwloc_obj_t obj;
    hwloc_obj_type_t type;
    hwloc_cpuset_t boundcpus, tgt;

    node = pptr->node;
    map = jdata->map;

    /* Find this proc in the node's proc array.  This routine can be entered
     * more than once for the same proc - e.g. a daemon loss marks the proc
     * PRTE_PROC_STATE_TERM_WO_SYNC and a subsequent job abort delivers a
     * second terminal state - so it must be idempotent.  If the proc is no
     * longer in the node array, its resources were already recovered on an
     * earlier pass; recovering again would double-decrement the node counters
     * and, worse, release the proc a second time (see the release below),
     * freeing an object still held in jdata->procs and corrupting the eventual
     * job teardown.  So treat an already-recovered proc as a no-op. */
    proc_idx = INT_MAX;
    for (n=0; n < node->procs->size; n++) {
        p = (prte_proc_t*)pmix_pointer_array_get_item(node->procs, n);
        if (NULL == p) {
            continue;
        }
        if (p == pptr) {
            proc_idx = n;
            break;
        }
    }
    if (INT_MAX == proc_idx) {
        return;
    }

    // recover node resources
    node->slots_inuse--;
    node->num_procs--;
    node->next_node_rank--;

    // find the node in the map
    node_idx = INT_MAX;
    for (n=0; n < jdata->map->nodes->size; n++) {
        nptr = (prte_node_t*)pmix_pointer_array_get_item(jdata->map->nodes, n);
        if (NULL == nptr) {
            continue;
        }
        if (nptr == node) {
            node_idx = n;
        }
    }

    // determine how cpus were handled
    takeall = false;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        type = HWLOC_OBJ_PU;
    } else {
        type = HWLOC_OBJ_CORE;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16) ||
        PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(map->mapping) ||
        PRTE_MAPPING_SEQ == PRTE_GET_MAPPING_POLICY(map->mapping)) {
        takeall = true;
    }

    boundcpus = hwloc_bitmap_alloc();
    /* release the resources held by the proc - only the first
     * cpu in the proc's cpuset was used to mark usage */
    if (NULL != pptr->cpuset) {
        if (0 != (rc = hwloc_bitmap_list_sscanf(boundcpus, pptr->cpuset))) {
            pmix_output(0, "hwloc_bitmap_sscanf returned %s for the string %s",
                        prte_strerror(rc), pptr->cpuset);
            goto next;
        }
        if (takeall) {
            tgt = boundcpus;
        } else {
            /* we only want to restore the first CPU of whatever region
             * the proc was bound to, so we have to first narrow the
             * bitmap down to only that region */
            hwloc_bitmap_andnot(prte_rmaps_base.available, boundcpus, node->available);
            /* the set bits in the result are the bound cpus that are still
             * marked as in-use */
            obj = hwloc_get_obj_inside_cpuset_by_type(node->topology->topo,
                                                      prte_rmaps_base.available, type, 0);
            if (NULL == obj) {
                pmix_output(0, "COULD NOT GET BOUND CPU FOR RESOURCE RELEASE");
                goto next;
            }
            tgt = obj->cpuset;
        }
        hwloc_bitmap_or(node->available, node->available, tgt);
    }

next:
    if (proc_idx < INT_MAX) {
        /* set the entry in the node's proc array to NULL */
        pmix_pointer_array_set_item(node->procs, proc_idx, NULL);
    }
    /* release the proc once for the map entry */
    PMIX_RELEASE(pptr);

    // if the node is empty for this job, then remove it from the map
    empty = true;
    for (n=0; n < node->procs->size; n++) {
        p = (prte_proc_t*)pmix_pointer_array_get_item(node->procs, n);
        if (NULL != p && PMIx_Check_nspace(p->name.nspace, jdata->nspace)) {
            empty = false;
            break;
        }
    }
    if (empty) {
        if (node_idx < INT_MAX) {
            /* set the node location to NULL */
            pmix_pointer_array_set_item(map->nodes, node_idx, NULL);
        }
        /* flag that the node is no longer in a map.  This must happen BEFORE
         * the release below: the map holds a reference, and dropping it can
         * be the last one, leaving the flag update to touch freed memory */
        PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
        /* maintain accounting */
        PMIX_RELEASE(node);
    }

    // release the scratch bitmap
    hwloc_bitmap_free(boundcpus);

}
