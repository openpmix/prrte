/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <time.h>

#include "constants.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

/******************
 * Local Functions
 ******************/

/******************
 * Object Stuff
 ******************/
static void process_set_construct(prte_filem_base_process_set_t *req)
{
    req->source = *PRTE_NAME_INVALID;
    req->sink = *PRTE_NAME_INVALID;
}

static void process_set_destruct(prte_filem_base_process_set_t *req)
{
    req->source = *PRTE_NAME_INVALID;
    req->sink = *PRTE_NAME_INVALID;
}

PMIX_CLASS_INSTANCE(prte_filem_base_process_set_t, pmix_list_item_t, process_set_construct,
                    process_set_destruct);

static void file_set_construct(prte_filem_base_file_set_t *req)
{
    req->local_target = NULL;
    req->local_hint = PRTE_FILEM_HINT_NONE;

    req->remote_target = NULL;
    req->remote_hint = PRTE_FILEM_HINT_NONE;

    req->target_flag = PRTE_FILEM_TYPE_UNKNOWN;
}

static void file_set_destruct(prte_filem_base_file_set_t *req)
{
    if (NULL != req->local_target) {
        free(req->local_target);
        req->local_target = NULL;
    }
    req->local_hint = PRTE_FILEM_HINT_NONE;

    if (NULL != req->remote_target) {
        free(req->remote_target);
        req->remote_target = NULL;
    }
    req->remote_hint = PRTE_FILEM_HINT_NONE;

    req->target_flag = PRTE_FILEM_TYPE_UNKNOWN;
}

PMIX_CLASS_INSTANCE(prte_filem_base_file_set_t, pmix_list_item_t, file_set_construct,
                    file_set_destruct);

static void req_construct(prte_filem_base_request_t *req)
{
    PMIX_CONSTRUCT(&req->process_sets, pmix_list_t);
    PMIX_CONSTRUCT(&req->file_sets, pmix_list_t);

    req->num_mv = 0;

    req->is_done = NULL;
    req->is_active = NULL;

    req->exit_status = NULL;

    req->movement_type = PRTE_FILEM_MOVE_TYPE_UNKNOWN;
}

static void req_destruct(prte_filem_base_request_t *req)
{
    pmix_list_item_t *item = NULL;

    while (NULL != (item = pmix_list_remove_first(&req->process_sets))) {
        PMIX_RELEASE(item);
    }
    PMIX_DESTRUCT(&req->process_sets);

    while (NULL != (item = pmix_list_remove_first(&req->file_sets))) {
        PMIX_RELEASE(item);
    }
    PMIX_DESTRUCT(&req->file_sets);

    req->num_mv = 0;

    if (NULL != req->is_done) {
        free(req->is_done);
        req->is_done = NULL;
    }

    if (NULL != req->is_active) {
        free(req->is_active);
        req->is_active = NULL;
    }

    if (NULL != req->exit_status) {
        free(req->exit_status);
        req->exit_status = NULL;
    }

    req->movement_type = PRTE_FILEM_MOVE_TYPE_UNKNOWN;
}

PMIX_CLASS_INSTANCE(prte_filem_base_request_t, pmix_list_item_t, req_construct, req_destruct);


/**********************
 * Path safety helpers
 *
 * A preloaded file is always delivered *inside* a directory PRRTE chooses
 * (the node's session directory on the way in, the app's working directory
 * on the way out). Keeping it there is a property of the name, so the
 * naming rules live here, next to the classes that carry those names,
 * where they can be tested without a DVM.
 **********************/
/* Strip any leading "./" and "../" components so a staged file can never
 * step above the directory it is to be placed in. A name that merely
 * *starts* with a dot but is not a dot-directory (".bashrc") is left alone.
 * Returns a pointer into the string it was given.
 */
const char *prte_filem_base_strip_leading_dots(const char *path)
{
    const char *cptr = path;
    const char *nxt = path + 1;

    while ('\0' != *cptr) {
        if ('.' == *cptr) {
            /* have to check the next character to see if this is a dot
             * directory or just a dot-file
             */
            if ('.' == *nxt || '/' == *nxt) {
                cptr = nxt;
                nxt++;
            } else {
                break;
            }
        } else if ('/' == *cptr) {
            cptr = nxt;
            nxt++;
        } else {
            /* the character isn't a dot or a slash, so this is the
             * beginning of the filename
             */
            break;
        }
    }
    return cptr;
}

/* Does any component of this relative path step upward? Stripping the
 * *leading* dot directories is not enough: "a/../../f" has none at the
 * front and still lands two levels above where it was meant to go, on
 * every node of the DVM.
 */
bool prte_filem_base_has_dotdot(const char *path)
{
    const char *p = path;

    while (NULL != p) {
        if ('.' == p[0] && '.' == p[1] && ('/' == p[2] || '\0' == p[2])) {
            return true;
        }
        p = strchr(p, '/');
        if (NULL != p) {
            ++p;
        }
    }
    return false;
}


/* Wrap a path for use in a shell command line. The archive commands and
 * the archive listing both go through a shell, and the path came from the
 * user's --preload-files list - a perfectly ordinary "my data.tar.gz"
 * would otherwise be handed to tar as two arguments.
 */
char *prte_filem_base_shell_quote(const char *path)
{
    size_t len = strlen(path);
    size_t i, j = 0;
    char *q;

    /* worst case every character is a quote, which costs four bytes */
    q = (char *) malloc(4 * len + 3);
    if (NULL == q) {
        return NULL;
    }
    q[j++] = '\'';
    for (i = 0; i < len; i++) {
        if ('\'' == path[i]) {
            /* close the quote, emit an escaped one, reopen */
            q[j++] = '\'';
            q[j++] = '\\';
            q[j++] = '\'';
            q[j++] = '\'';
        } else {
            q[j++] = path[i];
        }
    }
    q[j++] = '\'';
    q[j] = '\0';
    return q;
}

/***********************
 * None component stuff
 ************************/
int prte_filem_base_module_init(void)
{
    return PRTE_SUCCESS;
}

int prte_filem_base_module_finalize(void)
{
    return PRTE_SUCCESS;
}

void prte_filem_base_none_fault_handler(const prte_rml_recovery_status_t *status)
{
    PRTE_HIDE_UNUSED_PARAMS(status);
}

int prte_filem_base_none_put(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_put_nb(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_get(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_get_nb(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_rm(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_rm_nb(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_wait(prte_filem_base_request_t *request)
{
    PRTE_HIDE_UNUSED_PARAMS(request);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_wait_all(pmix_list_t *request_list)
{
    PRTE_HIDE_UNUSED_PARAMS(request_list);
    return PRTE_SUCCESS;
}

int prte_filem_base_none_preposition_files(prte_job_t *jdata,
                                           prte_filem_completion_cbfunc_t cbfunc,
                                           void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata);
    if (NULL != cbfunc) {
        cbfunc(PRTE_SUCCESS, cbdata);
    }
    return PRTE_SUCCESS;
}

int prte_filem_base_none_link_local_files(prte_job_t *jdata, prte_app_context_t *app)
{
    PRTE_HIDE_UNUSED_PARAMS(jdata, app);
    return PRTE_SUCCESS;
}
