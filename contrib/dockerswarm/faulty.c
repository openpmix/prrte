/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * faulty -- a PMIx client that fails on purpose, in each of the ways the
 * errmgr has a separate policy branch for.
 *
 *   faulty <mode> [seconds]
 *
 *       abort     the victim calls PMIx_Abort(7)      -> CALLED_ABORT
 *       exit      the victim exits with status 7      -> TERM_NON_ZERO
 *       signal    the victim raises SIGSEGV           -> ABORTED_BY_SIG
 *       nosync    the victim _exit(0)s without ever
 *                 calling PMIx_Finalize               -> TERM_WO_SYNC
 *       clean     nobody fails (the control case)
 *
 *       The victim is the HIGHEST rank, so it is not the rank the tool is
 *       colocated with and, mapped by node, not on the head node either.
 *       Every other rank sleeps [seconds] (default 10) and finalizes.
 *
 * Output lines, one per rank, all prefixed FLT so the harness can grep:
 *
 *   FLT <rank> HOST <hostname>
 *   FLT <rank> EVENT <status-string>     (an error event was delivered)
 *   FLT <rank> SURVIVED                  (reached the end of its sleep)
 *   FLT <rank> DONE <rc>
 *
 * Why this exists, and why it cannot be a unit test:
 *
 * Every branch above is a decision made in TWO processes that only exist in
 * a live DVM.  The prted that owns the failing proc classifies it and reports
 * it to the HNP (errmgr/prted); the HNP decides whether the job dies, whether
 * the DVM dies with it, and whether the survivors are told (errmgr/dvm).
 * Those are different components with opposite policies, and the second half
 * -- "notify the survivors and keep going" for a recoverable job -- has no
 * meaning at all in a process that is both.
 *
 * The event handler is what makes the recoverable case observable: with
 * --rtos recoverable,notifyerrors the HNP xcasts a PMIx event naming the
 * dead peer to every surviving rank instead of killing the job, and an
 * FLT ... EVENT line is a survivor having actually received it.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

static pmix_proc_t myproc;
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

    ++nevents;
    printf("FLT %u EVENT %s FROM %s:%u\n", myproc.rank, PMIx_Error_string(status),
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
    pmix_proc_t wildcard;
    uint32_t nprocs = 1;
    const char *mode = (1 < argc) ? argv[1] : "clean";
    int secs = (2 < argc) ? atoi(argv[2]) : 10;
    char host[256];
    int i;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "FLT init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (0 != gethostname(host, sizeof(host))) {
        strcpy(host, "unknown");
    }
    printf("FLT %u HOST %s\n", myproc.rank, host);
    fflush(stdout);

    /* how many of us are there?  the victim is the last rank */
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        nprocs = val->data.uint32;
        PMIX_VALUE_RELEASE(val);
    }

    /* catch whatever the runtime sends us about our peers */
    PMIx_Register_event_handler(NULL, 0, NULL, 0, evhandler, reg_cb, NULL);
    while (!handler_done) {
        usleep(10000);
    }

    /* line the ranks up so the victim fails while the others are running */
    rc = PMIx_Fence(&wildcard, 1, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        printf("FLT %u DONE %d\n", myproc.rank, rc);
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    if (myproc.rank == nprocs - 1) {
        sleep(1);
        if (0 == strcmp(mode, "abort")) {
            printf("FLT %u ABORTING\n", myproc.rank);
            fflush(stdout);
            PMIx_Abort(7, "faulty: deliberate abort", NULL, 0);
            /* PMIx_Abort returns; the runtime kills us */
            sleep(secs);
        } else if (0 == strcmp(mode, "exit")) {
            printf("FLT %u EXITING 7\n", myproc.rank);
            fflush(stdout);
            PMIx_Finalize(NULL, 0);
            return 7;
        } else if (0 == strcmp(mode, "signal")) {
            printf("FLT %u RAISING SIGSEGV\n", myproc.rank);
            fflush(stdout);
            raise(SIGSEGV);
            sleep(secs);
        } else if (0 == strcmp(mode, "nosync")) {
            printf("FLT %u EXITING WITHOUT FINALIZE\n", myproc.rank);
            fflush(stdout);
            _exit(0);
        }
    }

    /* the survivors: stay up long enough for the runtime to act, printing
     * nothing but whatever events reach us */
    for (i = 0; i < secs; i++) {
        sleep(1);
    }
    printf("FLT %u SURVIVED events %d\n", myproc.rank, nevents);
    fflush(stdout);

    rc = PMIx_Finalize(NULL, 0);
    printf("FLT %u DONE %d\n", myproc.rank, rc);
    fflush(stdout);
    return 0;
}
