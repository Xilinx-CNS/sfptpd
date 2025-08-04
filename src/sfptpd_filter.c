/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_filter.c
 * @brief  Various filters used by clock servos
 */

#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#include "sfptpd_statistics.h"
#include "sfptpd_logging.h"
#include "sfptpd_time.h"
#include "sfptpd_filter.h"
#include "sfptpd_ptp_timestamp_dataset.h"


/****************************************************************************
 * Types, Structures & Defines
 ****************************************************************************/


/* Filter that will select the sample with the smallest path delay subject to
 * ageing. */
struct sfptpd_smallest_filter {
	/* Maximum samples in data set */
	unsigned int max_samples;

	/* Ageing coefficient */
	long double ageing;

	/* Number of samples currently in filter */
	unsigned int num_samples;

	/* Index where next data will be written */
	unsigned int write_idx;

	/* Maximum age samples can be before they are disqualified */
	struct sfptpd_timespec timeout;

	/* Array of data samples. */
	sfptpd_ptp_tsd_t data[0];
};


/****************************************************************************
 * Constants
 ****************************************************************************/

#define PID_INTERVAL_FILTER_STIFFNESS (16)

const long double peirce_criterion_table[SFPTPD_PEIRCE_FILTER_SAMPLES_MAX] =
{
	0.000000, 1.000000, 1.216262, 1.382943, 1.509276, /* 1-5 */
	1.609828, 1.692841, 1.763236, 1.824156, 1.877719, /* 6-10 */
	1.925417, 1.968339, 2.007300, 2.042929, 2.075718, /* 11-15 */
	2.106060, 2.134273, 2.160620, 2.185316, 2.208544, /* 16-20 */
	2.230458, 2.251190, 2.270853, 2.289546, 2.307353, /* 21-25 */
	2.324351, 2.340604, 2.356171, 2.371105, 2.385452, /* 26-30 */
	2.399253, 2.412545, 2.425364, 2.437739, 2.449698, /* 31-35 */
	2.461267, 2.472468, 2.483323, 2.493852, 2.504072, /* 36-40 */
	2.514000, 2.523651, 2.533039, 2.542178, 2.551079, /* 41-45 */
	2.559753, 2.568212, 2.576465, 2.584522, 2.592390, /* 46-50 */
	2.600079, 2.607595, 2.614946, 2.622138, 2.629178, /* 51-55 */
	2.636072, 2.642825, 2.649442, 2.655929, 2.662291, /* 56-60 */
};


/****************************************************************************
 * Finite Impulse Response (FIR) Filter
 ****************************************************************************/

void sfptpd_fir_filter_init(sfptpd_fir_filter_t *fir, unsigned int stiffness)
{
	assert(fir != NULL);
	assert(stiffness <= SFPTPD_FIR_FILTER_STIFFNESS_MAX);

	fir->stiffness = stiffness;

	sfptpd_fir_filter_reset(fir);

}


void sfptpd_fir_filter_reset(sfptpd_fir_filter_t *fir)
{
	assert(fir != NULL);

	fir->num_samples = 0;
	fir->write_idx = 0;
}


long double sfptpd_fir_filter_update(sfptpd_fir_filter_t *fir,
				     long double delta)
{
	unsigned int idx;
	long double mean;
	assert(fir != NULL);

	fir->delta[fir->write_idx] = delta;
	fir->write_idx++;
	if (fir->write_idx == fir->stiffness)
		fir->write_idx = 0;
	if (fir->num_samples < fir->stiffness)
		fir->num_samples++;

	/* Calculate the mean value */
	mean = 0.0;
	for (idx = 0; idx < fir->num_samples; idx++)
		mean += fir->delta[idx];
	mean /= fir->num_samples;

	return mean;
}


/****************************************************************************
 * Proportional-Integral-Differential (PID) Filter
 ****************************************************************************/

void sfptpd_pid_filter_init(sfptpd_pid_filter_t *pid, long double k_p,
			    long double k_i, long double k_d,
			    long double interval)
{
	assert(pid != NULL);

	pid->k_p = k_p;
	pid->k_i = k_i;
	pid->k_d = k_d;
	pid->i_max = 0.0;
	pid->configured_interval = interval;

	sfptpd_pid_filter_reset(pid);
}


void sfptpd_pid_filter_set_i_term_max(sfptpd_pid_filter_t *pid,
				      long double i_max)
{
	assert(pid != NULL);
	assert(i_max >= 0.0);
	pid->i_max = i_max;
}


void sfptpd_pid_filter_set_interval(sfptpd_pid_filter_t *pid,
				    long double interval)
{
	assert(pid != NULL);
	assert(interval > 0.0);
	pid->configured_interval = interval;
}


void sfptpd_pid_filter_reset(sfptpd_pid_filter_t *pid)
{
	assert(pid != NULL);

	pid->p = 0.0;
	pid->i = 0.0;
	pid->d = 0.0;
	pid->freq_adjust = 0.0;
	sfptpd_time_zero(&pid->last_update);

	pid->average_interval = pid->configured_interval;
}


void sfptpd_pid_filter_adjust(sfptpd_pid_filter_t *pid, long double k_p,
			      long double k_i, long double k_d,
			      bool reset)
{
	assert(pid != NULL);

	if (!isnan(k_p))
		pid->k_p = k_p;

	if (!isnan(k_i))pid->k_i = k_i;

	if (!isnan(k_d))
		pid->k_d = k_d;

	if (reset)
		sfptpd_pid_filter_reset(pid);
}


long double sfptpd_pid_filter_update(sfptpd_pid_filter_t *pid, long double delta,
				     struct sfptpd_timespec *time)
{
	long double interval;
	struct sfptpd_timespec diff;

	assert(pid != NULL);

	interval = pid->configured_interval;
	if (time != NULL) {
		if (!sfptpd_time_is_zero(&pid->last_update)) {
			sfptpd_time_subtract(&diff, time, &pid->last_update);
			interval = sfptpd_time_timespec_to_float_s(&diff);

			/* Limit the interval to between half and double the
			 * average value */
			if (interval < 0.5 * pid->average_interval)
				interval = 0.5 * pid->average_interval;
			else if (interval > 2 * pid->average_interval)
				interval = 2 * pid->average_interval;

			/* Update the average value. This is a crude weighted
			 * mean where older values are given less weight */
			pid->average_interval =
				((PID_INTERVAL_FILTER_STIFFNESS - 1) * pid->average_interval +
				interval) / PID_INTERVAL_FILTER_STIFFNESS;
		}

		pid->last_update = *time;
	}

	/* Update the proportional term */
	pid->p = pid->k_p * delta;

	/* The differential term is a backwards difference equation */
	/* --- for now we don't calculate a differential term */

	pid->freq_adjust = 0.0 - pid->p - pid->i - pid->d;

	/* Finally calculate the integral term to use next time */
	pid->i += pid->k_i * interval * delta;

	/* Saturate the I-term to the maximum value to ensure that if there is
	 * a prolonged period of large error we do not build up an enormous
	 * integral correction term */
	if (pid->i_max > 0.0) {
		if (pid->i > pid->i_max)
			pid->i = pid->i_max;
		else if (pid->i < -pid->i_max)
			pid->i = -pid->i_max;
	}

	return pid->freq_adjust;
}


long double sfptpd_pid_filter_get_p_term(sfptpd_pid_filter_t *pid)
{
	assert(pid != NULL);
	return pid->p;
}


long double sfptpd_pid_filter_get_i_term(sfptpd_pid_filter_t *pid)
{
	assert(pid != NULL);
	return pid->i;
}


long double sfptpd_pid_filter_get_d_term(sfptpd_pid_filter_t *pid)
{
	assert(pid != NULL);
	return pid->d;
}


/****************************************************************************
 * Notch Filter
 ****************************************************************************/

void sfptpd_notch_filter_init(sfptpd_notch_filter_t *notch,
			      long double mid_point, long double width)
{
	assert(notch != NULL);
	assert(width > 0);

	notch->min = mid_point - width;
	notch->max = mid_point + width;
}


int sfptpd_notch_filter_update(sfptpd_notch_filter_t *notch,
			       long double interval)
{
	assert(notch != NULL);

	if ((interval >= notch->min) && (interval <= notch->max))
		return 0;
	
	return ERANGE;
}


/****************************************************************************
 * Peirce Filter
 ****************************************************************************/

long double peirce_filter_get_criterion(unsigned int num_samples)
{
	unsigned int idx = num_samples - 1;

	assert(idx < SFPTPD_PEIRCE_FILTER_SAMPLES_MAX);
	return peirce_criterion_table[idx];
}


struct sfptpd_peirce_filter *sfptpd_peirce_filter_create(unsigned int max_samples,
							 long double outlier_weighting)
{
	struct sfptpd_peirce_filter *new;
	size_t size;
	char *mem_ptr;

	assert((max_samples > 0) && (max_samples <= SFPTPD_PEIRCE_FILTER_SAMPLES_MAX));
	assert((outlier_weighting >= 0.0) && (outlier_weighting <= 1.0));

	/* Allocate sufficient memory for the structure, data samples,
	 * drift values and their time records */
	size = sizeof(*new) +
	       (max_samples * sizeof(long double)) +           /* data samples */
	       (max_samples * sizeof(struct sfptpd_timespec)) + /* timestamps */
	       (max_samples * sizeof(long double));             /* drift values */

	new = (struct sfptpd_peirce_filter *)calloc(1, size);
	if (new == NULL) {
		CRITICAL("failed to allocate memory for Peirce filter\n");
		return NULL;
	}

	/* Setup pointers to the arrays allocated after the structure */
	mem_ptr = (char *)new + sizeof(*new);
	new->data = (long double *)mem_ptr;
	mem_ptr += max_samples * sizeof(long double);
	new->timestamps = (struct sfptpd_timespec *)mem_ptr;
	mem_ptr += max_samples * sizeof(struct sfptpd_timespec);
	new->drift_values_ns = (long double *)mem_ptr;

	new->max_samples = max_samples;
	new->outlier_weighting = outlier_weighting;
	sfptpd_peirce_filter_reset(new);

	return new;
}


void sfptpd_peirce_filter_destroy(struct sfptpd_peirce_filter *filter)
{
	assert(filter != NULL);
	free(filter);
}


void sfptpd_peirce_filter_reset(struct sfptpd_peirce_filter *filter)
{
	assert(filter != NULL);
	sfptpd_stats_std_dev_init(&filter->std_dev);
	filter->num_samples = 0;
	filter->write_idx = 0;
	filter->cumulative_drift_sum_ns = 0.0;
	filter->update_count = 0;

	memset(filter->data, 0, filter->max_samples * sizeof(long double));
	memset(filter->timestamps, 0, filter->max_samples * sizeof(struct sfptpd_timespec));
	memset(filter->drift_values_ns, 0, filter->max_samples * sizeof(long double));
}


int sfptpd_peirce_filter_update(struct sfptpd_peirce_filter *filter,
			        long double sample,
			        long double freq_adj,
			        struct sfptpd_timespec *timestamp)
{
	long double sd, mean, deviation, criterion;
	long double current_drift_ns = 0.0;
	long double cumulative_drift_ns = 0.0;
	int rc = 0;

	assert(filter != NULL);

	/* Calculate drift for this sample. Drift is a recently applied frequency scaling rate
	 * multiplied by time delta between the current and the last sample.  This will tell us
	 * by how much we shifted our time base when analyzing a new sample. */
	if (filter->num_samples > 0) {
		struct sfptpd_timespec duration;
		long double duration_s;

		unsigned int prev_idx = (filter->write_idx + filter->max_samples - 1) % filter->max_samples;

		sfptpd_time_subtract(&duration, timestamp, &filter->timestamps[prev_idx]);
		duration_s = sfptpd_time_timespec_to_float_s(&duration);

		current_drift_ns = freq_adj * duration_s;
	}

	/* Calculate cumulative_drift - add drift from current sample and remove from the oldest */
	cumulative_drift_ns = filter->cumulative_drift_sum_ns + fabsl(current_drift_ns);
	if (filter->num_samples >= filter->max_samples) {
		cumulative_drift_ns -= fabsl(filter->drift_values_ns[filter->write_idx]);
	}

	/* If we have enough samples, apply the filter... */
	if (filter->num_samples >= SFPTPD_PEIRCE_FILTER_SAMPLES_MIN) {
		sd = sfptpd_stats_std_dev_get(&filter->std_dev, &mean);

		/* Get the criterion based on the current number of samples */
		criterion = peirce_filter_get_criterion(filter->num_samples);

		/* If the absolute deviation of this sample is greater than
		 * the residual multiplied by the standard deviation plus the
		 * cumulative drift, we consider that the sample is an outlier
		 * and return ERANGE.
		 * 
		 * If we find an outlier, we still need to include the sample
		 * in the stats such that the filter adapts if the quality of
		 * the samples improve or degrade over time. */
		deviation = sample - mean;
		if (fabsl(deviation) > criterion * sd + cumulative_drift_ns) {
			rc = ERANGE;
		}

		TRACE_L5("peirce: num samples %d, mean %Lf, sd %Lf, "
			 "sample %Lf, deviation %Lf, cumulative drift %Lf, rc %d\n",
			 filter->num_samples, mean, sd, sample, deviation, cumulative_drift_ns, rc);

		if (rc == ERANGE) {
			/* Apply the weighting factor and modify the sample */
			deviation *= filter->outlier_weighting;
			sample = mean + deviation;
		}
	}

	/* Update the data set with the new sample */
	
	/* If the filter is full, remove the oldest sample from the standard 
	 * deviation measure. The oldest sample is in the position where we will
	 * place the next sample i.e. the write_idx */
	if (filter->num_samples >= filter->max_samples) {
		sfptpd_stats_std_dev_remove_sample(&filter->std_dev,
						   filter->data[filter->write_idx]);
	}

	/* Update the standard deviation measure with the new data sample */
	sfptpd_stats_std_dev_add_sample(&filter->std_dev, sample);

	/* Update filter internals */
	filter->data[filter->write_idx] = sample;
	filter->timestamps[filter->write_idx] = *timestamp;
	filter->drift_values_ns[filter->write_idx] = current_drift_ns;
	filter->cumulative_drift_sum_ns = cumulative_drift_ns;
	filter->update_count++;

	/* Recalculate cumulative drift sum from scratch to clear all numerical errors */
	if (filter->num_samples > 0 && (filter->update_count % (filter->max_samples * SFPTPD_PEIRCE_FILTER_RECALCULATION_PERIOD) == 0)) {
		long double recalculated_sum = 0.0;
		int i;

		for (i = 0; i < filter->num_samples; i++) {
			recalculated_sum += fabsl(filter->drift_values_ns[i]);
		}

		/* Replace the accumulated sum with the recalculated one */
		filter->cumulative_drift_sum_ns = recalculated_sum;

		TRACE_L5("peirce: periodic recalculation at update %u, "
			 "recalculated cumulative drift  %Lf ns\n",
			 filter->update_count, recalculated_sum);
	}

	filter->write_idx++;
	if (filter->write_idx >= filter->max_samples)
		filter->write_idx = 0;
	if (filter->num_samples < filter->max_samples)
		filter->num_samples++;

	/* Return a status to indicate if the data is considered an outlier */
	return rc;
}


/****************************************************************************
 * Smallest Filter
 ****************************************************************************/

struct sfptpd_smallest_filter *sfptpd_smallest_filter_create(unsigned int max_samples,
							     long double ageing_coefficient,
							     time_t timeout)
{
	struct sfptpd_smallest_filter *new;
	size_t size;

	assert(max_samples > 0);
	assert(timeout > 0);

	size = sizeof(*new) + (max_samples * sizeof(sfptpd_ptp_tsd_t));

	new = (struct sfptpd_smallest_filter *)calloc(1, size);
	if (new == NULL) {
		CRITICAL("failed to allocate memory for Smallest filter\n");
		return NULL;
	}

	new->max_samples = max_samples;
	new->ageing = ageing_coefficient;
	sfptpd_time_from_s(&new->timeout, timeout);

	sfptpd_smallest_filter_reset(new);

	return new;
}


void sfptpd_smallest_filter_destroy(struct sfptpd_smallest_filter *filter)
{
	assert(filter != NULL);
	free(filter);
}


void sfptpd_smallest_filter_set_timeout(struct sfptpd_smallest_filter *filter,
					time_t timeout)
{
	assert(filter != NULL);
	sfptpd_time_from_s(&filter->timeout, timeout);
}


void sfptpd_smallest_filter_reset(struct sfptpd_smallest_filter *filter)
{
	assert(filter != NULL);
	filter->write_idx = 0;
	filter->num_samples = 0;
}


sfptpd_ptp_tsd_t *sfptpd_smallest_filter_update(struct sfptpd_smallest_filter *filter,
						sfptpd_ptp_tsd_t *sample)
{
	sfptpd_ptp_tsd_t *best_sample;
	sfptpd_time_t path_delay, best_path_delay, aged_path_delay;
	struct sfptpd_timespec now, age;
	unsigned int i;

	assert(filter != NULL);
	assert(sample != NULL);
	assert(sample->complete);

	now = sfptpd_ptp_tsd_get_monotonic_time(sample);

	/* Insert the new sample into the set. If we are already at the maximum
	 * acceptable samples, this replaces the oldest data. */
	filter->data[filter->write_idx] = *sample;
	filter->write_idx++;
	if (filter->write_idx >= filter->max_samples)
		filter->write_idx = 0;
	if (filter->num_samples < filter->max_samples)
		filter->num_samples++;

	best_sample = NULL;
	for (i = 0; i < filter->num_samples; i++) {
		/* Work out the age of the sample. The times logged for each
		 * sample are from the MONOTONIC clock so should be always
		 * increasing. The implication is that old data should never
		 * be fed into the filter. */
		age = sfptpd_ptp_tsd_get_monotonic_time(&filter->data[i]);
		sfptpd_time_subtract(&age, &now, &age);
		assert(age.sec >= 0);

		path_delay = sfptpd_ptp_tsd_get_path_delay(&filter->data[i]);
		aged_path_delay = path_delay
			        + filter->ageing * sfptpd_time_timespec_to_float_s(&age);

		/* If the sample is recent enough, its path delay is positive
		 * and it's has the smallest path delay then select it. */
		if (sfptpd_time_is_greater_or_equal(&filter->timeout, &age) &&
		    (path_delay >= 0.0) &&
		    ((best_sample == NULL) || (aged_path_delay < best_path_delay))) {
			best_sample = &filter->data[i];
			best_path_delay = aged_path_delay;
		}
	}

        /* If we still don't have a best sample, then the path delays of all
	 * the data are negative. In this case, take the latest sample. */
	if (best_sample == NULL)
		best_sample = sample;

	return best_sample;
}


/* fin */
