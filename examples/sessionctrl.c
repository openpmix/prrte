/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * Drive PMIx_Session_control against a running DVM.
 *
 * Sessions are the scheduler's business, so a DVM that has a scheduler
 * attached forwards everything this tool asks to that scheduler. Run against
 * a DVM with no scheduler - the ordinary "prte --daemonize" case - the DVM
 * master is its own authority and executes the request itself, which is what
 * makes this usable as a test client.
 *
 *   sessionctrl instantiate <id> [--hosts LIST] [--time SPEC] [--sep]
 *                                [-- <cmd> [args...]]
 *   sessionctrl pause      <id>
 *   sessionctrl resume     <id>
 *   sessionctrl preempt    <id> [--nspace NS]
 *   sessionctrl restore    <id> [--nspace NS]
 *   sessionctrl signal     <id> <signum>
 *   sessionctrl extend     <id> [--hosts LIST] [--time SPEC]
 *   sessionctrl terminate  <id>
 *
 * <id> may be given as "all" to address every session under our control.
 */

#include <pmix.h>
#include <pmix_tool.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* tiny lock so the main thread can block on the async callback */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    volatile int active;
    pmix_status_t status;
} lock_t;

static void lock_init(lock_t *l)
{
    pthread_mutex_init(&l->mtx, NULL);
    pthread_cond_init(&l->cond, NULL);
    l->active = 1;
    l->status = PMIX_SUCCESS;
}
static void lock_wait(lock_t *l)
{
    pthread_mutex_lock(&l->mtx);
    while (l->active) {
        pthread_cond_wait(&l->cond, &l->mtx);
    }
    pthread_mutex_unlock(&l->mtx);
}
static void lock_wake(lock_t *l, pmix_status_t st)
{
    pthread_mutex_lock(&l->mtx);
    l->status = st;
    l->active = 0;
    pthread_cond_signal(&l->cond);
    pthread_mutex_unlock(&l->mtx);
}

static void ctrl_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    lock_t *l = (lock_t *) cbdata;
    size_t n;

    fprintf(stderr, "SESSION CONTROL returned %s\n", PMIx_Error_string(status));
    for (n = 0; n < ninfo; n++) {
        switch (info[n].value.type) {
        case PMIX_STRING:
            fprintf(stderr, "    %s = %s\n", info[n].key, info[n].value.data.string);
            break;
        case PMIX_UINT32:
            fprintf(stderr, "    %s = %u\n", info[n].key, info[n].value.data.uint32);
            break;
        default:
            fprintf(stderr, "    %s (type %s)\n", info[n].key,
                    PMIx_Data_type_string(info[n].value.type));
            break;
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    lock_wake(l, status);
}

static void usage(const char *me)
{
    fprintf(stderr,
            "usage: %s <operation> <sessionid|all> [options] [-- cmd [args...]]\n"
            "  operations: instantiate pause resume preempt restore signal extend terminate\n"
            "  options:    --hosts LIST   comma-delimited node names\n"
            "              --time SPEC    [[[[months:]days:]hours:]minutes:]seconds\n"
            "              --np N         procs to start (default 1)\n"
            "              --mapby POLICY mapping policy for those procs (e.g. node)\n"
            "              --nspace NS    limit preempt/restore to one job\n"
            "              --sep          session terminates independently of its parent\n",
            me);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_info_t *dirs = NULL;
    pmix_app_t *apps = NULL;
    pmix_info_t *jobinfo = NULL;
    pmix_data_array_t appdarray, jobdarray;
    size_t ndirs = 0, n = 0;
    uint32_t sessionid;
    const char *op, *hosts = NULL, *timespec = NULL, *nspace = NULL;
    const char *mapby = NULL;
    int signum = 0, i, cmdstart = 0, np = 1;
    bool sep = false, all = false;
    lock_t lock;
    char *end;

    if (3 > argc) {
        usage(argv[0]);
        return 1;
    }
    op = argv[1];
    if (0 == strcmp(argv[2], "all")) {
        all = true;
        sessionid = UINT32_MAX;
    } else {
        errno = 0;
        sessionid = (uint32_t) strtoul(argv[2], &end, 10);
        if (0 != errno || '\0' != *end) {
            fprintf(stderr, "%s: bad session id \"%s\"\n", argv[0], argv[2]);
            return 1;
        }
    }
    (void) all;

    for (i = 3; i < argc; i++) {
        if (0 == strcmp(argv[i], "--")) {
            cmdstart = i + 1;
            break;
        } else if (0 == strcmp(argv[i], "--hosts") && i + 1 < argc) {
            hosts = argv[++i];
        } else if (0 == strcmp(argv[i], "--time") && i + 1 < argc) {
            timespec = argv[++i];
        } else if (0 == strcmp(argv[i], "--np") && i + 1 < argc) {
            np = (int) strtol(argv[++i], &end, 10);
            if ('\0' != *end || 1 > np) {
                fprintf(stderr, "%s: bad proc count \"%s\"\n", argv[0], argv[i]);
                return 1;
            }
        } else if (0 == strcmp(argv[i], "--mapby") && i + 1 < argc) {
            mapby = argv[++i];
        } else if (0 == strcmp(argv[i], "--nspace") && i + 1 < argc) {
            nspace = argv[++i];
        } else if (0 == strcmp(argv[i], "--sep")) {
            sep = true;
        } else if (0 == strcmp(op, "signal") && 0 == signum) {
            signum = (int) strtol(argv[i], &end, 10);
            if ('\0' != *end) {
                fprintf(stderr, "%s: bad signal \"%s\"\n", argv[0], argv[i]);
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    /* count the directives we are about to build */
    ndirs = 2;                                          /* operation + ctrl id */
    if (NULL != hosts) {
        ndirs++;
    }
    if (NULL != timespec) {
        ndirs++;
    }
    if (NULL != nspace) {
        ndirs++;
    }
    if (sep) {
        ndirs++;
    }
    if (0 < cmdstart) {
        ndirs++;                                     /* PMIX_SESSION_APP */
        if (NULL != mapby) {
            ndirs++;                                 /* PMIX_SESSION_JOB */
        }
    }
    PMIX_INFO_CREATE(dirs, ndirs);

    PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_CTRL_ID, "sessionctrl-example", PMIX_STRING);

    if (0 == strcmp(op, "instantiate")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_INSTANTIATE, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "pause")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_PAUSE, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "resume")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_RESUME, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "preempt")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_PREEMPT, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "restore")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_RESTORE, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "extend")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_EXTEND, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "terminate")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_TERMINATE, NULL, PMIX_BOOL);
    } else if (0 == strcmp(op, "signal")) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_SIGNAL, &signum, PMIX_INT);
    } else {
        usage(argv[0]);
        PMIX_INFO_FREE(dirs, ndirs);
        return 1;
    }

    if (NULL != hosts) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_ALLOC_NODE_LIST, hosts, PMIX_STRING);
    }
    if (NULL != timespec) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_ALLOC_TIME, timespec, PMIX_STRING);
    }
    if (NULL != nspace) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_NSPACE, nspace, PMIX_STRING);
    }
    if (sep) {
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_SEP, NULL, PMIX_BOOL);
    }
    if (0 < cmdstart) {
        PMIX_APP_CREATE(apps, 1);
        apps[0].cmd = strdup(argv[cmdstart]);
        apps[0].maxprocs = np;
        for (i = cmdstart; i < argc; i++) {
            PMIx_Argv_append_nosize(&apps[0].argv, argv[i]);
        }
        appdarray.type = PMIX_APP;
        appdarray.size = 1;
        appdarray.array = apps;
        PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_APP, &appdarray, PMIX_DATA_ARRAY);

        /* job-level directives that go with those apps - PMIX_SESSION_JOB is
         * only meaningful alongside PMIX_SESSION_APP */
        if (NULL != mapby) {
            PMIX_INFO_CREATE(jobinfo, 1);
            PMIX_INFO_LOAD(&jobinfo[0], PMIX_MAPBY, mapby, PMIX_STRING);
            jobdarray.type = PMIX_INFO;
            jobdarray.size = 1;
            jobdarray.array = jobinfo;
            PMIX_INFO_LOAD(&dirs[n++], PMIX_SESSION_JOB, &jobdarray, PMIX_DATA_ARRAY);
        }
    }

    rc = PMIx_tool_init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_FREE(dirs, ndirs);
        return 1;
    }

    lock_init(&lock);
    rc = PMIx_Session_control(sessionid, dirs, n, ctrl_cb, &lock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Session_control failed: %s\n", PMIx_Error_string(rc));
        lock_wake(&lock, rc);
    } else {
        lock_wait(&lock);
    }

    PMIX_INFO_FREE(dirs, ndirs);
    if (NULL != apps) {
        PMIX_APP_FREE(apps, 1);
    }
    if (NULL != jobinfo) {
        PMIX_INFO_FREE(jobinfo, 1);
    }
    PMIx_tool_finalize();
    return (PMIX_SUCCESS == lock.status) ? 0 : 1;
}
