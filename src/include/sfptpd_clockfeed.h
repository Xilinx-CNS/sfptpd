/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CLOCKFEED_MODULE_H
#define _SFPTPD_CLOCKFEED_MODULE_H

#include "sfptpd_config.h"


/****************************************************************************
 * Constants
 ****************************************************************************/



/****************************************************************************
 * Structures and Types
 ****************************************************************************/

struct sfptpd_clockfeed;

struct sfptpd_clockfeed_sub;

struct sfptpd_clockfeed_sample {
	uint64_t seq;
	struct sfptpd_timespec mono;
	struct sfptpd_timespec system;
	struct sfptpd_timespec snapshot;
	int rc;
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create a clock feed service.
 * @param threadret Returned pointer to created thread
 * @return clock feed module on success else NULL.
 */
struct sfptpd_clockfeed *sfptpd_clockfeed_create(struct sfptpd_thread **threadret,
						 int min_poll_period_log2);

void sfptpd_clockfeed_dump_state(struct sfptpd_clockfeed *clockfeed);

void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2);

void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock);

int sfptpd_clockfeed_subscribe(struct sfptpd_clock *clock,
			       struct sfptpd_clockfeed_sub **shm);

void sfptpd_clockfeed_unsubscribe(struct sfptpd_clockfeed_sub *clock);

int sfptpd_clockfeed_compare(struct sfptpd_clockfeed_sub *feed1,
			     struct sfptpd_clockfeed_sub *feed2,
			     struct sfptpd_timespec *diff,
			     struct sfptpd_timespec *t1,
			     struct sfptpd_timespec *t2,
			     struct sfptpd_timespec *mono);

void sfptpd_clockfeed_require_fresh(struct sfptpd_clockfeed_sub *sub);

void sfptpd_clockfeed_set_max_age(struct sfptpd_clockfeed_sub *sub,
				  const struct sfptpd_timespec *max_age);

void sfptpd_clockfeed_set_max_age_diff(struct sfptpd_clockfeed_sub *sub,
				       const struct sfptpd_timespec *max_age_diff);

void sfptpd_clockfeed_stats_end_period(struct sfptpd_clockfeed *module,
				       struct sfptpd_timespec *time);

/****************************************************************************
 * Public clock feed messages
 ****************************************************************************/

/* Macro used to define message ID values for clock feed messages */
#define SFPTPD_CLOCKFEED_MSG(x) (SFPTPD_MSG_BASE_CLOCK_FEED + (x))

/* Notification that a cycle of processing all ready clock feeds has
 * been completed. Sent by multicast.
 */
#define SFPTPD_CLOCKFEED_MSG_SYNC_EVENT   SFPTPD_CLOCKFEED_MSG(5)

#endif
