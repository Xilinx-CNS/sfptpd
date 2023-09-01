/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CLOCKFEED_MODULE_H
#define _SFPTPD_CLOCKFEED_MODULE_H

#include "sfptpd_config.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

#define SFPTPD_MAX_CLOCK_SAMPLES_LOG2 (4)
#define SFPTPD_MAX_CLOCK_SAMPLES      (1 << SFPTPD_MAX_CLOCK_SAMPLES_LOG2)


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

struct sfptpd_clockfeed;

struct sfptpd_clockfeed_sample {
	uint64_t seq;
	struct sfptpd_timespec mono;
	struct sfptpd_timespec system;
	struct sfptpd_timespec snapshot;
	int rc;
};

struct sfptpd_clockfeed_shm {
	struct sfptpd_clockfeed_sample samples[SFPTPD_MAX_CLOCK_SAMPLES];
	uint64_t write_counter;
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create a clock feed service.
 * @param threadret Returned pointer to created thread
 * @return clock feed module on success else NULL.
 */
struct sfptpd_clockfeed *sfptpd_clockfeed_create(struct sfptpd_thread **threadret);

void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2);

void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock);

int sfptpd_clockfeed_subscribe(struct sfptpd_clock *clock,
			       const struct sfptpd_clockfeed_shm **shm);

void sfptpd_clockfeed_unsubscribe(struct sfptpd_clock *clock);

int sfptpd_clockfeed_compare(const struct sfptpd_clockfeed_shm *feed1,
			     const struct sfptpd_clockfeed_shm *feed2,
			     struct sfptpd_timespec *diff,
			     struct sfptpd_timespec *t1,
			     struct sfptpd_timespec *t2);

#endif
