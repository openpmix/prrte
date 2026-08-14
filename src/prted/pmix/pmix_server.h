/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_SERVER_H_
#define _PMIX_SERVER_H_

#include "prte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

PRTE_EXPORT int pmix_server_init(void);
PRTE_EXPORT void pmix_server_start(void);
PRTE_EXPORT void pmix_server_finalize(void);
PRTE_EXPORT void pmix_server_register_params(void);

PRTE_EXPORT int prte_pmix_server_register_nspace(prte_job_t *jdata,
                                                 pmix_op_cbfunc_t cbfunc, void *cbdata);

PRTE_EXPORT void prte_pmix_server_clear(pmix_proc_t *pname);

/* Record that every local proc of this job has gone and its job object has
 * been released here, and ask whether that has happened.
 *
 * A daemon retires a job as soon as its own share of it finishes, while the
 * job goes on running elsewhere - so "I have no job object for this" has two
 * meanings, and a direct modex has to tell them apart. Not yet arrived means
 * wait; already gone means answer NOT_FOUND, because nothing will ever make
 * it answerable again. The record is dropped when the DVM declares the job
 * complete, so the list holds at most the jobs currently running. */
PRTE_EXPORT void prte_pmix_server_job_departed(const pmix_nspace_t nspace);
PRTE_EXPORT bool prte_pmix_server_job_has_departed(const pmix_nspace_t nspace);
PRTE_EXPORT void prte_pmix_server_forget_departed(const pmix_nspace_t nspace);

PRTE_EXPORT void pmix_server_notify_spawn(pmix_nspace_t jobid, int room, pmix_status_t ret);

END_C_DECLS

#endif /* PMIX_SERVER_H_ */
