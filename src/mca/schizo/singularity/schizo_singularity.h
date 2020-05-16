/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_MCA_SCHIZO_SINGULARITY_H_
#define PRTE_MCA_SCHIZO_SINGULARITY_H_

#include "prte_config.h"

#include "src/include/types.h"

#include "src/mca/base/base.h"
#include "src/mca/schizo/schizo.h"


BEGIN_C_DECLS

PRTE_EXPORT extern prte_schizo_base_component_t prte_schizo_singularity_component;
extern prte_schizo_base_module_t prte_schizo_singularity_module;

END_C_DECLS

#endif /* MCA_SCHIZO_SINGULARITY_H_ */

