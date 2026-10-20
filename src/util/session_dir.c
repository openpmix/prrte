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
#include <fcntl.h>
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
#include "src/util/prte_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/runtime.h"

#include "src/util/session_dir.h"

/*******************************
 * Local function Declarations
 *******************************/
static bool _check_file(const char *root, const char *path);

/* the node marker of the top-level directory being cleaned up - which
 * _check_file keeps, for _drop_marker to decide on */
static char *top_marker = NULL;

static bool setup_base_complete = false;

#define PRTE_PRINTF_FIX_STRING(a) ((NULL == a) ? "(null)" : a)

/****************************
 * Funcationality
 ****************************/
/*
 * Confirm that a job or rank directory PRRTE has just created - or found
 * already sitting at the name it composed - is really ours to use, and
 * say why not.
 *
 * These directories are made inside the top-level one, which mkdtemp()
 * created for us, mode 0700 (see _claim_dir), so no one else can have
 * put anything there. The check costs little and keeps the rule should a
 * directory ever be composed somewhere else.
 *
 * The inspection is done through a descriptor so the directory checked
 * is the directory the path names at that moment: O_NOFOLLOW refuses a
 * symlink at the final component (the composed names never end in a
 * separator, which would defeat it).
 *
 * A directory we own is refused too if it is writable by group or
 * other: we never create one that way.
 */
static int _check_owner(const pmix_nspace_t nspace, const char *directory)
{
    struct stat buf;
    int fd, save;

    fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        save = errno;
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:inspect", true,
                       directory, strerror(save));
        return PRTE_ERR_SILENT;
    }
    if (0 != fstat(fd, &buf)) {
        save = errno;
        close(fd);
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:inspect", true,
                       directory, strerror(save));
        return PRTE_ERR_SILENT;
    }
    close(fd);

    if (buf.st_uid != geteuid()) {
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:owner", true,
                       directory, (unsigned long) buf.st_uid, (unsigned long) geteuid());
        return PRTE_ERR_SILENT;
    }
    if (0 != (buf.st_mode & (S_IWGRP | S_IWOTH))) {
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:writable", true,
                       directory, (unsigned int) (buf.st_mode & 07777));
        return PRTE_ERR_SILENT;
    }
    return PRTE_SUCCESS;
}

/*
 * Check and create the directory requested
 */
static int _create_dir(const pmix_nspace_t nspace, const char *directory, bool *created)
{
    mode_t my_mode = S_IRWXU; /* I'm looking for full rights */
    int ret;

    if (NULL != created) {
        *created = false;
    }
    /* attempt to create it */
    ret = pmix_os_dirpath_create(directory, my_mode);
    if (PMIX_SUCCESS == ret && NULL != created) {
        *created = true;
    }
    if (PMIX_ERR_EXISTS == ret) {
        // existence is good enough - provided it is ours
        ret = PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        return prte_pmix_convert_status(ret);
    }
    // check a freshly created directory as well: the check is cheap,
    // and pmix_os_dirpath_create() answers success, not EXISTS, when
    // the final component appeared while it was building the tree
    return _check_owner(nspace, directory);
}

/*
 * The name of the empty file that says which node a top-level directory
 * belongs to: node.<nodename>, with any '/' in the name made '_' so the
 * file stays in the directory. Caller frees. NULL without a nodename.
 */
static char *_node_marker(void)
{
    char *name = NULL, *c;

    if (NULL == prte_process_info.nodename ||
        0 > pmix_asprintf(&name, "node.%s", prte_process_info.nodename)) {
        return NULL;
    }
    for (c = name; '\0' != *c; c++) {
        if ('/' == *c) {
            *c = '_';
        }
    }
    return name;
}

/*
 * Make a top-level directory - one whose name sits directly under a
 * shared temporary directory - and mark it with the node it belongs to.
 *
 * `*directory` names the stem, <tmpdir>/<prefix>. The directory made is
 * <stem>.XXXXXX, by mkdtemp(), and its name replaces the stem. mkdtemp()
 * creates it atomically, mode 0700, under a name no other process has -
 * on this node or, over a file system shared between nodes, on any
 * other - so any number of instances of each tool can run at once,
 * whatever their pids or pid namespaces, and a directory already there
 * is never taken over. Nothing looks a session directory up by name: it
 * is passed to PMIx (PMIX_SERVER_TMPDIR, PMIX_TMPDIR) and everywhere else
 * by value, and PMIx finds rendezvous files by searching the tree below
 * the temporary directory. The prefix is kept, so whatever looks for our
 * directories by prefix still finds them.
 *
 * Inside it goes an empty file, node.<nodename> (_node_marker), so that
 * whoever finds the directory - left behind on a file system shared
 * between nodes, say - can tell which node it belongs to. Cleanup keeps
 * it for as long as anything else is kept in the directory.
 *
 * Only a top-level directory needs this. The job and rank directories
 * below it are created inside a directory that is ours and that no one
 * else can write to.
 */
static int _claim_dir(const pmix_nspace_t nspace, char **directory, bool *created)
{
    char *dir = NULL, *marker = NULL, *path = NULL;
    int fd, save;

    if (NULL != created) {
        *created = false;
    }
    if (NULL == (marker = _node_marker())) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (0 > pmix_asprintf(&dir, "%s.XXXXXX", *directory) || NULL == dir) {
        free(marker);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (NULL == mkdtemp(dir)) {
        save = errno;
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:create", true,
                       dir, strerror(save));
        free(dir);
        free(marker);
        return PRTE_ERR_SILENT;
    }
    path = pmix_os_path(false, dir, marker, NULL);
    free(marker);
    if (NULL == path) {
        (void) rmdir(dir);
        free(dir);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* the directory is ours and no one else can write to it */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (0 > fd) {
        save = errno;
        prte_show_help(nspace, "help-prte-runtime.txt", "prte:session:dir:create", true,
                       path, strerror(save));
        free(path);
        (void) rmdir(dir);
        free(dir);
        return PRTE_ERR_SILENT;
    }
    close(fd);
    free(path);
    free(*directory);
    *directory = dir;
    if (NULL != created) {
        *created = true;
    }
    return PRTE_SUCCESS;
}

int prte_session_dir_create(char **directory, bool *created)
{
    return _claim_dir(PRTE_PROC_MY_NAME->nspace, directory, created);
}

static int _setup_top_session_dir(void)
{
    int rc = PRTE_SUCCESS;

    /* construct the top_session_dir if we need - <tmpdir>/<prefix>,
     * which _claim_dir makes unique */
    if (NULL == prte_process_info.top_session_dir) {
        if (NULL == prte_process_info.nodename ||
            NULL == prte_process_info.tmpdir_base) {
            /* we can't setup top session dir */
            rc = PRTE_ERR_BAD_PARAM;
            goto exit;
        }
        if (0 > pmix_asprintf(&prte_process_info.top_session_dir, "%s/%s",
                              prte_process_info.tmpdir_base, prte_process_info.sessdir_prefix)) {
            prte_process_info.top_session_dir = NULL;
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
    }
    rc = _claim_dir(PRTE_PROC_MY_NAME->nspace, &prte_process_info.top_session_dir, NULL);

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
        rc = _create_dir(jdata->nspace, jdata->session_dir, NULL);
        if (PRTE_SUCCESS != rc) {
            // forget a directory we did not get: the finalize path
            // recursively destroys whatever this names, and it may be
            // someone else's
            free(jdata->session_dir);
            jdata->session_dir = NULL;
        }
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
    rc = _create_dir(jdata->nspace, tmp, NULL);
    free(tmp);
    return rc;
}

static int setup_base(void)
{
    int rc;

    // only do this once, and only latch that once it has actually worked -
    // a failed attempt used to be remembered as a success
    if (setup_base_complete) {
        return PRTE_SUCCESS;
    }

    /* Ensure that system info is set */
    prte_proc_info();

    /* BEFORE doing anything else, check to see if this prefix is
     * allowed by the system
     */
    if (NULL != prte_prohibited_session_dirs && NULL != prte_process_info.tmpdir_base) {
        char **list;
        int i, len;
        /* break the string into tokens - it should be
         * separated by ','
         */
        list = PMIx_Argv_split(prte_prohibited_session_dirs, ',');
        len = PMIx_Argv_count(list);
        /* cycle through the list */
        for (i = 0; i < len; i++) {
            /* check if prefix matches */
            if (0 == strncmp(prte_process_info.tmpdir_base, list[i], strlen(list[i]))) {
                /* this is a prohibited location */
                prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-prte-runtime.txt", "prte:session:dir:prohibited", true,
                               prte_process_info.tmpdir_base, prte_prohibited_session_dirs);
                PMIx_Argv_free(list);
                return PRTE_ERR_FATAL;
            }
        }
        PMIx_Argv_free(list); /* done with this */
    }

    rc = _setup_top_session_dir();
    if (PRTE_SUCCESS == rc) {
        setup_base_complete = true;
    }

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

    if (prte_debug_daemons_flag) {
        pmix_output(0, "jobdir: %s", PRTE_PRINTF_FIX_STRING(jdata->session_dir));
        pmix_output(0, "top: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.top_session_dir));
        pmix_output(0, "tmp: %s", PRTE_PRINTF_FIX_STRING(prte_process_info.tmpdir_base));
    }

cleanup:
    return rc;
}

/*
 * Remove a top-level directory's node marker once it is all that is
 * left in it. While anything else is kept there - an output file, say -
 * the marker stays, so whoever finds the directory can tell which node
 * it belongs to.
 */
static void _drop_marker(const char *directory, const char *marker)
{
    DIR *dp;
    struct dirent *ep;
    int fd;
    bool alone = true;

    if (NULL == marker) {
        return;
    }
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        return;
    }
    if (NULL == (dp = fdopendir(fd))) {
        close(fd);
        return;
    }
    while (NULL != (ep = readdir(dp))) {
        if (0 == strcmp(ep->d_name, ".") || 0 == strcmp(ep->d_name, "..") ||
            0 == strcmp(ep->d_name, marker)) {
            continue;
        }
        alone = false;
        break;
    }
    if (alone) {
        (void) unlinkat(dirfd(dp), marker, 0);
    }
    closedir(dp);
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
                top_marker = _node_marker();
                pmix_os_dirpath_destroy(prte_process_info.top_session_dir, true, _check_file);
                _drop_marker(prte_process_info.top_session_dir, top_marker);
                free(top_marker);
                top_marker = NULL;
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

    /* the top-level directory's node marker is left to _drop_marker */
    if (NULL != top_marker && NULL != prte_process_info.top_session_dir &&
        0 == strcmp(root, prte_process_info.top_session_dir) && 0 == strcmp(path, top_marker)) {
        return false;
    }

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
            free(fullpath);
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
