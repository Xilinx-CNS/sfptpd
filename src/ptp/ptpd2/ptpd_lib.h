/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2020 Xilinx, Inc. */

/**
 * @file   ptpd_lib.h
 *
 * This header is the public API to the library form of PTPD.
 */

#ifndef _PTPD_LIB_H
#define _PTPD_LIB_H

#include <stdbool.h>
#include <time.h>
#include "ptpd.h"
#include "datatypes.h"
#include "constants.h"

#include "sfptpd_sync_module.h"


/* Initialise an interface config structure with the default PTP configuration */
void ptpd_config_intf_initialise(struct ptpd_intf_config *);

/* Initialise an instance config structure with the default PTP configuration */
void ptpd_config_port_initialise(struct ptpd_port_config *,
				 const char *name);

/* Forward declarations of structures */
struct ptpd_port_context;
struct ptpd_intf_context;
struct ptpd_statistics;


/* PTPD public snapshot */
struct ptpd_port_snapshot {
	struct {
		ptpd_state_e state;
		sfptpd_sync_module_alarms_t alarms;
		ptpd_delay_mechanism_e delay_mechanism;
		LongDouble announce_interval;
		UInteger8 domain_number;
		Boolean slave_only;
	} port;

	struct {
		UInteger32 steps_removed;
		sfptpd_time_t offset_from_master;
		sfptpd_time_t one_way_delay;
		struct timespec last_offset_time;
		LongDouble frequency_adjustment;
		LongDouble servo_p_term;
		LongDouble servo_i_term;
		LongDouble servo_outlier_threshold;
		Boolean transparent_clock;
		Boolean two_step;
	} current;

	struct {
		UInteger8 clock_id[8];
		UInteger32 port_num;
		UInteger8 grandmaster_id[8];
		UInteger8 grandmaster_clock_class;
		Enumeration8 grandmaster_clock_accuracy;
		UInteger16 grandmaster_offset_scaled_log_variance;
		UInteger8 grandmaster_priority1;
		UInteger8 grandmaster_priority2;
		UInteger8 grandmaster_time_source;
	} parent;

	struct {
		Integer16 current_utc_offset;
		Boolean current_utc_offset_valid;
		Boolean ptp_timescale;
		Boolean leap59;
		Boolean leap61;
		Boolean time_traceable;
		Boolean freq_traceable;
	} time;
};

struct ptpd_intf_fds {
	int event_sock;
	int general_sock;
};


/* Initialise PTPD based on the runtime options specified */
int ptpd_init(struct ptpd_global_context **ptpd_global);

/* Create a PTPD port based on the runtime options specified */
int ptpd_create_port(struct ptpd_port_config *config,
		     struct ptpd_intf_context *ifshared,
		     struct ptpd_port_context **ptpd);

/* Create a PTPD interface based on the runtime options specified */
int ptpd_create_interface(struct ptpd_intf_config *config,
			  struct ptpd_global_context *global,
			  struct ptpd_intf_context **ptpd_if);

/* Destroy port instance of PTPD */
void ptpd_port_destroy(struct ptpd_port_context *ptpd_port);

/* Destroy interface instance of PTPD */
void ptpd_interface_destroy(struct ptpd_intf_context *ptpd_if);

/* Destroy global instance of PTPD */
void ptpd_destroy(struct ptpd_global_context *ptpd_global);


/* A timer tick has occurred - update timers */
void ptpd_timer_tick(struct ptpd_port_context *ptpd,
		     sfptpd_sync_module_ctrl_flags_t ctrl_flags);

/* One or both of the PTP sockets is ready */
void ptpd_sockets_ready(struct ptpd_intf_context *ptpd_if, bool event,
			bool general);


/* Change which parts of PTP are enabled */
void ptpd_control(struct ptpd_port_context *ptpd,
		  sfptpd_sync_module_ctrl_flags_t ctrl_flags);

/* Update the grandmaster info. This is used when a PTP instance is
 * operating as a master and a different sync module instance is currently
 * selected. It allows the grandmaster information to be updated which in
 * turn will be communicated via Announce messages to downstream slaves. */
void ptpd_update_gm_info(struct ptpd_port_context *ptpd,
			 bool remote_grandmaster,
			 uint8_t clock_id[8],
			 uint8_t clock_class,
			 ptpd_time_source_e time_source,
			 ptpd_clock_accuracy_e clock_accuracy,
			 unsigned int offset_scaled_log_variance,
			 unsigned int steps_removed,
			 bool time_traceable,
			 bool freq_traceable);

/* Update the leap second state. This is used when a PTP instance is
 * operating as a master and a different sync module instance is currently
 * selected. It allows the leap second state to be updated which in turn
 * will be communicated via Announce messages to downstream slaves. */
void ptpd_update_leap_second(struct ptpd_port_context *ptpd,
			     bool leap59, bool leap61);

/* Step the clock by the specified amount */
void ptpd_step_clock(struct ptpd_port_context *ptpd, struct timespec *offset);


/* Change the interface being used for PTP */
int ptpd_change_interface(struct ptpd_intf_context *ptpd, Octet *logical_iface_name,
			  struct sfptpd_interface *physical_iface,
			  ptpd_timestamp_type_e timestamp_type);


/* Get snapshot of current state of PTPD */
int ptpd_get_snapshot(struct ptpd_port_context *ptpd, struct ptpd_port_snapshot *snapshot);

/* Get fds for this interface */
int ptpd_get_intf_fds(struct ptpd_intf_context *ptpd, struct ptpd_intf_fds *fds);

/* Get a snapshot of the PTPD counters */
int ptpd_get_counters(struct ptpd_port_context *ptpd, struct ptpd_counters *stats);


/* Clear PTPD counters */
int ptpd_clear_counters(struct ptpd_port_context *ptpd);


/* Test operation, master mode only. Set UTC offset */
int ptpd_test_set_utc_offset(struct ptpd_port_context *ptpd, int offset);

/* Test operation. Get the jitter type */
int ptpd_test_get_bad_timestamp_type(struct ptpd_port_context *ptpd);

/* Test operation. Set packet timestamp jitter */
int ptpd_test_set_bad_timestamp(struct ptpd_port_context *ptpd,
			   	int type,
			   	int interval_pkts,
			   	int max_jitter);

/* Test operation. Set transparent clock emulation */
int ptpd_test_set_transparent_clock_emulation(struct ptpd_port_context *ptpd,
					      int max_correction);

/* Test operation, master mode only. Set boundary clock emulation */
int ptpd_test_set_boundary_clock_emulation(struct ptpd_port_context *ptpd,
					   UInteger8 grandmaster_id[],
					   UInteger32 steps_removed);

/* Test operation, master mode only. Change grandmaster clock attributes */
int ptpd_test_change_grandmaster_clock(struct ptpd_port_context *ptpd,
				       UInteger8 clock_class,
				       Enumeration8 clock_accuracy,
				       UInteger16 offset_scaled_log_variance,
				       UInteger8 priority1,
				       UInteger8 priority2);

/* Test operation, master mode only. Suppress certain packet types */
int ptpd_test_pkt_suppression(struct ptpd_port_context *ptpd,
			      bool no_announce_pkts,
			      bool no_sync_pkts,
			      bool no_follow_ups,
			      bool no_delay_resps);

/* Save the last MTIE window */
void ptpd_publish_mtie_window(struct ptpd_port_context *ptpd,
			      bool mtie_valid,
			      uint32_t window_number,
			      int window_seconds,
			      long double min,
			      long double max,
			      const struct timespec *min_time,
			      const struct timespec *max_time);

/* Publish state changes */
void ptpd_publish_status(struct ptpd_port_context *ptpd,
			 int alarms,
			 bool selected,
			 bool in_sync,
			 bool bond_changed);

/** Turn an a bitfield of message types corresponding to missing message
 * alarms into an alarms bitfield.
 * @param msg_alarms Pointer to the message types bitfield. On return this updated
 * with all the converted alarm bits cleared.
 * @return The alarms output */
int ptpd_translate_alarms_from_msg_type_bitfield(int *msg_alarms);

/** Turn alarms not relating to missing messages into the format used
 * in slave status reporting for the Solarflare extension TLV.
 * @param alarms Pointer to the alarms bitfield. On return this updated
 * with all the converted alarm bits cleared.
 * @return The alarm bits in protocol format */
int ptpd_translate_alarms_from_protocol(int *other_alarms);


#endif /* _PTPD_LIB_H */
