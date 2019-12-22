/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_ESS_ALPS_H
#define PRRTE_ESS_ALPS_H

#include "prrte_config.h"
#include "src/mca/mca.h"
#include "src/mca/ess/ess.h"

#include "alps/alps.h"
#include "alps/alps_toolAssist.h"
#include "alps/libalpsutil.h"
#include "alps/libalpslli.h"

BEGIN_C_DECLS

/*
 * Module open / close
 */
int prrte_ess_alps_component_open(void);
int prrte_ess_alps_component_close(void);
int prrte_ess_alps_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * alps component internal utility functions
 */

int prrte_ess_alps_get_first_rank_on_node(int *first_rank);
int prrte_ess_alps_sync_start(void);
int prrte_ess_alps_sync_complete(void);

/*
 * ODLS Alps module
 */
extern prrte_ess_base_module_t prrte_ess_alps_module;
PRRTE_MODULE_EXPORT extern prrte_ess_base_component_t prrte_ess_alps_component;

END_C_DECLS

#endif /* PRRTE_ESS_ALPS_H */
