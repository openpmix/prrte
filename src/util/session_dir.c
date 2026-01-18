/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#ifdef HAVE_PWD_H
#    include <pwd.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif /* HAVE_SYS_PARAM_H */
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <errno.h>
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */
#ifdef HAVE_PWD_H
#    include <pwd.h>
#endif /* HAVE_PWD_H */

#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"

#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

#include "src/util/session_dir.h"

/*******************************
 * Local function Declarations
 *******************************/
static bool _check_file(const char *root, const char *path);

static bool setup_base_complete = false;

#define PRTE_PRINTF_FIX_STRING(a) ((NULL == a) ? "(null)" : a)

/****************************
 * Funcationality
 ****************************/
/*
 * Check and create the directory requested
 */
static int _create_dir(char *directory)
{
    mode_t my_mode = S_IRWXU; /* I'm looking for full rights */
    int ret;

    /* attempt to create it */
    ret = pmix_os_dirpath_create(directory, my_mode);
    if (PMIX_ERR_EXISTS == ret) {
        // existence is good enough
        ret = PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
    }
    ret = prte_pmix_convert_status(ret);
    return ret;
}

static int _setup_top_session_dir(void)
{
    int rc = PRTE_SUCCESS;
    /* get the effective uid */
    pid_t pid = getpid();

    /* construct the top_session_dir if we need */
    if (NULL == prte_process_info.top_session_dir) {
        if (NULL == prte_process_info.nodename ||
            NULL == prte_process_info.tmpdir_base) {
            /* we can't setup top session dir */
            rc = PRTE_ERR_BAD_PARAM;
            goto exit;
        }
        if (prte_process_info.shared_fs) {
            /* if it is a shared fs, then we need to include our nodename
             * to avoid collisions */
            if (0 > pmix_asprintf(&prte_process_info.top_session_dir, "%s/%s.%s.%lu",
                                  prte_process_info.tmpdir_base, prte_process_info.sessdir_prefix,
                                  prte_process_info.nodename,
                                  (unsigned long)pid)) {
                prte_process_info.top_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            /* if not a shared fs, then we can just use our pid as that will be unique */
            if (0 > pmix_asprintf(&prte_process_info.top_session_dir, "%s/%s.%lu",
                                  prte_process_info.tmpdir_base, prte_process_info.sessdir_prefix,
                                  (unsigned long)pid)) {
                prte_process_info.top_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        }
    }
    rc = _create_dir(prte_process_info.top_session_dir);

exit:
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        if (NULL != prte_process_info.top_session_dir) {
            free(prte_process_info.top_session_dir);
            prte_process_info.top_session_dir = NULL;
        }
    }
    return rc;
}

static int _setup_job_session_dir(prte_job_t *jdata)
{
    int rc = PRTE_SUCCESS;

    if (NULL == jdata->session_dir) {
        if (0 > pmix_asprintf(&jdata->session_dir, "%s/%s",
                              prte_process_info.top_session_dir,
                              PRTE_LOCAL_JOBID_PRINT(jdata->nspace))) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        rc = _create_dir(jdata->session_dir);
    }
    return rc;
}

static int _setup_proc_session_dir(prte_job_t *jdata,
                                   pmix_proc_t *p)
{
    int rc;
    char *tmp;

    if (0 > pmix_asprintf(&tmp, "%s/%s", jdata->session_dir,
                          PMIX_RANK_PRINT(p->rank))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    rc = _create_dir(tmp);
    free(tmp);
    return rc;
}

static int setup_base(void)
{
    int rc;

    // only do this once
    if (setup_base_complete) {
        return PRTE_SUCCESS;
    }
    setup_base_complete = true;

    /* Ensure that system info is set */
    prte_proc_info();

    /* BEFORE doing anything else, check to see if this prefix is
     * allowed by the system
     */
    if (NULL != prte_prohibited_session_dirs || NULL != prte_process_info.tmpdir_base) {
        char **list;
        int i, len;
        /* break the string into tokens - it should be
         * separated by ','
         */
        list = PMIX_ARGV_SPLIT_COMPAT(prte_prohibited_session_dirs, ',');
        len = PMIX_ARGV_COUNT_COMPAT(list);
        /* cycle through the list */
        for (i = 0; i < len; i++) {
            /* check if prefix matches */
            if (0 == strncmp(prte_process_info.tmpdir_base, list[i], strlen(list[i]))) {
                /* this is a prohibited location */
                pmix_show_help("help-prte-runtime.txt", "prte:session:dir:prohibited", true,
                               prte_process_info.tmpdir_base, prte_prohibited_session_dirs);
                PMIX_ARGV_FREE_COMPAT(list);
                return PRTE_ERR_FATAL;
            }
        }
        PMIX_ARGV_FREE_COMPAT(list); /* done with this */
    }

    rc = _setup_top_session_dir();

    return rc;
}

/*
 * Construct the session directory and create it if necessary
 */
int prte_session_dir(pmix_proc_t *proc)
{
    int rc = PRTE_SUCCESS;
    prte_job_t *jdata;

    /*
     * Get the session directory full name
     */
    if (PRTE_SUCCESS != (rc = setup_base())) {
        if (PRTE_ERR_FATAL == rc) {
            /* this indicates we should abort quietly */
            rc = PRTE_ERR_SILENT;
        }
        goto cleanup;
    }

    /* setup job and proc session directories */
    jdata = prte_get_job_data_object(proc->nspace);
    if (NULL == jdata) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    if (PRTE_SUCCESS != (rc = _setup_job_session_dir(jdata))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PMIX_RANK_IS_VALID(proc->rank)) {
        if (PRTE_SUCCESS != (rc = _setup_proc_session_dir(jdata, proc))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    if (prte_debug_flag) {
        pmix_output(0, "jobdir: %s", PRTE_PRINTF_FIX_STRING(jdata->session_dir));
        pmix_output(0, "top: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.top_session_dir));
        pmix_output(0, "tmp: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.tmpdir_base));
    }

cleanup:
    return rc;
}

void prte_job_session_dir_finalize(prte_job_t *jdata)
{
    if (prte_process_info.rm_session_dirs) {
        /* RM will clean them up for us */
        return;
    }

    /* special case - if a daemon is colocated with mpirun,
     * then we let mpirun do the rest to avoid a race
     * condition. this scenario always results in the rank=1
     * daemon colocated with mpirun */
    if (prte_ras_base.launch_orted_on_hn && PRTE_PROC_IS_DAEMON &&
        1 == PRTE_PROC_MY_NAME->rank) {
        return;
    }

    /* if this is the DVM job, then we destroy the top-level
     * session directory, but only if we are finalizing */
    if (NULL == jdata || PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, jdata->nspace)) {
        if (prte_finalizing) {
            if (NULL != prte_process_info.top_session_dir) {
                pmix_os_dirpath_destroy(prte_process_info.top_session_dir, true, _check_file);
                /* if the top session dir is now empty, remove it */
                rmdir(prte_process_info.top_session_dir);
                free(prte_process_info.top_session_dir);
                prte_process_info.top_session_dir = NULL;
            }
        }
        return;
    }

    if (NULL == jdata || NULL == jdata->session_dir) {
        return;
    }

    pmix_os_dirpath_destroy(jdata->session_dir, true, _check_file);
    /* if the job-level session dir is now empty, remove it */
    rmdir(jdata->session_dir);
    free(jdata->session_dir);
    jdata->session_dir = NULL;
    return;
}

static bool _check_file(const char *root, const char *path)
{
    struct stat st;
    char *fullpath;

    /*
     * Keep:
     *  - non-zero files starting with "output-"
     */
    if (0 == strncmp(path, "output-", strlen("output-"))) {
        memset(&st, 0, sizeof(struct stat));
        fullpath = pmix_os_path(false, root, path, NULL);
        if (0 != stat(fullpath, &st)) {
            pmix_output(0, "%s Syscall failure for stat: %s(%d)",
                        PMIX_NAME_PRINT(&pmix_globals.myid), strerror(errno), errno);
            return true;
        }
        free(fullpath);
        if (0 == st.st_size) {
            return true;
        }
        return false;
    }

    return true;
}
