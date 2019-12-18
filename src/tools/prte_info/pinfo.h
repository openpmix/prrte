/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_INFO_TOOL_H
#define PRRTE_INFO_TOOL_H
#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "src/class/prrte_pointer_array.h"
#include "src/util/cmd_line.h"
#include "src/util/printf.h"
#include "src/mca/mca.h"

BEGIN_C_DECLS

/*
 * Globals
 */

extern bool prrte_info_pretty;
extern prrte_cmd_line_t *prrte_info_cmd_line;

extern const char *prrte_info_type_all;
extern const char *prrte_info_type_prrte;
extern const char *prrte_info_type_base;

extern prrte_pointer_array_t mca_types;


/*
 * Version-related strings and functions
 */

extern const char *prrte_info_ver_full;
extern const char *prrte_info_ver_major;
extern const char *prrte_info_ver_minor;
extern const char *prrte_info_ver_release;
extern const char *prrte_info_ver_greek;
extern const char *prrte_info_ver_svn;

void prrte_info_do_version(bool want_all, prrte_cmd_line_t *cmd_line);
void prrte_info_show_prrte_version(const char *scope);
void prrte_info_show_component_version(const char *type_name,
                                      const char *component_name,
                                      const char *scope,
                                      const char *ver_type);

/*
 * Parameter/configuration-related functions
 */

extern const char *prrte_info_component_all;
extern const char *prrte_info_param_all;

extern const char *prrte_info_path_prefix;
extern const char *prrte_info_path_bindir;
extern const char *prrte_info_path_libdir;
extern const char *prrte_info_path_incdir;
extern const char *prrte_info_path_mandir;
extern const char *prrte_info_path_pkglibdir;
extern const char *prrte_info_path_sysconfdir;
extern const char *prrte_info_path_exec_prefix;
extern const char *prrte_info_path_sbindir;
extern const char *prrte_info_path_libexecdir;
extern const char *prrte_info_path_datarootdir;
extern const char *prrte_info_path_datadir;
extern const char *prrte_info_path_sharedstatedir;
extern const char *prrte_info_path_localstatedir;
extern const char *prrte_info_path_infodir;
extern const char *prrte_info_path_pkgdatadir;
extern const char *prrte_info_path_pkgincludedir;

void prrte_info_do_params(bool want_all, bool want_internal);
void prrte_info_show_mca_params(const char *type, const char *component,
                               bool want_internal);

void prrte_info_do_path(bool want_all, prrte_cmd_line_t *cmd_line);
void prrte_info_show_path(const char *type, const char *value);

void prrte_info_do_arch(void);
void prrte_info_do_hostname(void);
void prrte_info_do_config(bool want_all);
void prrte_info_show_prrte_version(const char *scope);

/*
 * Output-related functions
 */
void prrte_info_out(const char *pretty_message,
                   const char *plain_message,
                   const char *value);
void prrte_info_out_int(const char *pretty_message,
                       const char *plain_message,
                       int value);
/*
 * Component-related functions
 */
typedef struct {
    prrte_list_item_t super;
    char *type;
    prrte_list_t *components;
    prrte_list_t *failed_components;
} prrte_info_component_map_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_info_component_map_t);

extern prrte_pointer_array_t prrte_component_map;

void prrte_info_components_open(void);
void prrte_info_components_close(void);

END_C_DECLS

#endif /* PRRTE_INFO_H */
