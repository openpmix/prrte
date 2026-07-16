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

#include "prte_stdint.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/odls/base/base.h"

/* Runs in the parent. Renders, for --report-bindings, the binding we are
   about to apply to the child (the cpuset requested by the mapper).  It
   cannot read the child's actual applied binding - the child does not
   exist yet - but for a successful bind the two are identical, and this
   keeps the (allocating) rendering out of the async-signal-safe child. */
static void report_binding(prte_job_t *jobdat, int rank, hwloc_cpuset_t cpuset)
{
    char *tmp1;
    bool use_hwthread_cpus;
    bool physical;

    if (NULL == cpuset || hwloc_bitmap_iszero(cpuset)) {
        pmix_output(0, "Rank %d is not bound (or bound to all available processors)", rank);
        return;
    }
    /* check for type of cpu being used */
    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }
    physical = prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);
    tmp1 = prte_hwloc_base_cset2str(cpuset, use_hwthread_cpus, physical, prte_hwloc_topology);
    pmix_output(0, "Rank %d bound to %s", rank, tmp1);
    free(tmp1);
}

/* Report a fatal failure from inside the forked child and terminate it.
 *
 * This runs in the async-signal-safe window between fork() and execve(),
 * so it must not allocate, use stdio, or render a show_help message -
 * fork() leaves any lock another thread held frozen in the child, and
 * touching malloc/stdio/opendir can deadlock. We therefore write only a
 * fixed-size record carrying the failure code and errno; the parent
 * renders the human-readable diagnostic. We _exit() rather than exit() so
 * no atexit handlers run and no stdio buffers are flushed in the child. */
void prte_odls_base_child_fail(int write_fd, int exit_status, prte_odls_child_err_t which,
                               int errnum)
{
    prte_odls_pipe_err_msg_t msg;

    msg.fatal = true;
    msg.exit_status = exit_status;
    msg.which = (int) which;
    msg.errnum = errnum;

    /* best effort - if the write fails there is nothing safe we can do
     * here, and the parent will still see the pipe close */
    (void) pmix_fd_write(write_fd, sizeof(msg), &msg);

    _exit(exit_status);
}

/* Report a non-fatal condition from inside the forked child and return so
 * the child can continue on toward execve(). Same async-signal-safety
 * constraints as prte_odls_base_child_fail. */
void prte_odls_base_child_warn(int write_fd, prte_odls_child_err_t which, int errnum)
{
    prte_odls_pipe_err_msg_t msg;

    msg.fatal = false;
    msg.exit_status = 0; /* ignored */
    msg.which = (int) which;
    msg.errnum = errnum;

    /* best effort */
    (void) pmix_fd_write(write_fd, sizeof(msg), &msg);
}

/* Runs in the PARENT, before the fork. Does everything that requires
   allocation, parsing, or output - parsing the mapper's cpuset,
   classifying the binding, computing the memory-binding policy, precomputing
   the raw sched_setaffinity mask, and emitting --report-bindings and the
   "incorrectly bound" warning - and stashes the ready-to-apply result on
   the caddy. The forked child (prte_odls_base_set) then only issues the
   bind syscalls. */
void prte_odls_base_prepare_binding(prte_odls_spawn_caddy_t *cd)
{
    prte_job_t *jobdat = cd->jdata;
    prte_proc_t *child = cd->child;
    prte_app_context_t *context = cd->app;
    hwloc_cpuset_t cpuset;
    hwloc_obj_t root;
    bool report;

    cd->bind_cpuset = NULL;
    cd->bind_fatal = false;
    cd->do_membind = false;

    pmix_output_verbose(2, prte_odls_base_framework.framework_output,
                        "%s hwloc:prepare on child %s cpuset %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == child) ? "NULL" : PRTE_NAME_PRINT(&child->name),
                        (NULL == child || NULL == child->cpuset) ? "NULL" : child->cpuset);

    if (NULL == jobdat || NULL == child) {
        /* nothing for us to do */
        return;
    }

    report = prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_BINDINGS, NULL, PMIX_BOOL);

    /* Compute process affinity, if given */
    if (NULL == child->cpuset || 0 == strlen(child->cpuset)) {
        /* no explicit binding was computed for this proc */
        if (NULL != prte_daemon_cores) {
            /* the daemon is bound, so we must "free" this proc to all
               available processors */
            root = hwloc_get_root_obj(prte_hwloc_topology);
            if (NULL == root->userdata) {
                pmix_show_help("help-prte-odls-default.txt", "incorrectly bound", true,
                               prte_process_info.nodename, context->app);
            }
            cd->bind_cpuset = hwloc_bitmap_dup(
                hwloc_topology_get_allowed_cpuset(prte_hwloc_topology));
            if (report) {
                report_binding(jobdat, child->name.rank, cd->bind_cpuset);
            }
        } else if (report) {
            pmix_output(0, "Rank %d is not bound (or bound to all available processors)",
                        child->name.rank);
        }
        /* NB: no memory binding is applied on this path, matching the
           historical behavior */
    } else {
        /* an explicit cpuset was produced by the mapper - convert the list */
        cpuset = hwloc_bitmap_alloc();
        if (NULL == cpuset || 0 != hwloc_bitmap_list_sscanf(cpuset, child->cpuset)) {
            /* out-of-memory or a malformed string (the latter should never
               happen - the mapper produced it). If binding was explicitly
               required, flag it fatal so the child reports it up the pipe;
               otherwise warn now (safe here in the parent) and continue
               unbound. */
            if (NULL != cpuset) {
                hwloc_bitmap_free(cpuset);
            }
            if (PRTE_BINDING_REQUIRED(jobdat->map->binding)
                && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                cd->bind_fatal = true;
            } else if (PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                pmix_show_help("help-prte-odls-default.txt", "not bound", true,
                               prte_process_info.nodename, context->app,
                               "unable to apply the requested binding");
            }
            return;
        }
        cd->bind_cpuset = cpuset;
        if (report) {
            report_binding(jobdat, child->name.rank, cd->bind_cpuset);
        }
        /* precompute the memory-binding policy (mirrors
           prte_hwloc_base_set_process_membind_policy) so the child need not
           read any MCA state */
        cd->do_membind = true;
        switch (prte_hwloc_base_map) {
        case PRTE_HWLOC_BASE_MAP_LOCAL_ONLY:
            cd->membind_policy = HWLOC_MEMBIND_BIND;
            cd->membind_flags = HWLOC_MEMBIND_STRICT;
            break;
        case PRTE_HWLOC_BASE_MAP_NONE:
        default:
            cd->membind_policy = HWLOC_MEMBIND_DEFAULT;
            cd->membind_flags = 0;
            break;
        }
    }

#if PRTE_HAVE_SCHED_SETAFFINITY
    /* precompute the raw affinity mask so the child can bind with a bare
       sched_setaffinity() syscall instead of calling into hwloc (which
       allocates) in the async-signal-safe window before execve() */
    if (NULL != cd->bind_cpuset) {
        int last = hwloc_bitmap_last(cd->bind_cpuset);
        unsigned ncpus = (0 > last) ? 1 : (unsigned) (last + 1);
        cd->bind_mask = CPU_ALLOC(ncpus);
        if (NULL != cd->bind_mask) {
            unsigned id;
            cd->bind_masksize = CPU_ALLOC_SIZE(ncpus);
            CPU_ZERO_S(cd->bind_masksize, cd->bind_mask);
            hwloc_bitmap_foreach_begin(id, cd->bind_cpuset) {
                CPU_SET_S(id, cd->bind_masksize, cd->bind_mask);
            }
            hwloc_bitmap_foreach_end();
        }
    }
#endif
}

/* Runs in the forked CHILD, in the async-signal-safe window before
   execve(). It only issues the bind syscalls prepared by the parent and
   reports any failure up the pipe (prte_odls_base_child_fail/_warn); it
   performs no allocation, parsing, or rendering. */
void prte_odls_base_set(prte_odls_spawn_caddy_t *cd, int write_fd)
{
    prte_job_t *jobdat = cd->jdata;
    int rc;

    /* A required binding could not even be prepared (out-of-memory or a
       malformed cpuset in the parent) - report it now and exit. */
    if (cd->bind_fatal) {
        prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND, 0);
        /* does not return */
    }

    /* nothing to bind */
    if (NULL == cd->bind_cpuset) {
        return;
    }

    /* Apply CPU affinity. On systems with sched_setaffinity() we issue a
       bare syscall using the mask the parent precomputed - that is
       async-signal-safe. Elsewhere (e.g., macOS) we fall back to hwloc,
       which is not strictly async-signal-safe but is the best available. */
#if PRTE_HAVE_SCHED_SETAFFINITY
    if (NULL == cd->bind_mask) {
        errno = ENOMEM;
        rc = -1;
    } else {
        rc = sched_setaffinity(0, cd->bind_masksize, cd->bind_mask);
    }
#else
    rc = hwloc_set_cpubind(prte_hwloc_topology, cd->bind_cpuset, 0);
#endif
    /* if we got an error and this wasn't a default binding policy, report it.
       We send only the errno up the pipe; the parent renders the message. */
    if (0 != rc && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
        if (PRTE_BINDING_REQUIRED(jobdat->map->binding)) {
            /* required binding failed: fatal (child_fail exits) */
            prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND, errno);
            /* does not return */
        }
        prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_NOT_BOUND, errno);
        return;
    }

    /* Apply the memory-binding policy, if one was requested. hwloc is used
       on all platforms here; converting this to a bare syscall would mean
       reproducing hwloc's NUMA nodeset handling and is left for later. */
    if (cd->do_membind) {
        rc = hwloc_set_membind(prte_hwloc_topology, cd->bind_cpuset, cd->membind_policy,
                               cd->membind_flags);
        if (0 != rc && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            /* hwloc failing with ENOSYS when no explicit policy was set is
               not really an error (mirrors the historical membind path) */
            if (ENOSYS == errno && PRTE_HWLOC_BASE_MAP_NONE == prte_hwloc_base_map) {
                return;
            }
            if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND_MEM, errno);
                /* does not return */
            }
            prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_MEM_NOT_BOUND, errno);
            return;
        }
    }
}
