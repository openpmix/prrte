/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Voltaire All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_ALIGN_H
#define PRRTE_ALIGN_H

#define PRRTE_DOWN_ALIGN(x,a,t) ((x) & ~(((t)(a)-1)))
#define PRRTE_DOWN_ALIGN_PTR(x,a,t) ((t)PRRTE_DOWN_ALIGN((uintptr_t)x, a, uintptr_t))
#define PRRTE_ALIGN(x,a,t) (((x)+((t)(a)-1)) & ~(((t)(a)-1)))
#define PRRTE_ALIGN_PTR(x,a,t) ((t)PRRTE_ALIGN((uintptr_t)x, a, uintptr_t))
#define PRRTE_ALIGN_PAD_AMOUNT(x,s) ((~((uintptr_t)(x))+1) & ((uintptr_t)(s)-1))

#endif /* PRRTE_ALIGN_H */
