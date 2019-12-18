/*
 * Copyright (c) 2011-2012 IBM Corporation.  All rights reserved.
 * Copyright (c) 2016      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 */

/** @file
 *
 * Cross Memory Attach syscall definitions.
 *
 * These are only needed temporarily until these new syscalls
 * are incorporated into glibc
 */

#ifndef PRRTE_SYS_CMA_H
#define PRRTE_SYS_CMA_H 1

#if !defined(PRRTE_ASSEMBLY_ARCH)
/* need prrte_config.h for the assembly architecture */
#include "prrte_config.h"
#endif

#include "src/sys/architecture.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_UNISTD_H
#include <sys/unistd.h>
#endif

#ifdef __linux__

/* Cross Memory Attach is so far only supported under linux */

#if PRRTE_ASSEMBLY_ARCH == PRRTE_X86_64
#define __NR_process_vm_readv 310
#define __NR_process_vm_writev 311
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_IA32
#define __NR_process_vm_readv 347
#define __NR_process_vm_writev 348
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_IA64
#define __NR_process_vm_readv 1332
#define __NR_process_vm_writev 1333
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_POWERPC32
#define __NR_process_vm_readv 351
#define __NR_process_vm_writev 352
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_POWERPC64
#define __NR_process_vm_readv 351
#define __NR_process_vm_writev 352
#elif PRRTE_ASSEMBLY_ARCH == PRRTE_ARM

#define __NR_process_vm_readv 376
#define __NR_process_vm_writev 377

#elif PRRTE_ASSEMBLY_ARCH == PRRTE_ARM64

/* ARM64 uses the asm-generic syscall numbers */

#define __NR_process_vm_readv 270
#define __NR_process_vm_writev 271

#elif PRRTE_ASSEMBLY_ARCH == PRRTE_MIPS

#if _MIPS_SIM == _MIPS_SIM_ABI64

#define __NR_process_vm_readv 5304
#define __NR_process_vm_writev 5305

#elif _MIPS_SIM == _MIPS_SIM_NABI32

#define __NR_process_vm_readv 6309
#define __NR_process_vm_writev 6310

#else

#error "Unsupported MIPS architecture for process_vm_readv and process_vm_writev syscalls"

#endif

#elif PRRTE_ASSEMBLY_ARCH == PRRTE_S390

#define __NR_process_vm_readv	340
#define __NR_process_vm_writev	341

#elif PRRTE_ASSEMBLY_ARCH == PRRTE_S390X

#define __NR_process_vm_readv	340
#define __NR_process_vm_writev	341

#else
#error "Unsupported architecture for process_vm_readv and process_vm_writev syscalls"
#endif


static inline ssize_t
process_vm_readv(pid_t pid,
                 const struct iovec  *lvec,
                 unsigned long liovcnt,
                 const struct iovec *rvec,
                 unsigned long riovcnt,
                 unsigned long flags)
{
  return syscall(__NR_process_vm_readv, pid, lvec, liovcnt, rvec, riovcnt, flags);
}

static inline ssize_t
process_vm_writev(pid_t pid,
                  const struct iovec  *lvec,
                  unsigned long liovcnt,
                  const struct iovec *rvec,
                  unsigned long riovcnt,
                  unsigned long flags)
{
  return syscall(__NR_process_vm_writev, pid, lvec, liovcnt, rvec, riovcnt, flags);
}

#endif /* __linux__ */

#endif /* PRRTE_SYS_CMA_H */
