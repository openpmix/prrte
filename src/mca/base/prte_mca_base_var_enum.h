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
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PRTE_MCA_BASE_VAR_ENUM_H)
#    define PRTE_MCA_BASE_VAR_ENUM_H

#    include "prte_config.h"

#    include "constants.h"
#    include "src/class/pmix_object.h"

typedef struct prte_mca_base_var_enum_t prte_mca_base_var_enum_t;

/**
 * Get the number of values in the enumerator
 *
 * @param[in] self the enumerator
 * @param[out] count the number of values in the enumerator
 */
typedef int (*prte_mca_base_var_enum_get_count_fn_t)(prte_mca_base_var_enum_t *self, int *count);

/**
 * Get the value and its string representation for an index 0..get_count()
 *
 * @param[in] self the enumerator
 * @param[in] index the index to get the value of
 * @param[out] value integer value
 * @param[out] string_value string value
 */
typedef int (*prte_mca_base_var_enum_get_value_fn_t)(prte_mca_base_var_enum_t *self, int index,
                                                     int *value, const char **string_value);

/**
 * Look up the integer value of a string
 *
 * @param[in] self the enumerator
 * @param[in] string_value string to lookup
 * @param[out] value integer value for the string
 *
 * @retval PRTE_SUCCESS if found
 * @retval PRTE_ERR_VALUE_OUT_OF_BOUNDS if not
 */
typedef int (*prte_mca_base_var_enum_vfs_fn_t)(prte_mca_base_var_enum_t *self,
                                               const char *string_value, int *value);

/**
 * Dump a textual representation of all the values in an enumerator
 *
 * @param[in] self the enumerator
 * @param[out] out the string representation
 *
 * @retval PRTE_SUCCESS on success
 * @retval prte error on error
 */
typedef int (*prte_mca_base_var_enum_dump_fn_t)(prte_mca_base_var_enum_t *self, char **out);

/**
 * Get the string representation for an enumerator value
 *
 * @param[in] self the enumerator
 * @param[in] value integer value
 * @param[out] string_value string value for value
 *
 * @retval PRTE_SUCCESS on success
 * @retval PRTE_ERR_VALUE_OUT_OF_BOUNDS if not found
 *
 * @long This function returns the string value for a given interger value in the
 * {string_value} parameter. The {string_value} parameter may be NULL in which case
 * no string is returned. If a string is returned in {string_value} the caller
 * must free the string with free().
 */
typedef int (*prte_mca_base_var_enum_sfv_fn_t)(prte_mca_base_var_enum_t *self, const int value,
                                               char **string_value);

/**
 * The default enumerator class takes in a list of integer-string pairs. If a
 * string is read from an environment variable or a file value the matching
 * integer value is used for the MCA variable.
 */
struct prte_mca_base_var_enum_value_t {
    int value;
    const char *string;
};

typedef struct prte_mca_base_var_enum_value_t prte_mca_base_var_enum_value_t;

/**
 * enumerator base class
 */
struct prte_mca_base_var_enum_t {
    pmix_object_t super;

    /** Is the enumerator statically allocated */
    bool enum_is_static;

    /** Name of this enumerator. This value is duplicated from the argument provided to
        mca_base_var_enum_create() */
    char *enum_name;

    /** Get the number of values this enumerator represents. Subclasses should override
        the default function. */
    prte_mca_base_var_enum_get_count_fn_t get_count;
    /** Get the value and string representation for a particular index. Subclasses should
        override the default function */
    prte_mca_base_var_enum_get_value_fn_t get_value;
    /** Given a string return corresponding integer value. If the string does not match a
     valid value return PRTE_ERR_VALUE_OUT_OF_BOUNDS */
    prte_mca_base_var_enum_vfs_fn_t value_from_string;
    /** Given an integer return the corresponding string value. If the integer does not
        match a valid value return PRTE_ERR_VALUE_OUT_OF_BOUNDS */
    prte_mca_base_var_enum_sfv_fn_t string_from_value;
    /** Dump a textual representation of the enumerator. The caller is responsible for
        freeing the string */
    prte_mca_base_var_enum_dump_fn_t dump;

    int enum_value_count;
    /** Copy of the enumerators values (used by the default functions). This array and
        and the strings it contains are freed by the destructor if not NULL. */
    prte_mca_base_var_enum_value_t *enum_values;
};

/**
 * The default flag enumerator class takes in a list of integer-string pairs. If a
 * string is read from an environment variable or a file value the matching
 * flag value is used for the MCA variable. The conflicting_flag is used to
 * indicate any flags that should conflict.
 */
struct prte_mca_base_var_enum_value_flag_t {
    /** flag value (must be power-of-two) */
    int flag;
    /** corresponding string name */
    const char *string;
    /** conflicting flag(s) if any */
    int conflicting_flag;
};

typedef struct prte_mca_base_var_enum_value_flag_t prte_mca_base_var_enum_value_flag_t;

/**
 * flag enumerator base class
 */
struct prte_mca_base_var_enum_flag_t {
    /** use the existing enumerator interface */
    prte_mca_base_var_enum_t super;
    /** flag value(s) */
    prte_mca_base_var_enum_value_flag_t *enum_flags;
};

typedef struct prte_mca_base_var_enum_flag_t prte_mca_base_var_enum_flag_t;

/**
 * Object declaration for mca_base_var_enum_t
 */
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_mca_base_var_enum_t);

/**
 * Create a new default enumerator
 *
 * @param[in] name Name for this enumerator
 * @param[in] values List of values terminated with a NULL .string
 * member.
 * @param[out] enumerator Newly created enumerator.
 *
 * @retval PRTE_SUCCESS On success
 * @retval prte error code On error
 *
 * This function creates a value enumerator for integer variables. The
 * OUT enumerator value will be a newly PMIX_NEW'ed object that should
 * be released by the caller via PMIX_RELEASE.
 *
 * Note that the output enumerator can be PMIX_RELEASE'd after it has
 * been used in a cvar registration, because the variable
 * registration functions will PMIX_RETAIN the enumberator.
 *
 * Note that all the strings in the values[] array are strdup'ed into
 * internal storage, meaning that the caller can free all of the
 * strings passed in values[] after mca_base_var_enum_create()
 * returns.
 */
PRTE_EXPORT int prte_mca_base_var_enum_create(const char *name,
                                              const prte_mca_base_var_enum_value_t values[],
                                              prte_mca_base_var_enum_t **enumerator);

/**
 * Create a new default flag enumerator
 *
 * @param[in] name Name for this enumerator
 * @param[in] flags List of flags terminated with a NULL .string
 * member.
 * @param[out] enumerator Newly created enumerator.
 *
 * @retval PRTE_SUCCESS On success
 * @retval prte error code On error
 *
 * This function creates a flag enumerator for integer variables. The
 * OUT enumerator value will be a newly PMIX_NEW'ed object that should
 * be released by the caller via PMIX_RELEASE.
 *
 * Note that the output enumerator can be PMIX_RELEASE'd after it has
 * been used in a cvar registration, because the variable
 * registration functions will PMIX_RETAIN the enumberator.
 *
 * Note that all the strings in the values[] array are strdup'ed into
 * internal storage, meaning that the caller can free all of the
 * strings passed in values[] after mca_base_var_enum_create()
 * returns.
 */
PRTE_EXPORT int
prte_mca_base_var_enum_create_flag(const char *name,
                                   const prte_mca_base_var_enum_value_flag_t flags[],
                                   prte_mca_base_var_enum_flag_t **enumerator);

PRTE_EXPORT int prte_mca_base_var_enum_register(const char *project_name,
                                                const char *framework_name,
                                                const char *component_name, const char *enum_name,
                                                void *storage);
/* standard enumerators. it is invalid to call PMIX_RELEASE on any of these enumerators */
/**
 * Boolean enumerator
 *
 * This enumerator maps:
 *   positive integer, true, yes, enabled, t -> 1
 *   0, false, no, disabled, f -> 0
 */
extern prte_mca_base_var_enum_t prte_mca_base_var_enum_bool;

/**
 * Extended boolean enumerator
 *
 * This enumerator maps:
 *   positive integer, true, yes, enabled, t -> 1
 *   0, false, no, disabled, f -> 0
 *   auto -> -1
 */
extern prte_mca_base_var_enum_t prte_mca_base_var_enum_auto_bool;

/**
 * Verbosity level enumerator
 */
extern prte_mca_base_var_enum_t prte_mca_base_var_enum_verbose;

#endif /* !defined(MCA_BASE_VAR_ENUM_H) */
