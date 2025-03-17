/*
 * Copyright (c) 2025      NVIDIA Corporation.  All rights reserved.
 * Copyright (c) 2025      Nanook Consulting  All rights reserved.
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
#include <dlfcn.h>
#include "src/util/pmix_basename.h"

static int prteinstalldirs_runtime_open(void);

prte_prteinstalldirs_base_component_t prte_mca_prteinstalldirs_runtime_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    .component = {
        PRTE_INSTALLDIRS_BASE_VERSION_2_0_0,

         /* Component name and version */
         .pmix_mca_component_name = "runtime",
         PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PRTE_RELEASE_VERSION),
         /* Component open and close functions */
         .pmix_mca_open_component = prteinstalldirs_runtime_open
    },

    /* Next the prte_install_dirs_t install_dirs_data information */
    {
        .prefix = NULL,
        .exec_prefix = NULL,
        .bindir = NULL,
        .sbindir = NULL,
        .libexecdir = NULL,
        .datarootdir = NULL,
        .datadir = NULL,
        .sysconfdir = NULL,
        .sharedstatedir = NULL,
        .localstatedir = NULL,
        .libdir = NULL,
        .includedir = NULL,
        .infodir = NULL,
        .mandir = NULL,
        .prtedatadir = NULL,
        .prtelibdir = NULL,
        .prteincludedir = NULL,
    },
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, prteinstalldirs, runtime)

static int prteinstalldirs_runtime_open(void)
{
    Dl_info info;
    void* prte_fct;

    /* Casting from void* to fct pointer according to POSIX.1-2001 and POSIX.1-2008 */
    *(void **)&prte_fct = dlsym(RTLD_DEFAULT, "pmix_init_util");

    if( 0 == dladdr(prte_fct, &info) ) {
        /* Can't find the symbol */
        return PRTE_ERROR;
    }

    char* dname = pmix_dirname(info.dli_fname);
    char* prefix = pmix_dirname(dname);
    free(dname);

    prte_mca_prteinstalldirs_runtime_component.install_dirs_data.prefix = prefix;

    return PRTE_SUCCESS;
}
