/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2026 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_QUANTITIES_H
#define _SFPTPD_QUANTITIES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>


/****************************************************************************
 * Types and defines
****************************************************************************/

/* Accuracy in nanoseconds */
typedef float sfptpd_accuracy_t;


/****************************************************************************
 * Constant declarations
****************************************************************************/

extern const sfptpd_accuracy_t sfptpd_accuracy_buckets[];


/****************************************************************************
 * Function declarations
****************************************************************************/

sfptpd_accuracy_t sfptpd_accuracy_bucket_ceil(sfptpd_accuracy_t accuracy);
sfptpd_accuracy_t sfptpd_accuracy_bucket_midpoint(sfptpd_accuracy_t accuracy);


/****************************************************************************
 * Inline functions
****************************************************************************/

static inline sfptpd_accuracy_t sfptpd_total_accuracy(sfptpd_accuracy_t master,
						      sfptpd_accuracy_t local)
{
	return sfptpd_accuracy_bucket_ceil(sfptpd_accuracy_bucket_midpoint(master) + local);
}


static inline bool sfptpd_accuracy_equiv(sfptpd_accuracy_t a,
					 sfptpd_accuracy_t b)
{
	return sfptpd_accuracy_bucket_ceil(a) == sfptpd_accuracy_bucket_ceil(b);
}


#endif
