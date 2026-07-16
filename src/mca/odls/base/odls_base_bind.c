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

static void report_binding(prte_job_t *jobdat, int rank)
{
    char *tmp1;
    hwloc_cpuset_t mycpus;
    bool use_hwthread_cpus;
    bool physical;

    /* check for type of cpu being used */
    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthread_cpus = true;
    } else {
        use_hwthread_cpus = false;
    }
    /* get the cpus we are bound to */
    mycpus = hwloc_bitmap_alloc();
    if (hwloc_get_cpubind(prte_hwloc_topology, mycpus, HWLOC_CPUBIND_PROCESS) < 0) {
        pmix_output(0, "Rank %d is not bound", rank);
    } else {
        physical = prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_PHYSICAL_CPUS, NULL, PMIX_BOOL);
        tmp1 = prte_hwloc_base_cset2str(mycpus, use_hwthread_cpus, physical, prte_hwloc_topology);
        pmix_output(0, "Rank %d bound to %s", rank, tmp1);
        free(tmp1);
    }
    hwloc_bitmap_free(mycpus);
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

void prte_odls_base_set(prte_odls_spawn_caddy_t *cd, int write_fd)
{
    prte_job_t *jobdat = cd->jdata;
    prte_proc_t *child = cd->child;
    hwloc_cpuset_t cpuset;
    hwloc_obj_t root;
    int rc = PRTE_ERROR;

    pmix_output_verbose(2, prte_odls_base_framework.framework_output,
                        "%s hwloc:set on child %s cpuset %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == child) ? "NULL" : PRTE_NAME_PRINT(&child->name),
                        (NULL == child->cpuset) ? "NULL" : child->cpuset);

    if (NULL == jobdat || NULL == child) {
        /* nothing for us to do */
        pmix_output_verbose(2, prte_odls_base_framework.framework_output,
                            "%s hwloc:set jobdat %s child %s - nothing to do",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (NULL == jobdat) ? "NULL" : PRTE_JOBID_PRINT(jobdat->nspace),
                            (NULL == child) ? "NULL" : PRTE_NAME_PRINT(&child->name));
        return;
    }

    /* Set process affinity, if given */
    if (NULL == child->cpuset || 0 == strlen(child->cpuset)) {
        /* if the daemon is bound, then we need to "free" this proc */
        if (NULL != prte_daemon_cores) {
            root = hwloc_get_root_obj(prte_hwloc_topology);
            if (NULL == root->userdata) {
                prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_INCORRECT, 0);
            }
            /* bind this proc to all available processors */
            cpuset = (hwloc_cpuset_t)hwloc_topology_get_allowed_cpuset(prte_hwloc_topology);
            rc = hwloc_set_cpubind(prte_hwloc_topology, cpuset, 0);
            /* if we got an error and this wasn't a default binding policy, then report it.
             * We are between fork() and execve() here, so we cannot render the message -
             * we send the errno up the pipe and let the parent do the rendering. */
            if (rc < 0 && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                if (PRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                    /* If binding is required, send an error up the pipe (which exits
                       -- it doesn't return). */
                    prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND, errno);
                } else {
                    prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_NOT_BOUND, errno);
                    return;
                }
            }
            if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_BINDINGS, NULL,
                                   PMIX_BOOL)) {
                if (0 == rc) {
                    report_binding(jobdat, child->name.rank);
                } else {
                    pmix_output(0,
                                "Rank %d is not bound (or bound to all available processors)",
                                child->name.rank);
                }
            }
        } else if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_BINDINGS, NULL,
                                      PMIX_BOOL)) {
            pmix_output(0, "Rank %d is not bound (or bound to all available processors)",
                        child->name.rank);
        }
    } else {
        /* convert the list to a cpuset */
        cpuset = hwloc_bitmap_alloc();
        if (0 != (rc = hwloc_bitmap_list_sscanf(cpuset, child->cpuset))) {
            if (PRTE_BINDING_REQUIRED(jobdat->map->binding) &&
                PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                /* If binding is required and a binding directive was explicitly
                 * given (i.e., we are not binding due to a default policy),
                 * send an error up the pipe (which exits -- it doesn't return).
                 * This is a parse failure, not a syscall failure, so there is
                 * no meaningful errno to report. */
                hwloc_bitmap_free(cpuset);
                prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND, 0);
            } else {
                prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_NOT_BOUND, 0);
                hwloc_bitmap_free(cpuset);
                return;
            }
        }
        /* bind as specified */
        rc = hwloc_set_cpubind(prte_hwloc_topology, cpuset, 0);
        hwloc_bitmap_free(cpuset);
        /* if we got an error and this wasn't a default binding policy, then report it */
        if (rc < 0 && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            if (PRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND, errno);
            } else {
                prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_NOT_BOUND, errno);
                return;
            }
        }

        if (0 == rc
            && prte_get_attribute(&jobdat->attributes, PRTE_JOB_REPORT_BINDINGS, NULL, PMIX_BOOL)) {
            report_binding(jobdat, child->name.rank);
        }

        /* set memory affinity policy - if we get an error, don't report
         * anything unless the user actually specified the binding policy
         */
        rc = prte_hwloc_base_set_process_membind_policy();
        if (PRTE_SUCCESS != rc && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                prte_odls_base_child_fail(write_fd, 1, PRTE_ODLS_CHILD_ERR_BIND_MEM, errno);
            } else {
                prte_odls_base_child_warn(write_fd, PRTE_ODLS_CHILD_WARN_MEM_NOT_BOUND, errno);
                return;
            }
        }
    }
}
