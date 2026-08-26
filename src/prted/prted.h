/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTED_H
#define PRTED_H

#include "prte_config.h"
#include "types.h"

#include <time.h>

#include "src/class/pmix_pointer_array.h"
#include "src/rml/rml_types.h"
#include "src/mca/schizo/schizo.h"
#include "src/util/prte_cmd_line.h"

BEGIN_C_DECLS

/* main orted routine */
PRTE_EXPORT int prte_daemon(int argc, char *argv[]);

/* orted communication functions */
PRTE_EXPORT void prte_daemon_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata);

/* direct cmd processing entry points */
PRTE_EXPORT void prte_daemon_cmd_processor(int fd, short event, void *data);
PRTE_EXPORT int prte_daemon_process_commands(pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                             prte_rml_tag_t tag);

/**
 * Turn an application command line - with its ':'-separated app contexts -
 * into a list of prte_pmix_app_t.
 *
 * @param results  the tool's parse of the whole command line, or NULL if it
 *                 has none.  The job-level options ("--output", "--display",
 *                 "--rtos") are handed back here: that parse stops at the
 *                 first executable, so it cannot see one written in a later
 *                 app segment, and every consumer of them reads it.
 */
PRTE_EXPORT int prte_parse_locals(prte_schizo_base_module_t *schizo, pmix_list_t *jdata,
                                  char **argv, char ***hostfiles, char ***hosts,
                                  pmix_list_t *jobdata,
                                  pmix_cli_result_t *results);

PRTE_EXPORT int prun_common(pmix_cli_result_t *cli,
                            prte_schizo_base_module_t *schizo,
                            int argc, char **argv);

PRTE_EXPORT int prte_prun_parse_common_cli(void *jinfo, pmix_cli_result_t *results,
                                           prte_schizo_base_module_t *schizo,
                                           pmix_list_t *apps);

/* Normalize a prefix string in place, removing any trailing path
 * separators.  A value consisting solely of separators becomes a single
 * separator; an empty string is left alone. */
PRTE_EXPORT void prte_strip_trailing_pathsep(char *param);

/* Split a singleton identifier of the form "<nspace>.<rank>" into its
 * parts.  Returns PRTE_ERR_BAD_PARAM if the value is not of that form. */
PRTE_EXPORT int prte_parse_singleton_id(const char *name, pmix_nspace_t nspace,
                                        pmix_rank_t *rank);

/* Append the contents of an appfile to a command line, one app context per
 * line, joined with the ":" delimiter.  Blank lines are ignored.  Returns
 * PRTE_ERR_FILE_OPEN_FAILURE if the file cannot be read. */
PRTE_EXPORT int prte_parse_appfile(const char *path, char ***pargv, int *pargc);
END_C_DECLS

#endif /* PRTED_H */
