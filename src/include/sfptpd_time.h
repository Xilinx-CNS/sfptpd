/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_TIME_H
#define _SFPTPD_TIME_H

#include <time.h>
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

/* sfptpd high precision time struct */
struct sfptpd_timespec {
	uint64_t sec;
	uint32_t nsec;
	uint32_t nsec_frac;
};

extern const struct timespec SFPTPD_NULL_TIME;


/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Normalise a time value to ensure the nanoseconds are greater or equal to zero
 * and less than 1e9.
 * @param t  Time to normalise
 */
void sfptpd_time_normalise(struct timespec *t);

/** Add times a and b and return the result in c.
    The lvalue may also be one or both of the rvalues.
 * @param c  Pointer to time structure in which to store result (a + b)
 * @param a  First time to be added
 * @param b  Second time to be added
 */
void sfptpd_time_add(struct timespec *c, const struct timespec *a, const struct timespec *b);

/** Subtract time b from time a and return the result in c
    The lvalue may also be one or both of the rvalues.
 * @param c  Pointer to time structure in which to store result (a - b)
 * @param a  First time to be subtracted
 * @param b  Second time to be subtracted
 */
void sfptpd_time_subtract(struct timespec *c, const struct timespec *a, const struct timespec *b);

/** Compares two times.
 * @param a  First time
 * @param b  Second time
 * @return -1 if a < b, 1 if a > b or 0 if they are the same
 */
int sfptpd_time_cmp(const struct timespec *a, const struct timespec *b);

/** Find the negative of the supplied time
 * @param a  Negated result
 * @param b  Time to be negated
 */
void sfptpd_time_negate(struct timespec *a, struct timespec *b);

/** Compare time a and b and return true if a is greater than or equal to b
 * @param a  First time
 * @param b  Second time
 * @return Boolean indicating whether a is greater or equal to b
 */
bool sfptpd_time_is_greater_or_equal(const struct timespec *a, const struct timespec *b);

/** Convert a floating point number of seconds into a timespec structure.
 * @param s  Time to convert in seconds
 * @return Time converted to a timespec structure
 */
void sfptpd_time_float_s_to_timespec(const sfptpd_time_t s, struct timespec *t);

/** Convert a floating point number of nanoseconds into a timespec structure.
 * @param ns  Time to convert in nanoseconds
 * @return Time converted to a timespec structure
 */
void sfptpd_time_float_ns_to_timespec(const sfptpd_time_t ns, struct timespec *t);

/** Convert a timespec into our internal floating seconds representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in seconds
 */
sfptpd_time_t sfptpd_time_timespec_to_float_s(struct timespec *t);

/** Convert a timespec into our internal floating point representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in ns
 */
sfptpd_time_t sfptpd_time_timespec_to_float_ns(struct timespec *t);

/** Convert a scaled nanosecond value into our internal floating point representation.
 * @param t  Time to convert
 * @return A floating point representation of the time in ns
 */
sfptpd_time_t sfptpd_time_scaled_ns_to_float_ns(sfptpd_time_fp16_t t);

/** Convert a floating point ns value to a scaled nanosecond type
 * @param t  Time to convert
 * @return A scaled nanosecond representation of the value
 */
sfptpd_time_fp16_t sfptpd_time_float_ns_to_scaled_ns(sfptpd_time_t t);

/** Return the absolute value of a time difference
 * @param t Time to convert
 * @return The absolute value the time difference
 */
static inline sfptpd_time_t sfptpd_time_abs(sfptpd_time_t t) {
	return fabsl(t);
}

#endif /* _SFPTPD_TIME_H */
