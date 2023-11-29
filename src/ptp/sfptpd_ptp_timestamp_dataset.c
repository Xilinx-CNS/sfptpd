/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_ptp_timestamp_dataset.c
 * @brief  PTP timestamp dataset class
 */

#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#include "sfptpd_time.h"
#include "sfptpd_clock.h"
#include "sfptpd_ptp_timestamp_dataset.h"


/****************************************************************************
 * Types, Structures & Defines
 ****************************************************************************/


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static bool sfptpd_ptp_tsd_update(sfptpd_ptp_tsd_t *tsd)
{
	sfptpd_time_t path_delay, offset;

	assert(tsd != NULL);

	if (tsd->ts.m2s.valid && tsd->ts.s2m.valid) {
		struct sfptpd_timespec s2m, m2s, s2m2s;

		/* We have everything necessary to calculate the path-delay */
		sfptpd_time_subtract(&s2m, &tsd->ts.s2m.rx, &tsd->ts.s2m.tx);
		sfptpd_time_subtract(&m2s, &tsd->ts.m2s.rx, &tsd->ts.m2s.tx);
		sfptpd_time_add(&s2m2s, &s2m, &m2s);

		/* Convert the round trip time into a float before applying the
		 * corrections and halving the result- PTP makes the assumption
		 * that the path delay is half the round trip delay. */
		path_delay = sfptpd_time_timespec_to_float_ns(&s2m2s);
		path_delay -= sfptpd_time_timespec_to_float_ns(&tsd->ts.s2m.correction);
		path_delay -= sfptpd_time_timespec_to_float_ns(&tsd->ts.m2s.correction);
		path_delay /= 2.0;

		/* Store the result */
		tsd->path_delay = path_delay;
	}
	else if (tsd->ts.m2s.valid && tsd->ts.s2p.valid && tsd->ts.p2s.valid) {
		struct sfptpd_timespec s2p, p2s, s2p2s;

		/* We have everything necessary to calculate the path-delay */
		sfptpd_time_subtract(&s2p, &tsd->ts.s2p.rx, &tsd->ts.s2p.tx);
		sfptpd_time_subtract(&p2s, &tsd->ts.p2s.rx, &tsd->ts.p2s.tx);
		sfptpd_time_add(&s2p2s, &s2p, &p2s);

		/* Convert the round trip time into a float before applying the
		 * correction and halving the result- PTP makes the assumption
		 * that the path delay is half the round trip delay. */
		path_delay = sfptpd_time_timespec_to_float_ns(&s2p2s);
		path_delay -= sfptpd_time_timespec_to_float_ns(&tsd->ts.p2s.correction);
		path_delay /= 2.0;

		/* Store the result */
		tsd->path_delay = path_delay;
	} else {
		/* We don't have all the data yet. */
		tsd->complete = false;
		return false;
	}

	struct sfptpd_timespec m2s;
	sfptpd_time_subtract(&m2s, &tsd->ts.m2s.rx, &tsd->ts.m2s.tx);

	/* Convert the offset the into a float before applying the
	 * correction. */
	offset = sfptpd_time_timespec_to_float_ns(&m2s);
	offset -= sfptpd_time_timespec_to_float_ns(&tsd->ts.m2s.correction);
	offset -= path_delay;

	tsd->offset_from_master = offset;

	/* We have a complete data set with the path delay and offset
	 * calculated - return true */
	tsd->complete = true;
	return true;
}


/****************************************************************************
 * Public Interface
 ****************************************************************************/

void sfptpd_ptp_tsd_init(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd != NULL);

	memset(tsd, 0, sizeof(*tsd));

	tsd->complete = false;
	sfptpd_time_zero(&tsd->time_monotonic);
	tsd->path_delay = 0.0;
	tsd->offset_from_master = 0.0;
}

void sfptpd_ptp_tsd_clear_m2s(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd != NULL);
	tsd->ts.m2s.valid = false;
	tsd->complete = false;
}

void sfptpd_ptp_tsd_clear_s2m(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd != NULL);
	tsd->ts.s2m.valid = false;
	tsd->complete = false;
}

void sfptpd_ptp_tsd_clear_p2p(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd != NULL);
	tsd->ts.s2p.valid = false;
	tsd->ts.p2s.valid = false;
	tsd->complete = false;
}

bool sfptpd_ptp_tsd_set_m2s(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *tx_timestamp,
			    struct sfptpd_timespec *rx_timestamp,
			    struct sfptpd_timespec *correction)
{
	assert(tsd != NULL);
	assert(tx_timestamp != NULL);
	assert(rx_timestamp != NULL);

	/* Record the time that the dataset was updated */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &tsd->time_monotonic);
	tsd->time_protocol = *rx_timestamp;

	tsd->ts.m2s.valid = true;
	tsd->ts.m2s.tx = *tx_timestamp;
	tsd->ts.m2s.rx = *rx_timestamp;
	tsd->ts.m2s.correction = *correction;

	return sfptpd_ptp_tsd_update(tsd);
}

bool sfptpd_ptp_tsd_set_s2m(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *tx_timestamp,
			    struct sfptpd_timespec *rx_timestamp,
			    struct sfptpd_timespec *correction)
{
	assert(tsd != NULL);
	assert(tx_timestamp != NULL);
	assert(rx_timestamp != NULL);

	/* If we are using end-to-end delay mode then the peer delay timestamps
	 * must not be used. */
	tsd->ts.s2p.valid = false;
	tsd->ts.p2s.valid = false;

	/* Record the time that the dataset was updated */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &tsd->time_monotonic);
	tsd->time_protocol = *rx_timestamp;

	tsd->ts.s2m.valid = true;
	tsd->ts.s2m.tx = *tx_timestamp;
	tsd->ts.s2m.rx = *rx_timestamp;
	tsd->ts.s2m.correction = *correction;

	return sfptpd_ptp_tsd_update(tsd);
}

bool sfptpd_ptp_tsd_set_p2p(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *s2p_tx_timestamp,
			    struct sfptpd_timespec *s2p_rx_timestamp,
			    struct sfptpd_timespec *p2s_tx_timestamp,
			    struct sfptpd_timespec *p2s_rx_timestamp,
			    struct sfptpd_timespec *correction)
{
	assert(tsd != NULL);
	assert(s2p_tx_timestamp != NULL);
	assert(s2p_rx_timestamp != NULL);
	assert(p2s_tx_timestamp != NULL);
	assert(p2s_rx_timestamp != NULL);

	/* If we are using peer delay mode then the slave to master timestamps
	 * must not be used. */
	tsd->ts.s2m.valid = false;

	/* Record the time that the dataset was updated */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &tsd->time_monotonic);
	tsd->time_protocol = *p2s_rx_timestamp;

	tsd->ts.s2p.valid = true;
	tsd->ts.s2p.tx = *s2p_tx_timestamp;
	tsd->ts.s2p.rx = *s2p_rx_timestamp;
	sfptpd_time_zero(&tsd->ts.s2p.correction);
	tsd->ts.p2s.valid = true;
	tsd->ts.p2s.tx = *p2s_tx_timestamp;
	tsd->ts.p2s.rx = *p2s_rx_timestamp; 
	tsd->ts.p2s.correction = *correction;

	return sfptpd_ptp_tsd_update(tsd);
}

sfptpd_time_t sfptpd_ptp_tsd_get_offset_from_master(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd);
	assert(tsd->complete);
	return tsd->offset_from_master;
}

sfptpd_time_t sfptpd_ptp_tsd_get_path_delay(sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd);
	assert(tsd->complete);
	return tsd->path_delay;
}

struct sfptpd_timespec sfptpd_ptp_tsd_get_monotonic_time(sfptpd_ptp_tsd_t *tsd)
{
       assert(tsd);
       assert(tsd->complete);
       return tsd->time_monotonic;
}

struct sfptpd_timespec sfptpd_ptp_tsd_get_protocol_time(sfptpd_ptp_tsd_t *tsd)
{
       assert(tsd);
       return tsd->time_protocol;
}


/* fin */
