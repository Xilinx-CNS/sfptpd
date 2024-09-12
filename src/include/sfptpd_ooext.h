/* SPDX-License-Identifier: BSD-3-Clause */
/* SPDX-FileCopyrightText: (c) Copyright 2024 Advanced Micro Devices, Inc. */

/* Onload extensions API. Minimal definitions for extension timestamping. */

#ifndef _SFPTPD_OOEXT_H
#define _SFPTPD_OOEXT_H

#include <stdint.h>

#ifdef HAVE_ONLOAD_EXT
#include <onload/extensions.h>
#else

struct onload_timestamp {
	uint64_t sec;
	uint32_t nsec;
	unsigned nsec_frac:24;
	unsigned flags:8;
};

#endif

#ifndef SO_TIMESTAMPING_OOEXT
#define SO_TIMESTAMPING_OOEXT 0x000F5300
#define SCM_TIMESTAMPING_OOEXT SO_TIMESTAMPING_OOEXT

struct scm_timestamping_ooext {
	uint32_t type;
	uint32_t padding;
	struct onload_timestamp timestamp;
};
#endif

#endif
