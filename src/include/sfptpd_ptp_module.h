/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_PTP_MODULE_H
#define _SFPTPD_PTP_MODULE_H

#include "ptpd_lib.h"
#include "sfptpd_config.h"
#include "sfptpd_sync_module.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Identifier for the PTP sync module */
#define SFPTPD_PTP_MODULE_NAME "ptp"

/** PTP Sync module instance configuration */
typedef struct sfptpd_ptp_module_config {
	/** Common configuration header */
	sfptpd_config_section_t hdr;

	/** PTPD port-level configuration options */
	struct ptpd_port_config ptpd_port;

	/** PTPD interface-level configuration options */
	struct ptpd_intf_config ptpd_intf;

	/* Textual name of interface that PTP should use */
	char interface_name[IF_NAMESIZE];

	/* Priority of this instance */
	unsigned int priority;

	/* Convergence threshold */
	long double convergence_threshold;

	/** UUID filtering enabled */
	bool uuid_filtering;

	/** PTP Domain number enabled */
	bool domain_filtering;

	/** PPS statistics logging enabled */
	bool pps_logging;

	/** Remote monitor enabled */
	bool remote_monitor;

	/** PTP trace level */
	unsigned int trace_level;

	/** PTP profile */
	enum sfptpd_ptp_profile profile;
} sfptpd_ptp_module_config_t;


/* Forward structure declarations */
struct sfptpd_engine;
struct sfptpd_thread;
struct sfptpd_ptp_monitor;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the PTP module configuration options. This will
 * create a global instance of the PTP configuration and set the values to
 * defaults. The function also registers config options applicable to this
 * sync module type.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_ptp_module_config_init(struct sfptpd_config *config);

/** Get the PTP configuration. This will return the global configuration from
 * which PTP instances can be accessed.
 * @param config  Pointer to the configuration
 * @return A pointer to the PTP global configuration
 */
struct sfptpd_ptp_module_config *sfptpd_ptp_module_get_config(struct sfptpd_config *config);

/** Set the default interface to be used by the sync module. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_ptp_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name);

/** Get the UTC offset. This will return 0 if PTP is configured to use UTC.
 * Otherwise it returns the current difference between UTC and TAI such that
 * that UTC + Offset = TAI. Note that this function is provided to support leap
 * second testing.
 * @param config  Pointer to the configuration
 * @return Current difference between UTC and PTP timescale
 */
int sfptpd_ptp_module_get_utc_offset(struct sfptpd_config *config);

/** Create a PTP sync module instance based on the configuration supplied
 * @param config  Pointer to configuration
 * @param engine  Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync instance
 * @param instance_info_entries Number of entries in instance_info_buffer
 * @param link_table Pointer to initial link table
 * @param link_table_subscriber To be set to true if the sync module wishes to
 * subscribe to link table changes
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_ptp_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_table_subscriber);

/** Create a remote stats monitor.
 * @return the monitor object, or NULL on failure.
 */
struct sfptpd_ptp_monitor *sfptpd_ptp_monitor_create(void);

/** Destroy a remote stats monitor.
 * @return the monitor object, or NULL on failure.
 */
void sfptpd_ptp_monitor_destroy(struct sfptpd_ptp_monitor *monitor);

/** Post rx event timing stats received from a remote host to the monitor
 * @param logger a callback object used by ptpd that contains a reference to
 * the monitor object.
 * @param stats metadata for the stats.
 * @param num_timing_data the number of elements
 * @param timing_data the timing data
 */
void sfptpd_ptp_monitor_update_rx_timing(struct ptpd_remote_stats_logger *logger,
					 struct ptpd_remote_stats stats,
					 int num_timing_data,
					 SlaveRxSyncTimingDataElement *timing_data);

/** Post rx event computed stats received from a remote host to the monitor
 * @param logger a callback object used by ptpd that contains a reference to
 * the monitor object.
 * @param stats metadata for the stats.
 * @param num_computed_data the number of elements
 * @param computed_data the computed data
 */
void sfptpd_ptp_monitor_update_rx_computed(struct ptpd_remote_stats_logger *logger,
					   struct ptpd_remote_stats stats,
					   int num_computed_data,
					   SlaveRxSyncComputedDataElement *computed_data);

/** Post tx event timestamps received from a remote host to the monitor
 * @param logger a callback object used by ptpd that contains a reference to
 * the monitor object.
 * @param stats metadata for the stats.
 * @param message_type the time of message to which the timestamps relate
 * @param num_computed_data the number of timestamps
 * @param computed_data the timestamps
 */
void sfptpd_ptp_monitor_log_tx_timestamp(struct ptpd_remote_stats_logger *logger,
					 struct ptpd_remote_stats stats,
					 ptpd_msg_id_e message_type,
					 int num_timestamps,
					 SlaveTxEventTimestampsElement *timestamps);

/** Post slave status reports received from a remote host to the monitor
 * @param logger a callback object used by ptpd that contains a reference to
 * the monitor object.
 * @param stats metadata for the stats.
 * @param slave_status the status report.
 */
void sfptpd_ptp_monitor_update_slave_status(struct ptpd_remote_stats_logger *logger,
					    struct ptpd_remote_stats stats,
					    SlaveStatus *slave_status);

/** Flush the latest stats, e.g. to file, as appropriate.
 * @param monitor the monitor object.
 */
void sfptpd_ptp_monitor_flush(struct sfptpd_ptp_monitor *monitor);

const struct sfptpd_ptp_profile_def *sfptpd_ptp_get_profile_def(enum sfptpd_ptp_profile profile_index);

#endif /* _SFPTPD_PTP_MODULE_H */
