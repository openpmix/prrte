/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
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
