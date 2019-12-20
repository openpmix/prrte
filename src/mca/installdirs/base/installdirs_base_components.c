/*
 * Copyright (c) 2006-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"

#include "constants.h"
#include "src/mca/mca.h"
#include "src/mca/installdirs/installdirs.h"
#include "src/mca/installdirs/base/base.h"
#include "src/mca/installdirs/base/static-components.h"

prrte_install_dirs_t prrte_install_dirs = {0};

#define CONDITIONAL_COPY(target, origin, field)                 \
    do {                                                        \
        if (origin.field != NULL && target.field == NULL) {     \
            target.field = origin.field;                        \
        }                                                       \
    } while (0)

static int
prrte_installdirs_base_open(prrte_mca_base_open_flag_t flags)
{
    prrte_mca_base_component_list_item_t *component_item;
    int ret;

    ret = prrte_mca_base_framework_components_open (&prrte_installdirs_base_framework, flags);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    PRRTE_LIST_FOREACH(component_item, &prrte_installdirs_base_framework.framework_components, prrte_mca_base_component_list_item_t) {
        const prrte_installdirs_base_component_t *component =
            (const prrte_installdirs_base_component_t *) component_item->cli_component;

        /* copy over the data, if something isn't already there */
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         prefix);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         exec_prefix);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         bindir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         sbindir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         libexecdir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         datarootdir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         datadir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         sysconfdir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         sharedstatedir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         localstatedir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         libdir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         includedir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         infodir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         mandir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         prrtedatadir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         prrtelibdir);
        CONDITIONAL_COPY(prrte_install_dirs, component->install_dirs_data,
                         prrteincludedir);
    }

    /* expand out all the fields */
    prrte_install_dirs.prefix =
        prrte_install_dirs_expand_setup(prrte_install_dirs.prefix);
    prrte_install_dirs.exec_prefix =
        prrte_install_dirs_expand_setup(prrte_install_dirs.exec_prefix);
    prrte_install_dirs.bindir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.bindir);
    prrte_install_dirs.sbindir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.sbindir);
    prrte_install_dirs.libexecdir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.libexecdir);
    prrte_install_dirs.datarootdir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.datarootdir);
    prrte_install_dirs.datadir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.datadir);
    prrte_install_dirs.sysconfdir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.sysconfdir);
    prrte_install_dirs.sharedstatedir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.sharedstatedir);
    prrte_install_dirs.localstatedir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.localstatedir);
    prrte_install_dirs.libdir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.libdir);
    prrte_install_dirs.includedir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.includedir);
    prrte_install_dirs.infodir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.infodir);
    prrte_install_dirs.mandir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.mandir);
    prrte_install_dirs.prrtedatadir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.prrtedatadir);
    prrte_install_dirs.prrtelibdir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.prrtelibdir);
    prrte_install_dirs.prrteincludedir =
        prrte_install_dirs_expand_setup(prrte_install_dirs.prrteincludedir);

#if 0
    fprintf(stderr, "prefix:         %s\n", prrte_install_dirs.prefix);
    fprintf(stderr, "exec_prefix:    %s\n", prrte_install_dirs.exec_prefix);
    fprintf(stderr, "bindir:         %s\n", prrte_install_dirs.bindir);
    fprintf(stderr, "sbindir:        %s\n", prrte_install_dirs.sbindir);
    fprintf(stderr, "libexecdir:     %s\n", prrte_install_dirs.libexecdir);
    fprintf(stderr, "datarootdir:    %s\n", prrte_install_dirs.datarootdir);
    fprintf(stderr, "datadir:        %s\n", prrte_install_dirs.datadir);
    fprintf(stderr, "sysconfdir:     %s\n", prrte_install_dirs.sysconfdir);
    fprintf(stderr, "sharedstatedir: %s\n", prrte_install_dirs.sharedstatedir);
    fprintf(stderr, "localstatedir:  %s\n", prrte_install_dirs.localstatedir);
    fprintf(stderr, "libdir:         %s\n", prrte_install_dirs.libdir);
    fprintf(stderr, "includedir:     %s\n", prrte_install_dirs.includedir);
    fprintf(stderr, "infodir:        %s\n", prrte_install_dirs.infodir);
    fprintf(stderr, "mandir:         %s\n", prrte_install_dirs.mandir);
    fprintf(stderr, "pkgdatadir:     %s\n", prrte_install_dirs.pkgdatadir);
    fprintf(stderr, "pkglibdir:      %s\n", prrte_install_dirs.pkglibdir);
    fprintf(stderr, "pkgincludedir:  %s\n", prrte_install_dirs.pkgincludedir);
#endif

    /* NTH: Is it ok not to close the components? If not we can add a flag
       to mca_base_framework_components_close to indicate not to deregister
       variable groups */
    return PRRTE_SUCCESS;
}


static int
prrte_installdirs_base_close(void)
{
    free(prrte_install_dirs.prefix);
    free(prrte_install_dirs.exec_prefix);
    free(prrte_install_dirs.bindir);
    free(prrte_install_dirs.sbindir);
    free(prrte_install_dirs.libexecdir);
    free(prrte_install_dirs.datarootdir);
    free(prrte_install_dirs.datadir);
    free(prrte_install_dirs.sysconfdir);
    free(prrte_install_dirs.sharedstatedir);
    free(prrte_install_dirs.localstatedir);
    free(prrte_install_dirs.libdir);
    free(prrte_install_dirs.includedir);
    free(prrte_install_dirs.infodir);
    free(prrte_install_dirs.mandir);
    free(prrte_install_dirs.prrtedatadir);
    free(prrte_install_dirs.prrtelibdir);
    free(prrte_install_dirs.prrteincludedir);
    memset (&prrte_install_dirs, 0, sizeof (prrte_install_dirs));

    return prrte_mca_base_framework_components_close (&prrte_installdirs_base_framework, NULL);
}

/* Declare the installdirs framework */
PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, installdirs, NULL, NULL, prrte_installdirs_base_open,
                                 prrte_installdirs_base_close, prrte_installdirs_base_static_components,
                                 PRRTE_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER | PRRTE_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);
