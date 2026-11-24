/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The TM calls of a testbuild helper (see testbuild_tm.h): every one
 * fails as though there were no mom to talk to.
 */

#include "prte_config.h"

#include "testbuild_tm.h"

int tm_init(void *info, struct tm_roots *roots)
{
    (void) info;
    (void) roots;
    return TM_ENOTCONNECTED;
}

int tm_nodeinfo(tm_node_id **list, int *nnodes)
{
    (void) list;
    (void) nnodes;
    return TM_ENOTCONNECTED;
}

int tm_spawn(int argc, char **argv, char **envp, tm_node_id where, tm_task_id *tid,
             tm_event_t *event)
{
    (void) argc;
    (void) argv;
    (void) envp;
    (void) where;
    (void) tid;
    (void) event;
    return TM_ENOTCONNECTED;
}

int tm_obit(tm_task_id tid, int *obitval, tm_event_t *event)
{
    (void) tid;
    (void) obitval;
    (void) event;
    return TM_ENOTCONNECTED;
}

int tm_poll(tm_event_t poll_event, tm_event_t *result_event, int wait, int *tm_errno)
{
    (void) poll_event;
    (void) result_event;
    (void) wait;
    (void) tm_errno;
    return TM_ENOTCONNECTED;
}

int tm_finalize(void)
{
    return TM_SUCCESS;
}
