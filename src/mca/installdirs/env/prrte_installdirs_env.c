/*
 * Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
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

#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/mca/installdirs/installdirs.h"

static int installdirs_env_open(void);


prrte_installdirs_base_component_t prrte_installdirs_env_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {
        PRRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

        /* Component name and version */
        "env",
        PRRTE_MAJOR_VERSION,
        PRRTE_MINOR_VERSION,
        PRRTE_RELEASE_VERSION,

        /* Component open and close functions */
        installdirs_env_open,
        NULL
    },
    {
        /* This component is checkpointable */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    /* Next the prrte_install_dirs_t install_dirs_data information */
    {
        NULL,
    },
};


#define SET_FIELD(field, envname)                                         \
    do {                                                                  \
        char *tmp = getenv(envname);                                      \
         if (NULL != tmp && 0 == strlen(tmp)) {                           \
             tmp = NULL;                                                  \
         }                                                                \
         prrte_installdirs_env_component.install_dirs_data.field = tmp;   \
    } while (0)


static int
installdirs_env_open(void)
{
    SET_FIELD(prefix, "PRRTE_PREFIX");
    SET_FIELD(exec_prefix, "PRRTE_EXEC_PREFIX");
    SET_FIELD(bindir, "PRRTE_BINDIR");
    SET_FIELD(sbindir, "PRRTE_SBINDIR");
    SET_FIELD(libexecdir, "PRRTE_LIBEXECDIR");
    SET_FIELD(datarootdir, "PRRTE_DATAROOTDIR");
    SET_FIELD(datadir, "PRRTE_DATADIR");
    SET_FIELD(sysconfdir, "PRRTE_SYSCONFDIR");
    SET_FIELD(sharedstatedir, "PRRTE_SHAREDSTATEDIR");
    SET_FIELD(localstatedir, "PRRTE_LOCALSTATEDIR");
    SET_FIELD(libdir, "PRRTE_LIBDIR");
    SET_FIELD(includedir, "PRRTE_INCLUDEDIR");
    SET_FIELD(infodir, "PRRTE_INFODIR");
    SET_FIELD(mandir, "PRRTE_MANDIR");
    SET_FIELD(prrtedatadir, "PRRTE_PKGDATADIR");
    SET_FIELD(prrtelibdir, "PRRTE_PKGLIBDIR");
    SET_FIELD(prrteincludedir, "PRRTE_PKGINCLUDEDIR");

    return PRRTE_SUCCESS;
}
