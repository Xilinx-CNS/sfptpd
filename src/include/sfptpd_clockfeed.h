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

/* Opaque declaration of clock feed internal state */
struct sfptpd_clockfeed;

/* Opaque declaration of clock feed subscription object */
struct sfptpd_clockfeed_sub;

/* A sample from a clock feed. This captures NIC and system clock timestamps,
 * an error code relating to the sample (or zero) and a sequence number.
 * This structure is expected to be used via the helper functions. */
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

/** Dump clock feed state through logging service.
 * @param clockfeed Handle to the clock feed state.
 */
void sfptpd_clockfeed_dump_state(struct sfptpd_clockfeed *clockfeed);

/** Add a clock to the clock feed. Called by the sfptpd engine on a hotplug
 * event.
 * @param clockfeed Handle to the clock feed state.
 * @param clock The clock to be added.
 * @param poll_period_log2 Log to the base 2 in seconds of the period between
 *        which the clock should be polled.
 */
void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2);

/* Remove clock from clock feed.
 * @param clockfeed Handle to the clock feed state.
 * @param clock The clock to be removed.
 */
void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock);

/* Subscribe to the feed for a given clock.
 * @param clockfeed Handle to the clock feed state.
 * @param clock The clock to which to subscribe.
 * @param shm Pointer to which to write the subscription handle.
 * @return 0 on success, else errno.
 */
int sfptpd_clockfeed_subscribe(struct sfptpd_clockfeed *clockfeed,
			       struct sfptpd_clock *clock,
			       struct sfptpd_clockfeed_sub **shm);

/* Unsubscribe from the feed for a given clock.
 * @param clockfeed Handle to the clock feed state.
 * @param clock The clock to which to subscribe.
 */
void sfptpd_clockfeed_unsubscribe(struct sfptpd_clockfeed *clockfeed,
				  struct sfptpd_clockfeed_sub *clock);

/* Compare two clocks using clock feed samples.
 * @param feed1 The first clock feed.
 * @param feed2 The second clock feed.
 * @param diff Where to store the clock difference.
 * @param t1 Where to store the timestamp of the first clock, or NULL.
 * @param t2 Where to store the timestamp of the second clock, or NULL.
 * @param mono Where to store the monotonic clock snapshot, or NULL.
 * @return 0 on success, else errno.
 */
int sfptpd_clockfeed_compare(struct sfptpd_clockfeed_sub *feed1,
			     struct sfptpd_clockfeed_sub *feed2,
			     struct sfptpd_timespec *diff,
			     struct sfptpd_timespec *t1,
			     struct sfptpd_timespec *t2,
			     struct sfptpd_timespec *mono);

/* Require that the next sample in a clock feed subscription should not have
 * previously been consumed by this subscriber.
 */
void sfptpd_clockfeed_require_fresh(struct sfptpd_clockfeed_sub *sub);

/* Set the maximum age for a clock feed sample.
 * @param sub The clock feed subscription.
 * @param max_age The maximum age.
 */
void sfptpd_clockfeed_set_max_age(struct sfptpd_clockfeed_sub *sub,
				  const struct sfptpd_timespec *max_age);

/* Set the maximum age difference between the two samples which make up
 * a clock difference.
 * @param sub The clock feed subscription.
 * @param max_age_diff The maximum age difference.
 */
void sfptpd_clockfeed_set_max_age_diff(struct sfptpd_clockfeed_sub *sub,
				       const struct sfptpd_timespec *max_age_diff);

/* Handle the end of a stats collection period.
 * @param module Opaque handle to the clock feed state.
 * @paran time The time corresponding to this period.
 */
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


#endif /* _SFPTPD_CLOCKFEED_MODULE_H */
