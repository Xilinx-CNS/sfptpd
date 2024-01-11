/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

/* If SFPTPD_GLIBC_COMPAT is defined, specify old versions of specific
 * glibc symbols that are to be used at runtime.
 *
 * The current set of definitions allows builds with glibc-2.17 to be
 * run on glibc-2.12 targets. E.g. a build on EL7 to run on EL6.
 */

#ifndef _GLIBC_COMPAT_H
#define _GLIBC_COMPAT_H

#ifdef SFPTPD_GLIBC_COMPAT

#if defined(__x86_64__) || defined(_M_X64)
/* Solution only verified on amd64 architecture so far. */

__asm__(".symver memcpy,memcpy@GLIBC_2.2.5");
__asm__(".symver glob,glob@GLIBC_2.2.5");

#endif

#endif

#endif /* _GLIBC_COMPAT_H */
