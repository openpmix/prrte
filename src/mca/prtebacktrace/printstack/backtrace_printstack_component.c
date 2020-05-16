/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/mca/prtebacktrace/prtebacktrace.h"

const prte_backtrace_base_component_2_0_0_t prte_prtebacktrace_printstack_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    .backtracec_version = {
        PRTE_BACKTRACE_BASE_VERSION_2_0_0,

        /* Component name and version */
        .mca_component_name = "printstack",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),
    },
    .backtracec_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};
