/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_FILTER_H
#define _SFPTPD_FILTER_H

#include <stdbool.h>
#include "sfptpd_ptp_timestamp_dataset.h"



/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Maximum filter stiffness allowed for FIR filters */
#define SFPTPD_FIR_FILTER_STIFFNESS_MIN 1
#define SFPTPD_FIR_FILTER_STIFFNESS_MAX 128

/** FIR Filter data structure 
 * @stiffness: Filter stiffness
 * @num_samples: Number of samples currently in the filter
 * @write_idx: Index where next delta will be written
 * @delta: Filter data set
 */
typedef struct sfptpd_fir_filter {
	unsigned int stiffness;
	unsigned int num_samples;
	unsigned int write_idx;
	long double delta[SFPTPD_FIR_FILTER_STIFFNESS_MAX];
} sfptpd_fir_filter_t;


/** PID Filter data structure
 * @k_p: Proportional term constant
 * @k_i: Integral term constant
 * @k_d: Differential term constant
 * @configured_interval: The configured interval between updates. Used as a
 * constant when times are not provided at each update
 * @average_interval: The average interval between updates. When times are
 * provided this is used to filter out excessively large or small intervals
 * @last_update: Time of last PID filter update
 * @i_max: Maximum size for the integral term or 0 if feature not enabled
 * @p: Proportial term
 * @i: Integral term
 * @d: Differential term
 * @freq_adjust Calculated frequency adjustment
 */
typedef struct sfptpd_pid_filter {
	long double k_p;
	long double k_i;
	long double k_d;

	/** The configured interval between updates. Used as a constant when
	 * times are not provided at each update. */
	long double configured_interval;

	/** The average interval between updates. When times are provided,
	 * this is used to filter out excessively large or small intervals. */
	long double average_interval;

	/** Time of last PID filter update */
	struct sfptpd_timespec last_update;

	/** Maximum size for the integral term or 0 if feature not enabled */
	long double i_max;

	/** The proportional, integral and differential terms */
	long double p;
	long double i;
	long double d;

	/** Calculated frequency adjustment */
	long double freq_adjust;

} sfptpd_pid_filter_t;


/** Notch filter 
 * @min Minimum acceptable interval
 * @max Maximum acceptable interval
 */
typedef struct sfptpd_notch_filter {
	long double min;
	long double max;
} sfptpd_notch_filter_t;

/* Filter based on Peirce's criterion operating on the most recent n samples */
struct sfptpd_peirce_filter {
	/* Maximum samples to consider */
	unsigned int max_samples;

	/* Weighting to give to outliers */
	long double outlier_weighting;

	/* Standard deviation measure */
	struct sfptpd_stats_std_dev std_dev;

	/* Number of samples currently in filter */
	unsigned int num_samples;

	/* Index where next delta will be written */
	unsigned int write_idx;

	/* Array of data samples */
	long double data[0];
};

/** Minimum number of samples of the Peirce filter to operate correctly */
#define SFPTPD_PEIRCE_FILTER_SAMPLES_MIN 5

/** Maximum supported size of Peirce filter */
#define SFPTPD_PEIRCE_FILTER_SAMPLES_MAX 60

/** Forward declaration of Peirce filter */
struct sfptpd_peirce_filter;

/** Forward declaration of Smallest filter */
struct sfptpd_smallest_filter;

/** Minimum number of samples possible for smallest filter */
#define SFPTPD_SMALLEST_FILTER_SAMPLES_MIN 1

/** Maximum size for smallest filter, if larger then doesn't converge */
#define SFPTPD_SMALLEST_FILTER_SAMPLES_MAX 25

/** Minimum size for smallest filter timeout */
#define SFPTPD_SMALLEST_FILTER_TIMEOUT_MIN 10

/** Maximum size for smallest filter timeout */
#define SFPTPD_SMALLEST_FILTER_TIMEOUT_MAX 20

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise an FIR filter
 * @param fir Pointer to FIR filter structure
 * @param stiffness Filter stiffness
 * @return 0 on success or errno
 */
void sfptpd_fir_filter_init(sfptpd_fir_filter_t *fir, unsigned int stiffness);

/** Reset a FIR filter. This reinitialises the filter
 * @param fir FIR filter instance
 */
void sfptpd_fir_filter_reset(sfptpd_fir_filter_t *fir);

/** Update the FIR filter with new sample data.
 * @param fir FIR filter instance
 * @param delta Current difference between signal and control
 * @return The output of the FIR filter
 */
long double sfptpd_fir_filter_update(sfptpd_fir_filter_t *fir,
				     long double delta);


/** Initialise a PID filter
 * @param pid Pointer to PID filter structure
 * @param k_p Proportional term constant
 * @param k_i Integral term constant
 * @param k_d Differential term constant
 * @param interval Configured interval between updates
 * @return 0 on success or errno
 */
void sfptpd_pid_filter_init(sfptpd_pid_filter_t *pid, long double k_p,
			    long double k_i, long double k_d,
			    long double interval);

/** Set a maximum value for the integral term. This limits how big the integral
 * can grow if the difference between signal and control is very large.
 * @param pid PID filter instance
 * @param i_max Maximum amplitude of integral term or 0 to disable
 */
void sfptpd_pid_filter_set_i_term_max(sfptpd_pid_filter_t *pid,
				      long double i_max);

/** Set the default time interval for the PID filter. This sets the expected
 * interval between calls to update().
 * @param pid PID filter instance
 * @param interval Configured interval between updates
 */
void sfptpd_pid_filter_set_interval(sfptpd_pid_filter_t *pid,
				    long double interval);

/** Reset a PID filter. This reinitialises the filters and proportional,
 * intergral and differentiate terms
 * @param pid PID filter instance
 */
void sfptpd_pid_filter_reset(sfptpd_pid_filter_t *pid);

/** Reconfigure a PID filter.
 * @param pid PID filter instance
 * @param k_p The new proportional term coefficient or NAN
 * @param k_i The new integral term coefficient or NAN
 * @param k_d The new differential term coefficient or NAN
 * @param reset True if the filter should be reset
 */
void sfptpd_pid_filter_adjust(sfptpd_pid_filter_t *pid, long double k_p,
			      long double k_i, long double k_d,
			      bool reset);

/** Update the PID filter with new sample data.
 * @param pid PID filter instance
 * @param delta Current difference between signal and control
 * @param time Current time. If supplied, this will be used calculate the
 * interval between this and the last update to use for the I and D terms.
 * If NULL supplied, the default configured interval will be used.
 * @return The calculated frequency adjust that should be applied
 */
long double sfptpd_pid_filter_update(sfptpd_pid_filter_t *pid, long double delta,
				     struct sfptpd_timespec *time);

/** Get the current proportional term for the PID filter
 * @param pid PID filter instance
 * @return The current proportional term
 */
long double sfptpd_pid_filter_get_p_term(sfptpd_pid_filter_t *pid);

/** Get the current integral term for the PID filter
 * @param pid PID filter instance
 * @return The current integral term
 */
long double sfptpd_pid_filter_get_i_term(sfptpd_pid_filter_t *pid);

/** Get the current differential term for the PID filter
 * @param pid PID filter instance
 * @return The current differential term
 */
long double sfptpd_pid_filter_get_d_term(sfptpd_pid_filter_t *pid);


/** Initialise a notch filter
 * @param notch Pointer to notch filter structure
 * @param mid_point Nominal expected interval
 * @param width Acceptable variation from nominal value
 * @return 0 on success or errno
 */
void sfptpd_notch_filter_init(sfptpd_notch_filter_t *notch,
			      long double mid_point, long double width);

/** Apply the notch filter with new sample data.
 * @param notch Notch filter instance
 * @param interval New interval
 * @return 0 or ERANGE according to whether the data lies in the acceptable
 * range.
 */
int sfptpd_notch_filter_update(sfptpd_notch_filter_t *notch,
			       long double interval);


/** Create a Pierce filter
 * @param max_samples Maximum sample size to operate filter on
 * @param outlier_weighting Weighting given to outliers before feeding
 * the sample into the data set.
 * @return A pointer to the filter structure or NULL on error
 */
struct sfptpd_peirce_filter *sfptpd_peirce_filter_create(unsigned int max_samples,
							 long double outlier_weighting);

/** Destroy a filter instance
 * @param filter Pointer to filter instance
 */
void sfptpd_peirce_filter_destroy(struct sfptpd_peirce_filter *filter);

/** Reset a peirce filter. This re-initialises the filter
 * @param pid PID filter instance
 */
void sfptpd_peirce_filter_reset(struct sfptpd_peirce_filter *filter);

/** Update the peirce filter with new sample data.
 * @param filter Pointer to filter instance
 * @param sample New data sample
 * @return 0 or ERANGE according to whether the data is considered an outlier.
 */
int sfptpd_peirce_filter_update(struct sfptpd_peirce_filter *filter,
			        long double sample);


/** Get the Peirce filter criterion
 * @param num_samples Number of samples
 * @return The Peirce filter criterion
 */
long double peirce_filter_get_criterion(unsigned int num_samples);


/** Create a smallest filter
 * @param max_samples Maximum number of samples to hold in the filter
 * @param ageing_coefficient De-values older samples. Expressed in
 * nanoseconds per second.
 * @param timeout Maximum age samples can be before they are dis-qualified
 * in seconds
 * @return A pointer to the filter structure or NULL on error
 */
struct sfptpd_smallest_filter *sfptpd_smallest_filter_create(unsigned int max_samples,
							     long double ageing_coefficient,
							     time_t timeout);

/** Destroy a smallest filter instance
 * @param filter Pointer to filter instance
 */
void sfptpd_smallest_filter_destroy(struct sfptpd_smallest_filter *filter);

/** Set the timeout. Samples older than this will be disqualified.
 * @param filter Pointer ot the filter instance
 * @param timeout Maximum age samples can be before they are dis-qualified
 * in seconds
 */
void sfptpd_smallest_filter_set_timeout(struct sfptpd_smallest_filter *filter,
					time_t timeout);

/** Reset a smallest filter, removing all samples from it
 * @param filter Smallest filter instance
 */
void sfptpd_smallest_filter_reset(struct sfptpd_smallest_filter *filter);

/** Update the smallest filter with a new sample
 * @param filter Pointer to filter instance
 * @param sample New data sample
 * @return smallest data sample present in filter
 */
sfptpd_ptp_tsd_t *sfptpd_smallest_filter_update(struct sfptpd_smallest_filter *filter,
						sfptpd_ptp_tsd_t * sample);


#endif /* _SFPTPD_FILTER_H */
