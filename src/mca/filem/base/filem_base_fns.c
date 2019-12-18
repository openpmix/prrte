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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <time.h>

#include "constants.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/proc_info.h"

#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"

/******************
 * Local Functions
 ******************/

/******************
 * Object Stuff
 ******************/
static void process_set_construct(prrte_filem_base_process_set_t *req) {
    req->source = *PRRTE_NAME_INVALID;
    req->sink   = *PRRTE_NAME_INVALID;
}

static void process_set_destruct( prrte_filem_base_process_set_t *req) {
    req->source = *PRRTE_NAME_INVALID;
    req->sink   = *PRRTE_NAME_INVALID;
}

PRRTE_CLASS_INSTANCE(prrte_filem_base_process_set_t,
                   prrte_list_item_t,
                   process_set_construct,
                   process_set_destruct);

static void file_set_construct(prrte_filem_base_file_set_t *req) {
    req->local_target  = NULL;
    req->local_hint    = PRRTE_FILEM_HINT_NONE;

    req->remote_target = NULL;
    req->remote_hint   = PRRTE_FILEM_HINT_NONE;

    req->target_flag   = PRRTE_FILEM_TYPE_UNKNOWN;

}

static void file_set_destruct( prrte_filem_base_file_set_t *req) {
    if( NULL != req->local_target ) {
        free(req->local_target);
        req->local_target = NULL;
    }
    req->local_hint    = PRRTE_FILEM_HINT_NONE;

    if( NULL != req->remote_target ) {
        free(req->remote_target);
        req->remote_target = NULL;
    }
    req->remote_hint   = PRRTE_FILEM_HINT_NONE;

    req->target_flag   = PRRTE_FILEM_TYPE_UNKNOWN;
}

PRRTE_CLASS_INSTANCE(prrte_filem_base_file_set_t,
                   prrte_list_item_t,
                   file_set_construct,
                   file_set_destruct);

static void req_construct(prrte_filem_base_request_t *req) {
    PRRTE_CONSTRUCT(&req->process_sets,  prrte_list_t);
    PRRTE_CONSTRUCT(&req->file_sets,     prrte_list_t);

    req->num_mv = 0;

    req->is_done = NULL;
    req->is_active = NULL;

    req->exit_status = NULL;

    req->movement_type = PRRTE_FILEM_MOVE_TYPE_UNKNOWN;
}

static void req_destruct( prrte_filem_base_request_t *req) {
    prrte_list_item_t* item = NULL;

    while( NULL != (item = prrte_list_remove_first(&req->process_sets)) ) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&req->process_sets);

    while( NULL != (item = prrte_list_remove_first(&req->file_sets)) ) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&req->file_sets);

    req->num_mv = 0;

    if( NULL != req->is_done ) {
        free(req->is_done);
        req->is_done = NULL;
    }

    if( NULL != req->is_active ) {
        free(req->is_active);
        req->is_active = NULL;
    }

    if( NULL != req->exit_status ) {
        free(req->exit_status);
        req->exit_status = NULL;
    }

    req->movement_type = PRRTE_FILEM_MOVE_TYPE_UNKNOWN;
}

PRRTE_CLASS_INSTANCE(prrte_filem_base_request_t,
                   prrte_list_item_t,
                   req_construct,
                   req_destruct);

/***********************
 * None component stuff
 ************************/
int prrte_filem_base_module_init(void)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_module_finalize(void)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_put(prrte_filem_base_request_t *request )
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_put_nb(prrte_filem_base_request_t *request )
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_get(prrte_filem_base_request_t *request)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_get_nb(prrte_filem_base_request_t *request)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_rm(prrte_filem_base_request_t *request)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_rm_nb(prrte_filem_base_request_t *request)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_wait(prrte_filem_base_request_t *request)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_wait_all(prrte_list_t *request_list)
{
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_preposition_files(prrte_job_t *jdata,
                                           prrte_filem_completion_cbfunc_t cbfunc,
                                           void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PRRTE_SUCCESS, cbdata);
    }
    return PRRTE_SUCCESS;
}

int prrte_filem_base_none_link_local_files(prrte_job_t *jdata,
                                          prrte_app_context_t *app)
{
    return PRRTE_SUCCESS;
}
