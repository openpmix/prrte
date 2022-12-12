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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
static int prte_create_dir(char *directory);

static bool prte_dir_check_file(const char *root, const char *path);

#define PRTE_PRINTF_FIX_STRING(a) ((NULL == a) ? "(null)" : a)

/****************************
 * Funcationality
 ****************************/
/*
 * Check and create the directory requested
 */
static int prte_create_dir(char *directory)
{
    mode_t my_mode = S_IRWXU; /* I'm looking for full rights */
    int ret;

    /* Sanity check before creating the directory with the proper mode,
     * Make sure it doesn't exist already */
    if (PMIX_ERR_NOT_FOUND != (ret = pmix_os_dirpath_access(directory, my_mode))) {
        /* Failure because pmix_os_dirpath_access() indicated that either:
         * - The directory exists and we can access it (no need to create it again),
         *    return PRTE_SUCCESS, or
         * - don't have access rights, return PRTE_ERROR
         */
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
        }
        ret = prte_pmix_convert_status(ret);
        return (ret);
    }

    /* Get here if the directory doesn't exist, so create it */
    if (PMIX_SUCCESS != (ret = pmix_os_dirpath_create(directory, my_mode))) {
        PMIX_ERROR_LOG(ret);
    }
    ret = prte_pmix_convert_status(ret);
    return ret;
}

static int _setup_tmpdir_base(void)
{
    int rc = PRTE_SUCCESS;

    /* make sure that we have tmpdir_base set
     * if we need it
     */
    if (NULL == prte_process_info.tmpdir_base) {
        prte_process_info.tmpdir_base = strdup(pmix_tmp_directory());
        if (NULL == prte_process_info.tmpdir_base) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
    }
exit:
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

int prte_setup_top_session_dir(void)
{
    int rc = PRTE_SUCCESS;
    /* get the effective uid */
    uid_t uid = geteuid();
    pid_t pid = getpid();

    /* construct the top_session_dir if we need */
    if (NULL == prte_process_info.top_session_dir) {
        if (PRTE_SUCCESS != (rc = _setup_tmpdir_base())) {
            return rc;
        }
        if (NULL == prte_process_info.nodename || NULL == prte_process_info.tmpdir_base) {
            /* we can't setup top session dir */
            rc = PRTE_ERR_BAD_PARAM;
            goto exit;
        }
        if (prte_add_pid_to_session_dirname) {
            if (0 > pmix_asprintf(&prte_process_info.top_session_dir, "%s/prte.%s.%lu.%lu",
                                  prte_process_info.tmpdir_base, prte_process_info.nodename,
                                  (unsigned long)pid, (unsigned long) uid)) {
                prte_process_info.top_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            if (0 > pmix_asprintf(&prte_process_info.top_session_dir, "%s/prte.%s.%lu",
                                  prte_process_info.tmpdir_base, prte_process_info.nodename,
                                  (unsigned long) uid)) {
                prte_process_info.top_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        }
    }
exit:
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int _setup_jobfam_session_dir(pmix_proc_t *proc)
{
    int rc = PRTE_SUCCESS;
    PRTE_HIDE_UNUSED_PARAMS(proc);

    /* construct the top_session_dir if we need */
    if (NULL == prte_process_info.jobfam_session_dir) {
        if (PRTE_SUCCESS != (rc = prte_setup_top_session_dir())) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        if (0 > pmix_asprintf(&prte_process_info.jobfam_session_dir, "%s/dvm.%lu",
                              prte_process_info.top_session_dir,
                              (unsigned long) prte_process_info.pid)) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int _setup_job_session_dir(pmix_proc_t *proc)
{
    int rc = PRTE_SUCCESS;

    /* construct the top_session_dir if we need */
    if (NULL == prte_process_info.job_session_dir) {
        if (PRTE_SUCCESS != (rc = _setup_jobfam_session_dir(proc))) {
            return rc;
        }
        if (!PMIX_NSPACE_INVALID(proc->nspace)) {
            if (0 > pmix_asprintf(&prte_process_info.job_session_dir, "%s/%s",
                                  prte_process_info.jobfam_session_dir,
                                  PRTE_LOCAL_JOBID_PRINT(proc->nspace))) {
                prte_process_info.job_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            prte_process_info.job_session_dir = NULL;
        }
    }

exit:
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int _setup_proc_session_dir(pmix_proc_t *proc)
{
    int rc = PRTE_SUCCESS;

    /* construct the top_session_dir if we need */
    if (NULL == prte_process_info.proc_session_dir) {
        if (PRTE_SUCCESS != (rc = _setup_job_session_dir(proc))) {
            return rc;
        }
        if (PMIX_RANK_INVALID != proc->rank) {
            if (0 > pmix_asprintf(&prte_process_info.proc_session_dir, "%s/%s",
                                  prte_process_info.job_session_dir, PRTE_VPID_PRINT(proc->rank))) {
                prte_process_info.proc_session_dir = NULL;
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            prte_process_info.proc_session_dir = NULL;
        }
    }

exit:
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    return rc;
}

int prte_session_setup_base(pmix_proc_t *proc)
{
    int rc;

    /* Ensure that system info is set */
    prte_proc_info();

    /* setup job and proc session directories */
    if (PRTE_SUCCESS != (rc = _setup_job_session_dir(proc))) {
        return rc;
    }

    if (PRTE_SUCCESS != (rc = _setup_proc_session_dir(proc))) {
        return rc;
    }

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
    return PRTE_SUCCESS;
}

/*
 * Construct the session directory and create it if necessary
 */
int prte_session_dir(bool create, pmix_proc_t *proc)
{
    int rc = PRTE_SUCCESS;

    /*
     * Get the session directory full name
     */
    if (PRTE_SUCCESS != (rc = prte_session_setup_base(proc))) {
        if (PRTE_ERR_FATAL == rc) {
            /* this indicates we should abort quietly */
            rc = PRTE_ERR_SILENT;
        }
        goto cleanup;
    }

    /*
     * Now that we have the full path, go ahead and create it if necessary
     */
    if (create) {
        if (PRTE_SUCCESS != (rc = prte_create_dir(prte_process_info.proc_session_dir))) {
            PRTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    if (prte_debug_flag) {
        pmix_output(0, "procdir: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.proc_session_dir));
        pmix_output(0, "jobdir: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.job_session_dir));
        pmix_output(0, "job: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.jobfam_session_dir));
        pmix_output(0, "top: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.top_session_dir));
        pmix_output(0, "tmp: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.tmpdir_base));
    }

cleanup:
    return rc;
}

/*
 * A job has aborted - so force cleanup of the session directory
 */
int prte_session_dir_cleanup(pmix_nspace_t jobid)
{
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(jobid);

    /* special case - if a daemon is colocated with mpirun,
     * then we let mpirun do the rest to avoid a race
     * condition. this scenario always results in the rank=1
     * daemon colocated with mpirun */
    if (prte_ras_base.launch_orted_on_hn && PRTE_PROC_IS_DAEMON && 1 == PRTE_PROC_MY_NAME->rank) {
        return PRTE_SUCCESS;
    }

    if (prte_process_info.rm_session_dirs) {
        /* RM will clean them up for us */
        return PRTE_SUCCESS;
    }

    if (NULL == prte_process_info.jobfam_session_dir
        || NULL == prte_process_info.proc_session_dir) {
        /* this should never happen - it means we are calling
         * cleanup *before* properly setting up the session
         * dir system. This leaves open the possibility of
         * accidentally removing directories we shouldn't
         * touch
         */
        return PRTE_ERR_NOT_INITIALIZED;
    }

    /* recursively blow the whole session away for our job family,
     * saving only output files
     */
    pmix_os_dirpath_destroy(prte_process_info.jobfam_session_dir, true, prte_dir_check_file);

    if (pmix_os_dirpath_is_empty(prte_process_info.jobfam_session_dir)) {
        if (prte_debug_flag) {
            pmix_output(0, "sess_dir_cleanup: found jobfam session dir empty - deleting");
        }
        rmdir(prte_process_info.jobfam_session_dir);
    } else {
        if (prte_debug_flag) {
            ret = pmix_os_dirpath_access(prte_process_info.job_session_dir, 0);
            if (PMIX_ERR_NOT_FOUND == ret) {
                pmix_output(0, "sess_dir_cleanup: job session dir does not exist");
            } else {
                pmix_output(0, "sess_dir_cleanup: job session dir not empty - leaving");
            }
        }
    }

    if (NULL != prte_process_info.top_session_dir) {
        if (pmix_os_dirpath_is_empty(prte_process_info.top_session_dir)) {
            if (prte_debug_flag) {
                pmix_output(0, "sess_dir_cleanup: found top session dir empty - deleting");
            }
            rmdir(prte_process_info.top_session_dir);
        } else {
            if (prte_debug_flag) {
                ret = pmix_os_dirpath_access(prte_process_info.top_session_dir, 0);
                if (PMIX_ERR_NOT_FOUND == ret) {
                    pmix_output(0, "sess_dir_cleanup: top session dir does not exist");
                } else {
                    pmix_output(0, "sess_dir_cleanup: top session dir not empty - leaving");
                }
            }
        }
    }

    /* now attempt to eliminate the top level directory itself - this
     * will fail if anything is present, but ensures we cleanup if
     * we are the last one out
     */
    if (NULL != prte_process_info.top_session_dir) {
        pmix_os_dirpath_destroy(prte_process_info.top_session_dir, false, prte_dir_check_file);
    }

    return PRTE_SUCCESS;
}

int prte_session_dir_finalize(pmix_proc_t *proc)
{
    int ret;

    if (prte_process_info.rm_session_dirs) {
        /* RM will clean them up for us */
        return PRTE_SUCCESS;
    }

    if (NULL == prte_process_info.job_session_dir ||
        NULL == prte_process_info.proc_session_dir) {
        /* this should never happen - it means we are calling
         * cleanup *before* properly setting up the session
         * dir system. This leaves open the possibility of
         * accidentally removing directories we shouldn't
         * touch
         */
        return PRTE_ERR_NOT_INITIALIZED;
    }

    pmix_os_dirpath_destroy(prte_process_info.proc_session_dir, false, prte_dir_check_file);

    if (pmix_os_dirpath_is_empty(prte_process_info.proc_session_dir)) {
        if (prte_debug_flag) {
            pmix_output(0, "sess_dir_finalize: found proc session dir empty - deleting");
        }
        rmdir(prte_process_info.proc_session_dir);
    } else {
        if (prte_debug_flag) {
            ret = pmix_os_dirpath_access(prte_process_info.proc_session_dir, 0);
            if (PMIX_ERR_NOT_FOUND == ret) {
                pmix_output(0, "sess_dir_finalize: proc session dir does not exist");
            } else {
                pmix_output(0, "sess_dir_finalize: proc session dir not empty - leaving");
            }
        }
    }

    /* special case - if a daemon is colocated with mpirun,
     * then we let mpirun do the rest to avoid a race
     * condition. this scenario always results in the rank=1
     * daemon colocated with mpirun */
    if (prte_ras_base.launch_orted_on_hn && PRTE_PROC_IS_DAEMON && 1 == PRTE_PROC_MY_NAME->rank) {
        return PRTE_SUCCESS;
    }

    pmix_os_dirpath_destroy(prte_process_info.job_session_dir, false, prte_dir_check_file);

    /* only remove the jobfam session dir if we are the
     * local daemon and we are finalizing our own session dir */
    if ((PRTE_PROC_IS_MASTER || PRTE_PROC_IS_DAEMON) && (PRTE_PROC_MY_NAME == proc)) {
        pmix_os_dirpath_destroy(prte_process_info.jobfam_session_dir, false, prte_dir_check_file);
    }

    if (NULL != prte_process_info.top_session_dir) {
        pmix_os_dirpath_destroy(prte_process_info.top_session_dir, false, prte_dir_check_file);
    }

    if (pmix_os_dirpath_is_empty(prte_process_info.job_session_dir)) {
        if (prte_debug_flag) {
            pmix_output(0, "sess_dir_finalize: found job session dir empty - deleting");
        }
        rmdir(prte_process_info.job_session_dir);
    } else {
        if (prte_debug_flag) {
            ret = pmix_os_dirpath_access(prte_process_info.job_session_dir, 0);
            if (PMIX_ERR_NOT_FOUND == ret) {
                pmix_output(0, "sess_dir_finalize: job session dir does not exist");
            } else {
                pmix_output(0, "sess_dir_finalize: job session dir not empty - leaving");
            }
        }
    }

    if (pmix_os_dirpath_is_empty(prte_process_info.jobfam_session_dir)) {
        if (prte_debug_flag) {
            pmix_output(0, "sess_dir_finalize: found jobfam session dir empty - deleting");
        }
        rmdir(prte_process_info.jobfam_session_dir);
    } else {
        if (prte_debug_flag) {
            ret = pmix_os_dirpath_access(prte_process_info.jobfam_session_dir, 0);
            if (PMIX_ERR_NOT_FOUND == ret) {
                pmix_output(0, "sess_dir_finalize: jobfam session dir does not exist");
            } else {
                pmix_output(0, "sess_dir_finalize: jobfam session dir not empty - leaving");
            }
        }
    }

    if (NULL != prte_process_info.top_session_dir) {
        if (pmix_os_dirpath_is_empty(prte_process_info.top_session_dir)) {
            if (prte_debug_flag) {
                pmix_output(0, "sess_dir_finalize: found top session dir empty - deleting");
            }
            rmdir(prte_process_info.top_session_dir);
        } else {
            if (prte_debug_flag) {
                ret = pmix_os_dirpath_access(prte_process_info.top_session_dir, 0);
                if (PMIX_ERR_NOT_FOUND == ret) {
                    pmix_output(0, "sess_dir_finalize: top session dir does not exist");
                } else {
                    pmix_output(0, "sess_dir_finalize: top session dir not empty - leaving");
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

static bool prte_dir_check_file(const char *root, const char *path)
{
    struct stat st;
    char *fullpath;

    /*
     * Keep:
     *  - non-zero files starting with "output-"
     */
    if (0 == strncmp(path, "output-", strlen("output-"))) {
        fullpath = pmix_os_path(false, &fullpath, root, path, NULL);
        stat(fullpath, &st);
        free(fullpath);
        if (0 == st.st_size) {
            return true;
        }
        return false;
    }

    return true;
}
