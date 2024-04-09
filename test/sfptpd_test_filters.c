/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_filters.c
 * @brief  Filter unit tests
 */

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <float.h>

#include "sfptpd_config.h"
#include "sfptpd_misc.h"
#include "sfptpd_test.h"
#include "sfptpd_statistics.h"
#include "sfptpd_filter.h"
#include "sfptpd_ptp_timestamp_dataset.h"


/****************************************************************************
 * External declarations
 ****************************************************************************/

extern long double peirce_criterion_table[];


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define MAX_PEIRCE_SAMPLES (SFPTPD_PEIRCE_FILTER_SAMPLES_MAX)
#define MAX_SMALLEST_SAMPLES (12)
#define MIN_SMALLEST_SAMPLES (2)

#define NUM_PEIRCE_ITERATIONS (32)
#define NUM_SMALLEST_ITERATIONS (65)

/**
 * Minimum number of samples required to avoid bias
 * in calculation of normals.
 */
#define	NUM_TEST_SAMPLES	(5000)

#define SMALLEST_FILTER_TIMEOUT (12)


/****************************************************************************
 * Local Data
 ****************************************************************************/


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static long double appox_normal(void)
{
	long double s;
	unsigned int i;

	s = 0.0;
	for (i = 0; i < 32; i++) {
		s += (long double)rand();
		s += (long double)rand() / RAND_MAX;
	}

	return s;
}

/**
 * Box-Muller generation of Gaussian random numbers from a
 * series of uniform random numbers.
 */
static double normal_random(double mean, double stddev)
{
	double y1;
    static double y2 = 0.0;
    static bool generate_y2 = false;

    generate_y2 = !generate_y2;
    if (generate_y2)
    {
        double x1, x2, w;

        do
        {
            x1 = 2.0 * (rand() / (double) RAND_MAX) - 1.0;
            x2 = 2.0 * (rand() / (double) RAND_MAX) - 1.0;

            w = x1 * x1 + x2 * x2;
        } while ((w == 0.0) || (w >= 1.0));

		w = sqrt(-2.0 * log(w) / w);
		y1 = x1 * w;
		y2 = x2 * w;
    }
    else
    {
        y1 = y2;
    }

	return mean + y1 * stddev;

}

static sfptpd_ptp_tsd_t rand_path_delay(struct sfptpd_timespec time)
{
	sfptpd_ptp_tsd_t s;

	s.path_delay = normal_random (1.0e9, 1.0e8);
	s.time_monotonic = time;
	s.complete = true;

	return s;
}

/**
 * Test the smallest filter operation.  For a normally distributed population
 * of path delays we'd expect the average "length" of a run to be about half
 * the length of the filter.
 */
static int test_smallest_filter(void)
{
	int rc = 0;
	sfptpd_ptp_tsd_t data;
	sfptpd_ptp_tsd_t *min_samp, *prev_min_samp;
	unsigned int num_samples, i, j, total, ttl;
	float sum_ttl, num_ttls, exp_ttl;
	struct sfptpd_smallest_filter *filter;
	struct sfptpd_timespec time = sfptpd_time_null();

	total = 0;

	for (i = 0; i < NUM_SMALLEST_ITERATIONS; i++) {
		num_ttls = 0;
		ttl = 0, sum_ttl = 0;
		time.sec = 0;
		float limit;

		do
			num_samples = rand() % MAX_SMALLEST_SAMPLES;
		while (num_samples < MIN_SMALLEST_SAMPLES);

		filter = sfptpd_smallest_filter_create(num_samples, 0.0, SMALLEST_FILTER_TIMEOUT);
		if (filter == NULL) {
			printf("ERROR: failed to alloc smallest filter\n");
			return ENOMEM;
		}

		sfptpd_smallest_filter_reset(filter);

		prev_min_samp = NULL; /* Appease -Werror=maybe-unitialized */

		// Put NUM_TEST_SAMPLES samples into filter
		for (j = 0; j < NUM_TEST_SAMPLES; j++) {
			data = rand_path_delay(time);
			assert(data.complete);

			min_samp = sfptpd_smallest_filter_update(filter, &data);

			if ((0 == j) ||
			    (sfptpd_ptp_tsd_get_path_delay(min_samp) == sfptpd_ptp_tsd_get_path_delay(prev_min_samp))) {
				ttl++;
			} else {
				num_ttls++;
				sum_ttl += ttl;
				ttl = 1;
			}

			prev_min_samp = min_samp;
			time.sec++;
		}
		if (ttl != 1)
			num_ttls++;

		total += NUM_TEST_SAMPLES;

		/* Free the filter */
		sfptpd_smallest_filter_destroy(filter);

		exp_ttl = (num_samples + 1.0) / 2.0;
		sum_ttl /= num_ttls;

		/* Small filter lengths lead to more dispersed results */
		if (num_samples <= 3)
			limit = 2.0;
		else
			limit = 1.0;
		if ((sum_ttl < (exp_ttl - limit)) || (sum_ttl > (exp_ttl + limit))) {
			printf("ERROR: expected average ttl to be within +/-%f of %f but got %f\n",
				    limit, exp_ttl, sum_ttl);
			rc = 1;
		}
	}

	printf("overall: total samples processed %d\n", total);

	return rc;
}


static int test_outlier_filter(void)
{
	int rc = 0;
	long double data[MAX_PEIRCE_SAMPLES];
	unsigned int num_samples, i, j, s, total, outliers;
	struct sfptpd_stats_std_dev stat;
	struct sfptpd_peirce_filter *filter;
	int r;

	outliers = 0;
	total = 0;

	/* NUM_PEIRCE_ITERATIONS Iterations */
	for (i = 0; i < NUM_PEIRCE_ITERATIONS; i++) {
		do
			num_samples = rand() % MAX_PEIRCE_SAMPLES;
		while (num_samples < 10);

		//printf("iteration %d: samples = %d\n", i, num_samples);

		filter = sfptpd_peirce_filter_create(num_samples, 1.0);
		if (filter == NULL) {
			printf("ERROR: failed to alloc filter\n");
			return ENOMEM;
		}

		sfptpd_peirce_filter_reset(filter);

		/* Create initial data set */
		sfptpd_stats_std_dev_init(&stat);
		for (s = 0; s < num_samples; s++) {
			data[s] = appox_normal();

			r = sfptpd_peirce_filter_update(filter, data[s]);
			if (r != 0)
				outliers++;

			sfptpd_stats_std_dev_add_sample(&stat, data[s]);
		}

		for (j = 0; j < NUM_PEIRCE_ITERATIONS; j++) {
			for (s = 0; s < num_samples; s++) {
				sfptpd_stats_std_dev_remove_sample(&stat, data[s]);
				data[s] = appox_normal();
				sfptpd_stats_std_dev_add_sample(&stat, data[s]);

				r = sfptpd_peirce_filter_update(filter, data[s]);
				if (r != 0)
					outliers ++;
			}
		}

		total += num_samples * (NUM_PEIRCE_ITERATIONS + 1);

		/* Free the filter */
		sfptpd_peirce_filter_destroy(filter);
	}

	/* For a sample of n, we expect to get on average one outlier for each
	 * n samples added which is equivalent to the number of iterations */
	printf("overall: total samples processed %d, outliers %d\n",
	       total, outliers);
	if (total / outliers > 2.0 * (NUM_PEIRCE_ITERATIONS + 1)) {
		printf("ERROR: expected around %d outliers per sample processed\n",
		       NUM_PEIRCE_ITERATIONS + 1);
		rc = 1;
	}

	return rc;
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_filters(void)
{
	int errors;

	errors = test_smallest_filter();
	errors += test_outlier_filter();

	return errors == 0 ? 0 : ERANGE;
}


/* fin */
