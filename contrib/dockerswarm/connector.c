/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * connector -- drive PMIx_Connect / PMIx_Disconnect between two jobs across a
 * real, multi-node DVM, and see whether the runtime keeps the one promise
 * that "connected" makes.
 *
 *   connector [--child-host <node>] [--disconnect] [--abort] [--wait <s>]
 *
 * The parent (no PMIX_PARENT_ID in its job info) spawns one copy of itself,
 * registers for PMIX_ERR_PROC_TERM_WO_SYNC, and connects to the child.  The
 * child connects back and then leaves - either after both halves call
 * PMIx_Disconnect (--disconnect) or by simply exiting.
 *
 * With --abort the child instead dies on a signal after connecting, which
 * asks the other half of the definition: "connected" means the host treats
 * the assemblage as one application, so a failure that terminates the child
 * is to terminate the parent along with it.  The parent prints nothing more
 * in that case - it is killed - which is exactly what the harness checks.
 *
 * That difference is the whole test.  The PMIx definition of "connected" is
 * that the host environment must generate PMIX_ERR_PROC_TERM_WO_SYNC to the
 * assemblage should any member terminate without disconnecting first; a
 * member that DID disconnect owes nobody anything.  So the parent must see
 * the event in the first case and must not in the second, and the difference
 * can only come from the runtime having recorded who connected to whom.
 *
 * Output lines, all prefixed CNCT so the harness can grep:
 *
 *   CNCT <role> <rank> HOST <hostname>
 *   CNCT <role> <rank> CHILD <nspace>
 *   CNCT <role> <rank> CONNECT <status>
 *   CNCT <role> <rank> DISCONNECT <status>
 *   CNCT <role> <rank> EVENT <status> FROM <nspace>:<rank>
 *   CNCT <role> <rank> EVENTS <n>
 *   CNCT <role> <rank> DONE <rc>
 *
 * Why this cannot be a unit test, or even a single-node run: the PMIx server
 * library executes a connect whose participants are ALL local by itself and
 * never calls the host at all ("if all the participants are local, then we
 * don't need the host").  The host only hears about an assemblage that spans
 * nodes - so a DVM with at least two daemons, with the child mapped away from
 * the parent, is the smallest thing that exercises any of this.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;
static const char *role = "parent";
static volatile bool handler_done = false;
static volatile int nevents = 0;

static void evhandler(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t results[], size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) evhdlr_registration_id;
    (void) info;
    (void) ninfo;
    (void) results;
    (void) nresults;

    if (PMIX_ERR_PROC_TERM_WO_SYNC == status) {
        ++nevents;
    }
    printf("CNCT %s %u EVENT %s FROM %s:%u\n", role, myproc.rank,
           PMIx_Error_string(status),
           NULL == source ? "unknown" : source->nspace,
           NULL == source ? 0 : source->rank);
    fflush(stdout);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void reg_cb(pmix_status_t status, size_t errhandler_ref, void *cbdata)
{
    (void) status;
    (void) errhandler_ref;
    (void) cbdata;
    handler_done = true;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t parent, procs[2];
    pmix_app_t app;
    pmix_info_t jobinfo[2];
    pmix_nspace_t child;
    pmix_status_t code = PMIX_ERR_PROC_TERM_WO_SYNC;
    bool disconnect = false;
    bool doabort = false;
    bool ischild = false;
    int wait_secs = 15;
    char *child_host = NULL;
    char host[256];
    int i;

    for (i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--disconnect")) {
            disconnect = true;
        } else if (0 == strcmp(argv[i], "--abort")) {
            doabort = true;
        } else if (0 == strcmp(argv[i], "--child-host") && i + 1 < argc) {
            child_host = argv[++i];
        } else if (0 == strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_secs = atoi(argv[++i]);
        }
    }

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "CNCT init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* a spawned job is told who spawned it, and that is what tells us which
     * half of this program we are */
    rc = PMIx_Get(&myproc, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS == rc && NULL != val && PMIX_PROC == val->type) {
        memcpy(&parent, val->data.proc, sizeof(pmix_proc_t));
        ischild = true;
        role = "child";
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }

    if (0 != gethostname(host, sizeof(host))) {
        strcpy(host, "unknown");
    }
    printf("CNCT %s %u HOST %s\n", role, myproc.rank, host);
    fflush(stdout);

    /* both halves want to hear about a member departing */
    PMIx_Register_event_handler(&code, 1, NULL, 0, evhandler, reg_cb, NULL);
    while (!handler_done) {
        usleep(10000);
    }

    if (!ischild) {
        /* spawn the other half, and keep it off this node so the assemblage
         * genuinely spans daemons - which is the only shape the host is
         * asked about */
        PMIX_APP_CONSTRUCT(&app);
        app.cmd = strdup(argv[0]);
        app.maxprocs = 1;
        PMIx_Argv_append_nosize(&app.argv, argv[0]);
        if (disconnect) {
            PMIx_Argv_append_nosize(&app.argv, "--disconnect");
        }
        if (doabort) {
            PMIx_Argv_append_nosize(&app.argv, "--abort");
        }
        /* the child says where it landed, so a run that never left this node
         * cannot pass as one that did */
        if (NULL != child_host) {
            PMIX_INFO_CREATE(app.info, 1);
            PMIX_INFO_LOAD(&app.info[0], PMIX_HOST, child_host, PMIX_STRING);
            app.ninfo = 1;
        }

        PMIX_INFO_LOAD(&jobinfo[0], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);
        rc = PMIx_Spawn(jobinfo, 1, &app, 1, child);
        PMIX_INFO_DESTRUCT(&jobinfo[0]);
        PMIX_APP_DESTRUCT(&app);
        if (PMIX_SUCCESS != rc) {
            printf("CNCT %s %u DONE %d\n", role, myproc.rank, rc);
            fflush(stdout);
            PMIx_Finalize(NULL, 0);
            return 1;
        }
        printf("CNCT %s %u CHILD %s\n", role, myproc.rank, child);
        fflush(stdout);
        PMIX_LOAD_PROCID(&procs[0], myproc.nspace, PMIX_RANK_WILDCARD);
        PMIX_LOAD_PROCID(&procs[1], child, PMIX_RANK_WILDCARD);
    } else {
        PMIX_LOAD_PROCID(&procs[0], parent.nspace, PMIX_RANK_WILDCARD);
        PMIX_LOAD_PROCID(&procs[1], myproc.nspace, PMIX_RANK_WILDCARD);
    }

    rc = PMIx_Connect(procs, 2, NULL, 0);
    printf("CNCT %s %u CONNECT %s\n", role, myproc.rank, PMIx_Error_string(rc));
    fflush(stdout);
    if (PMIX_SUCCESS != rc) {
        printf("CNCT %s %u DONE %d\n", role, myproc.rank, rc);
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* Leaving the assemblage the sanctioned way is a COLLECTIVE over the set
     * that was connected, so both halves have to call it - a disconnect that
     * only the departing side issues never completes.  Whether it happens at
     * all is what the two runs of this program differ in. */
    if (disconnect) {
        rc = PMIx_Disconnect(procs, 2, NULL, 0);
        printf("CNCT %s %u DISCONNECT %s\n", role, myproc.rank, PMIx_Error_string(rc));
        fflush(stdout);
    }

    if (ischild && doabort) {
        /* fail, rather than leave - the assemblage is supposed to come down
         * with us */
        printf("CNCT %s %u ABORTING\n", role, myproc.rank);
        fflush(stdout);
        raise(SIGSEGV);
        sleep(5);
        return 1;
    }

    if (ischild) {
        printf("CNCT %s %u DONE 0\n", role, myproc.rank);
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 0;
    }

    /* the parent waits to be told - or not - that the child has gone */
    for (i = 0; i < wait_secs * 10 && 0 == nevents; i++) {
        usleep(100000);
    }
    printf("CNCT %s %u EVENTS %d\n", role, myproc.rank, nevents);
    printf("CNCT %s %u DONE 0\n", role, myproc.rank);
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return 0;
}
