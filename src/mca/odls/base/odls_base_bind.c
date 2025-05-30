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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
        tmp1 = prte_hwloc_base_cset2str(mycpus, use_hwthread_cpus, prte_hwloc_topology);
        pmix_output(0, "Rank %d bound to %s", rank, tmp1);
        free(tmp1);
    }
    hwloc_bitmap_free(mycpus);
}

static int write_help_msg(int fd, prte_odls_pipe_err_msg_t *msg, const char *file,
                          const char *topic, va_list ap)
{
    int ret;
    char *str;

    if (NULL == file || NULL == topic) {
        return PRTE_ERR_BAD_PARAM;
    }

    str = pmix_show_help_vstring(file, topic, true, ap);

    msg->file_str_len = (int) strlen(file);
    if (msg->file_str_len > PRTE_ODLS_MAX_FILE_LEN) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    msg->topic_str_len = (int) strlen(topic);
    if (msg->topic_str_len > PRTE_ODLS_MAX_TOPIC_LEN) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        return PRTE_ERR_BAD_PARAM;
    }
    msg->msg_str_len = (int) strlen(str);

    /* Only keep writing if each write() succeeds */
    if (PRTE_SUCCESS != (ret = pmix_fd_write(fd, sizeof(*msg), msg))) {
        goto out;
    }
    if (msg->file_str_len > 0
        && PRTE_SUCCESS != (ret = pmix_fd_write(fd, msg->file_str_len, file))) {
        goto out;
    }
    if (msg->topic_str_len > 0
        && PRTE_SUCCESS != (ret = pmix_fd_write(fd, msg->topic_str_len, topic))) {
        goto out;
    }
    if (msg->msg_str_len > 0 && PRTE_SUCCESS != (ret = pmix_fd_write(fd, msg->msg_str_len, str))) {
        goto out;
    }

out:
    free(str);
    return ret;
}

static int send_warn_show_help(int fd, const char *file, const char *topic, ...)
{
    int ret;
    va_list ap;
    prte_odls_pipe_err_msg_t msg;

    msg.fatal = false;
    msg.exit_status = 0; /* ignored */

    /* Send it */
    va_start(ap, topic);
    ret = write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    return ret;
}

static void send_error_show_help(int fd, int exit_status, const char *file,
                                 const char *topic, ...)
{
    va_list ap;
    prte_odls_pipe_err_msg_t msg;

    msg.fatal = true;
    msg.exit_status = exit_status;

    /* Send it */
    va_start(ap, topic);
    write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    exit(exit_status);
}

void prte_odls_base_set(prte_odls_spawn_caddy_t *cd, int write_fd)
{
    prte_job_t *jobdat = cd->jdata;
    prte_proc_t *child = cd->child;
    prte_app_context_t *context = cd->app;
    hwloc_cpuset_t cpuset;
    hwloc_obj_t root;
    int rc = PRTE_ERROR;
    char *msg;

    pmix_output_verbose(2, prte_odls_base_framework.framework_output,
                        "%s hwloc:set on child %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == child) ? "NULL" : PRTE_NAME_PRINT(&child->name));

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
                send_warn_show_help(write_fd, "help-prte-odls-default.txt",
                                    "incorrectly bound", prte_process_info.nodename,
                                    context->app, __FILE__, __LINE__);
            }
            /* bind this proc to all available processors */
            cpuset = (hwloc_cpuset_t)hwloc_topology_get_allowed_cpuset(prte_hwloc_topology);
            rc = hwloc_set_cpubind(prte_hwloc_topology, cpuset, 0);
            /* if we got an error and this wasn't a default binding policy, then report it */
            if (rc < 0 && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                if (errno == ENOSYS) {
                    msg = "hwloc indicates cpu binding not supported";
                } else if (errno == EXDEV) {
                    msg = "hwloc indicates cpu binding cannot be enforced";
                } else {
                    char *tmp;
                    (void) hwloc_bitmap_list_asprintf(&tmp, cpuset);
                    pmix_asprintf(&msg, "hwloc_set_cpubind returned \"%s\" for bitmap \"%s\"",
                                  prte_strerror(rc), tmp);
                    free(tmp);
                }
                if (PRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                    /* If binding is required, send an error up the pipe (which exits
                       -- it doesn't return). */
                    send_error_show_help(write_fd, 1, "help-prte-odls-default.txt",
                                         "binding generic error",
                                         prte_process_info.nodename, context->app,
                                         msg, __FILE__, __LINE__);
                } else {
                    send_warn_show_help(write_fd, "help-prte-odls-default.txt",
                                        "not bound", prte_process_info.nodename,
                                        context->app, msg, __FILE__, __LINE__);
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
            /* See comment above about "This may be a small memory leak" */
            pmix_asprintf(&msg, "hwloc_bitmap_sscanf returned \"%s\" for the string \"%s\"",
                          prte_strerror(rc), child->cpuset);
            if (NULL == msg) {
                msg = "failed to convert bitmap list to hwloc bitmap";
            }
            if (PRTE_BINDING_REQUIRED(jobdat->map->binding)
                && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                /* If binding is required and a binding directive was explicitly
                 * given (i.e., we are not binding due to a default policy),
                 * send an error up the pipe (which exits -- it doesn't return).
                 */
                send_error_show_help(write_fd, 1, "help-prte-odls-default.txt",
                                     "binding generic error",
                                     prte_process_info.nodename, context->app, msg,
                                     __FILE__, __LINE__);
            } else {
                send_warn_show_help(write_fd, "help-prte-odls-default.txt",
                                    "not bound", prte_process_info.nodename,
                                    context->app, msg, __FILE__, __LINE__);
                hwloc_bitmap_free(cpuset);
                return;
            }
        }
        /* bind as specified */
        rc = hwloc_set_cpubind(prte_hwloc_topology, cpuset, 0);
        hwloc_bitmap_free(cpuset);
        /* if we got an error and this wasn't a default binding policy, then report it */
        if (rc < 0 && PRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            char *tmp = NULL;
            if (errno == ENOSYS) {
                msg = "hwloc indicates cpu binding not supported";
            } else if (errno == EXDEV) {
                msg = "hwloc indicates cpu binding cannot be enforced";
            } else {
                pmix_asprintf(&msg, "hwloc_set_cpubind returned \"%s\" for bitmap \"%s\"",
                              prte_strerror(rc), child->cpuset);
            }
            if (PRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                send_error_show_help(write_fd, 1, "help-prte-odls-default.txt",
                                     "binding generic error",
                                     prte_process_info.nodename, context->app, msg,
                                     __FILE__, __LINE__);
            } else {
                send_warn_show_help(write_fd, "help-prte-odls-default.txt",
                                    "not bound", prte_process_info.nodename,
                                    context->app, msg, __FILE__, __LINE__);
                if (NULL != tmp) {
                    free(tmp);
                    free(msg);
                }
                return;
            }
            if (NULL != tmp) {
                free(tmp);
                free(msg);
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
            if (errno == ENOSYS) {
                msg = "hwloc indicates memory binding not supported";
            } else if (errno == EXDEV) {
                msg = "hwloc indicates memory binding cannot be enforced";
            } else {
                msg = "failed to bind memory";
            }
            if (PRTE_HWLOC_BASE_MBFA_ERROR == prte_hwloc_base_mbfa) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                send_error_show_help(write_fd, 1, "help-prte-odls-default.txt",
                                     "memory binding error",
                                     prte_process_info.nodename, context->app, msg,
                                     __FILE__, __LINE__);
            } else {
                send_warn_show_help(write_fd, "help-prte-odls-default.txt",
                                    "memory not bound", prte_process_info.nodename,
                                    context->app, msg, __FILE__, __LINE__);
                return;
            }
        }
    }
}
