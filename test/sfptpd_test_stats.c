/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_stats.c
 * @brief  Stats unit tests
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


/****************************************************************************
 * External declarations
 ****************************************************************************/


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define MAX_SAMPLES (1024)


/****************************************************************************
 * Local Data
 ****************************************************************************/


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static bool floats_nearly_equal(long double a, long double b, long double epsilon)
{
	long double abs_a = fabsl(a);
	long double abs_b = fabsl(b);
	long double diff = fabsl(a - b);

	if (a == b) { 
		/* handles infinities */
		return true;
	} else if ((a == 0) || (b == 0) || (diff < LDBL_MIN)) {
		/* a or b is zero or both are extremely close to zero. The
		 * relative error is less meaningful here */
		return diff < (epsilon * LDBL_MIN);
	}

	/* use relative error */
	return (diff / (abs_a + abs_b)) < epsilon;
}


static int test_std_dev(void)
{
	int rc = 0;
	long double data[MAX_SAMPLES];
	long double total, dev, sum_dev_sqr;
	long double expected_mean, expected_sd, actual_mean, actual_sd;
	unsigned int num_samples, i, s, r;
	struct sfptpd_stats_std_dev stat;

	/* 32 Iterations */
	for (i = 0; i < 32; i++) {
		do
			num_samples = rand() % MAX_SAMPLES;
		while (num_samples == 0);

		//printf("iteration %d: samples = %d\n", i, num_samples);

		total = 0.0;
		for (s = 0; s < num_samples; s++) {
			data[s] = rand() * rand() * sqrt(rand() + 1);
			total += data[s];
		}

		expected_mean = total / num_samples;

		sum_dev_sqr = 0.0;
		for (s = 0; s < num_samples; s++) { 
			dev = data[s] - expected_mean;
			sum_dev_sqr += dev * dev;
		}
		expected_sd = sqrt(sum_dev_sqr / num_samples);

		/*printf("Expected mean = %Lf, sd = %Lf\n", expected_mean, expected_sd);*/

		/* Now use the stats measure and check if result is the same */
		sfptpd_stats_std_dev_init(&stat);
		for (s = 0; s < num_samples; s++)
			sfptpd_stats_std_dev_add_sample(&stat, data[s]);
		/* Test the remove sample feature */
		for (s = 0; s < 8; s++) {
			r = rand() % num_samples;
			sfptpd_stats_std_dev_remove_sample(&stat, data[r]);
			sfptpd_stats_std_dev_add_sample(&stat, data[r]);
		}

		actual_sd = sfptpd_stats_std_dev_get(&stat, &actual_mean);

		/* Note that due to rounding errors, the means are only equal to the
		 * accuracy of a double. */
		if (!floats_nearly_equal(actual_mean, expected_mean, 8 * DBL_EPSILON)) {
			printf("ERROR: mean actual %Lf, expected %Lf\n",
			       actual_mean, expected_mean);
			rc = EIO;
		}

		/* Note that due to rounding errors, the means are only equal to the
		 * accuracy of a normal float! */
		if (!floats_nearly_equal(actual_sd, expected_sd, FLT_EPSILON)) {
			printf("ERROR: sd actual %Lf, expected %Lf\n",
			       actual_sd, expected_sd);
			rc = EIO;
		}
	}

	return rc;
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_stats(void)
{
	int rc;

	rc = test_std_dev();

	return rc;
}


/* fin */
