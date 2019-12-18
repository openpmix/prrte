/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/mca/dl/dl.h"
#include "src/mca/dl/base/base.h"


int prrte_dl_base_close(void)
{
    /* Close all available modules that are open */
    return prrte_mca_base_framework_components_close(&prrte_dl_base_framework, NULL);
}
