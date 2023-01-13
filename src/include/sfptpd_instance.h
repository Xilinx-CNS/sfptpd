/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/* Private include file between engine and bic */

#ifndef _SFPTPD_INSTANCE_H
#define _SFPTPD_INSTANCE_H

#include "sfptpd_sync_module.h"
#include "sfptpd_time.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/* Realtime stats entry, used for outputting to stdout and/or json file.
 * Fields below the /optional/ header may be omitted by leaving the default
 * values set in rt_stats_msg_init()
 * @time Request time, may be used to group entries
 * @instance Config instance name
 * @source Instance source
 * @clock_master Instance's master clock
 * @clock_slave Instance's slave clock
 * @is_disciplining Is the master disciplining the slave?
 * @is_blocked Is the Master being blocked from disciplining the slave?
 * @is_in_sync Is the clock in sync?
 * @offset Clock offset from master
 * @freq_adj Current frequency adjustment
 * @one_way_delay Current one way delay
 * @parent_id EUI64 identifier of parent clock
 * @gm_id EUI64 identifier of grandmaster
 * @pps_offset Current PPS offset value
 * @bad_period_count Number of bad periods
 * @overflow_count Number of overflows
 * @active_intf Active network interface
 * @bond_name Name of an eventual bond interface
 * @p_term Current value of PID filter's P term
 * @i_term Current value of PID filter's I term
 */
struct sfptpd_sync_instance_rt_stats_entry {
	struct sfptpd_log_time time;
	const char *instance_name;
	const char *source;
	const struct sfptpd_clock *clock_master;
	const struct sfptpd_clock *clock_slave;
	bool is_disciplining;
	bool is_blocked;
	bool is_in_sync;
	sfptpd_sync_module_alarms_t alarms;
	uint32_t stat_present;
	/* Following fields are optional */
	sfptpd_time_t offset;
	sfptpd_time_t freq_adj;
	sfptpd_time_t one_way_delay;
	uint8_t parent_id[8];
	uint8_t gm_id[8];
	sfptpd_time_t pps_offset;
	int bad_period_count;
	int overflow_count;
	struct sfptpd_interface *active_intf;
	char *bond_name;
	long double p_term;
	long double i_term;
};


/* Input to clustering determination.
 */
struct sfptpd_clustering_input {
	const char *instance_name;
	struct sfptpd_clock *clock;
	sfptpd_time_t offset_from_master;
	bool offset_valid;
};


/* The engine's record of a sync instance */
struct sync_instance_record {

	/* info */
	struct sfptpd_sync_instance_info info;

	/* Last status (updated on state changes) */
	struct sfptpd_sync_instance_status status;

	/* Manual selection */
	bool selected;

	/* Last received realtime stats (may be empty) */
	struct sfptpd_sync_instance_rt_stats_entry latest_rt_stats;

	/* Last received clustering determination input
	   (may be empty) */
	struct sfptpd_clustering_input latest_clustering_input;

	/* Rank - for diagnostic uses only */
	int rank;
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

#endif /* _SFPTPD_INSTANCE_H */
