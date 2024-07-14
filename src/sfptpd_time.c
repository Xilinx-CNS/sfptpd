/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_time.c
 * @brief  Time manipulation functions
 */

#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <math.h>

#include "sfptpd_time.h"


/****************************************************************************
 * Types
 ****************************************************************************/

/* sfptpd high precision time struct.
 * This version has a signed nsec value, which is not meaningful at the API
 * level but helps implement internal normalisation.
 */
struct sfptpd_signed_timespec {
	int64_t sec;
	int32_t nsec;
	uint32_t nsec_frac;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

const struct sfptpd_timespec SFPTPD_NULL_TIME = {
	.sec = 0,
	.nsec = 0,
	.nsec_frac = 0,
};


/****************************************************************************
 * Time Manipulation Functions
 ****************************************************************************/

void sfptpd_time_normalise(struct sfptpd_timespec *t)
{
	struct sfptpd_signed_timespec *st = (struct sfptpd_signed_timespec *) t;

	assert(st);

	st->sec += st->nsec / 1000000000;
	st->nsec -= (st->nsec / 1000000000) * 1000000000;

	if (st->nsec < 0) {
		st->sec -= 1;
		st->nsec += 1000000000;
	}
}

void sfptpd_time_add(struct sfptpd_timespec *c,
		     const struct sfptpd_timespec *a,
		     const struct sfptpd_timespec *b)
{
	uint64_t nsec_fp32;

	assert(a);
	assert(b);
	assert(c);

	nsec_fp32 =
		(((uint64_t) a->nsec) << 32) +
		(((uint64_t) b->nsec) << 32) +
		a->nsec_frac +
		b->nsec_frac;

	c->nsec_frac = nsec_fp32;
	c->nsec = nsec_fp32 >> 32;
	c->sec = a->sec + b->sec;
	
	sfptpd_time_normalise(c);
}

void sfptpd_time_subtract(struct sfptpd_timespec *c,
			  const struct sfptpd_timespec *a,
			  const struct sfptpd_timespec *b)
{
	int64_t a_nsec_fp32 = (((uint64_t) a->nsec) << 32) | a->nsec_frac;
	int64_t b_nsec_fp32 = (((uint64_t) b->nsec) << 32) | b->nsec_frac;
	int64_t c_nsec_fp32 = a_nsec_fp32 - b_nsec_fp32;

	c->sec = a->sec - b->sec;
	c->nsec = c_nsec_fp32 >> 32;
	c->nsec_frac = c_nsec_fp32;
	
	sfptpd_time_normalise(c);
}

bool sfptpd_time_equal_within(const struct sfptpd_timespec *a,
			      const struct sfptpd_timespec *b,
			      const struct sfptpd_timespec *threshold)
{
	struct sfptpd_timespec c;

	sfptpd_time_subtract(&c, a, b);
	if (c.sec < 0)
		sfptpd_time_negate(&c, &c);

	return sfptpd_time_is_greater_or_equal(threshold, &c);
}

int sfptpd_time_cmp(const struct sfptpd_timespec *a, const struct sfptpd_timespec *b)
{
	if (a->sec < b->sec)
		return -1;
	else if (a->sec > b->sec)
		return 1;
	else if (a->nsec < b->nsec)
		return -1;
	else if (a->nsec > b->nsec)
		return 1;
	else if (a->nsec_frac < b->nsec_frac)
		return -1;
	else if (a->nsec_frac > b->nsec_frac)
		return 1;
	else
		return 0;
}

void sfptpd_time_negate(struct sfptpd_timespec *a, struct sfptpd_timespec *b)
{
	sfptpd_time_subtract(a, &SFPTPD_NULL_TIME, b);
}

bool sfptpd_time_is_greater_or_equal(const struct sfptpd_timespec *a, const struct sfptpd_timespec *b)
{
	struct sfptpd_timespec c;
	assert(a);
	assert(b);

	sfptpd_time_subtract(&c, a, b);
	return (c.sec >= 0);
}

/* fin */
