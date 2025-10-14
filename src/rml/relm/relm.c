/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/rml/relm/relm.h"
#include "src/rml/relm/base/base.h"

prte_relm_module_t prte_relm = {0};

void prte_relm_register(void){
    // TODO: enum mca var to choose between modules
    // For now, just register and use the base implementation.
    prte_relm_base_register();
}

void prte_relm_open(void){
    prte_relm = prte_relm_base_module;
    prte_relm.init();
}

void prte_relm_close(void){
    prte_relm.finalize();
    prte_relm = (prte_relm_module_t) {0};
}
