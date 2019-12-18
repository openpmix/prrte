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
 * Copyright (c) 2014      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif  /* HAVE_SYS_PARAM_H */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <errno.h>
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif  /* HAVE_PWD_H */

#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/os_path.h"
#include "src/util/os_dirpath.h"
#include "src/util/basename.h"
#include "src/util/prrte_environ.h"
#include "src/util/printf.h"

#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prrte_globals.h"

#include "src/util/session_dir.h"

/*******************************
 * Local function Declarations
 *******************************/
static int prrte_create_dir(char *directory);

static bool prrte_dir_check_file(const char *root, const char *path);

#define OMPI_PRINTF_FIX_STRING(a) ((NULL == a) ? "(null)" : a)

/****************************
 * Funcationality
 ****************************/
/*
 * Check and create the directory requested
 */
static int prrte_create_dir(char *directory)
{
    mode_t my_mode = S_IRWXU;  /* I'm looking for full rights */
    int ret;

    /* Sanity check before creating the directory with the proper mode,
     * Make sure it doesn't exist already */
    if( PRRTE_ERR_NOT_FOUND !=
        (ret = prrte_os_dirpath_access(directory, my_mode)) ) {
        /* Failure because prrte_os_dirpath_access() indicated that either:
         * - The directory exists and we can access it (no need to create it again),
         *    return PRRTE_SUCCESS, or
         * - don't have access rights, return PRRTE_ERROR
         */
        if (PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
        }
        return(ret);
    }

    /* Get here if the directory doesn't exist, so create it */
    if (PRRTE_SUCCESS != (ret = prrte_os_dirpath_create(directory, my_mode))) {
        PRRTE_ERROR_LOG(ret);
    }
    return ret;
}


static int _setup_tmpdir_base(void)
{
    int rc = PRRTE_SUCCESS;

    /* make sure that we have tmpdir_base set
     * if we need it
     */
    if (NULL == prrte_process_info.tmpdir_base) {
        prrte_process_info.tmpdir_base =
                strdup(prrte_tmp_directory());
        if (NULL == prrte_process_info.tmpdir_base) {
            rc = PRRTE_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
    }
exit:
    if( PRRTE_SUCCESS != rc ){
        PRRTE_ERROR_LOG(rc);
    }
    return rc;
}

int prrte_setup_top_session_dir(void)
{
    int rc = PRRTE_SUCCESS;
    /* get the effective uid */
    uid_t uid = geteuid();

    /* construct the top_session_dir if we need */
    if (NULL == prrte_process_info.top_session_dir) {
        if (PRRTE_SUCCESS != (rc = _setup_tmpdir_base())) {
            return rc;
        }
        if( NULL == prrte_process_info.nodename ||
                NULL == prrte_process_info.tmpdir_base ){
            /* we can't setup top session dir */
            rc = PRRTE_ERR_BAD_PARAM;
            goto exit;
        }

        if (0 > prrte_asprintf(&prrte_process_info.top_session_dir,
                         "%s/prrte.%s.%lu", prrte_process_info.tmpdir_base,
                         prrte_process_info.nodename, (unsigned long)uid)) {
            prrte_process_info.top_session_dir = NULL;
            rc = PRRTE_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
    }
exit:
    if( PRRTE_SUCCESS != rc ){
        PRRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int _setup_jobfam_session_dir(prrte_process_name_t *proc)
{
    int rc = PRRTE_SUCCESS;

    /* construct the top_session_dir if we need */
    if (NULL == prrte_process_info.jobfam_session_dir) {
        if (PRRTE_SUCCESS != (rc = prrte_setup_top_session_dir())) {
            return rc;
        }

        if (PRRTE_PROC_IS_MASTER) {
            if (0 > prrte_asprintf(&prrte_process_info.jobfam_session_dir,
                             "%s/dvm", prrte_process_info.top_session_dir)) {
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else if (PRRTE_PROC_IS_MASTER) {
            if (0 > prrte_asprintf(&prrte_process_info.jobfam_session_dir,
                             "%s/pid.%lu", prrte_process_info.top_session_dir,
                             (unsigned long)prrte_process_info.pid)) {
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            /* we were not given one, so define it */
            if (NULL == proc || (PRRTE_JOBID_INVALID == proc->jobid)) {
                if (0 > prrte_asprintf(&prrte_process_info.jobfam_session_dir,
                                 "%s/jobfam", prrte_process_info.top_session_dir) ) {
                    rc = PRRTE_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
            } else {
                if (0 > prrte_asprintf(&prrte_process_info.jobfam_session_dir,
                                 "%s/jf.%d", prrte_process_info.top_session_dir,
                                 PRRTE_JOB_FAMILY(proc->jobid))) {
                    prrte_process_info.jobfam_session_dir = NULL;
                    rc = PRRTE_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
            }
        }
    }
exit:
    if( PRRTE_SUCCESS != rc ){
        PRRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int
_setup_job_session_dir(prrte_process_name_t *proc)
{
    int rc = PRRTE_SUCCESS;

    /* construct the top_session_dir if we need */
    if( NULL == prrte_process_info.job_session_dir ){
        if( PRRTE_SUCCESS != (rc = _setup_jobfam_session_dir(proc)) ){
            return rc;
        }
        if (PRRTE_JOBID_INVALID != proc->jobid) {
            if (0 > prrte_asprintf(&prrte_process_info.job_session_dir,
                             "%s/%d", prrte_process_info.jobfam_session_dir,
                             PRRTE_LOCAL_JOBID(proc->jobid))) {
                prrte_process_info.job_session_dir = NULL;
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            prrte_process_info.job_session_dir = NULL;
        }
    }

exit:
    if( PRRTE_SUCCESS != rc ){
        PRRTE_ERROR_LOG(rc);
    }
    return rc;
}

static int
_setup_proc_session_dir(prrte_process_name_t *proc)
{
    int rc = PRRTE_SUCCESS;

    /* construct the top_session_dir if we need */
    if( NULL == prrte_process_info.proc_session_dir ){
        if( PRRTE_SUCCESS != (rc = _setup_job_session_dir(proc)) ){
            return rc;
        }
        if (PRRTE_VPID_INVALID != proc->vpid) {
            if (0 > prrte_asprintf(&prrte_process_info.proc_session_dir,
                             "%s/%d", prrte_process_info.job_session_dir,
                             proc->vpid)) {
                prrte_process_info.proc_session_dir = NULL;
                rc = PRRTE_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        } else {
            prrte_process_info.proc_session_dir = NULL;
        }
    }

exit:
    if( PRRTE_SUCCESS != rc ){
        PRRTE_ERROR_LOG(rc);
    }
    return rc;
}

int prrte_session_setup_base(prrte_process_name_t *proc)
{
    int rc;

    /* Ensure that system info is set */
    prrte_proc_info();

    /* setup job and proc session directories */
    if( PRRTE_SUCCESS != (rc = _setup_job_session_dir(proc)) ){
        return rc;
    }

    if( PRRTE_SUCCESS != (rc = _setup_proc_session_dir(proc)) ){
        return rc;
    }

    /* BEFORE doing anything else, check to see if this prefix is
     * allowed by the system
     */
    if (NULL != prrte_prohibited_session_dirs ||
            NULL != prrte_process_info.tmpdir_base ) {
        char **list;
        int i, len;
        /* break the string into tokens - it should be
         * separated by ','
         */
        list = prrte_argv_split(prrte_prohibited_session_dirs, ',');
        len = prrte_argv_count(list);
        /* cycle through the list */
        for (i=0; i < len; i++) {
            /* check if prefix matches */
            if (0 == strncmp(prrte_process_info.tmpdir_base, list[i], strlen(list[i]))) {
                /* this is a prohibited location */
                prrte_show_help("help-prrte-runtime.txt",
                               "prrte:session:dir:prohibited",
                               true, prrte_process_info.tmpdir_base,
                               prrte_prohibited_session_dirs);
                prrte_argv_free(list);
                return PRRTE_ERR_FATAL;
            }
        }
        prrte_argv_free(list);  /* done with this */
    }
    return PRRTE_SUCCESS;
}

/*
 * Construct the session directory and create it if necessary
 */
int prrte_session_dir(bool create, prrte_process_name_t *proc)
{
    int rc = PRRTE_SUCCESS;

    /*
     * Get the session directory full name
     */
    if (PRRTE_SUCCESS != (rc = prrte_session_setup_base(proc))) {
        if (PRRTE_ERR_FATAL == rc) {
            /* this indicates we should abort quietly */
            rc = PRRTE_ERR_SILENT;
        }
        goto cleanup;
    }

    /*
     * Now that we have the full path, go ahead and create it if necessary
     */
    if( create ) {
        if( PRRTE_SUCCESS != (rc = prrte_create_dir(prrte_process_info.proc_session_dir)) ) {
            PRRTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    if (prrte_debug_flag) {
        prrte_output(0, "procdir: %s",
                    OMPI_PRINTF_FIX_STRING(prrte_process_info.proc_session_dir));
        prrte_output(0, "jobdir: %s",
                    OMPI_PRINTF_FIX_STRING(prrte_process_info.job_session_dir));
        prrte_output(0, "top: %s",
                    OMPI_PRINTF_FIX_STRING(prrte_process_info.jobfam_session_dir));
        prrte_output(0, "top: %s",
                    OMPI_PRINTF_FIX_STRING(prrte_process_info.top_session_dir));
        prrte_output(0, "tmp: %s",
                    OMPI_PRINTF_FIX_STRING(prrte_process_info.tmpdir_base));
    }

cleanup:
    return rc;
}

/*
 * A job has aborted - so force cleanup of the session directory
 */
int
prrte_session_dir_cleanup(prrte_jobid_t jobid)
{
    /* special case - if a daemon is colocated with mpirun,
     * then we let mpirun do the rest to avoid a race
     * condition. this scenario always results in the rank=1
     * daemon colocated with mpirun */
    if (prrte_ras_base.launch_orted_on_hn &&
        PRRTE_PROC_IS_DAEMON &&
        1 == PRRTE_PROC_MY_NAME->vpid) {
        return PRRTE_SUCCESS;
    }

    if (!prrte_create_session_dirs || prrte_process_info.rm_session_dirs ) {
        /* we haven't created them or RM will clean them up for us*/
        return PRRTE_SUCCESS;
    }

    if (NULL == prrte_process_info.jobfam_session_dir ||
        NULL == prrte_process_info.proc_session_dir) {
        /* this should never happen - it means we are calling
         * cleanup *before* properly setting up the session
         * dir system. This leaves open the possibility of
         * accidentally removing directories we shouldn't
         * touch
         */
        return PRRTE_ERR_NOT_INITIALIZED;
    }


    /* recursively blow the whole session away for our job family,
     * saving only output files
     */
    prrte_os_dirpath_destroy(prrte_process_info.jobfam_session_dir,
                            true, prrte_dir_check_file);

    if (prrte_os_dirpath_is_empty(prrte_process_info.jobfam_session_dir)) {
        if (prrte_debug_flag) {
            prrte_output(0, "sess_dir_cleanup: found jobfam session dir empty - deleting");
        }
        rmdir(prrte_process_info.jobfam_session_dir);
    } else {
        if (prrte_debug_flag) {
            if (PRRTE_ERR_NOT_FOUND ==
                    prrte_os_dirpath_access(prrte_process_info.job_session_dir, 0)) {
                prrte_output(0, "sess_dir_cleanup: job session dir does not exist");
            } else {
                prrte_output(0, "sess_dir_cleanup: job session dir not empty - leaving");
            }
        }
    }

    if (NULL != prrte_process_info.top_session_dir) {
        if (prrte_os_dirpath_is_empty(prrte_process_info.top_session_dir)) {
            if (prrte_debug_flag) {
                prrte_output(0, "sess_dir_cleanup: found top session dir empty - deleting");
            }
            rmdir(prrte_process_info.top_session_dir);
        } else {
            if (prrte_debug_flag) {
                if (PRRTE_ERR_NOT_FOUND ==
                        prrte_os_dirpath_access(prrte_process_info.top_session_dir, 0)) {
                    prrte_output(0, "sess_dir_cleanup: top session dir does not exist");
                } else {
                    prrte_output(0, "sess_dir_cleanup: top session dir not empty - leaving");
                }
            }
        }
    }

    /* now attempt to eliminate the top level directory itself - this
     * will fail if anything is present, but ensures we cleanup if
     * we are the last one out
     */
    if( NULL != prrte_process_info.top_session_dir ){
        prrte_os_dirpath_destroy(prrte_process_info.top_session_dir,
                                false, prrte_dir_check_file);
    }


    return PRRTE_SUCCESS;
}


int
prrte_session_dir_finalize(prrte_process_name_t *proc)
{
    if (!prrte_create_session_dirs || prrte_process_info.rm_session_dirs ) {
        /* we haven't created them or RM will clean them up for us*/
        return PRRTE_SUCCESS;
    }

    if (NULL == prrte_process_info.job_session_dir ||
        NULL == prrte_process_info.proc_session_dir) {
        /* this should never happen - it means we are calling
         * cleanup *before* properly setting up the session
         * dir system. This leaves open the possibility of
         * accidentally removing directories we shouldn't
         * touch
         */
        return PRRTE_ERR_NOT_INITIALIZED;
    }

    prrte_os_dirpath_destroy(prrte_process_info.proc_session_dir,
                            false, prrte_dir_check_file);

    if (prrte_os_dirpath_is_empty(prrte_process_info.proc_session_dir)) {
        if (prrte_debug_flag) {
            prrte_output(0, "sess_dir_finalize: found proc session dir empty - deleting");
        }
        rmdir(prrte_process_info.proc_session_dir);
    } else {
        if (prrte_debug_flag) {
            if (PRRTE_ERR_NOT_FOUND ==
                    prrte_os_dirpath_access(prrte_process_info.proc_session_dir, 0)) {
                prrte_output(0, "sess_dir_finalize: proc session dir does not exist");
            } else {
                prrte_output(0, "sess_dir_finalize: proc session dir not empty - leaving");
            }
        }
    }

    /* special case - if a daemon is colocated with mpirun,
     * then we let mpirun do the rest to avoid a race
     * condition. this scenario always results in the rank=1
     * daemon colocated with mpirun */
    if (prrte_ras_base.launch_orted_on_hn &&
        PRRTE_PROC_IS_DAEMON &&
        1 == PRRTE_PROC_MY_NAME->vpid) {
        return PRRTE_SUCCESS;
    }

    prrte_os_dirpath_destroy(prrte_process_info.job_session_dir,
                            false, prrte_dir_check_file);

    /* only remove the jobfam session dir if we are the
     * local daemon and we are finalizing our own session dir */
    if ((PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_DAEMON) &&
        (PRRTE_PROC_MY_NAME == proc)) {
        prrte_os_dirpath_destroy(prrte_process_info.jobfam_session_dir,
                                false, prrte_dir_check_file);
    }

    if( NULL != prrte_process_info.top_session_dir ){
        prrte_os_dirpath_destroy(prrte_process_info.top_session_dir,
                                false, prrte_dir_check_file);
    }

    if (prrte_os_dirpath_is_empty(prrte_process_info.job_session_dir)) {
        if (prrte_debug_flag) {
            prrte_output(0, "sess_dir_finalize: found job session dir empty - deleting");
        }
        rmdir(prrte_process_info.job_session_dir);
    } else {
        if (prrte_debug_flag) {
            if (PRRTE_ERR_NOT_FOUND ==
                    prrte_os_dirpath_access(prrte_process_info.job_session_dir, 0)) {
                prrte_output(0, "sess_dir_finalize: job session dir does not exist");
            } else {
                prrte_output(0, "sess_dir_finalize: job session dir not empty - leaving");
            }
        }
    }

    if (prrte_os_dirpath_is_empty(prrte_process_info.jobfam_session_dir)) {
        if (prrte_debug_flag) {
            prrte_output(0, "sess_dir_finalize: found jobfam session dir empty - deleting");
        }
        rmdir(prrte_process_info.jobfam_session_dir);
    } else {
        if (prrte_debug_flag) {
            if (PRRTE_ERR_NOT_FOUND ==
                    prrte_os_dirpath_access(prrte_process_info.jobfam_session_dir, 0)) {
                prrte_output(0, "sess_dir_finalize: jobfam session dir does not exist");
            } else {
                prrte_output(0, "sess_dir_finalize: jobfam session dir not empty - leaving");
            }
        }
    }

    if (prrte_os_dirpath_is_empty(prrte_process_info.jobfam_session_dir)) {
        if (prrte_debug_flag) {
            prrte_output(0, "sess_dir_finalize: found jobfam session dir empty - deleting");
        }
        rmdir(prrte_process_info.jobfam_session_dir);
    } else {
        if (prrte_debug_flag) {
            if (PRRTE_ERR_NOT_FOUND ==
                    prrte_os_dirpath_access(prrte_process_info.jobfam_session_dir, 0)) {
                prrte_output(0, "sess_dir_finalize: jobfam session dir does not exist");
            } else {
                prrte_output(0, "sess_dir_finalize: jobfam session dir not empty - leaving");
            }
        }
    }

    if (NULL != prrte_process_info.top_session_dir) {
        if (prrte_os_dirpath_is_empty(prrte_process_info.top_session_dir)) {
            if (prrte_debug_flag) {
                prrte_output(0, "sess_dir_finalize: found top session dir empty - deleting");
            }
            rmdir(prrte_process_info.top_session_dir);
        } else {
            if (prrte_debug_flag) {
                if (PRRTE_ERR_NOT_FOUND ==
                        prrte_os_dirpath_access(prrte_process_info.top_session_dir, 0)) {
                    prrte_output(0, "sess_dir_finalize: top session dir does not exist");
                } else {
                    prrte_output(0, "sess_dir_finalize: top session dir not empty - leaving");
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

static bool
prrte_dir_check_file(const char *root, const char *path)
{
    struct stat st;
    char *fullpath;

    /*
     * Keep:
     *  - non-zero files starting with "output-"
     */
    if (0 == strncmp(path, "output-", strlen("output-"))) {
        fullpath = prrte_os_path(false, &fullpath, root, path, NULL);
        stat(fullpath, &st);
        free(fullpath);
        if (0 == st.st_size) {
            return true;
        }
        return false;
    }

    return true;
}
