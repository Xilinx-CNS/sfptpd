/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_ptp_monitor.c
 * @brief  PTP Monitor
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <limits.h>

#include "sfptpd_sync_module.h"
#include "sfptpd_ptp_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_clock.h"
#include "sfptpd_statistics.h"
#include "sfptpd_engine.h"
#include "sfptpd_interface.h"
#include "sfptpd_misc.h"
#include "sfptpd_thread.h"
#include "sfptpd_pps_module.h"
#include "sfptpd_db.h"

#include "ptpd_lib.h"


/****************************************************************************
 * Types
 ****************************************************************************/

struct sfptpd_ptp_monitor {

	/* The nodes table stores information about each node */
	struct sfptpd_db_table *nodes_table;

	/* These tables are for logging data; they are periodically
	   written to JSON Lines output (if configured) and then
	   cleared. */
	struct sfptpd_db_table *rx_event_table;
	struct sfptpd_db_table *tx_event_table;
	struct sfptpd_db_table *slave_status_table;

	/* This tables stores the latest state for each slave
	   monitored and is not cleared. */
	struct sfptpd_db_table *slave_status_latest_table;

	int rx_event_seq_counter;
	int tx_event_seq_counter;
	int slave_status_seq_counter;
};

struct sfptpd_ptp_monitor_node {
	PortIdentity port_id;
	struct sockaddr_storage protocol_address;
	socklen_t protocol_address_len;
	int domain;

	/* Allow space for an IPv6 address and 12-character scope id */
	char host[53];
};

/* Base class for main event or similar records */
struct monitor_record_common {
	PortIdentity port_id;
	PortIdentity ref_port_id;
	struct sfptpd_timespec monitor_timestamp;
	int monitor_seq_id;
	uint16_t event_seq_id;
};

struct sfptpd_ptp_monitor_rx_event {
	/* Base class member, must be first */
	struct monitor_record_common common;

	SlaveRxSyncTimingDataElement timing_data;
	SlaveRxSyncComputedDataElement computed_data;
	bool timing_data_present;
	bool computed_data_present;
};

struct sfptpd_ptp_monitor_tx_event {
	/* Base class member, must be first element */
	struct monitor_record_common common;

	SlaveTxEventTimestampsElement timestamp;
	ptpd_msg_id_e message_type;
};

struct sfptpd_ptp_monitor_slave_status {
	/* Base class member, must be first element */
	struct monitor_record_common common;

	SlaveStatus slave_status;
};


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Function prototypes
 ****************************************************************************/


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

/* Nodes table */

static int node_compare_port_id(const void *key, const void *record)
{
	PortIdentity *my_key = (PortIdentity *) key;
	struct sfptpd_ptp_monitor_node *my_record = (struct sfptpd_ptp_monitor_node *) record;
	return memcmp(my_key, &my_record->port_id, sizeof *my_key);
}

SFPTPD_DB_SORT_FN(node_compare_port_id, struct sfptpd_ptp_monitor_node, rec, &rec->port_id)

enum sfptpd_db_node_fields {
	NODE_FIELD_PORT_ID,
};

struct sfptpd_db_field node_fields[] = {
	SFPTPD_DB_FIELD("port-id", NODE_FIELD_PORT_ID, node_compare_port_id, NULL)
};

struct sfptpd_db_table_def node_table_def = {
	.num_fields = 1,
	.fields = node_fields,
	.record_size = sizeof(struct sfptpd_ptp_monitor_node),
};

/* Shared for all event types */

static int common_compare_port_id(const void *key, const void *record)
{
	return memcmp(key, &((struct monitor_record_common *) record)->port_id, sizeof(PortIdentity));
}


static int common_compare_ref_port_id(const void *key, const void *record)
{
	return memcmp(key, &((struct monitor_record_common *) record)->ref_port_id, sizeof(PortIdentity));
}


static int common_compare_event_seq_id(const void *key, const void *record)
{
	return (((int) *((uint16_t *) key)) -
		((int) ((struct monitor_record_common *) record)->event_seq_id));
}

static int common_compare_monitor_seq_id(const void *key, const void *record)
{
	return (*((int *) key) -
		((struct monitor_record_common *) record)->monitor_seq_id);
}

static int common_compare_monitor_timestamp(const void *key, const void *record)
{
	return sfptpd_time_cmp((struct sfptpd_timespec *) key,
			       &((struct monitor_record_common *) record)->monitor_timestamp);
}

static int common_snprint_event_seq_id(char *buf, size_t sz, int width, const void *record)
{
	return snprintf(buf, sz, "%*d", width, ((struct monitor_record_common *) record)->event_seq_id);
}

static int common_snprint_monitor_seq_id(char *buf, size_t sz, int width, const void *record)
{
	return snprintf(buf, sz, "%*d", width, ((struct monitor_record_common *) record)->monitor_seq_id);
}

SFPTPD_DB_SORT_FN(common_compare_port_id, struct monitor_record_common, rec, &rec->port_id)
SFPTPD_DB_SORT_FN(common_compare_ref_port_id, struct monitor_record_common, rec, &rec->ref_port_id)
SFPTPD_DB_SORT_FN(common_compare_event_seq_id, struct monitor_record_common, rec, &rec->event_seq_id)
SFPTPD_DB_SORT_FN(common_compare_monitor_seq_id, struct monitor_record_common, rec, &rec->monitor_seq_id)
SFPTPD_DB_SORT_FN(common_compare_monitor_timestamp, struct monitor_record_common, rec, &rec->monitor_timestamp)

enum common_fields {
	COMMON_FIELD_PORT_ID = 0,
	COMMON_FIELD_REF_PORT_ID,
	COMMON_FIELD_EVENT_SEQ_ID,
	COMMON_FIELD_MONITOR_SEQ_ID,
	COMMON_FIELD_MONITOR_TIMESTAMP,
	COMMON_FIELD_END,
};

/* Rx Event table */

struct sfptpd_db_field rx_event_fields[] = {
	SFPTPD_DB_FIELD("port-id", COMMON_FIELD_PORT_ID, common_compare_port_id, NULL)
	SFPTPD_DB_FIELD("ref-port-id", COMMON_FIELD_REF_PORT_ID, common_compare_ref_port_id, NULL)
	SFPTPD_DB_FIELD("sync-seq", COMMON_FIELD_EVENT_SEQ_ID, common_compare_event_seq_id, common_snprint_event_seq_id)
	SFPTPD_DB_FIELD("monitor-seq-id", COMMON_FIELD_MONITOR_SEQ_ID, common_compare_monitor_seq_id, common_snprint_monitor_seq_id)
	SFPTPD_DB_FIELD("monitor-timestamp", COMMON_FIELD_MONITOR_TIMESTAMP, common_compare_monitor_timestamp, NULL)
};

struct sfptpd_db_table_def rx_event_table_def = {
	.num_fields = 5,
	.fields = rx_event_fields,
	.record_size = sizeof(struct sfptpd_ptp_monitor_rx_event),
};


/* Tx Event table */

struct sfptpd_db_field tx_event_fields[] = {
	SFPTPD_DB_FIELD("port-id", COMMON_FIELD_PORT_ID, common_compare_port_id, NULL)
	SFPTPD_DB_FIELD("monitor-seq-id", COMMON_FIELD_MONITOR_SEQ_ID, common_compare_monitor_seq_id, common_snprint_monitor_seq_id)
	SFPTPD_DB_FIELD("monitor-timestamp", COMMON_FIELD_MONITOR_TIMESTAMP, common_compare_monitor_timestamp, NULL)
};

struct sfptpd_db_table_def tx_event_table_def = {
	.num_fields = 3,
	.fields = tx_event_fields,
	.record_size = sizeof(struct sfptpd_ptp_monitor_tx_event),
};


/* Slave Status table */

struct sfptpd_db_field slave_status_fields[] = {
	SFPTPD_DB_FIELD("port-id", COMMON_FIELD_PORT_ID, common_compare_port_id, NULL)
	SFPTPD_DB_FIELD("monitor-seq-id", COMMON_FIELD_MONITOR_SEQ_ID, common_compare_monitor_seq_id, common_snprint_monitor_seq_id)
	SFPTPD_DB_FIELD("monitor-timestamp", COMMON_FIELD_MONITOR_TIMESTAMP, common_compare_monitor_timestamp, NULL)
};

struct sfptpd_db_table_def slave_status_table_def = {
	.num_fields = 3,
	.fields = slave_status_fields,
	.record_size = sizeof(struct sfptpd_ptp_monitor_slave_status),
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_ptp_monitor *sfptpd_ptp_monitor_create(void)
{
	struct sfptpd_ptp_monitor *new = calloc(1, sizeof *new);

	if (new == NULL) {
		ERROR("ptp: could not create monitor object, %s\n",
		      strerror(errno));
	} else {
		new->nodes_table = sfptpd_db_table_new(&node_table_def, STORE_DEFAULT);
		new->rx_event_table = sfptpd_db_table_new(&rx_event_table_def, STORE_DEFAULT);
		new->tx_event_table = sfptpd_db_table_new(&tx_event_table_def, STORE_DEFAULT);
		new->slave_status_table = sfptpd_db_table_new(&slave_status_table_def, STORE_DEFAULT);
		new->slave_status_latest_table = sfptpd_db_table_new(&slave_status_table_def, STORE_DEFAULT);
	}

	return new;
}


void sfptpd_ptp_monitor_destroy(struct sfptpd_ptp_monitor *monitor)
{
	/* DELETE the table contents */
	sfptpd_db_table_delete(monitor->rx_event_table);
	sfptpd_db_table_delete(monitor->tx_event_table);
	sfptpd_db_table_delete(monitor->slave_status_table);
	sfptpd_db_table_delete(monitor->slave_status_latest_table);
	sfptpd_db_table_delete(monitor->nodes_table);

	/* DROP the tables */
	sfptpd_db_table_free(monitor->rx_event_table);
	monitor->rx_event_table = NULL;
	sfptpd_db_table_free(monitor->tx_event_table);
	monitor->tx_event_table = NULL;
	sfptpd_db_table_free(monitor->slave_status_table);
	monitor->slave_status_table = NULL;
	sfptpd_db_table_free(monitor->slave_status_latest_table);
	monitor->slave_status_latest_table = NULL;
	sfptpd_db_table_free(monitor->nodes_table);
	monitor->nodes_table = NULL;

	free(monitor);
}


static void monitor_register_node(struct sfptpd_ptp_monitor *monitor,
				  const PortIdentity *port_identity,
				  struct sockaddr_storage *address,
				  socklen_t address_len,
				  int domain)
{
	struct sfptpd_db_record_ref node_ref;

	node_ref = sfptpd_db_table_find(monitor->nodes_table,
					NODE_FIELD_PORT_ID, port_identity);
	if (!sfptpd_db_record_exists(&node_ref)) {
		struct sfptpd_ptp_monitor_node node = {};
		int rc;

		/* Deal with new node we haven't seen before */
		node.port_id = *port_identity;
		node.protocol_address_len = address_len;
		node.domain = domain;
		memcpy(&node.protocol_address, address, address_len);

		rc = getnameinfo((struct sockaddr *) address, address_len,
				 node.host, sizeof node.host,
				 NULL, 0, NI_NUMERICHOST);
		if (rc != 0) {
			ERROR("ptp: getnameinfo: %s\n", gai_strerror(rc));
		}

		sfptpd_db_table_insert(monitor->nodes_table, &node);
	}
}


static struct sfptpd_db_record_ref monitor_obtain_rx_event_record(struct sfptpd_ptp_monitor *monitor,
								  struct sfptpd_ptp_monitor_rx_event *event,
								  const PortIdentity *port_identity,
								  const PortIdentity *ref_port_identity,
								  uint16_t sync_seq)
{
	struct sfptpd_db_record_ref event_ref;

	event_ref = sfptpd_db_table_find(monitor->rx_event_table,
					 COMMON_FIELD_PORT_ID, port_identity,
					 COMMON_FIELD_REF_PORT_ID, ref_port_identity,
					 COMMON_FIELD_EVENT_SEQ_ID, &sync_seq);

	if (sfptpd_db_record_exists(&event_ref)) {
		sfptpd_db_record_get_data(&event_ref, event, sizeof *event);
	} else {
		/* New Sync seq id seen */
		memset(event, '\0', sizeof *event);
		event->common.port_id = *port_identity;
		event->common.ref_port_id = *ref_port_identity;
		event->common.event_seq_id = sync_seq;
		event->common.monitor_seq_id = monitor->rx_event_seq_counter++;
		sfclock_gettime(CLOCK_REALTIME, &event->common.monitor_timestamp);

		/* Insert new record */
		event_ref = sfptpd_db_table_insert(monitor->rx_event_table, &event);
	}

	return event_ref;
}


void sfptpd_ptp_monitor_update_rx_timing(struct ptpd_remote_stats_logger *logger,
					 struct ptpd_remote_stats stats,
					 int num_timing_data,
					 SlaveRxSyncTimingDataElement *timing_data)

{
	struct sfptpd_ptp_monitor *monitor = logger->context;
	struct sfptpd_db_record_ref event_ref;
	struct sfptpd_ptp_monitor_rx_event event;
	int i;

	monitor_register_node(monitor, stats.port_identity,
			      stats.address, stats.address_len,
			      stats.domain);

	for (i = 0; i < num_timing_data; i++) {

		/* Find or create a record for this sync seq */
		event_ref = monitor_obtain_rx_event_record(monitor,
							   &event,
							   stats.port_identity,
							   stats.ref_port_identity,
							   timing_data[i].sequenceId);

		/* Populate the timing data */
		event.timing_data = timing_data[i];
		event.timing_data_present = true;

		/* Update the database */
		sfptpd_db_record_update(&event_ref, &event);
	}
}


void sfptpd_ptp_monitor_update_rx_computed(struct ptpd_remote_stats_logger *logger,
					   struct ptpd_remote_stats stats,
					   int num_computed_data,
					   SlaveRxSyncComputedDataElement *computed_data)
{
	struct sfptpd_ptp_monitor *monitor = logger->context;
	struct sfptpd_db_record_ref event_ref;
	struct sfptpd_ptp_monitor_rx_event event;
	int i;

	monitor_register_node(monitor, stats.port_identity,
			      stats.address, stats.address_len,
			      stats.domain);

	for (i = 0; i < num_computed_data; i++) {

		/* Find or create a record for this sync seq */
		event_ref = monitor_obtain_rx_event_record(monitor,
							   &event,
							   stats.port_identity,
							   stats.ref_port_identity,
							   computed_data[i].sequenceId);

		/* Populate the computed data */
		event.computed_data = computed_data[i];
		event.computed_data_present = true;

		/* Update the database */
		sfptpd_db_record_update(&event_ref, &event);
	}
}


void sfptpd_ptp_monitor_log_tx_timestamp(struct ptpd_remote_stats_logger *logger,
					 struct ptpd_remote_stats stats,
					 ptpd_msg_id_e message_type,
					 int num_timestamps,
					 SlaveTxEventTimestampsElement *timestamps)
{
	struct sfptpd_ptp_monitor *monitor = logger->context;
	struct sfptpd_ptp_monitor_tx_event event = {};
	int i;

	monitor_register_node(monitor, stats.port_identity,
			      stats.address, stats.address_len,
			      stats.domain);

	for (i = 0; i < num_timestamps; i++) {

		/* Create a record for this event */
		event.common.port_id = *stats.port_identity;
		event.common.ref_port_id = *stats.ref_port_identity;
		event.common.event_seq_id = timestamps[i].sequenceId;
		event.common.monitor_seq_id = monitor->tx_event_seq_counter++;
		sfclock_gettime(CLOCK_REALTIME, &event.common.monitor_timestamp);
		event.timestamp = timestamps[i];
		event.message_type = message_type;

		/* Insert into the database */
		sfptpd_db_table_insert(monitor->tx_event_table, &event);
	}
}


void sfptpd_ptp_monitor_update_slave_status(struct ptpd_remote_stats_logger *logger,
					    struct ptpd_remote_stats stats,
					    SlaveStatus *slave_status)
{
	struct sfptpd_ptp_monitor *monitor = logger->context;
	struct sfptpd_ptp_monitor_slave_status record;
	struct sfptpd_db_record_ref event_ref;

	monitor_register_node(monitor, stats.port_identity,
			      stats.address, stats.address_len,
			      stats.domain);

	memset(&record, '\0', sizeof record);
	copyPortIdentity(&record.common.port_id, (PortIdentity *) stats.port_identity);
	record.common.monitor_seq_id = monitor->slave_status_seq_counter++;
	record.slave_status = *slave_status;
	sfclock_gettime(CLOCK_REALTIME, &record.common.monitor_timestamp);

	/* Insert new record into log */
	sfptpd_db_table_insert(monitor->slave_status_table, &record);

	/* Look for an entry in the 'latest state' table */
	event_ref = sfptpd_db_table_find(monitor->slave_status_latest_table,
					 COMMON_FIELD_PORT_ID, stats.port_identity);

	if (sfptpd_db_record_exists(&event_ref)) {
		/* Update with the latest information */
		sfptpd_db_record_update(&event_ref, &record);
	} else {
		/* Insert information for the new node */
		sfptpd_db_table_insert(monitor->slave_status_latest_table, &record);
	}
}


#define CLOCK_ID_FORMAT "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx"
#define PORT_ID_FORMAT CLOCK_ID_FORMAT ".%-5d"
#define PORT_ID_FORMAT_VAR_WIDTH CLOCK_ID_FORMAT ".%d"

#define CLOCK_ID_CONTENT(variable)	\
	(variable)[0],			\
	(variable)[1],			\
	(variable)[2],			\
	(variable)[3],			\
	(variable)[4],			\
	(variable)[5],			\
	(variable)[6],			\
	(variable)[7]

#define PORT_ID_CONTENT(variable)			\
	CLOCK_ID_CONTENT((variable).clockIdentity),	\
	(variable).portNumber


static void monitor_output_slave_status_text(FILE *stream,
					     struct sfptpd_ptp_monitor *monitor,
					     struct sfptpd_db_table *table)
{
	struct sfptpd_db_query_result status_result;
	int i;
	const char *format_status_string = "| %25s | %13s | %8s | %6s | %4s | %5s | %4s | %19s |\n";
	const char *format_status_data = "| " PORT_ID_FORMAT " | %13s |  %c%c%c%c%c%c%c |  %c%c%c%c%c | %4s | %5d | %4d | " CLOCK_ID_FORMAT " |\n";

	sfptpd_log_table_row(stream, true,
			     format_status_string,
			     "port-id",
			     "state",
			     "msgalrms",
			     "alarms",
			     "bond",
			     "slctd",
			     "sync",
			     "gm-id");

	status_result = sfptpd_db_table_query(table,
					      SFPTPD_DB_SEL_ORDER_BY,
					      COMMON_FIELD_MONITOR_SEQ_ID);

	for (i = 0; i < status_result.num_records; i++) {
		struct sfptpd_ptp_monitor_slave_status *status = status_result.record_ptrs[i];
		SlaveStatus *s = &status->slave_status;
		const char *state = portState_getName(s->portState);

		/* Remove 'PTP_' prefix from state names */
		if (state != NULL && strlen(state) > 4) {
			state = state + 4;
		} else {
			state = "INVALID";
		}

		sfptpd_log_table_row(stream, i + 1 == status_result.num_records,
				     format_status_data,
				     PORT_ID_CONTENT(status->common.port_id),
				     state,
				     (s->missingMessageAlarms & (1 << PTPD_MSG_PDELAY_RESP_FOLLOW_UP)) ? 'f' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_DELAY_RESP)) ? 'R' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_FOLLOW_UP)) ? 'F' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_PDELAY_RESP)) ? 'q' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_PDELAY_REQ)) ? 'p' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_DELAY_REQ)) ? 'D' : '-',
				     (s->missingMessageAlarms & (1 << PTPD_MSG_SYNC)) ? 'S' : '-',
				     (s->otherAlarms & (1 << PTPD_SFC_ALARM_UNKNOWN)) ? '?' : '-',
				     (s->otherAlarms & (1 << PTPD_SFC_ALARM_SERVO_FAIL)) ? 's' : '-',
				     (s->otherAlarms & (1 << PTPD_SFC_ALARM_NO_INTERFACE)) ? 'i' : '-',
				     (s->otherAlarms & (1 << PTPD_SFC_ALARM_NO_RX_TIMESTAMPS)) ? 'r' : '-',
				     (s->otherAlarms & (1 << PTPD_SFC_ALARM_NO_TX_TIMESTAMPS)) ? 't' : '-',
				     (s->events & (1 << PTPD_SFC_EVENT_BOND_CHANGED)) ? "chgd" : "",
				     (s->flags & (1 << PTPD_SFC_FLAG_SELECTED)) ? 1 : 0,
				     (s->flags & (1 << PTPD_SFC_FLAG_IN_SYNC)) ? 1 : 0,
				     CLOCK_ID_CONTENT(s->grandmasterIdentity));
	}

	status_result.free(&status_result);
}


static const char *outgoing_event_msg_name(ptpd_msg_id_e id)
{
	const char *mtype;

	switch (id) {
	case PTPD_MSG_DELAY_REQ:
		mtype = "Delay_Req";
		break;
	case PTPD_MSG_PDELAY_REQ:
		mtype = "PDelay_Req";
		break;
	case PTPD_MSG_PDELAY_RESP:
		mtype = "PDelay_Resp";
		break;
	default:
		mtype = "Invalid";
	}

	return mtype;
}


static void monitor_output_text(struct sfptpd_ptp_monitor *monitor)
{
	struct sfptpd_log *log;
	FILE *stream;
	struct sfptpd_ptp_monitor_node *node;
	struct sfptpd_db_query_result nodes_result;
	struct sfptpd_db_query_result events_result;
	int i, j;

	log = sfptpd_log_open_remote_monitor();

	if (log == NULL)
		return;

	stream = sfptpd_log_file_get_stream(log);

	fputs("monitored slave ports\n=========\n", stream);

	const char *format_node_string = "| %25s | %6s | %*s |\n";
	const char *format_node_data = "| " PORT_ID_FORMAT " | %6d | %*s |\n";

	sfptpd_log_table_row(stream, true,
			     format_node_string,
			     "port-id",
			     "domain",
			     sizeof node->host - 1,
			     "protocol address");

	nodes_result = sfptpd_db_table_query(monitor->nodes_table,
					     SFPTPD_DB_SEL_ORDER_BY,
					     NODE_FIELD_PORT_ID);

	/* Output a nodes table at the top of the file */
	for (i = 0; i < nodes_result.num_records; i++) {
		node = nodes_result.record_ptrs[i];
		sfptpd_log_table_row(stream, i + 1 == nodes_result.num_records,
				     format_node_data,
				     PORT_ID_CONTENT(node->port_id),
				     node->domain,
				     sizeof node->host - 1,
				     node->host);
	}

	/* Output an event table per node */
	for (i = 0; i < nodes_result.num_records; i++) {
		node = nodes_result.record_ptrs[i];

		fprintf(stream,
			"\nlog of recent rx events on monitored slave port "
			PORT_ID_FORMAT "\n=========\n",
			PORT_ID_CONTENT(node->port_id));

		const char *format_rx_event_string = "| %25s | %5s | %21s | %21s | %32s |\n";
		const char *format_rx_event_data = "| " PORT_ID_FORMAT " | %5d | %+21.3Lf | %21.3Lf | %22ld.%09ld |\n";

		sfptpd_log_table_row(stream, true,
				     format_rx_event_string,
				     "ref-port-id",
				     "sync",
				     "offset-from-master",
				     "mean-path-delay",
				     //				     "scaled-neighbour-rate-ratio",
				     //				     "total-correction-field",
				     //				     "cumulative-scaled-rate-offset",
				     "ingress-timestamp");

		events_result = sfptpd_db_table_query(monitor->rx_event_table,
						      COMMON_FIELD_PORT_ID, &node->port_id,
						      SFPTPD_DB_SEL_ORDER_BY,
						      COMMON_FIELD_MONITOR_SEQ_ID);

		for (j = 0; j < events_result.num_records; j++) {
			struct sfptpd_ptp_monitor_rx_event *rx_event;
			sfptpd_time_t offset = NAN;
			sfptpd_time_t mpd = NAN;
			struct sfptpd_timespec ts = {};

			rx_event = events_result.record_ptrs[j];
			if (rx_event->computed_data_present) {
				offset = sfptpd_time_scaled_ns_to_float_ns(rx_event->computed_data.offsetFromMaster);
				mpd = sfptpd_time_scaled_ns_to_float_ns(rx_event->computed_data.meanPathDelay);
			}
			if (rx_event->timing_data_present) {
				toInternalTime(&ts, &rx_event->timing_data.syncEventIngressTimestamp);
			}
			sfptpd_log_table_row(stream, j + 1 == events_result.num_records,
					     format_rx_event_data,
					     PORT_ID_CONTENT(rx_event->common.ref_port_id),
					     rx_event->common.event_seq_id,
					     offset,
					     mpd,
					     ts.sec, ts.nsec);
		}
		events_result.free(&events_result);

		fprintf(stream,
			"\nlog of recent tx events on monitored slave port "
			PORT_ID_FORMAT "\n=========\n",
			PORT_ID_CONTENT(node->port_id));

		const char *format_tx_event_string = "| %25s | %11s | %5s | %32s |\n";
		const char *format_tx_event_data = "| " PORT_ID_FORMAT " | %11s | %5d | %22ld.%09ld |\n";

		sfptpd_log_table_row(stream, true,
				     format_tx_event_string,
				     "source-port-id",
				     "message-type",
				     "seq",
				     "egress-timestamp");

		events_result = sfptpd_db_table_query(monitor->tx_event_table,
						      COMMON_FIELD_PORT_ID, &node->port_id,
						      SFPTPD_DB_SEL_ORDER_BY,
						      COMMON_FIELD_MONITOR_SEQ_ID);

		for (j = 0; j < events_result.num_records; j++) {
			struct sfptpd_ptp_monitor_tx_event *tx_event;
			struct sfptpd_timespec ts = {};
			const char *mtype;

			tx_event = events_result.record_ptrs[j];
			toInternalTime(&ts, &tx_event->timestamp.eventEgressTimestamp);
			mtype = outgoing_event_msg_name(tx_event->message_type);

			sfptpd_log_table_row(stream, j + 1 == events_result.num_records,
					     format_tx_event_data,
					     PORT_ID_CONTENT(tx_event->common.ref_port_id),
					     mtype,
					     tx_event->common.event_seq_id,
					     ts.sec, ts.nsec);
		}
		events_result.free(&events_result);
	}
	nodes_result.free(&nodes_result);

	fputs("\nlog of recent slave status and alarms\n=========\n", stream);
	monitor_output_slave_status_text(stream, monitor, monitor->slave_status_table);

	fputs("\nlatest slave status and alarms\n=========\n", stream);
	monitor_output_slave_status_text(stream, monitor, monitor->slave_status_latest_table);

	sfptpd_log_file_close(log);
}


static void monitor_write_json_node(void *record, void *context)
{
	struct sfptpd_ptp_monitor_node *node = record;
	FILE *stream = context;

	fprintf(stream,
		"{ \"node\": {"
		"\"port-id\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"domain\": %d, "
		"\"address\": \"%s\" } }\n",
		PORT_ID_CONTENT(node->port_id),
		node->domain,
		node->host);
}


static void monitor_write_json_rx_event(void *record, void *context)
{
	struct sfptpd_ptp_monitor_rx_event *rx_event = record;
	FILE *stream = context;
	sfptpd_time_t offset = NAN;
	sfptpd_time_t mpd = NAN;
	struct sfptpd_timespec ts = {};
	char monitor_time[32];

	if (rx_event->computed_data_present) {
		offset = sfptpd_time_scaled_ns_to_float_ns(rx_event->computed_data.offsetFromMaster);
		mpd = sfptpd_time_scaled_ns_to_float_ns(rx_event->computed_data.meanPathDelay);
	}
	if (rx_event->timing_data_present) {
		toInternalTime(&ts, &rx_event->timing_data.syncEventIngressTimestamp);
	}

	sfptpd_local_strftime(monitor_time, sizeof monitor_time, "%Y-%m-%d %X",
			      &rx_event->common.monitor_timestamp.sec);

	fprintf(stream,
		"{ \"rx-event\": {"
		"\"monitor-seq-id\": %d, "
		"\"monitor-timestamp\": \"%s.%06d\", "
		"\"node\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"parent-port\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"sync-seq\": %d, "
		"\"offset-from-master\": %Lf, "
		"\"mean-path-delay\": %Lf, "
		"\"sync-ingress-timestamp\": " SFPTPD_FMT_SSFTIMESPEC_NS " } }\n",
		rx_event->common.monitor_seq_id,
		monitor_time,
		rx_event->common.monitor_timestamp.nsec / 1000,
		PORT_ID_CONTENT(rx_event->common.port_id),
		PORT_ID_CONTENT(rx_event->common.ref_port_id),
		rx_event->common.event_seq_id,
		isnormal(offset) ? offset : 0.0L,
		isnormal(mpd) ? mpd : 0.0L,
		SFPTPD_ARGS_SSFTIMESPEC_NS(ts));
}


static void monitor_write_json_tx_event(void *record, void *context)
{
	struct sfptpd_ptp_monitor_tx_event *tx_event = record;
	FILE *stream = context;
	struct sfptpd_timespec ts = {};
	char monitor_time[32];
	const char *mtype;

	toInternalTime(&ts, &tx_event->timestamp.eventEgressTimestamp);
	mtype = outgoing_event_msg_name(tx_event->message_type);

	sfptpd_local_strftime(monitor_time, sizeof monitor_time, "%Y-%m-%d %X",
			      &tx_event->common.monitor_timestamp.sec);

	fprintf(stream,
		"{ \"tx-event\": {"
		"\"monitor-seq-id\": %d, "
		"\"monitor-timestamp\": \"%s.%06d\", "
		"\"node\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"source-port\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"message-type\": \"%s\", "
		"\"event-seq-id\": %d, "
		"\"egress-timestamp\": " SFPTPD_FMT_SSFTIMESPEC_NS " } }\n",
		tx_event->common.monitor_seq_id,
		monitor_time,
		tx_event->common.monitor_timestamp.nsec / 1000,
		PORT_ID_CONTENT(tx_event->common.port_id),
		PORT_ID_CONTENT(tx_event->common.ref_port_id),
		mtype,
		tx_event->common.event_seq_id,
		SFPTPD_ARGS_SSFTIMESPEC_NS(ts));
}


static void monitor_write_json_slave_status(void *record, void *context)
{
	struct sfptpd_ptp_monitor_slave_status *slave_status = record;
	FILE *stream = context;
	char monitor_time[32];
	SlaveStatus *s = &slave_status->slave_status;
	const char *state = portState_getName(s->portState);
	int proto_msg_alarms;
	int proto_other_alarms;
	int msg_alarms;
	int other_alarms;

	/* Remove 'PTP_' prefix from state names */
	if (state != NULL && strlen(state) > 4) {
		state = state + 4;
	} else {
		state = "INVALID";
	}

	sfptpd_local_strftime(monitor_time, sizeof monitor_time, "%Y-%m-%d %X",
			      &slave_status->common.monitor_timestamp.sec);

	proto_msg_alarms = s->missingMessageAlarms;
	proto_other_alarms = s->otherAlarms;
	msg_alarms = ptpd_translate_alarms_from_msg_type_bitfield(&proto_msg_alarms);
	other_alarms = ptpd_translate_alarms_from_protocol(&proto_other_alarms);

	fprintf(stream,
		"{ \"slave-status\": {"
		"\"monitor-seq-id\": %d, "
		"\"monitor-timestamp\": \"%s.%06d\", "
		"\"node\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"gm-id\": \"" PORT_ID_FORMAT_VAR_WIDTH "\", "
		"\"state\": \"%s\", "
		"\"bond-changed\": %s, "
		"\"selected\": %s, "
		"\"in-sync\": %s, "
		"\"msg-alarms\": [",
		slave_status->common.monitor_seq_id,
		monitor_time,
		slave_status->common.monitor_timestamp.nsec / 1000,
		PORT_ID_CONTENT(slave_status->common.port_id),
		PORT_ID_CONTENT(slave_status->common.ref_port_id),
		state,
		(s->events & (1 << PTPD_SFC_EVENT_BOND_CHANGED)) ? "true" : "false",
		(s->flags & (1 << PTPD_SFC_FLAG_SELECTED)) ? "true" : "false",
		(s->flags & (1 << PTPD_SFC_FLAG_IN_SYNC)) ? "true" : "false");
	sfptpd_sync_module_alarms_stream(stream, msg_alarms, ",");
	fprintf(stream, "], \"alarms\": [");
	sfptpd_sync_module_alarms_stream(stream, other_alarms, ",");
	fprintf(stream, "]} }\n");
}


static void monitor_output_json(struct sfptpd_ptp_monitor *monitor, FILE *stream)
{
	sfptpd_db_table_foreach(monitor->nodes_table, monitor_write_json_node, stream);

	sfptpd_db_table_foreach(monitor->rx_event_table, monitor_write_json_rx_event, stream,
				SFPTPD_DB_SEL_ORDER_BY,
				COMMON_FIELD_MONITOR_SEQ_ID);

	sfptpd_db_table_foreach(monitor->tx_event_table, monitor_write_json_tx_event, stream,
				SFPTPD_DB_SEL_ORDER_BY,
				COMMON_FIELD_MONITOR_SEQ_ID);

	sfptpd_db_table_foreach(monitor->slave_status_table, monitor_write_json_slave_status, stream,
				SFPTPD_DB_SEL_ORDER_BY,
				COMMON_FIELD_MONITOR_SEQ_ID);

	fflush(stream);
}


void sfptpd_ptp_monitor_flush(struct sfptpd_ptp_monitor *monitor)
{
	FILE* stream;

	monitor_output_text(monitor);

	stream  = sfptpd_log_get_remote_monitor_out_stream();
	if (stream != NULL)
		monitor_output_json(monitor, stream);

	sfptpd_db_table_delete(monitor->rx_event_table);
	sfptpd_db_table_delete(monitor->tx_event_table);
	sfptpd_db_table_delete(monitor->slave_status_table);
}


/* fin */
