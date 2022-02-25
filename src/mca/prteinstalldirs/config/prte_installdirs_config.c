/*
 * Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/mca/prteinstalldirs/config/install_dirs.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"

const prte_prteinstalldirs_base_component_t prte_mca_prteinstalldirs_config_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    .component = {
        PRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "config",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),
    },
    .install_dirs_data = {
        .prefix = PRTE_PREFIX,
        .exec_prefix = PRTE_EXEC_PREFIX,
        .bindir = PRTE_BINDIR,
        .sbindir = PRTE_SBINDIR,
        .libexecdir = PRTE_LIBEXECDIR,
        .datarootdir = PRTE_DATAROOTDIR,
        .datadir = PRTE_DATADIR,
        .sysconfdir = PRTE_SYSCONFDIR,
        .sharedstatedir = PRTE_SHAREDSTATEDIR,
        .localstatedir = PRTE_LOCALSTATEDIR,
        .libdir = PRTE_LIBDIR,
        .includedir = PRTE_INCLUDEDIR,
        .infodir = PRTE_INFODIR,
        .mandir = PRTE_MANDIR,
        .prtedatadir = PRTE_PKGDATADIR,
        .prtelibdir = PRTE_PKGLIBDIR,
        .prteincludedir = PRTE_PKGINCLUDEDIR
    }
};
