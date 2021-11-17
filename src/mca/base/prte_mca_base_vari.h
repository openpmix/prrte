/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This is the private declarations for the MCA variable system.
 * This file is internal to the MCA variable system and should not
 * need to be used by any other elements in PRTE except the
 * special case of the ompi_info command.
 *
 * All the rest of the doxygen documentation in this file is marked as
 * "internal" and won't show up unless you specifically tell doxygen
 * to generate internal documentation (by default, it is skipped).
 */

#ifndef PRTE_MCA_BASE_VAR_INTERNAL_H
#define PRTE_MCA_BASE_VAR_INTERNAL_H

#include "prte_config.h"

#include "src/class/prte_hash_table.h"
#include "src/class/prte_list.h"
#include "src/class/prte_object.h"
#include "src/class/prte_pointer_array.h"
#include "src/class/prte_value_array.h"
#include "src/mca/base/prte_mca_base_var.h"

BEGIN_C_DECLS

/* Internal flags start at bit 16 */
#define PRTE_MCA_BASE_VAR_FLAG_EXTERNAL_MASK 0x0000ffff

typedef enum {
    /** Variable is valid */
    PRTE_MCA_BASE_VAR_FLAG_VALID = 0x00010000,
    /** Variable is a synonym */
    PRTE_MCA_BASE_VAR_FLAG_SYNONYM = 0x00020000,
    /** mbv_source_file needs to be freed */
    PRTE_MCA_BASE_VAR_FLAG_SOURCE_FILE_NEEDS_FREE = 0x00040000
} prte_mca_base_var_flag_internal_t;

#define PRTE_VAR_FLAG_ISSET(var, flag) (!!((var).mbp_flags & (flag)))

#define PRTE_VAR_IS_VALID(var)        (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_VALID))
#define PRTE_VAR_IS_SYNONYM(var)      (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_SYNONYM))
#define PRTE_VAR_IS_INTERNAL(var)     (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_INTERNAL))
#define PRTE_VAR_IS_DEFAULT_ONLY(var) (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY))
#define PRTE_VAR_IS_SETTABLE(var)     (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_SETTABLE))
#define PRTE_VAR_IS_DEPRECATED(var)   (!!((var).mbv_flags & PRTE_MCA_BASE_VAR_FLAG_DEPRECATED))

PRTE_EXPORT extern const char *prte_var_type_names[];
PRTE_EXPORT extern const size_t prte_var_type_sizes[];
PRTE_EXPORT extern bool prte_mca_base_var_initialized;

/**
 * \internal
 *
 * Structure for holding param names and values read in from files.
 */
struct prte_mca_base_var_file_value_t {
    /** Allow this to be an PRTE OBJ */
    prte_list_item_t super;

    /** Parameter name */
    char *mbvfv_var;
    /** Parameter value */
    char *mbvfv_value;
    /** File it came from */
    char *mbvfv_file;
    /** Line it came from */
    int mbvfv_lineno;
};

/**
 * \internal
 *
 * Convenience typedef
 */
typedef struct prte_mca_base_var_file_value_t prte_mca_base_var_file_value_t;

/**
 * Object declaration for mca_base_var_file_value_t
 */
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_mca_base_var_file_value_t);

/**
 * \internal
 *
 * Get a group
 *
 * @param[in]  group_index Group index
 * @param[out] group       Returned group if it exists
 * @param[in]  invalidok   Return group even if it has been deregistered
 */
PRTE_EXPORT int prte_mca_base_var_group_get_internal(const int group_index,
                                                     prte_mca_base_var_group_t **group,
                                                     bool invalidok);

PRTE_EXPORT int prte_mca_base_parse_paramfile(const char *paramfile, prte_list_t *list);

/**
 * \internal
 *
 * Add a variable to a group
 */
PRTE_EXPORT int prte_mca_base_var_group_add_var(const int group_index, const int param_index);

/**
 * \internal
 *
 * Add an enum to a group
 */
PRTE_EXPORT int prte_mca_base_var_group_add_enum(const int group_index, const void *storage);

/**
 * \internal
 *
 * Generate a full name with _ between all of the non-NULL arguments
 */
PRTE_EXPORT int prte_mca_base_var_generate_full_name4(const char *project, const char *framework,
                                                      const char *component, const char *variable,
                                                      char **full_name);

/**
 * \internal
 *
 * Initialize/finalize MCA variable groups
 */
PRTE_EXPORT int prte_mca_base_var_group_init(void);
PRTE_EXPORT int prte_mca_base_var_group_finalize(void);

END_C_DECLS

#endif /* PRTE_MCA_BASE_VAR_INTERNAL_H */
