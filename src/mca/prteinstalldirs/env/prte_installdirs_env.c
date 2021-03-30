/*
 * Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"

static int prteinstalldirs_env_open(void);

prte_prteinstalldirs_base_component_t prte_prteinstalldirs_env_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

     /* Component name and version */
     "env", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PRTE_RELEASE_VERSION,

     /* Component open and close functions */
     prteinstalldirs_env_open, NULL},
    {/* This component is checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},

    /* Next the prte_install_dirs_t install_dirs_data information */
    {
        NULL,
    },
};

#define SET_FIELD(field, envname)                                         \
    do {                                                                  \
        char *tmp = getenv(envname);                                      \
        if (NULL != tmp && 0 == strlen(tmp)) {                            \
            tmp = NULL;                                                   \
        }                                                                 \
        prte_prteinstalldirs_env_component.install_dirs_data.field = tmp; \
    } while (0)

static int prteinstalldirs_env_open(void)
{
    SET_FIELD(prefix, "PRTE_PREFIX");
    SET_FIELD(exec_prefix, "PRTE_EXEC_PREFIX");
    SET_FIELD(bindir, "PRTE_BINDIR");
    SET_FIELD(sbindir, "PRTE_SBINDIR");
    SET_FIELD(libexecdir, "PRTE_LIBEXECDIR");
    SET_FIELD(datarootdir, "PRTE_DATAROOTDIR");
    SET_FIELD(datadir, "PRTE_DATADIR");
    SET_FIELD(sysconfdir, "PRTE_SYSCONFDIR");
    SET_FIELD(sharedstatedir, "PRTE_SHAREDSTATEDIR");
    SET_FIELD(localstatedir, "PRTE_LOCALSTATEDIR");
    SET_FIELD(libdir, "PRTE_LIBDIR");
    SET_FIELD(includedir, "PRTE_INCLUDEDIR");
    SET_FIELD(infodir, "PRTE_INFODIR");
    SET_FIELD(mandir, "PRTE_MANDIR");
    SET_FIELD(prtedatadir, "PRTE_PKGDATADIR");
    SET_FIELD(prtelibdir, "PRTE_PKGLIBDIR");
    SET_FIELD(prteincludedir, "PRTE_PKGINCLUDEDIR");

    return PRTE_SUCCESS;
}
