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
 * Constants
 ****************************************************************************/

const struct timespec SFPTPD_NULL_TIME = {
  .tv_sec = 0,
  .tv_nsec = 0
};


/****************************************************************************
 * Time Manipulation Functions
 ****************************************************************************/

void sfptpd_time_normalise(struct timespec *t)
{
	assert(t);

	t->tv_sec += t->tv_nsec / 1000000000;
	t->tv_nsec -= (t->tv_nsec / 1000000000) * 1000000000;

	if (t->tv_nsec < 0) {
		t->tv_sec -= 1;
		t->tv_nsec += 1000000000;
	}
}

void sfptpd_time_add(struct timespec *c, const struct timespec *a,
		     const struct timespec *b)
{
	assert(a);
	assert(b);
	assert(c);

	c->tv_sec = a->tv_sec + b->tv_sec;
	c->tv_nsec = a->tv_nsec + b->tv_nsec;
	
	sfptpd_time_normalise(c);
}

void sfptpd_time_subtract(struct timespec *c, const struct timespec *a,
			  const struct timespec *b)
{
	assert(a);
	assert(b);
	assert(c);

	c->tv_sec = a->tv_sec - b->tv_sec;
	c->tv_nsec = a->tv_nsec - b->tv_nsec;
	
	sfptpd_time_normalise(c);
}


int sfptpd_time_cmp(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec < b->tv_sec)
		return -1;
	else if (a->tv_sec > b->tv_sec)
		return 1;
	else if (a->tv_nsec < b->tv_nsec)
		return -1;
	else if (a->tv_nsec > b->tv_nsec)
		return 1;
	else
		return 0;
}


void sfptpd_time_negate(struct timespec *a, struct timespec *b)
{
	sfptpd_time_subtract(a, &SFPTPD_NULL_TIME, b);
}


bool sfptpd_time_is_greater_or_equal(const struct timespec *a, const struct timespec *b)
{
	struct timespec c;
	assert(a);
	assert(b);

	sfptpd_time_subtract(&c, a, b);
	return (c.tv_sec >= 0);
}

void sfptpd_time_float_s_to_timespec(const sfptpd_time_t s, struct timespec *t)
{
	assert(t);
	t->tv_sec = (time_t)floor(s);
	t->tv_nsec = (long)(s * 1.0e9 - (t->tv_sec * 1.0e9));
	sfptpd_time_normalise(t);
}

void sfptpd_time_float_ns_to_timespec(const sfptpd_time_t ns, struct timespec *t)
{
	assert(t);
	t->tv_sec = (time_t)floor(ns / 1.0e9);
	t->tv_nsec = (long)(ns - (t->tv_sec * 1.0e9));
	sfptpd_time_normalise(t);
}


sfptpd_time_t sfptpd_time_timespec_to_float_s(struct timespec *t)
{
	assert(t);
	return (sfptpd_time_t)t->tv_sec + ((sfptpd_time_t)t->tv_nsec / 1.0e9);
}


sfptpd_time_t sfptpd_time_timespec_to_float_ns(struct timespec *t)
{
	assert(t);
	return ((sfptpd_time_t)t->tv_sec * 1.0e9) + (sfptpd_time_t)t->tv_nsec;
}


sfptpd_time_t sfptpd_time_scaled_ns_to_float_ns(sfptpd_time_fp16_t t)
{
	/* Convert from a 64bit fixed point integer of scaled nanoseconds to
	 * a floating point ns format. */
	return (sfptpd_time_t)t / 65536.0;
}


sfptpd_time_fp16_t sfptpd_time_float_ns_to_scaled_ns(sfptpd_time_t t)
{
	/* Convert to a scaled ns format and saturate. */
	t *= 65536.0;
	if (t > (sfptpd_time_t)INT64_MAX)
		return INT64_MAX;
	if (t < (sfptpd_time_t)INT64_MIN)
		return INT64_MIN;

	return (int64_t)t;
}


/* fin */
