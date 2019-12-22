/*
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 *
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"
#include "src/dss/dss.h"

#include "src/util/show_help.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/basename.h"

#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/rml/rml.h"

#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"

#include "filem_raw.h"

static int raw_init(void);
static int raw_finalize(void);
static int raw_preposition_files(prrte_job_t *jdata,
                                 prrte_filem_completion_cbfunc_t cbfunc,
                                 void *cbdata);
static int raw_link_local_files(prrte_job_t *jdata,
                                prrte_app_context_t *app);

prrte_filem_base_module_t prrte_filem_raw_module = {
    .filem_init = raw_init,
    .filem_finalize = raw_finalize,
    /* we don't use any of the following */
    .put = prrte_filem_base_none_put,
    .put_nb = prrte_filem_base_none_put_nb,
    .get = prrte_filem_base_none_get,
    .get_nb = prrte_filem_base_none_get_nb,
    .rm = prrte_filem_base_none_rm,
    .rm_nb = prrte_filem_base_none_rm_nb,
    .wait = prrte_filem_base_none_wait,
    .wait_all = prrte_filem_base_none_wait_all,
    /* now the APIs we *do* use */
    .preposition_files = raw_preposition_files,
    .link_local_files = raw_link_local_files
};

static prrte_list_t outbound_files;
static prrte_list_t incoming_files;
static prrte_list_t positioned_files;

static void send_chunk(int fd, short argc, void *cbdata);
static void recv_files(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata);
static void recv_ack(int status, prrte_process_name_t* sender,
                     prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                     void* cbdata);
static void write_handler(int fd, short event, void *cbdata);

static char *filem_session_dir(void)
{
    char *session_dir = prrte_process_info.jobfam_session_dir;
    if( NULL == session_dir ){
        /* if no job family session dir was provided -
         * use the job session dir */
        session_dir = prrte_process_info.job_session_dir;
    }
    return session_dir;
}

static int raw_init(void)
{
    PRRTE_CONSTRUCT(&incoming_files, prrte_list_t);

    /* start a recv to catch any files sent to me */
    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_FILEM_BASE,
                            PRRTE_RML_PERSISTENT,
                            recv_files,
                            NULL);

    /* if I'm the HNP, start a recv to catch acks sent to me */
    if (PRRTE_PROC_IS_MASTER) {
        PRRTE_CONSTRUCT(&outbound_files, prrte_list_t);
        PRRTE_CONSTRUCT(&positioned_files, prrte_list_t);
        prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                                PRRTE_RML_TAG_FILEM_BASE_RESP,
                                PRRTE_RML_PERSISTENT,
                                recv_ack,
                                NULL);
    }

    return PRRTE_SUCCESS;
}

static int raw_finalize(void)
{
    prrte_list_item_t *item;

    while (NULL != (item = prrte_list_remove_first(&incoming_files))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&incoming_files);

    if (PRRTE_PROC_IS_MASTER) {
        while (NULL != (item = prrte_list_remove_first(&outbound_files))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&outbound_files);
        while (NULL != (item = prrte_list_remove_first(&positioned_files))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&positioned_files);
    }

    return PRRTE_SUCCESS;
}

static void xfer_complete(int status, prrte_filem_raw_xfer_t *xfer)
{
    prrte_filem_raw_outbound_t *outbound = xfer->outbound;

    /* transfer the status, if not success */
    if (PRRTE_SUCCESS != status) {
        outbound->status = status;
    }

    /* this transfer is complete - remove it from list */
    prrte_list_remove_item(&outbound->xfers, &xfer->super);
    /* add it to the list of files that have been positioned */
    prrte_list_append(&positioned_files, &xfer->super);

    /* if the list is now empty, then the xfer is complete */
    if (0 == prrte_list_get_size(&outbound->xfers)) {
        /* do the callback */
        if (NULL != outbound->cbfunc) {
            outbound->cbfunc(outbound->status, outbound->cbdata);
        }
        /* release the object */
        prrte_list_remove_item(&outbound_files, &outbound->super);
        PRRTE_RELEASE(outbound);
    }
}

static void recv_ack(int status, prrte_process_name_t* sender,
                     prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                     void* cbdata)
{
    prrte_list_item_t *item, *itm;
    prrte_filem_raw_outbound_t *outbound;
    prrte_filem_raw_xfer_t *xfer;
    char *file;
    int st, n, rc;

    /* unpack the file */
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &file, &n, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the status */
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &st, &n, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw: recvd ack from %s for file %s status %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(sender), file, st));

    /* find the corresponding outbound object */
    for (item = prrte_list_get_first(&outbound_files);
         item != prrte_list_get_end(&outbound_files);
         item = prrte_list_get_next(item)) {
        outbound = (prrte_filem_raw_outbound_t*)item;
        for (itm = prrte_list_get_first(&outbound->xfers);
             itm != prrte_list_get_end(&outbound->xfers);
             itm = prrte_list_get_next(itm)) {
            xfer = (prrte_filem_raw_xfer_t*)itm;
            if (0 == strcmp(file, xfer->file)) {
                /* if the status isn't success, record it */
                if (0 != st) {
                    xfer->status = st;
                }
                /* track number of respondents */
                xfer->nrecvd++;
                /* if all daemons have responded, then this is complete */
                if (xfer->nrecvd == prrte_process_info.num_procs) {
                    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                         "%s filem:raw: xfer complete for file %s status %d",
                                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                         file, xfer->status));
                    xfer_complete(xfer->status, xfer);
                }
                free(file);
                return;
            }
        }
    }
}

static int raw_preposition_files(prrte_job_t *jdata,
                                 prrte_filem_completion_cbfunc_t cbfunc,
                                 void *cbdata)
{
    prrte_app_context_t *app;
    prrte_list_item_t *item, *itm, *itm2;
    prrte_filem_base_file_set_t *fs;
    int fd;
    prrte_filem_raw_xfer_t *xfer, *xptr;
    int flags, i, j;
    char **files=NULL;
    prrte_filem_raw_outbound_t *outbound, *optr;
    char *cptr, *nxt, *filestring;
    prrte_list_t fsets;
    bool already_sent;

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw: preposition files for job %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_JOBID_PRINT(jdata->jobid)));

    /* cycle across the app_contexts looking for files or
     * binaries to be prepositioned
     */
    PRRTE_CONSTRUCT(&fsets, prrte_list_t);
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_PRELOAD_BIN, NULL, PRRTE_BOOL)) {
            /* add the executable to our list */
            PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: preload executable %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 app->app));
            fs = PRRTE_NEW(prrte_filem_base_file_set_t);
            fs->local_target = strdup(app->app);
            fs->target_flag = PRRTE_FILEM_TYPE_EXE;
            prrte_list_append(&fsets, &fs->super);
            /* if we are preloading the binary, then the app must be in relative
             * syntax or we won't find it - the binary will be positioned in the
             * session dir, so ensure the app is relative to that location
             */
            cptr = prrte_basename(app->app);
            free(app->app);
            prrte_asprintf(&app->app, "./%s", cptr);
            free(app->argv[0]);
            app->argv[0] = strdup(app->app);
            fs->remote_target = strdup(app->app);
        }
        if (prrte_get_attribute(&app->attributes, PRRTE_APP_PRELOAD_FILES, (void**)&filestring, PRRTE_STRING)) {
            files = prrte_argv_split(filestring, ',');
            free(filestring);
            for (j=0; NULL != files[j]; j++) {
                fs = PRRTE_NEW(prrte_filem_base_file_set_t);
                fs->local_target = strdup(files[j]);
                /* check any suffix for file type */
                if (NULL != (cptr = strchr(files[j], '.'))) {
                    if (0 == strncmp(cptr, ".tar", 4)) {
                        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as TAR",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             files[j]));
                        fs->target_flag = PRRTE_FILEM_TYPE_TAR;
                    } else if (0 == strncmp(cptr, ".bz", 3)) {
                        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as BZIP",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             files[j]));
                        fs->target_flag = PRRTE_FILEM_TYPE_BZIP;
                    } else if (0 == strncmp(cptr, ".gz", 3)) {
                        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as GZIP",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             files[j]));
                        fs->target_flag = PRRTE_FILEM_TYPE_GZIP;
                    } else {
                        fs->target_flag = PRRTE_FILEM_TYPE_FILE;
                    }
                } else {
                    fs->target_flag = PRRTE_FILEM_TYPE_FILE;
                }
                /* if we are flattening directory trees, then the
                 * remote path is just the basename file name
                 */
                if (prrte_filem_raw_flatten_trees) {
                    fs->remote_target = prrte_basename(files[j]);
                } else {
                    /* if this was an absolute path, then we need
                     * to convert it to a relative path - we do not
                     * allow positioning of files to absolute locations
                     * due to the potential for unintentional overwriting
                     * of files
                     */
                    if (prrte_path_is_absolute(files[j])) {
                        fs->remote_target = strdup(&files[j][1]);
                    } else {
                        fs->remote_target = strdup(files[j]);
                    }
                }
                prrte_list_append(&fsets, &fs->super);
                /* prep the filename for matching on the remote
                 * end by stripping any leading '.' directories to avoid
                 * stepping above the session dir location - all
                 * files will be relative to that point. Ensure
                 * we *don't* mistakenly strip the dot from a
                 * filename that starts with one
                 */
                cptr = fs->remote_target;
                nxt = cptr;
                nxt++;
                while ('\0' != *cptr) {
                    if ('.' == *cptr) {
                        /* have to check the next character to
                         * see if it's a dotfile or not
                         */
                        if ('.' == *nxt || '/' == *nxt) {
                            cptr = nxt;
                            nxt++;
                        } else {
                            /* if the next character isn't a dot
                             * or a slash, then this is a dot-file
                             * and we need to leave it alone
                             */
                            break;
                        }
                    } else if ('/' == *cptr) {
                        /* move to the next character */
                        cptr = nxt;
                        nxt++;
                    } else {
                        /* the character isn't a dot or a slash,
                         * so this is the beginning of the filename
                         */
                        break;
                    }
                }
                free(files[j]);
                files[j] = strdup(cptr);
            }
            /* replace the app's file list with the revised one so we
             * can find them on the remote end
             */
            filestring = prrte_argv_join(files, ',');
            prrte_set_attribute(&app->attributes, PRRTE_APP_PRELOAD_FILES, PRRTE_ATTR_GLOBAL, filestring, PRRTE_STRING);
            /* cleanup for the next app */
            prrte_argv_free(files);
            free(filestring);
        }
    }
    if (0 == prrte_list_get_size(&fsets)) {
        /* nothing to preposition */
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: nothing to preposition",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        if (NULL != cbfunc) {
            cbfunc(PRRTE_SUCCESS, cbdata);
        }
        PRRTE_DESTRUCT(&fsets);
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw: found %d files to position",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         (int)prrte_list_get_size(&fsets)));

    /* track the outbound file sets */
    outbound = PRRTE_NEW(prrte_filem_raw_outbound_t);
    outbound->cbfunc = cbfunc;
    outbound->cbdata = cbdata;
    prrte_list_append(&outbound_files, &outbound->super);

    /* only the HNP should ever call this function - loop thru the
     * fileset and initiate xcast transfer of each file to every
     * daemon
     */
    while (NULL != (item = prrte_list_remove_first(&fsets))) {
        fs = (prrte_filem_base_file_set_t*)item;
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: checking prepositioning of file %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             fs->local_target));

        /* have we already sent this file? */
        already_sent = false;
        for (itm = prrte_list_get_first(&positioned_files);
             !already_sent && itm != prrte_list_get_end(&positioned_files);
             itm = prrte_list_get_next(itm)) {
            xptr = (prrte_filem_raw_xfer_t*)itm;
            if (0 == strcmp(fs->local_target, xptr->src)) {
                already_sent = true;
            }
        }
        if (already_sent) {
            /* no need to send it again */
            PRRTE_OUTPUT_VERBOSE((3, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: file %s is already in position - ignoring",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), fs->local_target));
            PRRTE_RELEASE(item);
            continue;
        }
        /* also have to check if this file is already in the process
         * of being transferred, or was included multiple times
         * for transfer
         */
        for (itm = prrte_list_get_first(&outbound_files);
             !already_sent && itm != prrte_list_get_end(&outbound_files);
             itm = prrte_list_get_next(itm)) {
            optr = (prrte_filem_raw_outbound_t*)itm;
            for (itm2 = prrte_list_get_first(&optr->xfers);
                 itm2 != prrte_list_get_end(&optr->xfers);
                 itm2 = prrte_list_get_next(itm2)) {
                xptr = (prrte_filem_raw_xfer_t*)itm2;
                if (0 == strcmp(fs->local_target, xptr->src)) {
                    already_sent = true;
                }
            }
        }
        if (already_sent) {
            /* no need to send it again */
            PRRTE_OUTPUT_VERBOSE((3, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: file %s is already queued for output - ignoring",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), fs->local_target));
            PRRTE_RELEASE(item);
            continue;
        }

        /* attempt to open the specified file */
        if (0 > (fd = open(fs->local_target, O_RDONLY))) {
            prrte_output(0, "%s CANNOT ACCESS FILE %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), fs->local_target);
            PRRTE_RELEASE(item);
            prrte_list_remove_item(&outbound_files, &outbound->super);
            PRRTE_RELEASE(outbound);
            return PRRTE_ERROR;
        }
        /* set the flags to non-blocking */
        if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
            prrte_output(prrte_filem_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                        __FILE__, __LINE__, errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(fd, F_SETFL, flags) < 0) {
                prrte_output(prrte_filem_base_framework.framework_output, "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                            __FILE__, __LINE__, errno);
            }
        }
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: setting up to position file %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), fs->local_target));
        xfer = PRRTE_NEW(prrte_filem_raw_xfer_t);
        /* save the source so we can avoid duplicate transfers */
        xfer->src = strdup(fs->local_target);
        /* strip any leading '.' directories to avoid
         * stepping above the session dir location - all
         * files will be relative to that point. Ensure
         * we *don't* mistakenly strip the dot from a
         * filename that starts with one
         */
        cptr = fs->remote_target;
        nxt = cptr;
        nxt++;
        while ('\0' != *cptr) {
            if ('.' == *cptr) {
                /* have to check the next character to
                 * see if it's a dotfile or not
                 */
                if ('.' == *nxt || '/' == *nxt) {
                    cptr = nxt;
                    nxt++;
                } else {
                    /* if the next character isn't a dot
                     * or a slash, then this is a dot-file
                     * and we need to leave it alone
                     */
                    break;
                }
            } else if ('/' == *cptr) {
                /* move to the next character */
                cptr = nxt;
                nxt++;
            } else {
                /* the character isn't a dot or a slash,
                 * so this is the beginning of the filename
                 */
                break;
            }
        }
        xfer->file = strdup(cptr);
        xfer->type = fs->target_flag;
        xfer->app_idx = fs->app_idx;
        xfer->outbound = outbound;
        prrte_list_append(&outbound->xfers, &xfer->super);
        prrte_event_set(prrte_event_base, &xfer->ev, fd, PRRTE_EV_READ, send_chunk, xfer);
        prrte_event_set_priority(&xfer->ev, PRRTE_MSG_PRI);
        xfer->pending = true;
        PRRTE_POST_OBJECT(xfer);
        prrte_event_add(&xfer->ev, 0);
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&fsets);

    /* check to see if anything remains to be sent - if everything
     * is a duplicate, then the list will be empty
     */
    if (0 == prrte_list_get_size(&outbound->xfers)) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: all duplicate files - no positioning reqd",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        prrte_list_remove_item(&outbound_files, &outbound->super);
        PRRTE_RELEASE(outbound);
        if (NULL != cbfunc) {
            cbfunc(PRRTE_SUCCESS, cbdata);
        }
        return PRRTE_SUCCESS;
    }

    if (0 < prrte_output_get_verbosity(prrte_filem_base_framework.framework_output)) {
        prrte_output(0, "%s Files to be positioned:", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        for (itm2 = prrte_list_get_first(&outbound->xfers);
             itm2 != prrte_list_get_end(&outbound->xfers);
             itm2 = prrte_list_get_next(itm2)) {
            xptr = (prrte_filem_raw_xfer_t*)itm2;
            prrte_output(0, "%s\t%s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), xptr->src);
        }
    }

    return PRRTE_SUCCESS;
}

static int create_link(char *my_dir, char *path,
                       char *link_pt)
{
    char *mypath, *fullname, *basedir;
    struct stat buf;
    int rc = PRRTE_SUCCESS;

    /* form the full source path name */
    mypath = prrte_os_path(false, my_dir, link_pt, NULL);
    /* form the full target path name */
    fullname = prrte_os_path(false, path, link_pt, NULL);
    /* there may have been multiple files placed under the
     * same directory, so check for existence first
     */
    if (0 != stat(fullname, &buf)) {
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: creating symlink to %s\n\tmypath: %s\n\tlink: %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), link_pt,
                             mypath, fullname));
        /* create any required path to the link location */
        basedir = prrte_dirname(fullname);
        if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(basedir, S_IRWXU))) {
            PRRTE_ERROR_LOG(rc);
            prrte_output(0, "%s Failed to symlink %s to %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), mypath, fullname);
            free(basedir);
            free(mypath);
            free(fullname);
            return rc;
        }
        free(basedir);
        /* do the symlink */
        if (0 != symlink(mypath, fullname)) {
            prrte_output(0, "%s Failed to symlink %s to %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), mypath, fullname);
            rc = PRRTE_ERROR;
        }
    }
    free(mypath);
    free(fullname);
    return rc;
}

static int raw_link_local_files(prrte_job_t *jdata,
                                prrte_app_context_t *app)
{
    char *session_dir, *path=NULL;
    prrte_proc_t *proc;
    int i, j, rc;
    prrte_filem_raw_incoming_t *inbnd;
    prrte_list_item_t *item;
    char **files=NULL, *bname, *filestring;

    /* check my jobfam session directory for files I have received and
     * symlink them to the proc-level session directory of each
     * local process in the job
     *
     * TODO: @rhc - please check that I've correctly interpret your
     *  intention here
     */
    session_dir = filem_session_dir();
    if( NULL == session_dir){
        /* we were unable to find any suitable directory */
        rc = PRRTE_ERR_BAD_PARAM;
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* get the list of files this app wants */
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_PRELOAD_FILES, (void**)&filestring, PRRTE_STRING)) {
        files = prrte_argv_split(filestring, ',');
        free(filestring);
    }
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_PRELOAD_BIN, NULL, PRRTE_BOOL)) {
        /* add the app itself to the list */
        bname = prrte_basename(app->app);
        prrte_argv_append_nosize(&files, bname);
        free(bname);
    }

    /* if there are no files to link, then ignore this */
    if (NULL == files) {
        return PRRTE_SUCCESS;
    }

    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            continue;
        }
        PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: working symlinks for proc %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name)));
        if (proc->name.jobid != jdata->jobid) {
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: proc %s not part of job %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 PRRTE_JOBID_PRINT(jdata->jobid)));
            continue;
        }
        if (proc->app_idx != app->idx) {
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: proc %s not part of app_idx %d",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 PRRTE_NAME_PRINT(&proc->name),
                                 (int)app->idx));
            continue;
        }
        /* ignore children we have already handled */
        if (PRRTE_FLAG_TEST(proc, PRRTE_PROC_FLAG_ALIVE) ||
            (PRRTE_PROC_STATE_INIT != proc->state &&
             PRRTE_PROC_STATE_RESTART != proc->state)) {
            continue;
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: creating symlinks for %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&proc->name)));

        /* get the session dir name in absolute form */
        path = prrte_process_info.proc_session_dir;

        /* create it, if it doesn't already exist */
        if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(path, S_IRWXU))) {
            PRRTE_ERROR_LOG(rc);
            /* doesn't exist with correct permissions, and/or we can't
             * create it - either way, we are done
             */
            free(files);
            return rc;
        }

        /* cycle thru the incoming files */
        for (item = prrte_list_get_first(&incoming_files);
             item != prrte_list_get_end(&incoming_files);
             item = prrte_list_get_next(item)) {
            inbnd = (prrte_filem_raw_incoming_t*)item;
            PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: checking file %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), inbnd->file));

            /* is this file for this app_context? */
            for (j=0; NULL != files[j]; j++) {
                if (0 == strcmp(inbnd->file, files[j])) {
                    /* this must be one of the files we are to link against */
                    if (NULL != inbnd->link_pts) {
                        PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                             "%s filem:raw: creating links for file %s",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             inbnd->file));
                        /* cycle thru the link points and create symlinks to them */
                        for (j=0; NULL != inbnd->link_pts[j]; j++) {
                            if (PRRTE_SUCCESS != (rc = create_link(session_dir, path, inbnd->link_pts[j]))) {
                                PRRTE_ERROR_LOG(rc);
                                free(files);
                                return rc;
                            }
                        }
                    } else {
                        PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                             "%s filem:raw: file %s has no link points",
                                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                             inbnd->file));
                    }
                    break;
                }
            }
        }
    }
    prrte_argv_free(files);
    return PRRTE_SUCCESS;
}

static void send_chunk(int fd, short argc, void *cbdata)
{
    prrte_filem_raw_xfer_t *rev = (prrte_filem_raw_xfer_t*)cbdata;
    unsigned char data[PRRTE_FILEM_RAW_CHUNK_MAX];
    int32_t numbytes;
    int rc;
    prrte_buffer_t chunk;
    prrte_grpcomm_signature_t *sig;

    PRRTE_ACQUIRE_OBJECT(rev);

    /* flag that event has fired */
    rev->pending = false;

    /* read up to the fragment size */
    numbytes = read(fd, data, sizeof(data));

    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */

        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            PRRTE_POST_OBJECT(rev);
            prrte_event_add(&rev->ev, 0);
            return;
        }

        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw:read error on file %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), rev->file));

        /* Un-recoverable error. Allow the code to flow as usual in order to
         * to send the zero bytes message up the stream, and then close the
         * file descriptor and delete the event.
         */
        numbytes = 0;
    }

    /* if job termination has been ordered, just ignore the
     * data and delete the read event
     */
    if (prrte_job_term_ordered) {
        PRRTE_RELEASE(rev);
        return;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw:read handler sending chunk %d of %d bytes for file %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         rev->nchunk, numbytes, rev->file));

    /* package it for transmission */
    PRRTE_CONSTRUCT(&chunk, prrte_buffer_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&chunk, &rev->file, 1, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        close(fd);
        return;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&chunk, &rev->nchunk, 1, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(rc);
        close(fd);
        return;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&chunk, data, numbytes, PRRTE_BYTE))) {
        PRRTE_ERROR_LOG(rc);
        close(fd);
        return;
    }
    /* if it is the first chunk, then add file type and index of the app */
    if (0 == rev->nchunk) {
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(&chunk, &rev->type, 1, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            close(fd);
            return;
        }
    }

    /* goes to all daemons */
    sig = PRRTE_NEW(prrte_grpcomm_signature_t);
    sig->signature = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    sig->signature[0].jobid = PRRTE_PROC_MY_NAME->jobid;
    sig->signature[0].vpid = PRRTE_VPID_WILDCARD;
    if (PRRTE_SUCCESS != (rc = prrte_grpcomm.xcast(sig, PRRTE_RML_TAG_FILEM_BASE, &chunk))) {
        PRRTE_ERROR_LOG(rc);
        close(fd);
        return;
    }
    PRRTE_DESTRUCT(&chunk);
    PRRTE_RELEASE(sig);
    rev->nchunk++;

    /* if num_bytes was zero, then we need to terminate the event
     * and close the file descriptor
     */
    if (0 == numbytes) {
        close(fd);
        return;
    } else {
        /* restart the read event */
        rev->pending = true;
        PRRTE_POST_OBJECT(rev);
        prrte_event_add(&rev->ev, 0);
    }
}

static void send_complete(char *file, int status)
{
    prrte_buffer_t *buf;
    int rc;

    buf = PRRTE_NEW(prrte_buffer_t);
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &file, 1, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buf, &status, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
        return;
    }
    if (0 > (rc = prrte_rml.send_buffer_nb(PRRTE_PROC_MY_HNP, buf,
                                          PRRTE_RML_TAG_FILEM_BASE_RESP,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(buf);
    }
}

/* This is a little tricky as the name of the archive doesn't
 * necessarily have anything to do with the paths inside it -
 * so we have to first query the archive to retrieve that info
 */
static int link_archive(prrte_filem_raw_incoming_t *inbnd)
{
    FILE *fp;
    char *cmd;
    char path[MAXPATHLEN];

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw: identifying links for archive %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         inbnd->fullpath));

    prrte_asprintf(&cmd, "tar tf %s", inbnd->fullpath);
    fp = popen(cmd, "r");
    free(cmd);
    if (NULL == fp) {
        PRRTE_ERROR_LOG(PRRTE_ERR_FILE_OPEN_FAILURE);
        return PRRTE_ERR_FILE_OPEN_FAILURE;
    }
    /* because app_contexts might share part or all of a
     * directory tree, but link to different files, we
     * have to link to each individual file
     */
    while (fgets(path, sizeof(path), fp) != NULL) {
        PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: path %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             path));
        /* protect against an empty result */
        if (0 == strlen(path)) {
            continue;
        }
        /* trim the trailing cr */
        path[strlen(path)-1] = '\0';
        /* ignore directories */
        if ('/' == path[strlen(path)-1]) {
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: path %s is a directory - ignoring it",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 path));
            continue;
        }
        /* ignore specific useless directory trees */
        if (NULL != strstr(path, ".deps")) {
            PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                                 "%s filem:raw: path %s includes .deps - ignoring it",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 path));
            continue;
        }
        PRRTE_OUTPUT_VERBOSE((10, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: adding path %s to link points",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             path));
        prrte_argv_append_nosize(&inbnd->link_pts, path);
    }
    /* close */
    pclose(fp);
    return PRRTE_SUCCESS;
}

static void recv_files(int status, prrte_process_name_t* sender,
                       prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                       void* cbdata)
{
    char *file, *session_dir;
    int32_t nchunk, n, nbytes;
    unsigned char data[PRRTE_FILEM_RAW_CHUNK_MAX];
    int rc;
    prrte_filem_raw_output_t *output;
    prrte_filem_raw_incoming_t *ptr, *incoming;
    prrte_list_item_t *item;
    int32_t type;
    char *cptr;

    /* unpack the data */
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &file, &n, PRRTE_STRING))) {
        PRRTE_ERROR_LOG(rc);
        send_complete(NULL, rc);
        return;
    }
    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &nchunk, &n, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(rc);
        send_complete(file, rc);
        free(file);
        return;
    }
    /* if the chunk number is < 0, then this is an EOF message */
    if (nchunk < 0) {
        /* just set nbytes to zero so we close the fd */
        nbytes = 0;
    } else {
        nbytes=PRRTE_FILEM_RAW_CHUNK_MAX;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, data, &nbytes, PRRTE_BYTE))) {
            PRRTE_ERROR_LOG(rc);
            send_complete(file, rc);
            free(file);
            return;
        }
    }
    /* if the chunk is 0, then additional info should be present */
    if (0 == nchunk) {
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &type, &n, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            send_complete(file, rc);
            free(file);
            return;
        }
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s filem:raw: received chunk %d for file %s containing %d bytes",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         nchunk, file, nbytes));

    /* do we already have this file on our list of incoming? */
    incoming = NULL;
    for (item = prrte_list_get_first(&incoming_files);
         item != prrte_list_get_end(&incoming_files);
         item = prrte_list_get_next(item)) {
        ptr = (prrte_filem_raw_incoming_t*)item;
        if (0 == strcmp(file, ptr->file)) {
            incoming = ptr;
            break;
        }
    }
    if (NULL == incoming) {
        /* nope - add it */
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: adding file %s to incoming list",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), file));
        incoming = PRRTE_NEW(prrte_filem_raw_incoming_t);
        incoming->file = strdup(file);
        incoming->type = type;
        prrte_list_append(&incoming_files, &incoming->super);
    }

    /* if this is the first chunk, we need to open the file descriptor */
    if (0 == nchunk) {
        /* separate out the top-level directory of the target */
        char *tmp;
        tmp = strdup(file);
        if (NULL != (cptr = strchr(tmp, '/'))) {
            *cptr = '\0';
        }
        /* save it */
        incoming->top = strdup(tmp);
        free(tmp);
        /* define the full path to where we will put it */
        session_dir = filem_session_dir();

        incoming->fullpath = prrte_os_path(false, session_dir, file, NULL);

        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s filem:raw: opening target file %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), incoming->fullpath));
        /* create the path to the target, if not already existing */
        tmp = prrte_dirname(incoming->fullpath);
        if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(tmp, S_IRWXU))) {
            PRRTE_ERROR_LOG(rc);
            send_complete(file, PRRTE_ERR_FILE_WRITE_FAILURE);
            free(file);
            free(tmp);
            PRRTE_RELEASE(incoming);
            return;
        }
        /* open the file descriptor for writing */
        if (PRRTE_FILEM_TYPE_EXE == type) {
            if (0 > (incoming->fd = open(incoming->fullpath, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU))) {
                prrte_output(0, "%s CANNOT CREATE FILE %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            incoming->fullpath);
                send_complete(file, PRRTE_ERR_FILE_WRITE_FAILURE);
                free(file);
                free(tmp);
                return;
            }
        } else {
            if (0 > (incoming->fd = open(incoming->fullpath, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR))) {
                prrte_output(0, "%s CANNOT CREATE FILE %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            incoming->fullpath);
                send_complete(file, PRRTE_ERR_FILE_WRITE_FAILURE);
                free(file);
                free(tmp);
                return;
            }
        }
        free(tmp);
        prrte_event_set(prrte_event_base, &incoming->ev, incoming->fd,
                       PRRTE_EV_WRITE, write_handler, incoming);
        prrte_event_set_priority(&incoming->ev, PRRTE_MSG_PRI);
    }
    /* create an output object for this data */
    output = PRRTE_NEW(prrte_filem_raw_output_t);
    if (0 < nbytes) {
        /* don't copy 0 bytes - we just need to pass
         * the zero bytes so the fd can be closed
         * after it writes everything out
         */
        memcpy(output->data, data, nbytes);
    }
    output->numbytes = nbytes;

    /* add this data to the write list for this fd */
    prrte_list_append(&incoming->outputs, &output->super);

    if (!incoming->pending) {
        /* add the event */
        incoming->pending = true;
        PRRTE_POST_OBJECT(incoming);
        prrte_event_add(&incoming->ev, 0);
    }

    /* cleanup */
    free(file);
}


static void write_handler(int fd, short event, void *cbdata)
{
    prrte_filem_raw_incoming_t *sink = (prrte_filem_raw_incoming_t*)cbdata;
    prrte_list_item_t *item;
    prrte_filem_raw_output_t *output;
    int num_written;
    char *dirname, *cmd;
    char homedir[MAXPATHLEN];
    int rc;

    PRRTE_ACQUIRE_OBJECT(sink);

    PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                         "%s write:handler writing data to %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         sink->fd));

    /* note that the event is off */
    sink->pending = false;

    while (NULL != (item = prrte_list_remove_first(&sink->outputs))) {
        output = (prrte_filem_raw_output_t*)item;
        if (0 == output->numbytes) {
            /* indicates we are to close this stream */
            PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                 "%s write:handler zero bytes - reporting complete for file %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 sink->file));
            /* close the file descriptor */
            close(sink->fd);
            sink->fd = -1;
            if (PRRTE_FILEM_TYPE_FILE == sink->type ||
                PRRTE_FILEM_TYPE_EXE == sink->type) {
                /* just link to the top as this will be the
                 * name we will want in each proc's session dir
                 */
                prrte_argv_append_nosize(&sink->link_pts, sink->top);
                send_complete(sink->file, PRRTE_SUCCESS);
            } else {
                /* unarchive the file */
                if (PRRTE_FILEM_TYPE_TAR == sink->type) {
                    prrte_asprintf(&cmd, "tar xf %s", sink->file);
                } else if (PRRTE_FILEM_TYPE_BZIP == sink->type) {
                    prrte_asprintf(&cmd, "tar xjf %s", sink->file);
                } else if (PRRTE_FILEM_TYPE_GZIP == sink->type) {
                    prrte_asprintf(&cmd, "tar xzf %s", sink->file);
                } else {
                    PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                if (NULL == getcwd(homedir, sizeof(homedir))) {
                    PRRTE_ERROR_LOG(PRRTE_ERROR);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                dirname = prrte_dirname(sink->fullpath);
                if (0 != chdir(dirname)) {
                    PRRTE_ERROR_LOG(PRRTE_ERROR);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                     "%s write:handler unarchiving file %s with cmd: %s",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     sink->file, cmd));
                if (0 != system(cmd)) {
                    PRRTE_ERROR_LOG(PRRTE_ERROR);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                if (0 != chdir(homedir)) {
                    PRRTE_ERROR_LOG(PRRTE_ERROR);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                free(dirname);
                free(cmd);
                /* setup the link points */
                if (PRRTE_SUCCESS != (rc = link_archive(sink))) {
                    PRRTE_ERROR_LOG(rc);
                    send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
                } else {
                    send_complete(sink->file, PRRTE_SUCCESS);
                }
            }
            return;
        }
        num_written = write(sink->fd, output->data, output->numbytes);
        PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                             "%s write:handler wrote %d bytes to file %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             num_written, sink->file));
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                prrte_list_prepend(&sink->outputs, item);
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                sink->pending = true;
                PRRTE_POST_OBJECT(sink);
                prrte_event_add(&sink->ev, 0);
                return;
            }
            /* otherwise, something bad happened so all we can do is abort
             * this attempt
             */
            PRRTE_OUTPUT_VERBOSE((1, prrte_filem_base_framework.framework_output,
                                 "%s write:handler error on write for file %s: %s",
                                 PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                 sink->file, strerror(errno)));
            PRRTE_RELEASE(output);
            prrte_list_remove_item(&incoming_files, &sink->super);
            send_complete(sink->file, PRRTE_ERR_FILE_WRITE_FAILURE);
            PRRTE_RELEASE(sink);
            return;
        } else if (num_written < output->numbytes) {
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* push this item back on the front of the list */
            prrte_list_prepend(&sink->outputs, item);
            /* leave the write event running so it will call us again
             * when the fd is ready
             */
            sink->pending = true;
            PRRTE_POST_OBJECT(sink);
            prrte_event_add(&sink->ev, 0);
            return;
        }
        PRRTE_RELEASE(output);
    }
}

static void xfer_construct(prrte_filem_raw_xfer_t *ptr)
{
    ptr->outbound = NULL;
    ptr->app_idx = 0;
    ptr->pending = false;
    ptr->src = NULL;
    ptr->file = NULL;
    ptr->nchunk = 0;
    ptr->status = PRRTE_SUCCESS;
    ptr->nrecvd = 0;
}
static void xfer_destruct(prrte_filem_raw_xfer_t *ptr)
{
    if (ptr->pending) {
        prrte_event_del(&ptr->ev);
    }
    if (NULL != ptr->src) {
        free(ptr->src);
    }
    if (NULL != ptr->file) {
        free(ptr->file);
    }
}
PRRTE_CLASS_INSTANCE(prrte_filem_raw_xfer_t,
                   prrte_list_item_t,
                   xfer_construct, xfer_destruct);

static void out_construct(prrte_filem_raw_outbound_t *ptr)
{
    PRRTE_CONSTRUCT(&ptr->xfers, prrte_list_t);
    ptr->status = PRRTE_SUCCESS;
    ptr->cbfunc = NULL;
    ptr->cbdata = NULL;
}
static void out_destruct(prrte_filem_raw_outbound_t *ptr)
{
    prrte_list_item_t *item;

    while (NULL != (item = prrte_list_remove_first(&ptr->xfers))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&ptr->xfers);
}
PRRTE_CLASS_INSTANCE(prrte_filem_raw_outbound_t,
                   prrte_list_item_t,
                   out_construct, out_destruct);

static void in_construct(prrte_filem_raw_incoming_t *ptr)
{
    ptr->app_idx = 0;
    ptr->pending = false;
    ptr->fd = -1;
    ptr->file = NULL;
    ptr->top = NULL;
    ptr->fullpath = NULL;
    ptr->link_pts = NULL;
    PRRTE_CONSTRUCT(&ptr->outputs, prrte_list_t);
}
static void in_destruct(prrte_filem_raw_incoming_t *ptr)
{
    prrte_list_item_t *item;

    if (ptr->pending) {
        prrte_event_del(&ptr->ev);
    }
    if (0 <= ptr->fd) {
        close(ptr->fd);
    }
    if (NULL != ptr->file) {
        free(ptr->file);
    }
    if (NULL != ptr->top) {
        free(ptr->top);
    }
    if (NULL != ptr->fullpath) {
        free(ptr->fullpath);
    }
    prrte_argv_free(ptr->link_pts);
    while (NULL != (item = prrte_list_remove_first(&ptr->outputs))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&ptr->outputs);
}
PRRTE_CLASS_INSTANCE(prrte_filem_raw_incoming_t,
                   prrte_list_item_t,
                   in_construct, in_destruct);

static void output_construct(prrte_filem_raw_output_t *ptr)
{
    ptr->numbytes = 0;
}
PRRTE_CLASS_INSTANCE(prrte_filem_raw_output_t,
                   prrte_list_item_t,
                   output_construct, NULL);
