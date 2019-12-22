/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2011-2019 IBM Corporation.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file:
 * Part of the rsh launcher. See plm_rsh.h for an overview of how it works.
 */

#ifndef PRRTE_PLM_RSH_EXPORT_H
#define PRRTE_PLM_RSH_EXPORT_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <time.h>

#include "src/mca/mca.h"

#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/base.h"

BEGIN_C_DECLS

/**
 * PLS Component
 */
struct prrte_plm_rsh_component_t {
    prrte_plm_base_component_t super;
    bool force_rsh;
    bool disable_qrsh;
    bool using_qrsh;
    bool daemonize_qrsh;
    bool disable_llspawn;
    bool using_llspawn;
    bool daemonize_llspawn;
    struct timespec delay;
    int priority;
    bool no_tree_spawn;
    int num_concurrent;
    char *agent;
    char *agent_path;
    char **agent_argv;
    bool assume_same_shell;
    bool pass_environ_mca_params;
    char *ssh_args;
    char *pass_libpath;
    char *chdir;
};
typedef struct prrte_plm_rsh_component_t prrte_plm_rsh_component_t;

PRRTE_MODULE_EXPORT extern prrte_plm_rsh_component_t prrte_plm_rsh_component;
extern prrte_plm_base_module_t prrte_plm_rsh_module;

PRRTE_MODULE_EXPORT char **prrte_plm_rsh_search(const char* agent_list, const char *path);

END_C_DECLS

#endif /* PRRTE_PLS_RSH_EXPORT_H */
