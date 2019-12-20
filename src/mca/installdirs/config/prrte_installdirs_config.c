/*
 * Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/mca/installdirs/installdirs.h"
#include "src/mca/installdirs/config/install_dirs.h"

const prrte_installdirs_base_component_t prrte_installdirs_config_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {
        PRRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

        /* Component name and version */
        "config",
        PRRTE_MAJOR_VERSION,
        PRRTE_MINOR_VERSION,
        PRRTE_RELEASE_VERSION,

        /* Component open and close functions */
        NULL,
        NULL
    },
    {
        /* This component is Checkpointable */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    {
        PRRTE_PREFIX,
        PRRTE_EXEC_PREFIX,
        PRRTE_BINDIR,
        PRRTE_SBINDIR,
        PRRTE_LIBEXECDIR,
        PRRTE_DATAROOTDIR,
        PRRTE_DATADIR,
        PRRTE_SYSCONFDIR,
        PRRTE_SHAREDSTATEDIR,
        PRRTE_LOCALSTATEDIR,
        PRRTE_LIBDIR,
        PRRTE_INCLUDEDIR,
        PRRTE_INFODIR,
        PRRTE_MANDIR,
        PRRTE_PKGDATADIR,
        PRRTE_PKGLIBDIR,
        PRRTE_PKGINCLUDEDIR
    }
};
