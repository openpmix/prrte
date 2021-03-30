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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/class/prte_list.h"
#include "src/mca/base/base.h"

/*
 * Local functions
 */
static void cl_constructor(prte_object_t *obj);
static void cpl_constructor(prte_object_t *obj);

/*
 * Class instance of the mca_base_component_list_item_t class
 */
PRTE_CLASS_INSTANCE(prte_mca_base_component_list_item_t, prte_list_item_t, cl_constructor, NULL);

/*
 * Class instance of the mca_base_component_priority_list_item_t class
 */
PRTE_CLASS_INSTANCE(prte_mca_base_component_priority_list_item_t,
                    prte_mca_base_component_list_item_t, cpl_constructor, NULL);

/*
 * Just do basic sentinel intialization
 */
static void cl_constructor(prte_object_t *obj)
{
    prte_mca_base_component_list_item_t *cli = (prte_mca_base_component_list_item_t *) obj;
    cli->cli_component = NULL;
}

/*
 * Just do basic sentinel intialization
 */
static void cpl_constructor(prte_object_t *obj)
{
    prte_mca_base_component_priority_list_item_t *cpli
        = (prte_mca_base_component_priority_list_item_t *) obj;
    cpli->cpli_priority = -1;
}
