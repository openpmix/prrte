/*
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 *
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"

#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

#include "filem_raw.h"

static int raw_init(void);
static int raw_finalize(void);
static void raw_fault_handler(const prte_rml_recovery_status_t *status);
static int raw_preposition_files(prte_job_t *jdata, prte_filem_completion_cbfunc_t cbfunc,
                                 void *cbdata);
static int raw_link_local_files(prte_job_t *jdata, prte_app_context_t *app);

prte_filem_base_module_t prte_filem_raw_module = {.filem_init = raw_init,
                                                  .filem_finalize = raw_finalize,
                                                  .fault_handler = raw_fault_handler,
                                                  /* we don't use any of the following */
                                                  .put = prte_filem_base_none_put,
                                                  .put_nb = prte_filem_base_none_put_nb,
                                                  .get = prte_filem_base_none_get,
                                                  .get_nb = prte_filem_base_none_get_nb,
                                                  .rm = prte_filem_base_none_rm,
                                                  .rm_nb = prte_filem_base_none_rm_nb,
                                                  .wait = prte_filem_base_none_wait,
                                                  .wait_all = prte_filem_base_none_wait_all,
                                                  /* now the APIs we *do* use */
                                                  .preposition_files = raw_preposition_files,
                                                  .link_local_files = raw_link_local_files};

static pmix_list_t outbound_files;
static pmix_list_t incoming_files;
static pmix_list_t positioned_files;

static void send_chunk(int fd, short argc, void *cbdata);
static void recv_files(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                       prte_rml_tag_t tag, void *cbdata);
static void recv_ack(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                     prte_rml_tag_t tag, void *cbdata);
static void write_handler(int fd, short event, void *cbdata);

static int raw_init(void)
{
    PMIX_CONSTRUCT(&incoming_files, pmix_list_t);

    /* start a recv to catch any files sent to me */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FILEM_BASE,
                  PRTE_RML_PERSISTENT, recv_files, NULL);

    /* if I'm the HNP, start a recv to catch acks sent to me */
    if (PRTE_PROC_IS_MASTER) {
        PMIX_CONSTRUCT(&outbound_files, pmix_list_t);
        PMIX_CONSTRUCT(&positioned_files, pmix_list_t);
        PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FILEM_BASE_RESP,
                      PRTE_RML_PERSISTENT, recv_ack, NULL);
    }

    return PRTE_SUCCESS;
}

static int raw_finalize(void)
{
    pmix_list_item_t *item;

    while (NULL != (item = pmix_list_remove_first(&incoming_files))) {
        PMIX_RELEASE(item);
    }
    PMIX_DESTRUCT(&incoming_files);

    if (PRTE_PROC_IS_MASTER) {
        while (NULL != (item = pmix_list_remove_first(&outbound_files))) {
            PMIX_RELEASE(item);
        }
        PMIX_DESTRUCT(&outbound_files);
        while (NULL != (item = pmix_list_remove_first(&positioned_files))) {
            PMIX_RELEASE(item);
        }
        PMIX_DESTRUCT(&positioned_files);
    }

    return PRTE_SUCCESS;
}

static void raw_fault_handler(const prte_rml_recovery_status_t *status){
    PRTE_HIDE_UNUSED_PARAMS(status);
    /* TODO: Make this actually resilient. Seems pretty trivial, since xcast is
     * already resilient */
    if (0 < pmix_list_get_size(&incoming_files) ||
        0 < pmix_list_get_size(&outbound_files)) {
        PMIX_OUTPUT_VERBOSE((0, prte_filem_base_framework.framework_output,
                             "%s filem:raw daemon failed during active file"
                             " transfer operation(s)",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED);
    }
}

static void xfer_complete(int status, prte_filem_raw_xfer_t *xfer)
{
    prte_filem_raw_outbound_t *outbound = xfer->outbound;

    /* transfer the status, if not success */
    if (PRTE_SUCCESS != status) {
        outbound->status = status;
    }

    /* this transfer is complete - remove it from list */
    pmix_list_remove_item(&outbound->xfers, &xfer->super);
    /* add it to the list of files that have been positioned */
    pmix_list_append(&positioned_files, &xfer->super);

    /* if the list is now empty, then the xfer is complete */
    if (0 == pmix_list_get_size(&outbound->xfers)) {
        /* do the callback */
        if (NULL != outbound->cbfunc) {
            outbound->cbfunc(outbound->status, outbound->cbdata);
        }
        /* release the object */
        pmix_list_remove_item(&outbound_files, &outbound->super);
        PMIX_RELEASE(outbound);
    }
}

static void recv_ack(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                     prte_rml_tag_t tag, void *cbdata)
{
    pmix_list_item_t *item, *itm;
    prte_filem_raw_outbound_t *outbound;
    prte_filem_raw_xfer_t *xfer;
    char *file;
    int st, n, rc;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    /* unpack the file */
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &file, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the status */
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: recvd ack from %s for file %s status %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender), file, st));

    /* find the corresponding outbound object */
    for (item = pmix_list_get_first(&outbound_files); item != pmix_list_get_end(&outbound_files);
         item = pmix_list_get_next(item)) {
        outbound = (prte_filem_raw_outbound_t *) item;
        for (itm = pmix_list_get_first(&outbound->xfers);
             itm != pmix_list_get_end(&outbound->xfers); itm = pmix_list_get_next(itm)) {
            xfer = (prte_filem_raw_xfer_t *) itm;
            if (0 == strcmp(file, xfer->file)) {
                /* if the status isn't success, record it */
                if (0 != st) {
                    xfer->status = st;
                }
                /* track number of respondents */
                xfer->nrecvd++;
                /* if all daemons have responded, then this is complete */
                if (xfer->nrecvd == prte_process_info.num_daemons) {
                    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                         "%s filem:raw: xfer complete for file %s status %d",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), file, xfer->status));
                    xfer_complete(xfer->status, xfer);
                }
                free(file);
                return;
            }
        }
    }
    /* no matching xfer found (e.g. the file was already fully positioned
     * and removed) - avoid leaking the unpacked filename
     */
    free(file);
}

static int raw_preposition_files(prte_job_t *jdata,
                                 prte_filem_completion_cbfunc_t cbfunc,
                                 void *cbdata)
{
    prte_app_context_t *app;
    pmix_list_item_t *item, *itm, *itm2;
    prte_filem_base_file_set_t *fs;
    int fd;
    prte_filem_raw_xfer_t *xfer, *xptr;
    int flags, i, j;
    char **files = NULL;
    prte_filem_raw_outbound_t *outbound, *optr;
    char *cptr, *nxt, *filestring;
    pmix_list_t fsets;
    bool already_sent;

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: preposition files for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));

    /* cycle across the app_contexts looking for files or
     * binaries to be prepositioned
     */
    PMIX_CONSTRUCT(&fsets, pmix_list_t);
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }

        if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, NULL, PMIX_BOOL)) {
            /* add the executable to our list */
            PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: preload executable %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), app->app));
            fs = PMIX_NEW(prte_filem_base_file_set_t);
            fs->local_target = strdup(app->app);
            fs->target_flag = PRTE_FILEM_TYPE_EXE;
            pmix_list_append(&fsets, &fs->super);
            /* if we are preloading the binary, then the app must be in relative
             * syntax or we won't find it - the binary will be positioned in the
             * session dir, so ensure the app is relative to that location
             */
            cptr = pmix_basename(app->app);
            free(app->app);
            pmix_asprintf(&app->app, "./%s", cptr);
            if (NULL == app->app) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            free(app->argv[0]);
            app->argv[0] = strdup(app->app);
            /* the delivered name is what the app will open, relative to
             * the directory it lands in - so it carries no leading "./"
             */
            fs->remote_target = strdup(prte_filem_base_strip_leading_dots(app->app));
            /* ensure the app uses that location as its cwd */
            prte_set_attribute(&app->attributes, PRTE_APP_SSNDIR_CWD,
                               PRTE_ATTR_GLOBAL, NULL, PMIX_BOOL);
        }

        filestring = NULL;
        if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES,
                               (void **) &filestring, PMIX_STRING) &&
            NULL != filestring) {
            files = PMIx_Argv_split(filestring, ',');
            free(filestring);
            for (j = 0; NULL != files[j]; j++) {
                fs = PMIX_NEW(prte_filem_base_file_set_t);
                fs->local_target = strdup(files[j]);
                /* check any suffix for file type */
                if (NULL != (cptr = strchr(files[j], '.'))) {
                    if (0 == strncmp(cptr, ".tar", 4)) {
                        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as TAR",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), files[j]));
                        fs->target_flag = PRTE_FILEM_TYPE_TAR;
                    } else if (0 == strncmp(cptr, ".bz", 3)) {
                        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as BZIP",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), files[j]));
                        fs->target_flag = PRTE_FILEM_TYPE_BZIP;
                    } else if (0 == strncmp(cptr, ".gz", 3)) {
                        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                             "%s filem:raw: marking file %s as GZIP",
                                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), files[j]));
                        fs->target_flag = PRTE_FILEM_TYPE_GZIP;
                    } else {
                        fs->target_flag = PRTE_FILEM_TYPE_FILE;
                    }
                } else {
                    fs->target_flag = PRTE_FILEM_TYPE_FILE;
                }
                /* if we are flattening directory trees, then the
                 * remote path is just the basename file name
                 */
                if (prte_filem_raw_flatten_trees) {
                    fs->remote_target = pmix_basename(files[j]);
                } else {
                    /* the file is going to be placed in the working
                     * directory of the processes that asked for it, so the
                     * name it lands under has to be a relative one - we do
                     * not allow positioning of files to absolute locations
                     * due to the potential for unintentional overwriting
                     * of files. A relative specification is preserved as
                     * given, since that is the name the app will use to
                     * open it. An absolute one cannot be: recreating its
                     * source tree ("/home/me/f.dat" -> "./home/me/f.dat")
                     * puts the file somewhere the app never asked for and
                     * scatters directories through the user's working
                     * directory, so it is placed under its basename.
                     */
                    if (pmix_path_is_absolute(files[j])) {
                        fs->remote_target = pmix_basename(files[j]);
                    } else {
                        fs->remote_target = strdup(files[j]);
                    }
                }
                /* strip any leading '.' directories to avoid stepping
                 * above the session dir location - all files will be
                 * relative to that point. Normalize the file set's own
                 * copy, so the name we check for clashes, the name we
                 * broadcast, and the name we hand back to the app are
                 * all one and the same
                 */
                cptr = (char *) prte_filem_base_strip_leading_dots(fs->remote_target);
                if (cptr != fs->remote_target) {
                    nxt = strdup(cptr);
                    free(fs->remote_target);
                    fs->remote_target = nxt;
                }
                pmix_list_append(&fsets, &fs->super);
                /* prep the filename for matching on the remote end */
                free(files[j]);
                files[j] = strdup(fs->remote_target);
            }
            /* replace the app's file list with the revised one so we
             * can find them on the remote end
             */
            filestring = PMIx_Argv_join(files, ',');
            prte_set_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES, PRTE_ATTR_GLOBAL,
                               filestring, PMIX_STRING);
            /* cleanup for the next app */
            PMIx_Argv_free(files);
            free(filestring);
        }
    }
    /* Every delivered name has to be a plain relative path *under* the
     * directory the file lands in. Stripping the leading dot directories
     * above is not enough to guarantee that: a ".." anywhere else in the
     * path ("a/../../f") still steps above the session directory on every
     * node, where the file would be created with O_TRUNC over whatever is
     * already there. Refuse the request rather than honor it.
     */
    for (item = pmix_list_get_first(&fsets); item != pmix_list_get_end(&fsets);
         item = pmix_list_get_next(item)) {
        fs = (prte_filem_base_file_set_t *) item;
        if (NULL == fs->remote_target || '\0' == fs->remote_target[0] ||
            0 == strcmp(fs->remote_target, ".") ||
            prte_filem_base_has_dotdot(fs->remote_target)) {
            pmix_show_help("help-prte-filem-raw.txt", "preload-bad-path", true,
                           fs->local_target);
            PMIX_LIST_DESTRUCT(&fsets);
            if (NULL != cbfunc) {
                cbfunc(PRTE_ERR_BAD_PARAM, cbdata);
            }
            return PRTE_SUCCESS;
        }
    }

    /* Two different files that would be delivered under the same name in
     * the working directory cannot both be delivered - the second would
     * overwrite the first, silently, and whichever app opened that name
     * would get whichever one happened to be staged last. Say so here
     * rather than picking one: this is the same refusal the daemon makes
     * when the name collides with something already in the directory.
     */
    for (item = pmix_list_get_first(&fsets); item != pmix_list_get_end(&fsets);
         item = pmix_list_get_next(item)) {
        fs = (prte_filem_base_file_set_t *) item;
        for (itm = pmix_list_get_next(item); itm != pmix_list_get_end(&fsets);
             itm = pmix_list_get_next(itm)) {
            prte_filem_base_file_set_t *fs2 = (prte_filem_base_file_set_t *) itm;
            if (0 == strcmp(fs->remote_target, fs2->remote_target) &&
                0 != strcmp(fs->local_target, fs2->local_target)) {
                pmix_show_help("help-prte-filem-raw.txt", "preload-name-clash", true,
                               fs->local_target, fs2->local_target, fs->remote_target);
                PMIX_DESTRUCT(&fsets);
                if (NULL != cbfunc) {
                    cbfunc(PRTE_ERR_PRELOAD_CONFLICT, cbdata);
                }
                return PRTE_SUCCESS;
            }
        }
    }

    if (0 == pmix_list_get_size(&fsets)) {
        /* nothing to preposition */
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: nothing to preposition",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        if (NULL != cbfunc) {
            cbfunc(PRTE_SUCCESS, cbdata);
        }
        PMIX_DESTRUCT(&fsets);
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: found %d files to position",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) pmix_list_get_size(&fsets)));

    /* track the outbound file sets */
    outbound = PMIX_NEW(prte_filem_raw_outbound_t);
    outbound->cbfunc = cbfunc;
    outbound->cbdata = cbdata;
    pmix_list_append(&outbound_files, &outbound->super);

    /* only the HNP should ever call this function - loop thru the
     * fileset and initiate xcast transfer of each file to every
     * daemon
     */
    while (NULL != (item = pmix_list_remove_first(&fsets))) {
        fs = (prte_filem_base_file_set_t *) item;
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: checking prepositioning of file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fs->local_target));

        /* have we already sent this file? */
        already_sent = false;
        for (itm = pmix_list_get_first(&positioned_files);
             !already_sent && itm != pmix_list_get_end(&positioned_files);
             itm = pmix_list_get_next(itm)) {
            xptr = (prte_filem_raw_xfer_t *) itm;
            if (0 == strcmp(fs->local_target, xptr->src)) {
                already_sent = true;
            }
        }
        if (already_sent) {
            /* no need to send it again */
            PMIX_OUTPUT_VERBOSE((3, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: file %s is already in position - ignoring",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fs->local_target));
            PMIX_RELEASE(item);
            continue;
        }
        /* also have to check if this file is already in the process
         * of being transferred, or was included multiple times
         * for transfer
         */
        for (itm = pmix_list_get_first(&outbound_files);
             !already_sent && itm != pmix_list_get_end(&outbound_files);
             itm = pmix_list_get_next(itm)) {
            optr = (prte_filem_raw_outbound_t *) itm;
            for (itm2 = pmix_list_get_first(&optr->xfers); itm2 != pmix_list_get_end(&optr->xfers);
                 itm2 = pmix_list_get_next(itm2)) {
                xptr = (prte_filem_raw_xfer_t *) itm2;
                if (0 == strcmp(fs->local_target, xptr->src)) {
                    already_sent = true;
                }
            }
        }
        if (already_sent) {
            /* no need to send it again */
            PMIX_OUTPUT_VERBOSE((3, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: file %s is already queued for output - ignoring",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fs->local_target));
            PMIX_RELEASE(item);
            continue;
        }

        /* attempt to open the specified file */
        if (0 > (fd = open(fs->local_target, O_RDONLY))) {
            pmix_output(0, "%s CANNOT ACCESS FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        fs->local_target);
            PMIX_RELEASE(item);
            /* record the failure and stop queueing further files, but do NOT
             * tear down the outbound here: files already handed to send_chunk
             * still have live events queued on the progress thread, and their
             * xfers hold open fds. Freeing the outbound now would drain those
             * xfers (use-after-free on the queued events, leaked fds) and
             * would also double-drive FILES_POSN_FAILED, since the caller
             * treats our error return as a failure *and* the completion
             * callback would fire again. Instead let the async path report
             * the error: the callback delivers outbound->status once every
             * queued xfer has acked (or immediately below if none were
             * queued).
             */
            outbound->status = PRTE_ERR_FILE_OPEN_FAILURE;
            break;
        }
        /* set the flags to non-blocking */
        if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
            pmix_output(prte_filem_base_framework.framework_output,
                        "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n", __FILE__, __LINE__,
                        errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(fd, F_SETFL, flags) < 0) {
                pmix_output(prte_filem_base_framework.framework_output,
                            "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n", __FILE__, __LINE__,
                            errno);
            }
        }
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: setting up to position file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fs->local_target));
        xfer = PMIX_NEW(prte_filem_raw_xfer_t);
        /* save the source so we can avoid duplicate transfers */
        xfer->src = strdup(fs->local_target);
        xfer->fd = fd;
        /* remote_target was normalized and validated above - it is already
         * the relative name the receiving daemon is to write it under
         */
        xfer->file = strdup(fs->remote_target);
        xfer->type = fs->target_flag;
        xfer->app_idx = fs->app_idx;
        xfer->outbound = outbound;
        pmix_list_append(&outbound->xfers, &xfer->super);
        PRTE_PMIX_THREADSHIFT(xfer, prte_event_base, send_chunk);
        PMIX_RELEASE(item);
    }
    /* a mid-loop break (open failure) can leave entries on fsets, so use
     * PMIX_LIST_DESTRUCT to release any that remain
     */
    PMIX_LIST_DESTRUCT(&fsets);

    /* check to see if anything remains to be sent - the list is empty if
     * every file was a duplicate, or if the very first file failed to open
     */
    if (0 == pmix_list_get_size(&outbound->xfers)) {
        /* capture the status before releasing the outbound: it is a failure
         * only if an open() failed above, success if all were duplicates
         */
        int st = outbound->status;
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: nothing to position (all duplicates or open failed)",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        pmix_list_remove_item(&outbound_files, &outbound->super);
        PMIX_RELEASE(outbound);
        if (NULL != cbfunc) {
            cbfunc(st, cbdata);
        }
        return PRTE_SUCCESS;
    }

    if (0 < pmix_output_get_verbosity(prte_filem_base_framework.framework_output)) {
        pmix_output(0, "%s Files to be positioned:", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        for (itm2 = pmix_list_get_first(&outbound->xfers);
             itm2 != pmix_list_get_end(&outbound->xfers); itm2 = pmix_list_get_next(itm2)) {
            xptr = (prte_filem_raw_xfer_t *) itm2;
            pmix_output(0, "%s\t%s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), xptr->src);
        }
    }

    return PRTE_SUCCESS;
}

/* read up to len bytes, tolerating short reads and signals - returns the
 * number of bytes actually read (less than len only at EOF), or -1
 */
static ssize_t read_bytes(int fd, char *buf, size_t len)
{
    size_t total = 0;
    ssize_t nb;

    while (total < len) {
        nb = read(fd, &buf[total], len - total);
        if (0 > nb) {
            if (EINTR == errno) {
                continue;
            }
            return -1;
        }
        if (0 == nb) {
            break;
        }
        total += nb;
    }
    return (ssize_t) total;
}

static int write_bytes(int fd, char *buf, size_t len)
{
    size_t total = 0;
    ssize_t nb;

    while (total < len) {
        nb = write(fd, &buf[total], len - total);
        if (0 > nb) {
            if (EINTR == errno) {
                continue;
            }
            return PRTE_ERR_FILE_WRITE_FAILURE;
        }
        total += nb;
    }
    return PRTE_SUCCESS;
}

/* Is what already sits at "dest" byte-for-byte what we were about to put
 * there?
 *
 * This is the test that separates "the file would overwrite itself" from
 * "the file would overwrite the user's data". It answers yes for every way
 * the same staged file can legitimately arrive at the same destination
 * twice - a second app or a second job in this session sharing the working
 * directory, a working directory shared across nodes where another daemon
 * placed the identical bytes, or the user's own copy of the very file they
 * asked us to stage sitting in the directory they launched from. It answers
 * no for anything else, which is then somebody else's data.
 */
static bool same_contents(char *src, char *dest)
{
    int fsrc, fdest;
    struct stat sbuf, dbuf;
    char sdata[PRTE_FILEM_RAW_COPY_MAX], ddata[PRTE_FILEM_RAW_COPY_MAX];
    ssize_t ns, nd;
    bool same = false;

    if (0 > (fsrc = open(src, O_RDONLY))) {
        return false;
    }
    /* Deliberately follows a symlink at the destination: a link pointing
     * at an identical file is as much "already there" as the file itself.
     * O_NONBLOCK because we have no idea what is sitting there - opening a
     * fifo for reading would otherwise park the progress thread until
     * somebody opened the other end. The fstat below is what actually
     * decides; anything that is not a plain file is simply "not ours".
     */
    if (0 > (fdest = open(dest, O_RDONLY | O_NONBLOCK))) {
        close(fsrc);
        return false;
    }
    if (0 != fstat(fsrc, &sbuf) || 0 != fstat(fdest, &dbuf) ||
        !S_ISREG(dbuf.st_mode) || sbuf.st_size != dbuf.st_size) {
        goto done;
    }
    while (1) {
        ns = read_bytes(fsrc, sdata, sizeof(sdata));
        nd = read_bytes(fdest, ddata, sizeof(ddata));
        if (0 > ns || 0 > nd || ns != nd) {
            goto done;
        }
        if (0 == ns) {
            /* both hit EOF having matched all the way */
            same = true;
            goto done;
        }
        if (0 != memcmp(sdata, ddata, ns)) {
            goto done;
        }
    }

done:
    close(fsrc);
    close(fdest);
    return same;
}

/* Put a staged file into the working directory the app will run in.
 *
 * A copy, not a symlink: the bytes were written into this node's session
 * directory, which is not a path any other node can resolve (nor one that
 * outlives the DVM), so a link would be dangling for every proc whose
 * working directory is on a shared filesystem written by some other
 * daemon. Copying is also what "preload the files to the remote machine's
 * working directory" has always claimed to do.
 *
 * We refuse to overwrite anything that is not already exactly what we
 * would have written - see same_contents().
 */
static int place_file(char *my_dir, char *wdir, char *fname)
{
    char *src = NULL, *dest = NULL, *tmpname = NULL, *basedir;
    struct stat sbuf;
    mode_t mode;
    char data[PRTE_FILEM_RAW_COPY_MAX];
    int fsrc = -1, fdest = -1;
    ssize_t nb;
    int rc = PRTE_SUCCESS;

    src = pmix_os_path(false, my_dir, fname, NULL);
    dest = pmix_os_path(false, wdir, fname, NULL);
    if (NULL == src || NULL == dest) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    if (0 == lstat(dest, &sbuf)) {
        if (same_contents(src, dest)) {
            PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: %s is already in place at %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fname, dest));
            goto cleanup;
        }
        pmix_show_help("help-prte-filem-raw.txt", "preload-collision", true,
                       fname, wdir, prte_process_info.nodename);
        rc = PRTE_ERR_PRELOAD_CONFLICT;
        goto cleanup;
    }

    PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                         "%s filem:raw: placing %s in %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), fname, wdir));

    /* create any directories the file is to sit under - but only if they
     * are not already there. pmix_os_dirpath_create() chmods a path that
     * already exists to the mode it was given, and the working directory
     * here belongs to the user: handing it their own cwd would silently
     * reduce it to 0700. Directories we do create are left to the user's
     * umask, like any other directory made on their behalf.
     */
    basedir = pmix_dirname(dest);
    if (NULL != basedir && 0 != stat(basedir, &sbuf)) {
        rc = pmix_os_dirpath_create(basedir, S_IRWXU | S_IRWXG | S_IRWXO);
        if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
            PMIX_ERROR_LOG(rc);
            rc = prte_pmix_convert_status(rc);
            free(basedir);
            goto cleanup;
        }
        rc = PRTE_SUCCESS;
    }
    free(basedir);

    if (0 > (fsrc = open(src, O_RDONLY))) {
        pmix_output(0, "%s CANNOT ACCESS STAGED FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), src);
        rc = PRTE_ERR_FILE_OPEN_FAILURE;
        goto cleanup;
    }
    /* the staging path knows only whether the file was executable, so that
     * is all we carry across: give the copy the user's default permissions
     * for a file of that kind by letting their umask trim it
     */
    mode = S_IRUSR | S_IWUSR;
    if (0 == fstat(fsrc, &sbuf) && 0 != (sbuf.st_mode & S_IXUSR)) {
        mode |= S_IXUSR;
    }
    mode |= (mode >> 3) | (mode >> 6);
    /* write a temporary alongside the target and rename it into place. The
     * rename is atomic, so a proc never sees a half-written file - which
     * matters when the working directory is shared and several daemons are
     * placing the identical file at the same moment.
     */
    pmix_asprintf(&tmpname, "%s.prte-tmp.%lu", dest, (unsigned long) getpid());
    if (NULL == tmpname) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 > (fdest = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, mode))) {
        pmix_output(0, "%s CANNOT CREATE FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tmpname);
        rc = PRTE_ERR_FILE_OPEN_FAILURE;
        free(tmpname);
        tmpname = NULL;
        goto cleanup;
    }
    while (0 < (nb = read_bytes(fsrc, data, sizeof(data)))) {
        if (PRTE_SUCCESS != (rc = write_bytes(fdest, data, (size_t) nb))) {
            pmix_output(0, "%s FAILED TO WRITE FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), dest);
            goto cleanup;
        }
    }
    if (0 > nb) {
        pmix_output(0, "%s FAILED TO READ STAGED FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), src);
        rc = PRTE_ERR_FILE_READ_FAILURE;
        goto cleanup;
    }
    close(fdest);
    fdest = -1;
    if (0 != rename(tmpname, dest)) {
        pmix_output(0, "%s FAILED TO PLACE FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), dest);
        rc = PRTE_ERR_FILE_WRITE_FAILURE;
        goto cleanup;
    }
    free(tmpname);
    tmpname = NULL;

cleanup:
    if (0 <= fsrc) {
        close(fsrc);
    }
    if (0 <= fdest) {
        close(fdest);
    }
    if (NULL != tmpname) {
        /* we never got it into place */
        unlink(tmpname);
        free(tmpname);
    }
    free(src);
    free(dest);
    return rc;
}

static int create_link(char *my_dir, char *path, char *link_pt)
{
    char *mypath, *fullname, *basedir;
    struct stat buf;
    int rc = PRTE_SUCCESS;

    /* form the full source path name */
    mypath = pmix_os_path(false, my_dir, link_pt, NULL);
    /* form the full target path name */
    fullname = pmix_os_path(false, path, link_pt, NULL);
    /* there may have been multiple files placed under the
     * same directory, so check for existence first
     */
    if (0 != stat(fullname, &buf)) {
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: creating symlink to %s\n\tmypath: %s\n\tlink: %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), link_pt, mypath, fullname));
        /* create any required path to the link location */
        basedir = pmix_dirname(fullname);
        rc = pmix_os_dirpath_create(basedir, S_IRWXU);
        if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
            PMIX_ERROR_LOG(rc);
            pmix_output(0, "%s Failed to symlink %s to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        mypath, fullname);
            free(basedir);
            free(mypath);
            free(fullname);
            rc = prte_pmix_convert_status(rc);
            return rc;
        }
        free(basedir);
        /* the directory now exists (freshly created or already present) -
         * clear the PMIX_ERR_EXISTS that dirpath_create may have left in
         * rc, else a successful link is reported to the caller as a failure
         */
        rc = PRTE_SUCCESS;
        /* do the symlink */
        if (0 != symlink(mypath, fullname)) {
            pmix_output(0, "%s Failed to symlink %s to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        mypath, fullname);
            rc = PRTE_ERROR;
        }
    }
    free(mypath);
    free(fullname);
    return rc;
}

static int raw_link_local_files(prte_job_t *jdata, prte_app_context_t *app)
{
    char *wdir;
    prte_proc_t *proc;
    int i, j, k, rc;
    bool launching = false;
    prte_filem_raw_incoming_t *inbnd;
    pmix_list_item_t *item;
    char **files = NULL, *bname, *filestring;

    /* The files this app asked us to preload go where the app will look
     * for them: its working directory. odls resolved that immediately
     * before calling us (setup_path, which has already chdir'd there), so
     * app->cwd is the directory every one of this app's procs on this node
     * will start in - whether that is the user's cwd, a -wdir value, or
     * the session directory a --preload-binary/--set-cwd-to-session-dir
     * job runs from. It is a per-app value, so the files are placed once
     * here and serve every local proc of the app.
     */
    wdir = app->cwd;
    if (NULL == wdir) {
        /* we have no idea where the procs will run */
        rc = PRTE_ERR_BAD_PARAM;
        PRTE_ERROR_LOG(rc);
        return rc;
    }
    /* the staged bytes were written under this node's top session dir
     * (see recv_files), so that is where every placement comes from
     */
    if (NULL == prte_process_info.top_session_dir) {
        rc = PRTE_ERR_BAD_PARAM;
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* get the list of files this app wants */
    filestring = NULL;
    if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES,
                           (void **) &filestring, PMIX_STRING) &&
        NULL != filestring) {
        files = PMIx_Argv_split(filestring, ',');
        free(filestring);
    }
    if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, NULL, PMIX_BOOL)) {
        /* add the app itself to the list */
        bname = pmix_basename(app->app);
        PMIx_Argv_append_nosize(&files, bname);
        free(bname);
    }

    /* if there are no files to link, then ignore this */
    if (NULL == files) {
        return PRTE_SUCCESS;
    }

    /* only do this if we actually have a proc of this app to launch here -
     * there is no reason to touch the user's working directory otherwise
     */
    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL == (proc = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i))) {
            continue;
        }
        if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace) ||
            proc->app_idx != app->idx) {
            continue;
        }
        /* ignore children we have already handled */
        if (PRTE_FLAG_TEST(proc, PRTE_PROC_FLAG_ALIVE) ||
            (PRTE_PROC_STATE_INIT != proc->state && PRTE_PROC_STATE_RESTART != proc->state)) {
            continue;
        }
        launching = true;
        break;
    }
    if (!launching) {
        PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                             "%s filem:raw: no procs of app %d to launch for job %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) app->idx,
                             PRTE_JOBID_PRINT(jdata->nspace)));
        PMIx_Argv_free(files);
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: placing preloaded files for job %s in %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace),
                         wdir));

    /* cycle thru the incoming files */
    for (item = pmix_list_get_first(&incoming_files);
         item != pmix_list_get_end(&incoming_files); item = pmix_list_get_next(item)) {
        inbnd = (prte_filem_raw_incoming_t *) item;
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: checking file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), inbnd->file));

        /* is this file for this app_context? */
        for (j = 0; NULL != files[j]; j++) {
            if (0 != strcmp(inbnd->file, files[j])) {
                continue;
            }
            /* this must be one of the files we are to place */
            if (NULL == inbnd->link_pts) {
                PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                                     "%s filem:raw: file %s has no link points",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), inbnd->file));
                break;
            }
            for (k = 0; NULL != inbnd->link_pts[k]; k++) {
                /* A preloaded binary is the one thing we link instead of
                 * copy: --preload-binary sets PRTE_APP_SSNDIR_CWD, so the
                 * working directory resolved above is this job's own
                 * session directory on this node - never shared, never
                 * outliving the job - and a link there costs nothing where
                 * a second copy of an executable can cost a great deal.
                 */
                if (PRTE_FILEM_TYPE_EXE == inbnd->type) {
                    rc = create_link(prte_process_info.top_session_dir, wdir, inbnd->link_pts[k]);
                } else {
                    rc = place_file(prte_process_info.top_session_dir, wdir, inbnd->link_pts[k]);
                }
                if (PRTE_SUCCESS != rc) {
                    if (PRTE_ERR_PRELOAD_CONFLICT != rc) {
                        /* the collision already said its piece */
                        PRTE_ERROR_LOG(rc);
                    }
                    PMIx_Argv_free(files);
                    return rc;
                }
            }
            break;
        }
    }
    PMIx_Argv_free(files);
    return PRTE_SUCCESS;
}

static void send_chunk(int xxx, short argc, void *cbdata)
{
    prte_filem_raw_xfer_t *rev = (prte_filem_raw_xfer_t *) cbdata;
    int fd = rev->fd;
    unsigned char data[PRTE_FILEM_RAW_CHUNK_MAX];
    int32_t numbytes;
    int rc;
    pmix_data_buffer_t chunk;
    PRTE_HIDE_UNUSED_PARAMS(xxx, argc);

    PMIX_ACQUIRE_OBJECT(rev);

    /* read up to the fragment size */
    numbytes = read(fd, data, sizeof(data));

    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */

        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            PMIX_POST_OBJECT(rev);
            prte_event_add(&rev->ev, 0);
            return;
        }

        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw:read error %s(%d) on file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             strerror(errno), errno, rev->file));

        /* Un-recoverable error. Allow the code to flow as usual in order to
         * to send the zero bytes message up the stream, and then close the
         * file descriptor and delete the event.
         */
        numbytes = 0;
    }

    /* if job termination has been ordered, just ignore the
     * data and delete the read event
     */
    if (prte_dvm_abort_ordered) {
        PMIX_RELEASE(rev);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw:read handler sending chunk %d of %d bytes for file %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), rev->nchunk, numbytes, rev->file));

    /* package it for transmission */
    PMIX_DATA_BUFFER_CONSTRUCT(&chunk);
    rc = PMIx_Data_pack(NULL, &chunk, &rev->file, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        close(fd);
        PMIX_DATA_BUFFER_DESTRUCT(&chunk);
        return;
    }
    rc = PMIx_Data_pack(NULL, &chunk, &rev->nchunk, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        close(fd);
        PMIX_DATA_BUFFER_DESTRUCT(&chunk);
        return;
    }
    rc = PMIx_Data_pack(NULL, &chunk, data, numbytes, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        close(fd);
        PMIX_DATA_BUFFER_DESTRUCT(&chunk);
        return;
    }
    /* if it is the first chunk, then add file type and index of the app */
    if (0 == rev->nchunk) {
        rc = PMIx_Data_pack(NULL, &chunk, &rev->type, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            close(fd);
            PMIX_DATA_BUFFER_DESTRUCT(&chunk);
            return;
        }
    }

    /* goes to all daemons */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(PRTE_RML_TAG_FILEM_BASE, &chunk))) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&chunk);
        close(fd);
        return;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&chunk);
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
        PMIX_POST_OBJECT(rev);
        prte_event_active(&rev->ev, PRTE_EV_WRITE, 1);
    }
}

static void send_complete(char *file, int status)
{
    pmix_data_buffer_t *buf;
    int rc;

    PMIX_DATA_BUFFER_CREATE(buf);
    rc = PMIx_Data_pack(NULL, buf, &file, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    rc = PMIx_Data_pack(NULL, buf, &status, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_FILEM_BASE_RESP);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

/* This is a little tricky as the name of the archive doesn't
 * necessarily have anything to do with the paths inside it -
 * so we have to first query the archive to retrieve that info
 */
static int link_archive(prte_filem_raw_incoming_t *inbnd)
{
    FILE *fp;
    char *cmd;
    char path[PRTE_PATH_MAX];

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: identifying links for archive %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), inbnd->fullpath));

    pmix_asprintf(&cmd, "tar tf %s", inbnd->fullpath);
    fp = popen(cmd, "r");
    free(cmd);
    if (NULL == fp) {
        PRTE_ERROR_LOG(PRTE_ERR_FILE_OPEN_FAILURE);
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }
    /* because app_contexts might share part or all of a
     * directory tree, but link to different files, we
     * have to link to each individual file
     */
    while (fgets(path, sizeof(path), fp) != NULL) {
        PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                             "%s filem:raw: path %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), path));
        /* protect against an empty result */
        if (0 == strlen(path)) {
            continue;
        }
        /* trim the trailing cr */
        path[strlen(path) - 1] = '\0';
        /* ignore directories */
        if ('/' == path[strlen(path) - 1]) {
            PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: path %s is a directory - ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), path));
            continue;
        }
        /* an archive member that steps above the directory it is unpacked
         * in is not ours to place - tar itself refuses to extract one, so
         * there is nothing there to link to anyway
         */
        if (prte_filem_base_has_dotdot(path) || '/' == path[0]) {
            PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: path %s is not relative - ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), path));
            continue;
        }
        /* ignore specific useless directory trees */
        if (NULL != strstr(path, ".deps")) {
            PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                                 "%s filem:raw: path %s includes .deps - ignoring it",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), path));
            continue;
        }
        PMIX_OUTPUT_VERBOSE((10, prte_filem_base_framework.framework_output,
                             "%s filem:raw: adding path %s to link points",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), path));
        PMIx_Argv_append_nosize(&inbnd->link_pts, path);
    }
    /* close */
    pclose(fp);
    return PRTE_SUCCESS;
}

static void recv_files(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                       prte_rml_tag_t tag, void *cbdata)
{
    char *file, *session_dir;
    int32_t nchunk, n, nbytes;
    unsigned char data[PRTE_FILEM_RAW_CHUNK_MAX];
    int rc;
    prte_filem_raw_output_t *output;
    prte_filem_raw_incoming_t *ptr, *incoming;
    pmix_list_item_t *item;
    int32_t type = PRTE_FILEM_TYPE_UNKNOWN;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    /* unpack the data */
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &file, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        send_complete(NULL, rc);
        return;
    }
    /* the sender is supposed to have forced this name relative to the
     * session directory - refuse it if it steps above, rather than
     * truncating whatever sits at the resulting path
     */
    if (prte_filem_base_has_dotdot(file)) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        send_complete(file, PRTE_ERR_BAD_PARAM);
        free(file);
        return;
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nchunk, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        send_complete(file, rc);
        free(file);
        return;
    }
    /* if the chunk number is < 0, then this is an EOF message */
    if (nchunk < 0) {
        /* just set nbytes to zero so we close the fd */
        nbytes = 0;
    } else {
        nbytes = PRTE_FILEM_RAW_CHUNK_MAX;
        rc = PMIx_Data_unpack(NULL, buffer, data, &nbytes, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            send_complete(file, rc);
            free(file);
            return;
        }
    }
    /* if the chunk is 0, then additional info should be present */
    if (0 == nchunk) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &type, &n, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            send_complete(file, rc);
            free(file);
            return;
        }
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:raw: received chunk %d for file %s containing %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nchunk, file, nbytes));

    /* do we already have this file on our list of incoming? */
    incoming = NULL;
    for (item = pmix_list_get_first(&incoming_files); item != pmix_list_get_end(&incoming_files);
         item = pmix_list_get_next(item)) {
        ptr = (prte_filem_raw_incoming_t *) item;
        if (0 == strcmp(file, ptr->file)) {
            incoming = ptr;
            break;
        }
    }
    if (NULL == incoming) {
        /* nope - add it */
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: adding file %s to incoming list",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), file));
        incoming = PMIX_NEW(prte_filem_raw_incoming_t);
        incoming->file = strdup(file);
        incoming->type = type;
        pmix_list_append(&incoming_files, &incoming->super);
    }

    /* if this is the first chunk, we need to open the file descriptor */
    if (0 == nchunk) {
        char *tmp;
        /* define the full path to where we will put it */
        session_dir = prte_process_info.top_session_dir;

        incoming->fullpath = pmix_os_path(false, session_dir, file, NULL);

        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:raw: opening target file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), incoming->fullpath));
        /* create the path to the target, if not already existing */
        tmp = pmix_dirname(incoming->fullpath);
        rc = pmix_os_dirpath_create(tmp, S_IRWXU);
        if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
            PMIX_ERROR_LOG(rc);
            send_complete(file, PRTE_ERR_FILE_WRITE_FAILURE);
            free(file);
            free(tmp);
            /* drop this file from the incoming list before releasing it,
             * else a later chunk would walk a dangling pointer
             */
            pmix_list_remove_item(&incoming_files, &incoming->super);
            PMIX_RELEASE(incoming);
            return;
        }
        /* open the file descriptor for writing */
        if (PRTE_FILEM_TYPE_EXE == type) {
            if (0
                > (incoming->fd = open(incoming->fullpath, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU))) {
                pmix_output(0, "%s CANNOT CREATE FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            incoming->fullpath);
                send_complete(file, PRTE_ERR_FILE_WRITE_FAILURE);
                free(file);
                free(tmp);
                pmix_list_remove_item(&incoming_files, &incoming->super);
                PMIX_RELEASE(incoming);
                return;
            }
        } else {
            if (0 > (incoming->fd = open(incoming->fullpath, O_RDWR | O_CREAT | O_TRUNC,
                                         S_IRUSR | S_IWUSR))) {
                pmix_output(0, "%s CANNOT CREATE FILE %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            incoming->fullpath);
                send_complete(file, PRTE_ERR_FILE_WRITE_FAILURE);
                free(file);
                free(tmp);
                pmix_list_remove_item(&incoming_files, &incoming->super);
                PMIX_RELEASE(incoming);
                return;
            }
        }
        free(tmp);
        incoming->pending = true;
        PRTE_PMIX_THREADSHIFT(incoming, prte_event_base, write_handler);
    }
    /* create an output object for this data */
    output = PMIX_NEW(prte_filem_raw_output_t);
    if (0 < nbytes) {
        /* don't copy 0 bytes - we just need to pass
         * the zero bytes so the fd can be closed
         * after it writes everything out
         */
        memcpy(output->data, data, nbytes);
    }
    output->numbytes = nbytes;

    /* add this data to the write list for this fd */
    pmix_list_append(&incoming->outputs, &output->super);

    if (!incoming->pending) {
        /* add the event */
        incoming->pending = true;
        prte_event_active(&incoming->ev, PRTE_EV_WRITE, 1);
    }

    /* cleanup */
    free(file);
}

static void write_handler(int fd, short event, void *cbdata)
{
    prte_filem_raw_incoming_t *sink = (prte_filem_raw_incoming_t *) cbdata;
    pmix_list_item_t *item;
    prte_filem_raw_output_t *output;
    int num_written;
    char *dirname, *cmd;
    char homedir[PRTE_PATH_MAX];
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(fd, event);

    PMIX_ACQUIRE_OBJECT(sink);

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s write:handler writing data to %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         sink->fd));

    /* note that the event is off */
    sink->pending = false;

    while (NULL != (item = pmix_list_remove_first(&sink->outputs))) {
        output = (prte_filem_raw_output_t *) item;
        if (0 == output->numbytes) {
            /* indicates we are to close this stream */
            PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                 "%s write:handler zero bytes - reporting complete for file %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sink->file));
            /* close the file descriptor */
            close(sink->fd);
            sink->fd = -1;
            /* we are done with this EOF marker - release it so it isn't
             * leaked on the finalize paths below
             */
            PMIX_RELEASE(output);
            if (PRTE_FILEM_TYPE_FILE == sink->type || PRTE_FILEM_TYPE_EXE == sink->type) {
                /* the file itself is the one thing to place, under exactly
                 * the relative name the app will open it by
                 */
                PMIx_Argv_append_nosize(&sink->link_pts, sink->file);
                send_complete(sink->file, PRTE_SUCCESS);
            } else {
                /* Unarchive the file. It is unpacked at the root of the
                 * session dir - the same point every link point is relative
                 * to - rather than beside the archive itself: preloading an
                 * archive means "unpack this in my working directory", so
                 * "sub/bundle.tar" must still deliver its contents at the
                 * paths the archive names them by, not under "sub/". The
                 * archive is therefore named by its full path, since we are
                 * about to chdir away from it.
                 */
                if (PRTE_FILEM_TYPE_TAR == sink->type) {
                    pmix_asprintf(&cmd, "tar xf %s", sink->fullpath);
                } else if (PRTE_FILEM_TYPE_BZIP == sink->type) {
                    pmix_asprintf(&cmd, "tar xjf %s", sink->fullpath);
                } else if (PRTE_FILEM_TYPE_GZIP == sink->type) {
                    pmix_asprintf(&cmd, "tar xzf %s", sink->fullpath);
                } else {
                    PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                    return;
                }
                if (NULL == getcwd(homedir, sizeof(homedir))) {
                    PRTE_ERROR_LOG(PRTE_ERROR);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                    free(cmd);
                    return;
                }
                dirname = strdup(prte_process_info.top_session_dir);
                if (0 != chdir(dirname)) {
                    PRTE_ERROR_LOG(PRTE_ERROR);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                    free(cmd);
                    free(dirname);
                    return;
                }
                PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                     "%s write:handler unarchiving file %s with cmd: %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sink->file, cmd));
                if (0 != system(cmd)) {
                    PRTE_ERROR_LOG(PRTE_ERROR);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                    /* best-effort restore of our working directory before
                     * bailing; log but don't mask the original failure
                     */
                    if (0 != chdir(homedir)) {
                        PRTE_ERROR_LOG(PRTE_ERROR);
                    }
                    free(cmd);
                    free(dirname);
                    return;
                }
                if (0 != chdir(homedir)) {
                    PRTE_ERROR_LOG(PRTE_ERROR);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                    free(cmd);
                    free(dirname);
                    return;
                }
                free(dirname);
                free(cmd);
                /* setup the link points */
                if (PRTE_SUCCESS != (rc = link_archive(sink))) {
                    PRTE_ERROR_LOG(rc);
                    send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
                } else {
                    send_complete(sink->file, PRTE_SUCCESS);
                }
            }
            return;
        }
        num_written = write(sink->fd, output->data, output->numbytes);
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s write:handler wrote %d bytes to file %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), num_written, sink->file));
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                pmix_list_prepend(&sink->outputs, item);
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                sink->pending = true;
                PMIX_POST_OBJECT(sink);
                prte_event_add(&sink->ev, 0);
                return;
            }
            /* otherwise, something bad happened so all we can do is abort
             * this attempt
             */
            PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                 "%s write:handler error on write for file %s: %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sink->file, strerror(errno)));
            PMIX_RELEASE(output);
            pmix_list_remove_item(&incoming_files, &sink->super);
            send_complete(sink->file, PRTE_ERR_FILE_WRITE_FAILURE);
            PMIX_RELEASE(sink);
            return;
        } else if (num_written < output->numbytes) {
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* push this item back on the front of the list */
            pmix_list_prepend(&sink->outputs, item);
            /* leave the write event running so it will call us again
             * when the fd is ready
             */
            sink->pending = true;
            PMIX_POST_OBJECT(sink);
            prte_event_active(&sink->ev, PRTE_EV_WRITE, 1);
            return;
        }
        PMIX_RELEASE(output);
    }
}

static void xfer_construct(prte_filem_raw_xfer_t *ptr)
{
    memset(&ptr->ev, 0, sizeof(prte_event_t));
    ptr->fd = -1;
    ptr->outbound = NULL;
    ptr->app_idx = 0;
    ptr->pending = false;
    ptr->src = NULL;
    ptr->file = NULL;
    ptr->nchunk = 0;
    ptr->status = PRTE_SUCCESS;
    ptr->nrecvd = 0;
}
static void xfer_destruct(prte_filem_raw_xfer_t *ptr)
{
    if (ptr->pending) {
        prte_event_del(&ptr->ev);
    }
    if (NULL != ptr->src) {
        free(ptr->src);
    }
    if (NULL != ptr->file) {
        free(ptr->file);
    }
}
PMIX_CLASS_INSTANCE(prte_filem_raw_xfer_t,
                    pmix_list_item_t,
                    xfer_construct, xfer_destruct);

static void out_construct(prte_filem_raw_outbound_t *ptr)
{
    PMIX_CONSTRUCT(&ptr->xfers, pmix_list_t);
    ptr->status = PRTE_SUCCESS;
    ptr->cbfunc = NULL;
    ptr->cbdata = NULL;
}
static void out_destruct(prte_filem_raw_outbound_t *ptr)
{
    PMIX_LIST_DESTRUCT(&ptr->xfers);
}
PMIX_CLASS_INSTANCE(prte_filem_raw_outbound_t,
                    pmix_list_item_t,
                    out_construct, out_destruct);

static void in_construct(prte_filem_raw_incoming_t *ptr)
{
    ptr->app_idx = 0;
    ptr->pending = false;
    ptr->fd = -1;
    ptr->file = NULL;
    ptr->fullpath = NULL;
    ptr->link_pts = NULL;
    PMIX_CONSTRUCT(&ptr->outputs, pmix_list_t);
}
static void in_destruct(prte_filem_raw_incoming_t *ptr)
{
    if (ptr->pending) {
        prte_event_del(&ptr->ev);
    }
    if (0 <= ptr->fd) {
        close(ptr->fd);
    }
    if (NULL != ptr->file) {
        free(ptr->file);
    }
    if (NULL != ptr->fullpath) {
        free(ptr->fullpath);
    }
    PMIx_Argv_free(ptr->link_pts);
    PMIX_LIST_DESTRUCT(&ptr->outputs);
}
PMIX_CLASS_INSTANCE(prte_filem_raw_incoming_t,
                    pmix_list_item_t,
                    in_construct, in_destruct);

static void output_construct(prte_filem_raw_output_t *ptr)
{
    ptr->numbytes = 0;
}
PMIX_CLASS_INSTANCE(prte_filem_raw_output_t,
                    pmix_list_item_t,
                    output_construct, NULL);
