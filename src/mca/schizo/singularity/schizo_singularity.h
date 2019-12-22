/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_MCA_SCHIZO_SINGULARITY_H_
#define PRRTE_MCA_SCHIZO_SINGULARITY_H_

#include "prrte_config.h"

#include "src/include/types.h"

#include "src/mca/base/base.h"
#include "src/mca/schizo/schizo.h"


BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_schizo_base_component_t prrte_schizo_singularity_component;
extern prrte_schizo_base_module_t prrte_schizo_singularity_module;

END_C_DECLS

#endif /* MCA_SCHIZO_SINGULARITY_H_ */

