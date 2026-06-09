/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2026 Advanced Micro Devices, Inc. */

#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "sfptpd_quantities.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

/* Accuracy bounds used for bucketing of sync sources. These should
 * be a superset of IEEE1588 values for best operation. */
const sfptpd_accuracy_t sfptpd_accuracy_buckets[] = {
        1.0e-1, 2.5e-1,
        1.0e0, 2.5e0,
        1.0e1, 2.5e1 /* PTP min bucket, 25ns */,
        1.0e2, 2.5e2,
        1.0e3, 2.5e3,
        1.0e4, 2.5e4,
        1.0e5, 2.5e5,
        1.0e6, 2.5e6,
        1.0e7, 2.5e7,
        1.0e8, 2.5e8,
        1.0e9, 2.5e9 /* not present in PTP wire defs */,
        1.0e10 /* PTP max finite bucket, 10s */,
        INFINITY
};


/****************************************************************************
 * Functions
 ****************************************************************************/

sfptpd_accuracy_t sfptpd_accuracy_bucket_ceil(sfptpd_accuracy_t accuracy)
{
	const sfptpd_accuracy_t *bucket;

	if (isnan(accuracy))
		return INFINITY;
	assert(accuracy >= 0.0);
	for (bucket = sfptpd_accuracy_buckets; accuracy > *bucket; bucket++);
	return *bucket;
}

sfptpd_accuracy_t sfptpd_accuracy_bucket_midpoint(sfptpd_accuracy_t accuracy)
{
	const sfptpd_accuracy_t *bucket;
	sfptpd_accuracy_t prev = 0.0;

	if (isnan(accuracy))
		return INFINITY;
	assert(accuracy >= 0.0);
	for (bucket = sfptpd_accuracy_buckets; accuracy > *bucket; bucket++)
		prev = *bucket;

	/* The buckets have a geometric rather than linear nature. */
	return sqrtf(prev * *bucket);
}
