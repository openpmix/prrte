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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_PLM_LSF_TESTBUILD_H
#define PRTE_PLM_LSF_TESTBUILD_H

#include "prte_config.h"

#include "src/mca/mca.h"
#include "src/mca/plm/plm.h"

BEGIN_C_DECLS

#define LSF_DJOB_REPLACE_ENV 0x1
#define LSF_DJOB_NOWAIT 0x2

int lsb_init(char *str);
int lsb_launch(char **nodelist_argv, char **argv, int flags, char **env);
char *lsb_sysmsg(void);

extern int lsberrno;

END_C_DECLS

#endif /* PRTE_PLM_LSF_TESTBUILD_H */
