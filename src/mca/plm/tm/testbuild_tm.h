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

#ifndef PRTE_PLM_TM_TESTBUILD_H
#define PRTE_PLM_TM_TESTBUILD_H

#include "prte_config.h"

#include "src/mca/mca.h"
#include "src/mca/plm/plm.h"

BEGIN_C_DECLS

typedef int tm_event_t;
typedef int tm_node_id;
typedef unsigned long tm_task_id;

typedef struct  tm_roots {
    tm_task_id  tm_me;
    tm_task_id  tm_parent;
    int     tm_nnodes;
    int     tm_ntasks;
    int     tm_taskpoolid;
    tm_task_id  *tm_tasklist;
} tm_roots;

#define TM_NULL_EVENT   ((tm_event_t)0)
#define TM_SUCCESS 0

int tm_init(void *info, struct tm_roots *roots);

int tm_poll(tm_event_t poll_event, tm_event_t *result_event, int wait, int *tm_errno);

int tm_spawn(int argc, char **argv, char **env, tm_node_id launchid,
             tm_task_id *tid, tm_event_t *event);

int tm_finalize(void);

END_C_DECLS

#endif /* PRTE_PLM_TM_TESTBUILD_H */
