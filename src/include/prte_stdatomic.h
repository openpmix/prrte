/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PRTE_STDATOMIC_H)
#    define PRTE_STDATOMIC_H

#    include "prte_stdint.h"
#    include <stdbool.h>

/* PRRTE shares almost all of its threading primitives with PMIx and needs
 * exactly one atomic type of its own: the flag the OOB's listener thread
 * spins on while the main thread clears it (oob_tcp_listener.c).  The other
 * nine typedefs this header used to carry - int/long/int32/uint32/int64/
 * uint64/size/ssize/intptr/uintptr - had no users at all.
 *
 * C11 atomics are a requirement, not a preference, so there is no
 * alternative arm here - PMIx's pmix_stdatomic.h says the same thing the
 * same way, and the two projects share a threading model.  There used to be
 * a `volatile`-typed fallback selected by PRTE_ATOMIC_C11 == 0; `volatile`
 * is not an atomic type, so that arm was a way to build something that
 * could not be correct.  PRTE_CONFIG_ASM (config/prte_config_asm.m4) now
 * fails configure outright when the compiler cannot supply C11 atomics.
 */
#    include <stdatomic.h>

typedef _Atomic bool prte_atomic_bool_t;

#endif /* !defined(PRTE_STDATOMIC_H) */
