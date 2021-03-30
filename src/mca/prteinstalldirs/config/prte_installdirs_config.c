/*
 * Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/mca/prteinstalldirs/config/install_dirs.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"

const prte_prteinstalldirs_base_component_t prte_prteinstalldirs_config_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

     /* Component name and version */
     "config", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,

     /* Component open and close functions */
     NULL, NULL},
    {/* This component is Checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},

    {PRTE_PREFIX, PRTE_EXEC_PREFIX, PRTE_BINDIR, PRTE_SBINDIR, PRTE_LIBEXECDIR, PRTE_DATAROOTDIR,
     PRTE_DATADIR, PRTE_SYSCONFDIR, PRTE_SHAREDSTATEDIR, PRTE_LOCALSTATEDIR, PRTE_LIBDIR,
     PRTE_INCLUDEDIR, PRTE_INFODIR, PRTE_MANDIR, PRTE_PKGDATADIR, PRTE_PKGLIBDIR,
     PRTE_PKGINCLUDEDIR}};
