/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_SCHIZO_PMIX_H_
#define _MCA_SCHIZO_PMIX_H_

#include "prrte_config.h"

#include "types.h"

#include "src/mca/base/base.h"
#include "src/mca/schizo/schizo.h"


BEGIN_C_DECLS

PRRTE_MODULE_EXPORT extern prrte_schizo_base_component_t prrte_schizo_pmix_component;
extern prrte_schizo_base_module_t prrte_schizo_pmix_module;

END_C_DECLS

#endif /* MCA_SCHIZO_PMIX_H_ */

