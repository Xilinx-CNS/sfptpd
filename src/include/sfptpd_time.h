/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_TIME_H
#define _SFPTPD_TIME_H

#ifdef HAVE_TIME_TYPES
#include <linux/time_types.h>
#endif
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>


/****************************************************************************
 * Types and defines
****************************************************************************/

/* Floating point type used for internal representation of time */
typedef long double sfptpd_time_t;

#define ONE_BILLION (1.0e9)
#define ONE_MILLION (1.0e6)

typedef int64_t sfptpd_time_fp16_t;
typedef int64_t sfptpd_secs_t;

/* sfptpd high precision time struct */
struct sfptpd_timespec {
	sfptpd_secs_t sec;
	uint32_t nsec;
	uint32_t nsec_frac;
};

extern const struct sfptpd_timespec SFPTPD_NULL_TIME;


/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Normalise a time value to ensure the nanoseconds are greater or equal to zero
 * and less than 1e9.
 * @param t  Time to normalise
 */
void sfptpd_time_normalise(struct sfptpd_timespec *t);

/** Add times a and b and return the result in c.
    The lvalue may also be one or both of the rvalues.
 * @param c  Pointer to time structure in which to store result (a + b)
 * @param a  First time to be added
 * @param b  Second time to be added
 */
void sfptpd_time_add(struct sfptpd_timespec *c, const struct sfptpd_timespec *a, const struct sfptpd_timespec *b);

/** Subtract time b from time a and return the result in c
    The lvalue may also be one or both of the rvalues.
 * @param c  Pointer to time structure in which to store result (a - b)
 * @param a  First time to be subtracted
 * @param b  Second time to be subtracted
 */
void sfptpd_time_subtract(struct sfptpd_timespec *c, const struct sfptpd_timespec *a, const struct sfptpd_timespec *b);

/** Compares two times.
 * @param a  First time
 * @param b  Second time
 * @return -1 if a < b, 1 if a > b or 0 if they are the same
 */
int sfptpd_time_cmp(const struct sfptpd_timespec *a, const struct sfptpd_timespec *b);

/** Find the negative of the supplied time
 * @param a  Negated result
 * @param b  Time to be negated
 */
void sfptpd_time_negate(struct sfptpd_timespec *a, struct sfptpd_timespec *b);

/** Compare time a and b and return true if a is greater than or equal to b
 * @param a  First time
 * @param b  Second time
 * @return Boolean indicating whether a is greater or equal to b
 */
bool sfptpd_time_is_greater_or_equal(const struct sfptpd_timespec *a, const struct sfptpd_timespec *b);

/** Compare time a and b and return true if a and b differ by no more then
 * threshold.
 * @param a  First time
 * @param b  Second time
 * @param c  Equivalence threshold
 * @return Boolean indicating whether a and b are the same within threshold
 */
bool sfptpd_time_equal_within(const struct sfptpd_timespec *a,
			      const struct sfptpd_timespec *b,
			      const struct sfptpd_timespec *threshold);

/** Convert a floating point number of seconds into a timespec structure.
 * @param s  Time to convert in seconds
 * @return Time converted to a timespec structure
 */
static inline void sfptpd_time_float_s_to_timespec(const sfptpd_time_t s, struct sfptpd_timespec *t)
{
	sfptpd_time_t nsecf;

	t->sec = (time_t) floor(s);
	nsecf = s * 1.0e9 - (t->sec * 1.0e9);
	t->nsec = (long) nsecf;
	t->nsec_frac = ((sfptpd_time_t)(1ull << 32)) * (nsecf - (sfptpd_time_t) t->nsec);
	sfptpd_time_normalise(t);
}

/** Convert a floating point number of nanoseconds into a timespec structure.
 * @param ns  Time to convert in nanoseconds
 * @return Time converted to a timespec structure
 */
static inline void sfptpd_time_float_ns_to_timespec(const sfptpd_time_t ns, struct sfptpd_timespec *t)
{
	sfptpd_time_t nsecf;

	t->sec = (time_t)floor(ns / 1.0e9);
	nsecf = ns - (t->sec * 1.0e9);
	t->nsec = (long) (nsecf);
	t->nsec_frac = ((sfptpd_time_t)(1ull << 32)) * (nsecf - (sfptpd_time_t) t->nsec);
	sfptpd_time_normalise(t);
}

/** Convert a timespec into our internal floating seconds representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in seconds
 */
static inline sfptpd_time_t sfptpd_time_timespec_to_float_s(const struct sfptpd_timespec *t)
{
	return
		((sfptpd_time_t) t->sec) +
		((sfptpd_time_t) t->nsec) / 1.0e9 +
		((sfptpd_time_t) t->nsec_frac) / (1.0e9 * (1ull << 32));
}
/** Convert a timespec into our internal floating point representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in ns
 */
static inline sfptpd_time_t sfptpd_time_timespec_to_float_ns(const struct sfptpd_timespec *t)
{
	return
		((sfptpd_time_t) t->sec) * 1.0e9+
		((sfptpd_time_t) t->nsec) +
		((sfptpd_time_t) t->nsec_frac) / (1ull << 32);
}

/** Convert a scaled nanosecond value into our internal floating point representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in ns
 */
static inline sfptpd_time_t sfptpd_time_scaled_ns_to_float_ns(sfptpd_time_fp16_t t)
{
	/* Convert from a 64bit fixed point integer of scaled nanoseconds to
	 * a floating point ns format. */
	return ((sfptpd_time_t) t) / 65536.0;
}

/** Convert a floating point ns value to a scaled nanosecond type
 * @param t  Time to convert
 * @return A scaled nanosecond representation of the value
 */
static inline sfptpd_time_fp16_t sfptpd_time_float_ns_to_scaled_ns(sfptpd_time_t t)
{
	/* Convert to a scaled ns format and saturate. */
	t *= 65536.0;
	if (t > (sfptpd_time_t)INT64_MAX)
		return INT64_MAX;
	if (t < (sfptpd_time_t)INT64_MIN)
		return INT64_MIN;

	return (int64_t)t;
}

/** Return the absolute value of a time difference
 * @param t Time to convert
 * @return The absolute value the time difference
 */
static inline sfptpd_time_t sfptpd_time_abs(sfptpd_time_t t) {
	return fabsl(t);
}


/****************************************************************************
 * Inline functions
****************************************************************************/

/** Initialise a struct sfptpd_timespec with integral nanoseconds,
 * normalising if necessary, which should be compiled out if this is a small
 * literal.
 * @param ts  The structure to fill.
 * @param ns  The nanoseconds to set.
 */
static inline void sfptpd_time_from_ns(struct sfptpd_timespec *ts, int64_t ns)
{
	bool neg = ns < 0;;

	ts->sec = 0;
	ts->nsec = neg ? -ns : ns;
	ts->nsec_frac = 0;

	if (ns >= 1000000000)
		sfptpd_time_normalise(ts);

	if (neg)
		sfptpd_time_negate(ts, ts);
}

/** Initialise a struct sfptpd_timespec with 1588-2019 scaled nanoseconds,
 * normalising if necessary.
 * @param ts  The structure to fill.
 * @param ns  The 16-bit fixed-point nanoseconds to set.
 */
static inline void sfptpd_time_from_ns16(struct sfptpd_timespec *ts, sfptpd_time_fp16_t nsec_fp16)
{
	bool neg = nsec_fp16 < 0;
	uint64_t nsec_fp16u = neg ? -nsec_fp16 : nsec_fp16;
	uint64_t nsec = nsec_fp16u >> 16;
	ts->nsec_frac = nsec_fp16u << 16;
	if (nsec >= 1000000000) {
		ts->sec = nsec / 1000000000;
		ts->nsec = nsec - ts->sec * 1000000000;
	} else {
		ts->sec = 0;
		ts->nsec = nsec;
	}

	if (neg)
		sfptpd_time_negate(ts, ts);
}

static inline sfptpd_time_fp16_t sfptpd_time_to_ns16(struct sfptpd_timespec ts)
{
	bool neg = ts.sec < 0;
	uint64_t nsec_fp16u;

	if (neg)
		sfptpd_time_negate(&ts, &ts);

	/* Look for saturation */
	if (ts.sec >= (0x1000000000000ULL / 1000000000))
		return neg ? INT64_MIN : INT64_MAX;

	nsec_fp16u =
		((uint64_t) (ts.nsec_frac >> 16)) +
		((((uint64_t) ts.sec) * 1000000000 + ((uint64_t) ts.nsec)) << 16);

	return neg ? -nsec_fp16u : nsec_fp16u;
}

/** Initialise a struct sfptpd_timespec without normalisation.
 * @param ts  The structure to fill.
 * @param s  The seconds to set.
 * @param ns  The nanoseconds to set.
 * @param ns_frac The fractional nanoseconds to set.
 */
static inline void sfptpd_time_init(struct sfptpd_timespec *ts, int64_t s, uint32_t ns, uint32_t ns_frac)
{
	ts->sec = s;
	ts->nsec = ns;
	ts->nsec_frac = ns_frac;
}

/** Initialise a struct sfptpd_timespec with integral seconds.
 * @param ts  The structure to fill.
 * @param s  The seconds to set.
 */
static inline void sfptpd_time_from_s(struct sfptpd_timespec *ts, int64_t s)
{
	ts->sec = s;
	ts->nsec = 0;
	ts->nsec_frac = 0;
}

/** Convert a struct sfptpd_timespec to struct timespec.
 * @param ts  The structure to fill.
 * @param sfts The structure to copy.
 */
static inline void sfptpd_time_to_std_nearest(struct timespec *ts, const struct sfptpd_timespec *sfts)
{
	ts->tv_sec = sfts->sec;
	ts->tv_nsec = sfts->nsec;
	if (sfts->nsec_frac & 0x80000000UL)
		ts->tv_nsec++;
}

/** Convert a struct timespec to struct sfptpd_timespec.
 * @param sfts  The structure to fill.
 * @param ts The structure to copy.
 */
static inline void sfptpd_time_from_std_nearest(struct sfptpd_timespec *sfts, const struct timespec *ts)
{
	sfts->sec = ts->tv_sec;
	sfts->nsec = ts->tv_nsec;
	sfts->nsec_frac = 0x7FFFFFFFUL;
}

/** Convert a struct sfptpd_timespec to struct timespec.
 * @param ts  The structure to fill.
 * @param sfts The structure to copy.
 */
static inline void sfptpd_time_to_std_floor(struct timespec *ts, const struct sfptpd_timespec *sfts)
{
	ts->tv_sec = sfts->sec;
	ts->tv_nsec = sfts->nsec;
}

/** Convert a struct timespec to struct sfptpd_timespec.
 * @param sfts  The structure to fill.
 * @param ts The structure to copy.
 */
static inline void sfptpd_time_from_std_floor(struct sfptpd_timespec *sfts, const struct timespec *ts)
{
	sfts->sec = ts->tv_sec;
	sfts->nsec = ts->tv_nsec;
	sfts->nsec_frac = 0;
}

#ifdef HAVE_TIME_TYPES
/** Convert a kernel timestamp to struct sfptpd_timespec.
 * @param sfts  The structure to fill.
 * @param ts The structure to copy.
 */
static inline void sfptpd_time_from_kernel_floor(struct sfptpd_timespec *sfts, const struct __kernel_timespec *ts)
{
	sfts->sec = ts->tv_sec;
	sfts->nsec = ts->tv_nsec;
	sfts->nsec_frac = 0;
}
#endif

/** Initialise a struct sfptpd_timespec to zero.
 * @param ts  The structure to clear.
 */
static inline void sfptpd_time_zero(struct sfptpd_timespec *sfts)
{
	sfptpd_time_from_s(sfts, 0);
}

/** check if a struct sfptpd_timespec is zero.
 * @param ts  The structure to clear.
 * @return true if zero, false otherwise.
 */
static inline bool sfptpd_time_is_zero(const struct sfptpd_timespec *sfts)
{
	return sfts->sec == 0 && sfts->nsec == 0 && sfts->nsec_frac == 0;
}

/** Return zero timespec.
 * @return zero timespec.
 */
static inline struct sfptpd_timespec sfptpd_time_null(void)
{
	return (struct sfptpd_timespec) { .sec = 0, .nsec = 0, .nsec_frac = 0 };
}

/** Return max timespec.
 * @return max timespec.
 */
static inline struct sfptpd_timespec sfptpd_time_max(void)
{
	return (struct sfptpd_timespec) { .sec = INT64_MAX,
					  .nsec = ONE_BILLION -1,
					  .nsec_frac = UINT32_MAX };
}


#endif /* _SFPTPD_TIME_H */
