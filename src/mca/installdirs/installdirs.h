/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2006-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_MCA_INSTALLDIRS_INSTALLDIRS_H
#define PRRTE_MCA_INSTALLDIRS_INSTALLDIRS_H

#include "prrte_config.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

BEGIN_C_DECLS

/*
 * Most of this file is just for ompi_info.  The only public interface
 * once prrte_init has been called is the prrte_install_dirs structure
 * and the prrte_install_dirs_expand() call */
struct prrte_install_dirs_t {
    char* prefix;
    char* exec_prefix;
    char* bindir;
    char* sbindir;
    char* libexecdir;
    char* datarootdir;
    char* datadir;
    char* sysconfdir;
    char* sharedstatedir;
    char* localstatedir;
    char* libdir;
    char* includedir;
    char* infodir;
    char* mandir;

    /* Note that the following fields intentionally have an "ompi"
       prefix, even though they're down in the PRRTE layer.  This is
       not abstraction break because the "ompi" they're referring to
       is for the build system of the overall software tree -- not an
       individual project within that overall tree.

       Rather than using pkg{data,lib,includedir}, use our own
       ompi{data,lib,includedir}, which is always set to
       {datadir,libdir,includedir}/openmpi. This will keep us from
       having help files in prefix/share/open-rte when building
       without PRRTE, but in prefix/share/openmpi when building
       with PRRTE.

       Note that these field names match macros set by configure that
       are used in Makefile.am files.  E.g., project help files are
       installed into $(prrtedatadir). */
    char* prrtedatadir;
    char* prrtelibdir;
    char* prrteincludedir;
};
typedef struct prrte_install_dirs_t prrte_install_dirs_t;

/* Install directories.  Only available after prrte_init() */
PRRTE_EXPORT extern prrte_install_dirs_t prrte_install_dirs;

/**
 * Expand out path variables (such as ${prefix}) in the input string
 * using the current prrte_install_dirs structure */
PRRTE_EXPORT char * prrte_install_dirs_expand(const char* input);


/**
 * Structure for installdirs components.
 */
struct prrte_installdirs_base_component_2_0_0_t {
    /** MCA base component */
    prrte_mca_base_component_t component;
    /** MCA base data */
    prrte_mca_base_component_data_t component_data;
    /** install directories provided by the given component */
    prrte_install_dirs_t install_dirs_data;
};
/**
 * Convenience typedef
 */
typedef struct prrte_installdirs_base_component_2_0_0_t prrte_installdirs_base_component_t;

/*
 * Macro for use in components that are of type installdirs
 */
#define PRRTE_INSTALLDIRS_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("installdirs", 2, 0, 0)

END_C_DECLS

#endif /* PRRTE_MCA_INSTALLDIRS_INSTALLDIRS_H */
