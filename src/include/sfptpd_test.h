/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_TEST_H
#define _SFPTPD_TEST_H

/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Enum defining test modes. If test mode is enabled, send these to sfptpd
 * by sending the signals SIGRTMIN + test mode
 */
enum sfptpd_test_id {
	/** Test modes for leap second testing. If test mode enabled and PTP
	 * sync module is configured in master mode, signals to slaves to
	 * schedule or cancel a leap second. If scheduled, at midnight, clears
	 * leap second flag and changes the UTC offset. If cancelled, just
	 * clears the flags. */
	SFPTPD_TEST_ID_LEAP_SECOND_61,
	SFPTPD_TEST_ID_LEAP_SECOND_59,
	SFPTPD_TEST_ID_LEAP_SECOND_CANCEL,

	/** Local versions of local leap second testing. Causes a leap second
	 * to occur on the master. */
	SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_61,
	SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_59,
	SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_CANCEL,

	/* Note that the UTC offset test mode is used internally within the
	 * daemon and not really usable via a signal. */
	SFPTPD_TEST_ID_UTC_OFFSET,

	/** Test mode for testing PTP performance with timestamp jitter.
	 * Use this to emulate an inaccurate master or jitter on the network */
	SFPTPD_TEST_ID_TIMESTAMP_JITTER,

	/** Test mode to emulate a PTP transparent clock. Causes PTP to modify
	 * transmit timestamps and add an equal and opposite correction field. */
	SFPTPD_TEST_ID_TRANSPARENT_CLOCK,

	/** Test mode to emulate a PTP boundary clock change. Causes PTP to
	 * modify Grandmaster ID and Steps Removed fields of announce message
	 * to emulate a boundary clock connecting and disconnecting from an
	 * upstream Grandmaster. */
	SFPTPD_TEST_ID_BOUNDARY_CLOCK_CHANGE,

	/** Test mode to emulate a PTP grandmaster clock change. Causes PTP
	 * to modify the Clock attributes of announce message to test the Best
	 * Master Clock algorithm e.g. change of clock class as a result of GPS
	 * signal loss. */
	SFPTPD_TEST_ID_GRANDMASTER_CLOCK_CHANGE,

	/** Test mode to emulate loss of Sync packets in the network. Causes
	 * a PTP master to toggle between sending and not sending Sync
	 * packets. */
	SFPTPD_TEST_ID_NO_SYNC_PKTS,

	/** Test mode to emulate loss of Follow Up packets in the network.
	 * Causes a PTP master to toggle between sending and not sending 
	 * Follow Up messages. */
	SFPTPD_TEST_ID_NO_FOLLOW_UPS,

	/** Test mode to emulate failure to receive Delay Request packets or
	 * loss of Delay Response packets in the network. Causes a PTP master
	 * to toggle between sending and not sending Delay Response messages. */
	SFPTPD_TEST_ID_NO_DELAY_RESPS,

	/** Test mode to emulate a bad PPS signal i.e. occassional bogus PPS
	 * events */
	SFPTPD_TEST_ID_BOGUS_PPS_EVENTS,

	/** Test mode to adjust the frequency of the local reference clock
	 * for the selected sync instance */
	SFPTPD_TEST_ID_ADJUST_FREQUENCY,

	/** Test mode to emulate loss of Announce packets in the network. Causes
	 * a PTP master to toggle between sending and not sending Announce
	 * packets. */
	SFPTPD_TEST_ID_NO_ANNOUNCE_PKTS,

	SFPTPD_TEST_ID_MAX
};

struct sfptpd_test_mode_descriptor {
	const char *name;
	enum sfptpd_test_id id;
};

#define SFPTPD_TESTS_ARRAY {						\
	{ "leap_second_61", SFPTPD_TEST_ID_LEAP_SECOND_61 },	\
	{ "leap_second_59", SFPTPD_TEST_ID_LEAP_SECOND_59 },	\
	{ "leap_second_cancel", SFPTPD_TEST_ID_LEAP_SECOND_CANCEL }, \
	{ "local_leap_second_61", SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_61 },	\
	{ "local_leap_second_59", SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_59 },	\
	{ "local_leap_second_cancel", SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_CANCEL }, \
	{ "utc_offset", SFPTPD_TEST_ID_UTC_OFFSET },		\
	{ "timestamp_jitter", SFPTPD_TEST_ID_TIMESTAMP_JITTER }, \
	{ "transparent_clock", SFPTPD_TEST_ID_TRANSPARENT_CLOCK }, \
	{ "boundary_clock_change", SFPTPD_TEST_ID_BOUNDARY_CLOCK_CHANGE }, \
	{ "grandmaster_clock_change", SFPTPD_TEST_ID_GRANDMASTER_CLOCK_CHANGE }, \
	{ "no_sync_pkts", SFPTPD_TEST_ID_NO_SYNC_PKTS },	\
	{ "no_follow_ups", SFPTPD_TEST_ID_NO_FOLLOW_UPS },	\
	{ "no_delay_resps", SFPTPD_TEST_ID_NO_DELAY_RESPS }, \
	{ "bogus_pps_events", SFPTPD_TEST_ID_BOGUS_PPS_EVENTS }, \
	{ "adjust_frequency", SFPTPD_TEST_ID_ADJUST_FREQUENCY }, \
	{ "no_announce_pkts", SFPTPD_TEST_ID_NO_ANNOUNCE_PKTS },	\
	{ NULL, SFPTPD_TEST_ID_MAX }				\
}

/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Unit test to verify configuration parsing
 * @return 0 for success or an errno on failure.
 */
int sfptpd_test_config(void);

/* Unit tests for other functional elements */
int sfptpd_test_ht(void);
int sfptpd_test_stats(void);
int sfptpd_test_filters(void);
int sfptpd_test_threading(void);
int sfptpd_test_bic(void);
int sfptpd_test_fmds(void);
int sfptpd_test_link(void);


#endif /* _SFPTPD_TEST_H */
