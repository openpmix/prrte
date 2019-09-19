/*
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2007-2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef PRRTE_INSTALLDIRS_BASE_H
#define PRRTE_INSTALLDIRS_BASE_H

#include "prrte_config.h"
#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/mca/installdirs/installdirs.h"

/*
 * Global functions for MCA overall installdirs open and close
 */
BEGIN_C_DECLS

/**
 * Framework structure declaration
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_installdirs_base_framework;

/* Just like prrte_install_dirs_expand() (see installdirs.h), but will
   also insert the value of the environment variable $PRRTE_DESTDIR, if
   it exists/is set.  This function should *only* be used during the
   setup routines of installdirs. */
char * prrte_install_dirs_expand_setup(const char* input);

END_C_DECLS

#endif /* PRRTE_BASE_INSTALLDIRS_H */
