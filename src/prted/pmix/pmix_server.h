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
 * Copyright (c) 2010-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_SERVER_H_
#define _PMIX_SERVER_H_

#include "prrte_config.h"

#include "src/pmix/pmix-internal.h"
#include "src/runtime/prrte_globals.h"

BEGIN_C_DECLS

PRRTE_EXPORT int pmix_server_init(void);
PRRTE_EXPORT void pmix_server_start(void);
PRRTE_EXPORT void pmix_server_finalize(void);
PRRTE_EXPORT void pmix_server_register_params(void);


PRRTE_EXPORT int prrte_pmix_server_register_nspace(prrte_job_t *jdata);

PRRTE_EXPORT void prrte_pmix_server_clear(pmix_proc_t *pname);


END_C_DECLS

#endif /* PMIX_SERVER_H_ */
