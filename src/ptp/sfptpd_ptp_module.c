/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_ptp_module.c
 * @brief  PTP Synchronization Module
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
#include <sys/inotify.h>

#include "sfptpd_app.h"
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
#include "sfptpd_link.h"

#include "ptpd_lib.h"


/****************************************************************************
 * Types
 ****************************************************************************/

#define PTP_TIMER_ID (0)
#define PTP_TIMER_INTERVAL_NS (62500000)

#define PTP_MAX_PHYSICAL_IFS (16)

/* Minimum time used to limit how often bond/team
 * is scanned for changes. */
#define MIN_BOND_UPDATE_INTERVAL_NS (30 * 1000 * 1000)

enum ptp_stats_ids {
	PTP_STATS_ID_OFFSET,
	PTP_STATS_ID_ONE_WAY_DELAY,
	PTP_STATS_ID_FREQ_ADJ,
	PTP_STATS_ID_SYNCHRONIZED,
	PTP_STATS_ID_ANNOUNCE_TXED,
	PTP_STATS_ID_ANNOUNCE_RXED,
	PTP_STATS_ID_ANNOUNCE_TIMEOUTS,
	PTP_STATS_ID_SYNC_PKT_TXED,
	PTP_STATS_ID_SYNC_PKT_RXED,
	PTP_STATS_ID_SYNC_PKT_TIMEOUTS,
	PTP_STATS_ID_FOLLOW_UP_TXED,
	PTP_STATS_ID_FOLLOW_UP_RXED,
	PTP_STATS_ID_FOLLOW_UP_TIMEOUTS,
	PTP_STATS_ID_OUT_OF_ORDER_FOLLOW_UPS,
	PTP_STATS_ID_DELAY_REQ_TXED,
	PTP_STATS_ID_DELAY_REQ_RXED,
	PTP_STATS_ID_DELAY_RESP_TXED,
	PTP_STATS_ID_DELAY_RESP_RXED,
	PTP_STATS_ID_DELAY_RESP_TIMEOUTS,
	PTP_STATS_ID_DELAY_MODE_MISMATCH,
	PTP_STATS_ID_CLOCK_STEPS,
	PTP_STATS_ID_OUTLIERS,
	PTP_STATS_ID_OUTLIER_THRESHOLD,
	PTP_STATS_ID_TX_PKT_NO_TIMESTAMP,
	PTP_STATS_ID_RX_PKT_NO_TIMESTAMP,
	PTP_STATS_ID_PPS_OFFSET,
	PTP_STATS_ID_PPS_PERIOD,
	PTP_STATS_ID_NUM_PTP_NODES
};

typedef struct sfptpd_ptp_module sfptpd_ptp_module_t;
typedef struct sfptpd_ptp_instance sfptpd_ptp_instance_t;
typedef struct sfptpd_ptp_bond_info sfptpd_ptp_bond_info_t;
typedef struct sfptpd_ptp_intf sfptpd_ptp_intf_t;

/* Main objects making up the sfptpd PTP sync module

   sfptpd_ptp_module_t
   (sync module)
        |   /|\
       \|/   |
   sfptpd_ptp_intf_t ---> stfptpd_ptp_intf_t ----> ... ---> NULL
   (per-bond or unbonded logical interface)
        |   /|\
       \|/   |
   sfptpd_ptp_instance_t ---> stfptpd_ptp_instance_t ----> ... ---> NULL
   (sync instance)

*/


struct sfptpd_ptp_instance {

	/* Parent: reference to the interface object */
	sfptpd_ptp_intf_t *intf;

	/* Instance configuration */
	struct sfptpd_ptp_module_config *config;

	/* Which elements of the PTP instance are enabled */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* Snapshot of control flags */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags_snapshot;

	/* PPS propagation delay */
	sfptpd_time_t pps_delay;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchronized to the master */
	bool synchronized;

	/* Snapshot of in-sync flag */
	bool synchronized_snapshot;

	/* Snapshot of clustering score */
	int clustering_score_snapshot;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;

	/* Snapshot of PTPD public state */
	struct ptpd_port_snapshot ptpd_port_snapshot;

	/* PTPD private data structure */
	struct ptpd_port_context *ptpd_port_private;

	/* Local alarms - alarms set within this module, not ptpd2 */
	unsigned int local_alarms;

	/* Local alarms snapshot */
	unsigned int local_alarms_snapshot;

	/* Stats collected in sync module */
	struct sfptpd_stats_collection stats;

	/* SWPTP-906: external clock discriminator for BMCA */
	enum { DISC_NONE, DISC_SYNC_INSTANCE, DISC_CLOCK } discriminator_type;
	union {
		const struct sfptpd_sync_instance_info *sync_instance;
		struct sfptpd_clock *clock;
	} discriminator;

	/* Data associated with test modes */
	struct {
		/* Transparent clock simulation enabled */
		bool transparent_clock;

		/* Boundary clock test state */
		unsigned int boundary_clock_state;

		/* Grandmaster clock test state */
		unsigned int grandmaster_clock_state;

		/* Packet suppression test states */
		bool no_announce_pkts;
		bool no_sync_pkts;
		bool no_follow_ups;
		bool no_delay_resps;
	} test;

	sfptpd_ptp_instance_t *next;
};

/* Structure containing interface configuration */
struct sfptpd_ptp_bond_info {
	/* Logical interface specified when sfptpd was launched. This could
	 * be a VLAN, a bond or a real interface */
	char logical_if[IF_NAMESIZE];

	/* Number of VLAN tags and set of tags */
	unsigned int num_vlan_tags;
	uint16_t vlan_tags[SFPTPD_MAX_VLAN_TAGS];

	/* Bond interface name. Note that if the supplied interface is not part
	 * of a bond this will be in fact the physical interface name */
	char bond_if[IF_NAMESIZE];

	/* Type of bond detected including none for no bond */
	enum sfptpd_bond_mode bond_mode;

	/* Is a bridge */
	bool is_bridge;

	/* Set of physical interfaces associated with the logical interface.
	 * If no bond is involved this will be a set of 1. */
	unsigned int num_physical_ifs;

	/* Set of physical interfaces */
	struct sfptpd_interface *physical_ifs[PTP_MAX_PHYSICAL_IFS];

	/* Active physical interface */
	struct sfptpd_interface *active_if;
};

/* Structure containing interface state */
struct sfptpd_ptp_intf {

	/* Parent: reference to the sync module */
	sfptpd_ptp_module_t *module;

	/* Children: linked list of sync instances using this interface */
	struct sfptpd_ptp_instance *instance_list;

	/* The interface name as specified in the configuration.
	   This is used as a key for the logical interface state. */
	const char *defined_name;

	/* The name of the transport that qualifies this interface */
	const char *transport_name;
	
	/* The configuration of this logical interface, i.e.
	   bond characteristics etc. */
	struct sfptpd_ptp_bond_info bond_info;

	/* Set when the interface has tried to start */
	bool start_attempted;

	/* Set when the interface is started successfully */
	bool start_successful;

	/* PTPD private data structure */
	struct ptpd_intf_context *ptpd_intf_private;

	/* Current set of fds to be polled */
	struct ptpd_intf_fds ptpd_intf_fds;

	/* Handle of the PTP clock */
	struct sfptpd_clock *clock;

	/* A representative instance configuration for this
	 * interface for access to interface-level options. */
	struct sfptpd_ptp_module_config *representative_config;

	/* A bond change has been detected but not handled */
	bool bond_changed;

	/* Next time bond state can be refreshed. */
	struct timespec next_bond_refresh_time;

	sfptpd_ptp_intf_t *next;
};

struct sfptpd_ptp_module {

	/* Children: linked list of interfaces used by our sync instances */
	struct sfptpd_ptp_intf *intf_list;

	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* PTPD private shared global data structure */
	struct ptpd_global_context *ptpd_global_private;

	/* PTP remote monitor */
	struct sfptpd_ptp_monitor *remote_monitor;

	/* Whether the timers have been started */
	bool timers_started;

	/* Copy of current link table */
	struct sfptpd_link_table link_table;
};

struct sfptpd_ptp_accuracy_map {
	ptpd_clock_accuracy_e enumeration;
	long double float_ns;
};

/****************************************************************************
 * Constants
 ****************************************************************************/

static const struct sfptpd_stats_collection_defn ptp_stats_defns[] =
{
	{PTP_STATS_ID_OFFSET,                  SFPTPD_STATS_TYPE_RANGE, "offset-from-master", "ns", 0},
	{PTP_STATS_ID_ONE_WAY_DELAY,           SFPTPD_STATS_TYPE_RANGE, "one-way-delay", "ns", 0},
	{PTP_STATS_ID_FREQ_ADJ,                SFPTPD_STATS_TYPE_RANGE, "freq-adjustment", "ppb", 3},
	{PTP_STATS_ID_OUTLIER_THRESHOLD,       SFPTPD_STATS_TYPE_RANGE, "outlier-threshold", "ns", 0},
	{PTP_STATS_ID_SYNCHRONIZED,            SFPTPD_STATS_TYPE_COUNT, "synchronized"},
	{PTP_STATS_ID_ANNOUNCE_TXED,           SFPTPD_STATS_TYPE_COUNT, "announce-pkts-txed"},
	{PTP_STATS_ID_ANNOUNCE_RXED,           SFPTPD_STATS_TYPE_COUNT, "announce-pkts-rxed"},
	{PTP_STATS_ID_ANNOUNCE_TIMEOUTS,       SFPTPD_STATS_TYPE_COUNT, "announce-timeouts"},
	{PTP_STATS_ID_SYNC_PKT_TXED,           SFPTPD_STATS_TYPE_COUNT, "sync-pkts-txed"},
	{PTP_STATS_ID_SYNC_PKT_RXED,           SFPTPD_STATS_TYPE_COUNT, "sync-pkts-rxed"},
	{PTP_STATS_ID_SYNC_PKT_TIMEOUTS,       SFPTPD_STATS_TYPE_COUNT, "sync-pkt-timeouts"},
	{PTP_STATS_ID_FOLLOW_UP_TXED,          SFPTPD_STATS_TYPE_COUNT, "follow-up-pkts-txed"},
	{PTP_STATS_ID_FOLLOW_UP_RXED,          SFPTPD_STATS_TYPE_COUNT, "follow-up-pkts-rxed"},
	{PTP_STATS_ID_FOLLOW_UP_TIMEOUTS,      SFPTPD_STATS_TYPE_COUNT, "follow-up-timeouts"},
	{PTP_STATS_ID_OUT_OF_ORDER_FOLLOW_UPS, SFPTPD_STATS_TYPE_COUNT, "out-of-order-follow-ups"},
	{PTP_STATS_ID_DELAY_REQ_TXED,          SFPTPD_STATS_TYPE_COUNT, "delay-req-pkts-txed"},
	{PTP_STATS_ID_DELAY_REQ_RXED,          SFPTPD_STATS_TYPE_COUNT, "delay-req-pkts-rxed"},
	{PTP_STATS_ID_DELAY_RESP_TXED,         SFPTPD_STATS_TYPE_COUNT, "delay-resp-pkts-txed"},
	{PTP_STATS_ID_DELAY_RESP_RXED,         SFPTPD_STATS_TYPE_COUNT, "delay-resp-pkts-rxed"},
	{PTP_STATS_ID_DELAY_RESP_TIMEOUTS,     SFPTPD_STATS_TYPE_COUNT, "delay-resp-timeouts"},
	{PTP_STATS_ID_DELAY_MODE_MISMATCH,     SFPTPD_STATS_TYPE_COUNT, "delay-mode-mismatch-errors"},
	{PTP_STATS_ID_CLOCK_STEPS,             SFPTPD_STATS_TYPE_COUNT, "clock-steps"},
	{PTP_STATS_ID_OUTLIERS,                SFPTPD_STATS_TYPE_COUNT, "adaptive-outlier-filter-discards"},
	{PTP_STATS_ID_TX_PKT_NO_TIMESTAMP,     SFPTPD_STATS_TYPE_COUNT, "tx-pkt-no-timestamp"},
	{PTP_STATS_ID_RX_PKT_NO_TIMESTAMP,     SFPTPD_STATS_TYPE_COUNT, "rx-pkt-no-timestamp"},
	{PTP_STATS_ID_NUM_PTP_NODES,           SFPTPD_STATS_TYPE_RANGE, "num-ptp-nodes", NULL, 0}
};

/* Note that this table must be in order of increasing values for the translations
 * functions to operate correctly. */
static const struct sfptpd_ptp_accuracy_map ptp_accuracy_map[] =
{
	{PTPD_ACCURACY_WITHIN_25NS, 2.5e1},
	{PTPD_ACCURACY_WITHIN_100NS, 1.0e2},
	{PTPD_ACCURACY_WITHIN_250NS, 2.5e2},
	{PTPD_ACCURACY_WITHIN_1US, 1.0e3},
	{PTPD_ACCURACY_WITHIN_2US5, 2.5e3},
	{PTPD_ACCURACY_WITHIN_10US, 1.0e4},
	{PTPD_ACCURACY_WITHIN_25US, 2.5e4},
	{PTPD_ACCURACY_WITHIN_100US, 1.0e5},
	{PTPD_ACCURACY_WITHIN_250US, 2.5e5},
	{PTPD_ACCURACY_WITHIN_1MS, 1.0e6},
	{PTPD_ACCURACY_WITHIN_2MS5, 2.5e6},
	{PTPD_ACCURACY_WITHIN_10MS, 1.0e7},
	{PTPD_ACCURACY_WITHIN_25MS, 2.5e7},
	{PTPD_ACCURACY_WITHIN_100MS, 1.0e8},
	{PTPD_ACCURACY_WITHIN_250MS, 2.5e8},
	{PTPD_ACCURACY_WITHIN_1S, 1.0e9},
	{PTPD_ACCURACY_WITHIN_10S, 1.0e10},
	{PTPD_ACCURACY_UNKNOWN, INFINITY},
	/* Note that the 'more than 10s' enum goes last so that we will
	 * prefer 'unknown'. This enum value is moronic. */
	{PTPD_ACCURACY_MORE_THAN_10S, INFINITY}
};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/

static void ptp_send_instance_rt_stats_update(struct sfptpd_engine *engine,
											  struct sfptpd_ptp_instance *instance,
											  struct sfptpd_log_time time);

static void ptp_send_rt_stats_update(struct sfptpd_ptp_module *ptp,
									 struct sfptpd_log_time time);

/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static const char *ts_name(ptpd_timestamp_type_e type)
{
	switch (type) {
	case PTPD_TIMESTAMP_TYPE_AUTO:
		return "auto";
	case PTPD_TIMESTAMP_TYPE_SW:
		return "sw";
	case PTPD_TIMESTAMP_TYPE_HW_RAW:
		return "hw";
	default:
		return "invalid";
	}
}


static sfptpd_sync_module_state_t ptp_translate_state(ptpd_state_e ptpd_port_state)
{
	sfptpd_sync_module_state_t sfptpd_port_state;
	switch(ptpd_port_state) {
	case PTPD_INITIALIZING: sfptpd_port_state = SYNC_MODULE_STATE_LISTENING; break;
	case PTPD_FAULTY: sfptpd_port_state = SYNC_MODULE_STATE_FAULTY; break;
	case PTPD_DISABLED: sfptpd_port_state = SYNC_MODULE_STATE_DISABLED; break;
	case PTPD_LISTENING: sfptpd_port_state = SYNC_MODULE_STATE_LISTENING; break;
	case PTPD_MASTER: sfptpd_port_state = SYNC_MODULE_STATE_MASTER; break;
	case PTPD_PASSIVE: sfptpd_port_state = SYNC_MODULE_STATE_PASSIVE; break;
	case PTPD_SLAVE: sfptpd_port_state = SYNC_MODULE_STATE_SLAVE; break;

	/* We don't expect these - treat the same as the default case */
	case PTPD_PRE_MASTER:
	case PTPD_UNCALIBRATED:
	default:
		sfptpd_port_state = SYNC_MODULE_STATE_FAULTY;
		break;
	}

	return sfptpd_port_state;
}


const char *ptp_state_text(ptpd_state_e state, unsigned int alarms)
{
	static const char *states_text[] = {
		"ptp-faulty",		/* PTPD_UNINITIALIZED */
		"ptp-listening",	/* PTPD_INITIALIZING */
		"ptp-faulty",		/* PTPD_FAULTY */
		"ptp-disabled",		/* PTPD_DISABLED */
		"ptp-listening",	/* PTPD_LISTENING*/
		"ptp-faulty",		/* PTPD_PRE_MASTER */
		"ptp-master",		/* PTPD_MASTER */
		"ptp-passive",		/* PTPD_PASSIVE */
		"ptp-faulty",		/* PTPD_UNCALIBRATED */
		"ptp-slave",		/* PTPD_SLAVE */
	};

	assert(state <= PTPD_SLAVE);

	if ((state == PTPD_SLAVE) && (alarms != 0))
		return "ptp-slave-alarm";

	return states_text[state];
}


static uint8_t ptp_translate_clock_class_to_ieee1588(enum sfptpd_clock_class clock_class)
{
	switch (clock_class) {
	case SFPTPD_CLOCK_CLASS_LOCKED:
		return 6;
	case SFPTPD_CLOCK_CLASS_HOLDOVER:
		return 7;
	case SFPTPD_CLOCK_CLASS_FREERUNNING:
		return 248;
	default:
		break;
	}

	return 255;
}


static enum sfptpd_clock_class ptp_translate_clock_class_from_ieee1588(uint8_t clock_class)
{
	switch (clock_class) {
	case 6:
	case 13:
		return SFPTPD_CLOCK_CLASS_LOCKED;
	case 7:
	case 14:
		return SFPTPD_CLOCK_CLASS_HOLDOVER;
	case 52:
	case 58:
	case 187:
	case 193:
	case 248:
	case 255:
		return SFPTPD_CLOCK_CLASS_FREERUNNING;
	default:
		break;
	}
 
	return SFPTPD_CLOCK_CLASS_UNKNOWN;
}


static ptpd_clock_accuracy_e ptp_translate_accuracy_to_enum(long double accuracy_ns)
{
	unsigned int i;
	
	for (i = 0; i < sizeof(ptp_accuracy_map)/sizeof(ptp_accuracy_map[0]); i++) {
		if ((accuracy_ns >= -ptp_accuracy_map[i].float_ns) &&
		    (accuracy_ns <= ptp_accuracy_map[i].float_ns))
			return ptp_accuracy_map[i].enumeration;
	}

	return PTPD_ACCURACY_UNKNOWN;
}


static long double ptp_translate_accuracy_to_float(ptpd_clock_accuracy_e accuracy_enum)
{
	unsigned int i;
	
	for (i = 0; i < sizeof(ptp_accuracy_map)/sizeof(ptp_accuracy_map[0]); i++) {
		if (accuracy_enum == ptp_accuracy_map[i].enumeration)
			return ptp_accuracy_map[i].float_ns;
	}

	return INFINITY;
}


static uint16_t ptp_translate_allan_variance_to_ieee1588(long double variance)
{
	/* Refer to section to 7.6.3.3 of the IEEE1588 spec for information on
	 * the format used by the PTP protocol. */
	long double log_variance = log2l(variance);
	return (int32_t)lroundl(log_variance * 256.0) + 0x8000;
}


static long double ptp_translate_allan_variance_from_ieee1588(uint16_t variance)
{
	/* Refer to section to 7.6.3.3 of the IEEE1588 spec for information on
	 * the format used by the PTP protocol. */
	int32_t scaled_log_variance = (int32_t)variance - 0x8000;
	return powl(2.0L, ((long double)scaled_log_variance) / 256.0L);
}


static void ptp_translate_master_characteristics(struct sfptpd_ptp_instance *instance,
						 sfptpd_sync_instance_status_t *status)
{
	uint16_t variance = instance->ptpd_port_snapshot.parent.grandmaster_offset_scaled_log_variance;
	uint8_t clock_class = instance->ptpd_port_snapshot.parent.grandmaster_clock_class;
	ptpd_clock_accuracy_e accuracy = instance->ptpd_port_snapshot.parent.grandmaster_clock_accuracy;

	memcpy(status->master.clock_id.id,
	       instance->ptpd_port_snapshot.parent.grandmaster_id,
	       sizeof status->master.clock_id.id);
	status->master.remote_clock = (status->state == SYNC_MODULE_STATE_SLAVE);
	status->master.clock_class = ptp_translate_clock_class_from_ieee1588(clock_class);
	status->master.time_source = instance->ptpd_port_snapshot.parent.grandmaster_time_source;
	status->master.accuracy = ptp_translate_accuracy_to_float(accuracy);
	status->master.allan_variance = status->master.remote_clock ? ptp_translate_allan_variance_from_ieee1588(variance) : NAN;
	status->master.time_traceable = instance->ptpd_port_snapshot.time.time_traceable;
	status->master.freq_traceable = instance->ptpd_port_snapshot.time.freq_traceable;
	status->master.steps_removed = instance->ptpd_port_snapshot.current.steps_removed;
}


static void ptp_configure_ptpd(struct sfptpd_ptp_module_config *config)
{
	assert(config != NULL);

	/* Set the component trace level for internal PTPD2 */
	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_PTPD2, config->trace_level);

	/* Set up the clock control options based on the top level config */
	config->ptpd_port.clock_ctrl
		= sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(config))->clocks.control;
}


static void ptp_convergence_init(sfptpd_ptp_instance_t *instance)
{
	assert(instance != NULL);

	/* Initialise the convergence measure. */
	instance->synchronized = false;
	sfptpd_stats_convergence_init(&instance->convergence);
}

static unsigned int ptp_get_alarms_snapshot(sfptpd_ptp_instance_t *instance)
{
	assert(instance != NULL);

	return instance->local_alarms_snapshot | instance->ptpd_port_snapshot.port.alarms;
}

static void ptp_convergence_update(sfptpd_ptp_instance_t *instance)
{
	sfptpd_time_t ofm_ns;
	struct timespec time;
	int rc;
	assert(instance != NULL);

	rc = clock_gettime(CLOCK_MONOTONIC, &time);
	if (rc < 0) {
		ERROR("ptp %s: failed to get monotonic time, %s\n",
		       SFPTPD_CONFIG_GET_NAME(instance->config), strerror(errno));
	}

	/* If not in the slave state or we failed to get the time for some
	 * reason, reset the convergence measure. */
	if ((rc < 0) || (instance->ptpd_port_snapshot.port.state != PTPD_SLAVE)) {
		instance->synchronized = false;
		sfptpd_stats_convergence_reset(&instance->convergence);
	} else if (ptp_get_alarms_snapshot(instance) != 0 ||
		   ((instance->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* If one or more alarms is triggered or timestamp processing
		 * is disabled, we consider the slave to be unsynchronized.
		 * However, don't reset the convergence measure as it is
		 * probably a temporary situation. */
		instance->synchronized = false;
	} else {
		/* Update the synchronized state based on the current offset
		 * from master */
		ofm_ns = instance->ptpd_port_snapshot.current.offset_from_master;

		instance->synchronized = sfptpd_stats_convergence_update(&instance->convergence,
									 time.tv_sec, ofm_ns);
	}
}


static struct sfptpd_ptp_bond_info *ptp_get_bond_info(sfptpd_ptp_instance_t *instance) {

	/* There is only one for now */
	assert(instance != NULL);
	assert(instance->intf != NULL);

	return &instance->intf->bond_info;
}


static int ptp_stats_init(sfptpd_ptp_instance_t *instance)
{
	int rc;
	struct sfptpd_ptp_bond_info *bond_info = ptp_get_bond_info(instance);
	assert(instance != NULL);

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&instance->stats, "ptp",
					    sizeof(ptp_stats_defns)/sizeof(ptp_stats_defns[0]),
					    ptp_stats_defns);

	/* If PPS logging is enabled, add PPS offset and period */
	if ((rc == 0) && instance->config->pps_logging &&
	    sfptpd_interface_supports_pps(bond_info->active_if)) {
		rc = sfptpd_stats_collection_add(&instance->stats,
						 PTP_STATS_ID_PPS_OFFSET,
						 SFPTPD_STATS_TYPE_RANGE,
						 "pps-offset", "ns", 0);
		if (rc == 0)
			rc = sfptpd_stats_collection_add(&instance->stats,
							 PTP_STATS_ID_PPS_PERIOD,
							 SFPTPD_STATS_TYPE_RANGE,
							 "pps-period", "ns", 0);
	}

	return rc;
}


static void ptp_publish_mtie_window(sfptpd_ptp_instance_t *instance)
{
	int qualified;
	long double mean;
	long double min;
	long double max;
	struct timespec min_time = { .tv_sec = 0, .tv_nsec = 0};
	struct timespec max_time = { .tv_sec = 0, .tv_nsec = 0};
	struct sfptpd_stats_time_interval interval;
	int rc;

	rc = sfptpd_stats_collection_get_range(&instance->stats,
					       PTP_STATS_ID_OFFSET,
					       SFPTPD_STATS_PERIOD_MINUTE,
					       SFPTPD_STATS_HISTORY_1,
					       &mean, &min, &max,
					       &qualified, &min_time, &max_time);
	if (rc != 0)
		goto error;

	rc = sfptpd_stats_collection_get_interval(&instance->stats,
						  SFPTPD_STATS_PERIOD_MINUTE,
						  SFPTPD_STATS_HISTORY_1,
						  &interval);
	if (rc != 0)
		goto error;

	ptpd_publish_mtie_window(instance->ptpd_port_private,
				 (bool) qualified, interval.seq_num, 60, min, max, &min_time, &max_time);
	return;

 error:
	assert(rc == ENOENT);
	ptpd_publish_mtie_window(instance->ptpd_port_private,
				 false, 0, 0, 0.0, 0.0, &min_time, &max_time);
}


static void ptp_critical_stats_update(struct ptpd_critical_stats_logger *logger,
				      const struct ptpd_critical_stats critical_stats) {
	sfptpd_ptp_instance_t *instance = (sfptpd_ptp_instance_t *) logger->private;
	struct sfptpd_stats_collection *stats;

	assert(instance != NULL);

	stats = &instance->stats;

	/* Offset, frequency correction, one-way-delay */
	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_OFFSET, critical_stats.ofm_ns,
					     critical_stats.sync_time, critical_stats.valid);
	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_FREQ_ADJ, critical_stats.freq_adj,
					     critical_stats.sync_time, critical_stats.valid);
	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_ONE_WAY_DELAY, critical_stats.owd_ns,
					     critical_stats.sync_time, critical_stats.valid);
}


static void ptp_stats_update(sfptpd_ptp_instance_t *instance)
{
	struct ptpd_port_snapshot *ptpd_port_snapshot;
	struct sfptpd_stats_collection *stats;
	struct ptpd_counters ptpd_counters;
	struct sfptpd_ptp_bond_info *bond_info = ptp_get_bond_info(instance);
	struct timespec sync_time;
	ptpd_state_e port_state;
	int rc;

	assert(instance != NULL);

	ptpd_port_snapshot = &instance->ptpd_port_snapshot;
	stats = &instance->stats;
	
	sync_time = ptpd_port_snapshot->current.last_offset_time;
	port_state = ptpd_port_snapshot->port.state;

	if (port_state != PTPD_SLAVE) {
		struct timespec sync_time;

		/* Record unqualified samples if not in slave state by polling.
		   Qualified samples are recorded by the servo when available. */
		sync_time = ptpd_port_snapshot->current.last_offset_time;
		sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_OFFSET, NAN, sync_time, false);
		sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_ONE_WAY_DELAY, NAN, sync_time, false);
	}

	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_FREQ_ADJ, ptpd_port_snapshot->current.frequency_adjustment, sync_time, true);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_SYNCHRONIZED, instance->synchronized? 1: 0);

	/* Get the latest statistics from ptpd and update the historical stats measures */
	rc = ptpd_get_counters(instance->ptpd_port_private, &ptpd_counters);
	if (rc != 0) {
		ERROR("ptp %s: couldn't get statistics from ptpd, %s\n",
		       SFPTPD_CONFIG_GET_NAME(instance->config), strerror(rc));
		return;
	}
	
	/* Packet counts */
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_ANNOUNCE_TXED, ptpd_counters.announceMessagesSent);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_ANNOUNCE_RXED, ptpd_counters.announceMessagesReceived);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_ANNOUNCE_TIMEOUTS, ptpd_counters.announceTimeouts);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_SYNC_PKT_TXED, ptpd_counters.syncMessagesSent);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_SYNC_PKT_RXED, ptpd_counters.syncMessagesReceived);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_SYNC_PKT_TIMEOUTS, ptpd_counters.syncTimeouts);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_FOLLOW_UP_TXED, ptpd_counters.followUpMessagesSent);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_FOLLOW_UP_RXED, ptpd_counters.followUpMessagesReceived);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_FOLLOW_UP_TIMEOUTS, ptpd_counters.followUpTimeouts);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_OUT_OF_ORDER_FOLLOW_UPS, ptpd_counters.outOfOrderFollowUps);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_REQ_TXED, ptpd_counters.delayReqMessagesSent);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_REQ_RXED, ptpd_counters.delayReqMessagesReceived);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_RESP_TXED, ptpd_counters.delayRespMessagesSent);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_RESP_RXED, ptpd_counters.delayRespMessagesReceived);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_RESP_TIMEOUTS, ptpd_counters.delayRespTimeouts);

	/* Miscellaneous */
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_DELAY_MODE_MISMATCH, ptpd_counters.delayModeMismatchErrors);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_CLOCK_STEPS, ptpd_counters.clockSteps);
	sfptpd_stats_collection_update_count_samples(stats, PTP_STATS_ID_OUTLIERS, ptpd_counters.outliers, ptpd_counters.outliersNumSamples);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_TX_PKT_NO_TIMESTAMP, ptpd_counters.txPktNoTimestamp);
	sfptpd_stats_collection_update_count(stats, PTP_STATS_ID_RX_PKT_NO_TIMESTAMP, ptpd_counters.rxPktNoTimestamp);
	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_OUTLIER_THRESHOLD, ptpd_port_snapshot->current.servo_outlier_threshold, sync_time, true);
	sfptpd_stats_collection_update_range(stats, PTP_STATS_ID_NUM_PTP_NODES, sfptpd_ht_get_num_entries(instance->ptpd_port_private->interface->nodeSet),
					     sync_time, port_state != PTPD_INITIALIZING && port_state >= PTPD_LISTENING);

	/* If PPS logging enable and we are using a PTP interface, update the
	 * PPS offset statistic */
	if (instance->config->pps_logging &&
	    sfptpd_interface_supports_pps(bond_info->active_if)) {
		sfptpd_stats_pps_t pps_stats;
		long double offset;
		struct sfptpd_interface *primary_intf;

		primary_intf = sfptpd_clock_get_primary_interface(instance->intf->clock);
		if (primary_intf != NULL) {
			rc = sfptpd_stats_get_pps_statistics(primary_intf, &pps_stats);
		} else {
			rc = ENOENT;
		}

		if (rc != 0) {
			WARNING("ptp %s: couldn't read PPS statistics, %s\n",
				SFPTPD_CONFIG_GET_NAME(instance->config), strerror(rc));
		} else {
			bool qualified = pps_stats.period.last > 0;
			/* Adjust the offset to take account of the PPS propagation delay */
			offset = pps_stats.offset.last - instance->pps_delay;
			sfptpd_stats_collection_update_range(stats,
							     PTP_STATS_ID_PPS_OFFSET,
							     offset,
							     sync_time, qualified);
			sfptpd_stats_collection_update_range(stats,
							     PTP_STATS_ID_PPS_PERIOD,
							     pps_stats.period.last,
							     sync_time, qualified);
		}
	}

	/* Clear the ptpd counters */
	ptpd_clear_counters(instance->ptpd_port_private);

	/* Save the MTIE window, i.e. the last complete minute of min/max stats */
	ptp_publish_mtie_window(instance);
}


/* Return a configuration object suitable for accessing interface-level options */
static sfptpd_ptp_module_config_t *ptp_get_config_for_interface(struct sfptpd_ptp_intf *intf)
{
	assert(intf);
	assert(intf->representative_config);

	return intf->representative_config;
}


static bool ptp_is_ptpd_interface_hw_timestamping(struct sfptpd_ptp_intf *intf)
{
	ptpd_timestamp_type_e mode;

	if (intf->ptpd_intf_private == NULL) {
		/* At startup use configuration value */
		mode = intf->representative_config->ptpd_intf.timestampType;
	} else {
		mode = ptpd_get_timestamping(intf->ptpd_intf_private);
	}

	return mode != PTPD_TIMESTAMP_TYPE_SW;
}


static bool ptp_is_instance_hw_timestamping(struct sfptpd_ptp_instance *instance)
{
	return ptp_is_ptpd_interface_hw_timestamping(instance->intf);
}


static bool ptp_is_interface_hw_timestamping(struct sfptpd_ptp_intf *logical,
					     struct sfptpd_interface *physical,
					     ptpd_timestamp_type_e logical_ts)
{
	bool logical_is_hw = (logical_ts != PTPD_TIMESTAMP_TYPE_SW);

	if (logical && logical_ts == PTPD_TIMESTAMP_TYPE_AUTO)
		logical_is_hw = ptp_is_ptpd_interface_hw_timestamping(logical);
	return ((!logical || logical_is_hw) &&
	        (!physical || sfptpd_interface_supports_ptp(physical)));
}


/**
 * Return the local accuracy of the PTP instance based on the characteristics
 * of the underlying interface.
 */
static long double ptp_get_instance_accuracy(struct sfptpd_ptp_instance *instance)
{
	long double rc;
	sfptpd_interface_ts_caps_t caps;
	struct sfptpd_ptp_bond_info *bond_info = ptp_get_bond_info(instance);

	caps = sfptpd_interface_ptp_caps(bond_info->active_if);
	if (caps & SFPTPD_INTERFACE_TS_CAPS_HW &&
	    ptp_is_instance_hw_timestamping(instance)) {
		rc = SFPTPD_ACCURACY_PTP_HW;
	} else {
		/* Could test for CAPS_SW too but what sort of system would it be without that? */
		rc = SFPTPD_ACCURACY_PTP_SW;
	}

	return rc;
}


static void ptp_timestamp_filtering_deconfigure_one(struct sfptpd_interface *interface)
{
	assert(interface != NULL);

	TRACE_L3("ptp: deconfiguring timestamp filtering on interface %s\n",
		 sfptpd_interface_get_name(interface));

	if (sfptpd_interface_supports_ptp(interface)) {
		sfptpd_interface_hw_timestamping_disable(interface);

		/* If the interface is a Solarflare Siena based PTP adapter,
		 * clear the PTP receive filters */
		if (sfptpd_interface_is_siena(interface)) {
			(void)sfptpd_interface_ptp_set_vlan_filter(interface, 0, NULL);
			(void)sfptpd_interface_ptp_set_domain_filter(interface, false, 0);
			(void)sfptpd_interface_ptp_set_uuid_filter(interface, false, NULL);
		}
	}
}


static int ptp_timestamp_filtering_configure_one(struct sfptpd_ptp_intf *intf,
						 struct sfptpd_interface *interface,
						 bool enable_uuid_filtering,
						 bool already_checked)
{
	int rc;
	sfptpd_ptp_module_config_t *config;
	struct sfptpd_ptp_bond_info *bond_info;
	struct sfptpd_ptp_instance *instance;
	uint8_t *uuid;

	assert(interface != NULL);
	assert(intf != NULL);

	TRACE_L3("ptp: configuring timestamp filtering on interface %s:%s\n",
		 intf->defined_name,
		 sfptpd_interface_get_name(interface));

	bond_info = &intf->bond_info;
	config = ptp_get_config_for_interface(intf);

	/* If we are not using hardware timestamping for this logical PTP
	 * interface or the physical interface does not support timestamping
         * then do not configure it. */
	if (!ptp_is_interface_hw_timestamping(intf, interface, PTPD_TIMESTAMP_TYPE_AUTO) && !already_checked)
		return 0;

	/* If the interface is a Solarflare Siena based PTP adapter, i.e. it
	 * doesn't support general hw timestamping, configure PTP filters. */
	if (sfptpd_interface_is_siena(interface)) {
		/* We have to disable timestamping before configuring the
		 * receive filtering */
		sfptpd_interface_hw_timestamping_disable(interface);

		/* Set the VLAN tag information */
		rc = sfptpd_interface_ptp_set_vlan_filter(interface,
							  bond_info->num_vlan_tags,
							  bond_info->vlan_tags);
		if (rc != 0)
			goto fail;

		/* If configured to do so, filter by PTP Domain Number */
		rc = sfptpd_interface_ptp_set_domain_filter(interface,
							    config->domain_filtering,
							    config->ptpd_port.domainNumber);
		if (rc != 0)
			goto fail;

		/* In order to correctly set up UUID filtering, we need to
		 * refer to the instance using this interface (in the case of
		 * Siena we enforce that there can only be one). We enable UUID
		 * if requested to do so and UUID filtering is configured and
		 * the PTP port is in slave state. */
		instance = intf->instance_list;
		if (enable_uuid_filtering && config->uuid_filtering &&
		    (instance != NULL) &&
		    (instance->ptpd_port_snapshot.port.state == PTPD_SLAVE)) {
			uuid = instance->ptpd_port_snapshot.parent.clock_id;
		} else {
			enable_uuid_filtering = false;
			uuid = NULL;
		}

		rc = sfptpd_interface_ptp_set_uuid_filter(interface,
							  enable_uuid_filtering,
							  uuid);
		if (rc != 0)
			goto fail;
	}

	/* Enable timestamping on this interface */
	rc = sfptpd_interface_hw_timestamping_enable(interface);
	if (rc != 0)
		goto fail;

	return 0;

fail:
	ptp_timestamp_filtering_deconfigure_one(interface);
	return rc;
}


static void ptp_timestamp_filtering_deconfigure_all(struct sfptpd_ptp_intf *intf)
{
	unsigned int i;
	struct sfptpd_config_general *general_cfg;
	struct sfptpd_ptp_bond_info *bond_info;
	sfptpd_ptp_module_config_t *config;

	assert(intf != NULL);
	bond_info = &intf->bond_info;
	config = ptp_get_config_for_interface(intf);

	general_cfg = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(config));
	if (!general_cfg->timestamping.disable_on_exit)
		return;

	if (!ptp_is_interface_hw_timestamping(intf, NULL, PTPD_TIMESTAMP_TYPE_AUTO))
		return;

	TRACE_L3("ptp: deconfiguring timestamp filtering on %s:*\n",
		 intf->defined_name);

	for (i = 0; i < bond_info->num_physical_ifs; i++) {
		ptp_timestamp_filtering_deconfigure_one(bond_info->physical_ifs[i]);
	}
}


static int ptp_timestamp_filtering_configure_all(struct sfptpd_ptp_intf *intf)
{
	int rc, error;
	unsigned int i;
	struct sfptpd_ptp_bond_info *bond_info;

	assert(intf != NULL);
	bond_info = &intf->bond_info;

	TRACE_L3("ptp: configuring timestamp filtering on %s:*\n",
		 intf->defined_name);

	error = 0;
	for (i = 0; i < bond_info->num_physical_ifs; i++) {
		rc = ptp_timestamp_filtering_configure_one(intf, bond_info->physical_ifs[i],
							   false, false);
		if (rc != 0)
			error = rc;
	}

	return error;
}


static void ptp_timestamp_filtering_reconfigure_all(struct sfptpd_ptp_intf *intf,
						    struct sfptpd_ptp_bond_info *new_bond_info,
						    ptpd_timestamp_type_e new_active_intf_mode)
{
	unsigned int i, j;
	struct sfptpd_interface *candidate;
	const struct sfptpd_ptp_bond_info *old_bond_info;

	assert(intf != NULL);
	assert(new_bond_info != NULL);

	TRACE_L3("ptp: reconfiguring timestamp filtering on %s:*\n",
		 intf->defined_name);

	old_bond_info = &intf->bond_info;

	/* Disable timestamping on any interfaces that have been removed from
	 * the bond. */
	for (i = 0; i < old_bond_info->num_physical_ifs; i++) {
		candidate = old_bond_info->physical_ifs[i];

		for (j = 0; j < new_bond_info->num_physical_ifs; j++) {
			if (candidate == new_bond_info->physical_ifs[j])
				break;
		}

		/* If the interface from the old config is not in the new
		 * configuration, then disable timestamping for this
		 * interface. */
		if (j >= new_bond_info->num_physical_ifs) {
			INFO("ptp %s: interface %s removed from bond\n",
			     old_bond_info->bond_if,
			     sfptpd_interface_get_name(candidate));

			if (ptp_is_interface_hw_timestamping(intf, candidate, PTPD_TIMESTAMP_TYPE_AUTO))
				ptp_timestamp_filtering_deconfigure_one(candidate);
		}
	}

	/* Enable timestamping on any interfaces that have been added to the
	 * bond. */
	for (i = 0; i < new_bond_info->num_physical_ifs; i++) {
		candidate = new_bond_info->physical_ifs[i];

		for (j = 0; j < old_bond_info->num_physical_ifs; j++) {
			if (candidate == old_bond_info->physical_ifs[j])
				break;
		}
		/* If the interface from the new config is not in the existing
		 * configuration, then report this for symmetry when reading logs */
		if (j >= old_bond_info->num_physical_ifs) {
			INFO("ptp %s: interface %s added to bond\n",
			     old_bond_info->bond_if,
			     sfptpd_interface_get_name(candidate));
		}

		/* Enable timestamping on all interfaces to avoid races.
		   The interface module knows if they're already enabled. */
		if (candidate == new_bond_info->active_if) {
			if (ptp_is_interface_hw_timestamping(intf, candidate,
							     new_active_intf_mode))
				(void)ptp_timestamp_filtering_configure_one(intf, candidate, true, true);
		} else {
			if (ptp_is_interface_hw_timestamping(intf, candidate,
							     PTPD_TIMESTAMP_TYPE_AUTO))
				(void)ptp_timestamp_filtering_configure_one(intf, candidate, true, true);
		}

	}
}


static void ptp_timestamp_filtering_set_uuid(struct sfptpd_ptp_intf *intf,
					     struct sfptpd_ptp_instance *instance)
{
	unsigned int i;
	struct sfptpd_interface *interface;
	struct sfptpd_ptp_bond_info *bond_info;
	sfptpd_ptp_module_config_t *config;
	bool enable;

	assert(intf != NULL);
	assert(instance != NULL);
	bond_info = &intf->bond_info;
	config = ptp_get_config_for_interface(intf);
	assert(config != NULL);

	/* If we are in PTP slave state, enable UUID filtering. Otherwise,
	 * disable it */
	enable = (instance->ptpd_port_snapshot.port.state == PTPD_SLAVE);

	for (i = 0; i < bond_info->num_physical_ifs; i++) {
		interface = bond_info->physical_ifs[i];

		/* If we are using UUID filtering and this is a Siena based
		 * PTP adapters, configure the filter. */
		if (ptp_is_interface_hw_timestamping(intf, interface, PTPD_TIMESTAMP_TYPE_AUTO) &&
		    config->uuid_filtering &&
		    sfptpd_interface_is_siena(interface)) {
			sfptpd_interface_hw_timestamping_disable(interface);

			(void)sfptpd_interface_ptp_set_uuid_filter
				(interface, enable,
				 instance->ptpd_port_snapshot.parent.clock_id);

			(void)sfptpd_interface_hw_timestamping_enable(interface);
		}
	}
}


static void ptp_pps_stats_init(sfptpd_ptp_instance_t *instance)
{
	struct sfptpd_interface *interface, *primary;

	assert(instance != NULL);

	/* To reset the PPS stats we need to be using a PTP capable NIC */
	interface = instance->intf->bond_info.active_if;
	if (!sfptpd_interface_supports_pps(interface))
		return;

	/* Record the PPS propagation delay for use when logging stats */
	instance->pps_delay
		= sfptpd_pps_module_config_get_propagation_delay(SFPTPD_CONFIG_TOP_LEVEL(instance->config),
								 sfptpd_interface_get_clock(interface));

	/* Reset the statistics using the primary interface for the adapter we
	 * are using. */
	primary = sfptpd_clock_get_primary_interface(instance->intf->clock);
	sfptpd_stats_reset_pps_statistics(primary);
}


static void ptp_log_pps_stats(struct sfptpd_engine *engine,
							  sfptpd_ptp_instance_t *instance,
							  sfptpd_stats_pps_t *pps_stats_out)
{
	int rc;
	long double value;
	struct sfptpd_interface *primary_intf;

	/* To log PPS stats we need to be using a PTP capable NIC */
	if (!sfptpd_interface_supports_pps(instance->intf->bond_info.active_if))
		return;

	primary_intf = sfptpd_clock_get_primary_interface(instance->intf->clock);
	if (primary_intf != NULL) {
		rc = sfptpd_stats_get_pps_statistics(primary_intf, pps_stats_out);
	} else {
		rc = ENOENT;
	}

	if (rc != 0) {
		WARNING("ptp %s: couldn't read PPS statistics, %s\n",
			SFPTPD_CONFIG_GET_NAME(instance->config), strerror(rc));
		return;
	}

	/* Adjust the stats based on the PPS propagation delay */
	value = (long double)pps_stats_out->offset.last - instance->pps_delay;
	if (value > (long double)INT_MAX) value = (long double)INT_MAX;
	if (value < (long double)INT_MIN) value = (long double)INT_MIN;
	pps_stats_out->offset.last = (int)value;
	value = (long double)pps_stats_out->offset.mean - instance->pps_delay;
	if (value > (long double)INT_MAX) value = (long double)INT_MAX;
	if (value < (long double)INT_MIN) value = (long double)INT_MIN;
	pps_stats_out->offset.mean = (int)value;
	value = (long double)pps_stats_out->offset.min - instance->pps_delay;
	if (value > (long double)INT_MAX) value = (long double)INT_MAX;
	if (value < (long double)INT_MIN) value = (long double)INT_MIN;
	pps_stats_out->offset.min = (int)value;
	value = (long double)pps_stats_out->offset.max - instance->pps_delay;
	if (value > (long double)INT_MAX) value = (long double)INT_MAX;
	if (value < (long double)INT_MIN) value = (long double)INT_MIN;
	pps_stats_out->offset.max = (int)value;
}


static int ptp_is_interface_vlan(const struct sfptpd_link *logical_if, bool *is_vlan,
				 const struct sfptpd_link **physical_if, uint16_t *vlan_tag,
				 const struct sfptpd_link_table *link_table)
{
	bool have_physical_if, have_vlan_tag;

	assert(logical_if != NULL);
	assert(is_vlan != NULL);
	assert(physical_if != NULL);
	assert(vlan_tag != NULL);

	if (logical_if->type != SFPTPD_LINK_VLAN) {
		TRACE_L1("ptp: interface %s is not a VLAN\n", logical_if->if_name);
		*is_vlan = false;
		return 0;
	}

	have_physical_if = false;
	have_vlan_tag = false;
	if (logical_if->vlan_id != 0) {
		*vlan_tag = logical_if->vlan_id;
		have_vlan_tag = true;
	}
	if (logical_if->if_link > 0) {
		const struct sfptpd_link *physical_link;

		physical_link = sfptpd_link_by_if_index(link_table, logical_if->if_link);
		if (physical_link != NULL) {
			*physical_if = physical_link;
			have_physical_if = true;
		}
	}

	if (have_vlan_tag & have_physical_if) {
		TRACE_L1("ptp: interface %s is a VLAN. underlying if %s, vid %d\n",
			  logical_if->if_name, (*physical_if)->if_name, *vlan_tag);
		*is_vlan = true;
		return 0;
	}

	if (!have_vlan_tag) 
		ERROR("ptp: couldn't find vlan tag for %s\n", logical_if->if_name);
	if (!have_physical_if) 
		ERROR("ptp: couldn't find physical link for %s\n", logical_if->if_name);
	
	return EINVAL;
}


/* Wrapper for strcmp so that it can be used with qsort */
int qsort_intfnamecmp(const void *p1, const void *p2)
{
	return strcmp(sfptpd_interface_get_name(*(struct sfptpd_interface * const *) p1),
		      sfptpd_interface_get_name(*(struct sfptpd_interface * const *) p2));
}


/* Parse a bond within a bridge */
static int parse_nested_bond(struct sfptpd_ptp_bond_info *bond_info, bool verbose,
			     const struct sfptpd_link_table *link_table,
			     const struct sfptpd_link *logical_link)
{
	int rc;
	struct sfptpd_interface *interface;
	int row;
	int first_phy = bond_info->num_physical_ifs;

	rc = 0;

	/* Even though the bridge is indeterminate, try to identify the active
	 * slave because if it includes an active-backup bond it is likely that
	 * this will be the only locus of PTP traffic. */
	if (logical_link->bond.bond_mode == SFPTPD_BOND_MODE_ACTIVE_BACKUP &&
	    logical_link->bond.active_slave > 0 &&
	    bond_info->active_if == NULL) {
		bond_info->active_if = sfptpd_interface_find_by_if_index(logical_link->bond.active_slave);
		if (verbose)
			TRACE_L3("%s: active nested slave %s\n",
				 bond_info->bond_if, sfptpd_interface_get_name(bond_info->active_if));
	}

	for (row = 0; row < link_table->count &&
		      (bond_info->num_physical_ifs <
		       sizeof(bond_info->physical_ifs)/sizeof(bond_info->physical_ifs[0]))
	     ; row++) {
		const struct sfptpd_link *link = link_table->rows + row;

		if (link->is_slave && link->bond.if_master == logical_link->if_index) {
			if (verbose)
				TRACE_L3("ptp %s: nested slave interface %s\n",
					 bond_info->bond_if, link->if_name);
			interface = sfptpd_interface_find_by_if_index(link->if_index);
			if (interface == NULL) {
				ERROR("ptp: couldn't find interface object for "
				      "nested slave %s\n", link->if_name);
				/* Not fatal unless we find none overall! */
			} else {
				bond_info->physical_ifs[bond_info->num_physical_ifs] = interface;
				bond_info->num_physical_ifs++;
			}
		}
	}

	if (logical_link->type == SFPTPD_LINK_TEAM)
		qsort(bond_info->physical_ifs + first_phy,
		      bond_info->num_physical_ifs - first_phy,
		      sizeof(struct sfptpd_interface *), qsort_intfnamecmp);

	return rc;
}


static int parse_bond(struct sfptpd_ptp_bond_info *bond_info, bool verbose,
		      const struct sfptpd_link_table *link_table,
		      const struct sfptpd_link *logical_link)
{
	int rc;
	struct sfptpd_interface *interface;
	int row;

	rc = 0;

	bond_info->bond_mode = logical_link->bond.bond_mode;
	bond_info->active_if = NULL;

	if (bond_info->bond_mode == SFPTPD_BOND_MODE_ACTIVE_BACKUP &&
	    logical_link->bond.active_slave > 0) {
		bond_info->active_if = sfptpd_interface_find_by_if_index(logical_link->bond.active_slave);
		if (verbose)
			TRACE_L3("%s: active slave %s\n",
				 bond_info->bond_if, sfptpd_interface_get_name(bond_info->active_if));
	}

	for (row = 0; row < link_table->count &&
		      (bond_info->num_physical_ifs <
		       sizeof(bond_info->physical_ifs)/sizeof(bond_info->physical_ifs[0]))
	     ; row++) {
		const struct sfptpd_link *link = link_table->rows + row;

		if (link->is_slave && link->bond.if_master == logical_link->if_index) {
			if (verbose)
				TRACE_L3("ptp %s: slave interface %s\n",
					 bond_info->bond_if, link->if_name);
			interface = sfptpd_interface_find_by_if_index(link->if_index);
			if (interface == NULL) {
				if (link->type == SFPTPD_LINK_BOND ||
				    link->type == SFPTPD_LINK_TEAM) {
					TRACE_L3("%s: probing nested bond %s\n",
						 bond_info->bond_if, link->if_name);
					rc = parse_nested_bond(bond_info, verbose, link_table, link);
				} else {
					WARNING("ptp: couldn't find interface object for %s\n",
					      link->if_name);
					/* Not fatal unless we find none! */
				}
			} else {
				bond_info->physical_ifs[bond_info->num_physical_ifs] = interface;
				bond_info->num_physical_ifs++;
			}
		}
	}

	/* Check whether we support the bond mode */
	if (bond_info->bond_mode == SFPTPD_BOND_MODE_ACTIVE_BACKUP) {
		if (verbose)
			TRACE_L3("ptp %s: mode is active-backup\n",
				 bond_info->bond_if);
	} else if (bond_info->bond_mode == SFPTPD_BOND_MODE_LACP) {
		if (verbose)
			TRACE_L3("ptp %s: mode is 802.3ad (LACP)\n",
				 bond_info->bond_if);
	} else if (bond_info->is_bridge) {
		if (verbose)
			TRACE_L3("ptp %s: mode is bridge\n",
				 bond_info->bond_if);
	} else {
		ERROR("ptp %s: Found bond of unsupported type\n");
		rc = EINVAL;
	}

	return rc;
}


static int parse_team(struct sfptpd_ptp_bond_info *bond_info, bool verbose,
		      const struct sfptpd_link_table *link_table,
		      const struct sfptpd_link *logical_link)
{
	int rc;

	rc = parse_bond(bond_info, verbose, link_table, logical_link);

	/* Teamdctl reorders the interfaces it finds so need to sort them
	 * to make sure they're always stored in the same order */
	if (rc == 0)
		qsort(bond_info->physical_ifs, bond_info->num_physical_ifs, sizeof(struct sfptpd_interface *), qsort_intfnamecmp);

	return rc;
}


/* Examine a logical interface. If a bond, probe for slaves etc.
 * Returns ENOENT for no active slaves, ENODEV if the logical
 * interface doesn't exist or is invalid. */
static int ptp_probe_bonding(const struct sfptpd_link *logical_link,
			     struct sfptpd_ptp_bond_info *bond_info, bool verbose,
			     const struct sfptpd_link_table *link_table)
{
	struct sfptpd_interface *interface;
	bool link_detected, found_bond;
	unsigned int i;
	int rc;
	const char *logical_if = logical_link->if_name;

	assert(logical_link != NULL);
	assert(bond_info != NULL);

	const unsigned int max_physical_ifs =
		sizeof(bond_info->physical_ifs)/sizeof(bond_info->physical_ifs[0]);

	sfptpd_strncpy(bond_info->bond_if, logical_if, sizeof(bond_info->bond_if));
	bond_info->num_physical_ifs = 0;
	bond_info->active_if = NULL;
	bond_info->bond_mode = SFPTPD_BOND_MODE_NONE;
	bond_info->is_bridge = false;

	found_bond = true;
	rc = 0;

	if (logical_link->type == SFPTPD_LINK_BOND) {
		/* Bond created with bonding module */
		if (verbose)
			TRACE_L3("ptp %s: parsing bond config\n", logical_if);
		rc = parse_bond(bond_info, verbose, link_table, logical_link);
	} else if (logical_link->type == SFPTPD_LINK_TEAM) {
		/* Bond created with teaming module */
		if (verbose)
			TRACE_L3("ptp %s: uses teaming driver, parsing team\n",
				 bond_info->bond_if);
		rc = parse_team(bond_info, verbose, link_table, logical_link);
	} else if (logical_link->type == SFPTPD_LINK_BRIDGE) {
		/* Bridge */
		if (verbose)
			TRACE_L3("ptp %s: parsing bridge config\n", logical_if);
		bond_info->is_bridge = true;
		rc = parse_bond(bond_info, verbose, link_table, logical_link);
	} else {
		found_bond = false;
	}

	if (found_bond && bond_info->num_physical_ifs == 0) {
		ERROR("ptp %s: no slave interfaces found\n", bond_info->bond_if);
		rc = ENOENT;
	}

	if (bond_info->num_physical_ifs >= max_physical_ifs) {
		WARNING("ptp %s: exceeded the maximum supported number of "
			"slave interfaces (%d)\n",
			bond_info->bond_if, max_physical_ifs);
	}

	if (rc != 0 && rc != ENOENT) {
		ERROR("ptp %s: bond parsing failed\n", logical_if);
		return rc;
	}

	/* Select active interface depending on bond type */
	if (bond_info->bond_mode == SFPTPD_BOND_MODE_ACTIVE_BACKUP) {
		/* For active-backup bonds we require an active interface and
		 * expect to find it in the list of slave interfaces. */
		if (bond_info->active_if == NULL) {
			TRACE_L4("ptp %s: active-backup. Couldn't find current "
			      "active slave\n", bond_info->bond_if);
			rc = ENOENT;
		} else {
			for (i = 0; i < bond_info->num_physical_ifs; i++) {
				if (bond_info->active_if == bond_info->physical_ifs[i]) {
					break;
				}
			}

			if (i >= bond_info->num_physical_ifs) {
				ERROR("ptp %s: active interface %s not in slave "
				      "interface list\n",
				      bond_info->bond_if,
				      sfptpd_interface_get_name(bond_info->active_if));
				bond_info->active_if = NULL;
				rc = ENOENT;
			}
		}
	} else if (bond_info->bond_mode == SFPTPD_BOND_MODE_LACP ||
		   bond_info->is_bridge) {
		/* For LACP all slave interfaces are active. We have to pick
		 * one, so pick the first with link up.
		 * Bridges have the same problem so treat as LACP */
		for (i = 0; i < bond_info->num_physical_ifs; i++) {
			rc = sfptpd_interface_is_link_detected(bond_info->physical_ifs[i],
							       &link_detected);
			if ((rc == 0) && link_detected) {
				bond_info->active_if = bond_info->physical_ifs[i];
				if (verbose) {
					TRACE_L3("ptp %s: selected active slave %s\n",
						 bond_info->bond_if,
						 sfptpd_interface_get_name(bond_info->active_if));
				}
				break;
			}
		}

		/* If we can't find a slave interface with link up, warn and
		 * just pick the first slave for now. */
		if (i >= bond_info->num_physical_ifs) {
			bond_info->active_if = bond_info->physical_ifs[0];
			if (verbose) {
				WARNING("ptp %s: no slave interfaces have link up. "
					"Selecting slave interface %s\n",
					bond_info->bond_if,
					sfptpd_interface_get_name(bond_info->active_if));
			}
		}
	} else if (logical_link->type == SFPTPD_LINK_MACVLAN) {
		interface = sfptpd_interface_find_by_if_index(logical_link->if_link);
		if (interface == NULL || sfptpd_interface_is_deleted(interface)) {
			WARNING("ptp: physical interface for macvlan %s does not exist. "
				"We could be in a network namespace: will try to use the "
				"logical interface directly. Capabilities may be limited "
				"(no EFX ioctl, if applicable).\n", logical_if);
			interface = sfptpd_interface_find_by_if_index(logical_link->if_index);
		}
		if (interface == NULL || sfptpd_interface_is_deleted(interface)) {
			ERROR("ptp: no interface found for macvlan %s\n", logical_if);
			bond_info->num_physical_ifs = 0;
			rc = ENODEV;
		} else {
			TRACE_L3("ptp %s: using physical interface %s for macvlan\n",
				 logical_if,
				 sfptpd_interface_get_name(interface));
			bond_info->num_physical_ifs = 1;
			bond_info->active_if = bond_info->physical_ifs[0] = interface;
		}
	} else {
		assert(bond_info->bond_mode == SFPTPD_BOND_MODE_NONE);
		TRACE_L1("ptp: interface %s is not a bond, bridge or macvlan\n", logical_if);

		interface = sfptpd_interface_find_by_name(logical_if);
		if (interface == NULL || sfptpd_interface_is_deleted(interface)) {
			ERROR("ptp: logical interface %s does not exist\n", logical_if);
			bond_info->num_physical_ifs = 0;
			rc = ENODEV;
		} else {
			/* There is no bond. Set up a set of 1 physical interface to
			 * make parsing later less painful */
			bond_info->num_physical_ifs = 1;
			bond_info->active_if = bond_info->physical_ifs[0] = interface;
		}
	}

	return rc;
}


static int ptp_parse_interface_topology(struct sfptpd_ptp_bond_info *bond_info,
					const char *interface_name,
					const struct sfptpd_link_table *link_table)
{
	int rc;
	uint16_t vlan_tag;
	bool is_vlan;
	const struct sfptpd_link *logical_link = NULL;
	const struct sfptpd_link *target_if = NULL;

	assert(bond_info != NULL);
	assert(interface_name != NULL);

	/* Make sure that the user has specified an interface */
	if (interface_name[0] == '\0') {
		ERROR("ptp: no interface specified\n");
		return ENODEV;
	}

	logical_link = sfptpd_link_by_name(link_table, interface_name);
	if (logical_link == NULL) {
		ERROR("ptp: could not find interface %s in link table\n",
		      interface_name, strerror(errno));
		return errno;
	}

	/* Determine the underlying interface based on the interface name that
	 * has been supplied. This is based on the a potential topology of 
	 *     > zero or more VLANs on top of
	 *     > zero or one Bonds containing
	 *     > one or more physical devices
	 */
	sfptpd_strncpy(bond_info->logical_if, interface_name, sizeof(bond_info->logical_if));

	/* Work out the what VLAN tags, if any, are in use. We use a temporary
	 * string here to hold the current interface we are investigating,
	 * over-writing it each time round the loop as we dig through any nested
	 * VLANs. */
	target_if = logical_link;
	bond_info->num_vlan_tags = 0;
	do {
		rc = ptp_is_interface_vlan(target_if, &is_vlan,
					   &target_if, &vlan_tag,
					   link_table);
		if (rc != 0)
			return rc;

		if (is_vlan) {
			if (bond_info->num_vlan_tags >= SFPTPD_MAX_VLAN_TAGS) {
				ERROR("ptp: too many nested VLANs. sfptpd supports max of %d.\n",
				      SFPTPD_MAX_VLAN_TAGS);
				return ENOSPC;
			}

			bond_info->vlan_tags[bond_info->num_vlan_tags] = vlan_tag;
			bond_info->num_vlan_tags++;
		}
	} while (is_vlan);

	/* Work out if the 'bond' interface is a bond and if so parse the
	 * bond configuration. Note that at this point we've parsed all the VLANs
	 * so target_if points at either a physical interface or a bond. */
	rc = ptp_probe_bonding(target_if, bond_info, true, link_table);

	return rc == ENOENT ? 0 : rc;
}


static int ptp_check_clock_discipline_flags(struct sfptpd_ptp_intf *intf,
					    struct sfptpd_ptp_bond_info *bond_info)
{
	unsigned int i;
	struct sfptpd_clock *clock;
	struct sfptpd_interface *interface;
	int rc;

	assert(intf != NULL);

	rc = 0;
	for (i = 0; i < bond_info->num_physical_ifs; i++) {
		interface = bond_info->physical_ifs[i];
		clock = sfptpd_interface_get_clock(interface);

		if (!sfptpd_clock_get_discipline(clock)) {
			ERROR("ptp: interface %s associated clock %s "
			      "is not configured to be disciplined\n",
			      sfptpd_interface_get_name(interface),
			      sfptpd_clock_get_long_name(clock));
			rc = EPERM;
		}
	}

	return rc;
}


int ptp_determine_timestamp_type(ptpd_timestamp_type_e *timestamp_type,
			         struct sfptpd_ptp_intf *logical_intf,
			         struct sfptpd_interface *physical_intf)
{
	struct sfptpd_ptp_instance *instance;
	bool must_be_hw = false;
	bool must_be_sw = false;
	const char *hw_instance = "(no-instnace)";
	const char *sw_instance = "(no-instance)";

	assert(logical_intf != NULL);

	if (physical_intf == NULL) {
		*timestamp_type = PTPD_TIMESTAMP_TYPE_SW;
		TRACE_L3("ptp: using software timestamping on non-physical interface %s\n",
			 logical_intf->bond_info.logical_if);
	} else if (!sfptpd_interface_supports_ptp(physical_intf)) {
		*timestamp_type = PTPD_TIMESTAMP_TYPE_SW;
		TRACE_L3("ptp: interface %s (%s) does not support PTP; using software timestamping\n",
			 sfptpd_interface_get_name(physical_intf),
			 logical_intf->bond_info.logical_if);
	} else {
		*timestamp_type = PTPD_TIMESTAMP_TYPE_HW_RAW;
		TRACE_L3("ptp: using interface %s (%s) as PTP clock\n",
			 sfptpd_interface_get_name(physical_intf),
			 logical_intf->bond_info.logical_if);
	}

	for (instance = logical_intf->instance_list; instance != NULL; instance = instance->next) {
		struct ptpd_port_config *iconf = &instance->config->ptpd_port;

		if (iconf->timestamp_pref == PTPD_TIMESTAMP_TYPE_SW) {
			must_be_sw = true;
			sw_instance = SFPTPD_CONFIG_GET_NAME(instance->config);
		}

		if (iconf->timestamp_pref == PTPD_TIMESTAMP_TYPE_HW_RAW) {
			must_be_hw = true;
			hw_instance = SFPTPD_CONFIG_GET_NAME(instance->config);
		}
	}

	if (must_be_hw && must_be_sw) {
		CRITICAL("ptp: conflicting timestamping requirements between "
			 "%s (hw) and %s (sw) instances for interface %s (%s)\n",
			 hw_instance, sw_instance,
			 sfptpd_interface_get_name(physical_intf),
			 logical_intf->bond_info.logical_if);
		return EINVAL;
	} else if (must_be_hw && *timestamp_type == PTPD_TIMESTAMP_TYPE_SW) {
		CRITICAL("ptp %s: interface %s (%s) cannot support configured "
			 "requirement for hardware timestamping\n",
			 hw_instance,
			 sfptpd_interface_get_name(physical_intf),
			 logical_intf->bond_info.logical_if);
		return ENOTSUP;
	} else if (must_be_sw && *timestamp_type == PTPD_TIMESTAMP_TYPE_HW_RAW) {
		*timestamp_type = PTPD_TIMESTAMP_TYPE_SW;
		NOTICE("ptp %s: downgrading to configured software timestamping "
		       "on interface %s (%s)\n",
		       sw_instance,
		       sfptpd_interface_get_name(physical_intf),
		       logical_intf->bond_info.logical_if);
	}

	return 0;
}


static int ptp_configure_clock(struct sfptpd_ptp_intf *interface)
{
	struct sfptpd_clock *system_clock;
	struct timespec time;
	int rc;
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_config_general *general_cfg;
	sfptpd_ptp_module_config_t *config;

	/* Shared interface configuration is duplicated for each interface.
	 * We read from one representative configuration but write back to all. */
	config = ptp_get_config_for_interface(interface);
	assert(config != NULL);

	/* Access global configuration */
	general_cfg = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(config));
	assert(general_cfg != NULL);

	/* Determine which clock to use based on the interface */
	interface->clock = sfptpd_interface_get_clock(interface->bond_info.active_if);
	assert(interface->clock != NULL);

	system_clock = sfptpd_clock_get_system_clock();

	INFO("ptp: clock is %s\n", sfptpd_clock_get_long_name(interface->clock));

	/* Check the clock discipline flags for each clock that that may be
	 * disciplined. An error at this point is considered a fatal
	 * configuration error. */
	rc = ptp_check_clock_discipline_flags(interface, &interface->bond_info);
	if (rc != 0) {
		CRITICAL("ptp: one or more clocks required by PTP is not "
			 "configured to be disciplined\n");
		if (general_cfg->ignore_critical[SFPTPD_CRITICAL_NO_PTP_CLOCK])
			NOTICE("ptp: ignoring critical error by configuration\n");
		else {
			NOTICE("configure \"ignore_critical: no-ptp-clock\" to allow sfptpd to start in spite of this condition\n");
			return rc;
		}
	}

	/* Set the logical and physical interfaces in the PTPD configuration for
	 * each instance sharing this interface. */
	for (instance = interface->instance_list; instance != NULL; instance = instance->next) {
		sfptpd_ptp_module_config_t *iconf = instance->config;
		sfptpd_strncpy(iconf->ptpd_intf.ifaceName, interface->bond_info.logical_if,
			       sizeof(iconf->ptpd_intf.ifaceName));
		iconf->ptpd_intf.physIface = interface->bond_info.active_if;
		rc = ptp_determine_timestamp_type(&iconf->ptpd_intf.timestampType,
						  interface,
						  interface->bond_info.active_if);
		if (rc != 0)
			return rc;
	}

	/* For each physical interface configure timestamping. */
	rc = ptp_timestamp_filtering_configure_all(interface);
	if (rc != 0) {
		CRITICAL("ptp: failed to configure timestamping on one or more "
			 "interfaces\n");
		return rc;
	}

	/* If using a NIC clock and we are in a PTP master mode then step the
	 * NIC clock to the current system time. */
	if ((interface->clock != system_clock) && !config->ptpd_port.slaveOnly) {
		/* Rather than set the NIC time, get an accurate difference
		 * between the system and NIC time and apply this offset */
		rc = sfptpd_clock_compare(system_clock, interface->clock, &time);
		if (rc != 0) {
			TRACE_L4("ptp: failed to compare clock %s and system clock, %s\n",
				 sfptpd_clock_get_short_name(interface->clock),
				 strerror(rc));
			if (rc != EAGAIN)
				return rc;
		} else {
			sfptpd_clock_adjust_time(interface->clock, &time);
		}
	}

	return 0;
}


static int ptp_handle_bonding_interface_change(struct sfptpd_ptp_intf *intf,
					       bool *bond_changed)
{
	struct sfptpd_ptp_bond_info new_bond_info;
	struct sfptpd_ptp_instance *instance;
	const struct sfptpd_link *logical_link;
	ptpd_timestamp_type_e timestamp_type;
	int rc;
	bool set_changed;
	bool active_changed;
	bool ts_changed;

	assert(intf != NULL);
	assert(intf->ptpd_intf_private != NULL);
	assert(bond_changed != NULL);

	/* Determine if the active interface has changed... */
	*bond_changed = false;

	/* Re-evaluate the bond to see what has changed. We can handle some 
	 * changes but others are fatal:
	 *   - bond deleted - fatal
	 *   - new physical interface created and added to the bond - ok
	 *   - number or set of slave interfaces changed - ok
	 *   - active/backup mode, active interface changed - ok
	 *   - slave interface added but clock not configured to be disciplined -
	 *       warn and continue anyway
	 */

	logical_link = sfptpd_link_by_name(&intf->module->link_table, intf->bond_info.bond_if);
	if (logical_link == NULL) {
		ERROR("ptp: could not find interface %s in link table\n",
		      intf->bond_info.bond_if, strerror(errno));
		rc = errno;
		goto finish;
	}

	/* Take a copy of the interface configuration and reparse the bond
	 * configuration into it */
	new_bond_info = intf->bond_info;
	rc = ptp_probe_bonding(logical_link, &new_bond_info, false, &intf->module->link_table);
	if (rc != 0 && rc != ENOENT && rc != ENODEV) {
		CRITICAL("ptp: interface %s error parsing bond configuration\n",
			 intf->bond_info.bond_if);
		rc = EIO;
		goto finish;
	}

	/* Work out the correct time mode to use based on whether the NIC is
	 * PTP-capable or not. */
	rc = ptp_determine_timestamp_type(&timestamp_type, intf,
					  new_bond_info.active_if);
	if (rc != 0)
		goto finish;

	/* If the set of slave interfaces has changed, we need to reconfigure
	 * timestamping. */
	set_changed = (new_bond_info.num_physical_ifs != intf->bond_info.num_physical_ifs) ||
		      (memcmp(new_bond_info.physical_ifs, &intf->bond_info.physical_ifs,
			      sizeof(new_bond_info.physical_ifs[0]) * new_bond_info.num_physical_ifs) != 0);
	active_changed = (new_bond_info.active_if != intf->bond_info.active_if);
	ts_changed = (timestamp_type != intf->ptpd_intf_private->ifOpts.timestampType);

	if (set_changed) {
		INFO("ptp: interface %s number or set of slave interfaces changed (%d -> %d)\n",
		     intf->bond_info.bond_if, intf->bond_info.num_physical_ifs,
		     new_bond_info.num_physical_ifs);

		/* The slave interfaces have changed so we check the clock
		 * discipline flags on the changed set of clocks. Note that
		 * there is a problem here if an interface has been added to
		 * the bond and the clock associated with the interface is not
		 * configured to be disciplined. We don't want to fail with a
		 * fatal error in this case so instead, we just warn and
		 * continue anyway. */
		(void)ptp_check_clock_discipline_flags(intf, &new_bond_info);
	}

	if (ts_changed) {
		INFO("ptp: interface %s timestamping changed %s -> %s\n",
		     intf->bond_info.bond_if,
		     ts_name(intf->ptpd_intf_private->ifOpts.timestampType),
		     ts_name(timestamp_type));
	}

	if (set_changed || active_changed || ts_changed) {
		/* Reconfigure timestamping based on any changes to the
		 * interface set. */
		ptp_timestamp_filtering_reconfigure_all(intf, &new_bond_info,
							timestamp_type);
	}

	if (active_changed) {
		INFO("ptp: interface %s changed %s (%s) -> %s (%s)\n",
		     intf->bond_info.bond_if,
		     sfptpd_interface_get_name(intf->bond_info.active_if),
		     intf->bond_info.logical_if,
		     sfptpd_interface_get_name(new_bond_info.active_if),
		     new_bond_info.logical_if);
	}

	if (active_changed || ts_changed) {
		/* Determine and store the new PTP clock */
		intf->clock = sfptpd_interface_get_clock(new_bond_info.active_if);

		/* Reconfigure PTPD to use the new interface */
		rc = ptpd_change_interface(intf->ptpd_intf_private, new_bond_info.logical_if,
					   new_bond_info.active_if, timestamp_type);

		*bond_changed = true;
	} else {
		rc = 0;
		*bond_changed = false;
	}

	if (active_changed && rc != 0 && rc != ENOENT) {
		CRITICAL("ptp %s: failed to change interface from %s (%s) to %s (%s)\n",
			 intf->bond_info.bond_if,
			 sfptpd_interface_get_name(intf->bond_info.active_if),
			 intf->bond_info.logical_if,
			 sfptpd_interface_get_name(new_bond_info.active_if),
			 new_bond_info.logical_if);
	} else if (ts_changed && rc != 0 && rc != ENOENT) {
		CRITICAL("ptp %s: failed to change timesetamping\n",
			 intf->bond_info.bond_if);
	}

	/* Store the new interface configuration */
	intf->bond_info = new_bond_info;

finish:
	/* Set alarm based on interface availability */
	for (instance = intf->instance_list; instance; instance = instance->next) {
		if (rc == 0 && new_bond_info.num_physical_ifs != 0) {
			SYNC_MODULE_ALARM_CLEAR(instance->local_alarms, NO_INTERFACE);
		} else {
			SYNC_MODULE_ALARM_SET(instance->local_alarms, NO_INTERFACE);
		}
	}

	/* Failure to find a physical interface is not fatal, we may get one
	 * later. */
	if (rc == ENOENT)
		rc = 0;

	return rc;
}


static void ptp_update_sockets(int old_sock, int new_sock)
{
	int rc;

	/* It is impossible to check for equivalence between the underlying
	   resources using file descriptors because they can be re-allocated,
	   so always remove and add the fds presented here. */

	/* Ignore any errors removing fds; these can be expected if the
	   resource has been closed. */
	if (old_sock >= 0)
		sfptpd_thread_user_fd_remove(old_sock);

	if (new_sock >= 0) {
		rc = sfptpd_thread_user_fd_add(new_sock, true, false);
		if (rc != 0) {
			ERROR("ptp: failed to add new PTP socket %d to epoll, %s\n",
			      new_sock, strerror(rc));
		}
	}
}

/* Sets an appropriate convergence threshold */
static void ptp_set_convergence_threshold(struct sfptpd_ptp_instance *instance) {

	/* Check if overriden by user */
	long double threshold = instance->config->convergence_threshold;

	/* Otherwise pick a value based on whether hardware or software
	 * timestamping is in use */
	if (threshold == 0) {
		if (ptp_is_instance_hw_timestamping(instance)) {
			threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT;
		} else {
			threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_SW_TS;
		}
	}

	sfptpd_stats_convergence_set_max_offset(&instance->convergence, threshold);
}


/* Returns whether the instance state changed */
static bool ptp_update_instance_state(struct sfptpd_ptp_instance *instance,
				      bool bond_changed)
{
	struct sfptpd_ptp_module *module;
	struct ptpd_port_snapshot snapshot;
	sfptpd_time_t ofm;
	bool update_uuid_filter, state_changed, leap_second_changed, instance_changed, offset_changed;
	int rc;

	assert(instance!= NULL);

	module = instance->intf->module;
	assert(module != NULL);

	update_uuid_filter = false;
	state_changed = false;
	leap_second_changed = false;

	ofm = instance->ptpd_port_snapshot.current.offset_from_master;

	instance_changed = ((instance->synchronized != instance->synchronized_snapshot) ||
			    (instance->ctrl_flags != instance->ctrl_flags_snapshot));

	/* Make snapshots of non-ptpd state */
	instance->synchronized_snapshot = instance->synchronized;
	instance->ctrl_flags_snapshot = instance->ctrl_flags;

	/* Get a snapshot of PTPD's state */
	rc = ptpd_get_snapshot(instance->ptpd_port_private, &snapshot);
	if (rc != 0) {
		ERROR("ptp %s: failed to get PTPD state snapshot, %s\n",
		      SFPTPD_CONFIG_GET_NAME(instance->config), strerror(rc));
		return state_changed;
	}

	/* If the state has changed, make sure we update the packet timestamp
	 * filter and notify the sync engine. */
	if (snapshot.port.state != instance->ptpd_port_snapshot.port.state) {
		state_changed = true;
		update_uuid_filter = true;
	}

	/* If the grandmaster ID has changed, modify the packet timestamp UUID
	 * filter. */
	if (memcmp(snapshot.parent.clock_id, instance->ptpd_port_snapshot.parent.clock_id,
		   sizeof(snapshot.parent.clock_id)) != 0)
		update_uuid_filter = true;

	/* If any of the parent data has changed, generate a state change to
	 * the engine to ensure that the topology and state files get updated. */
	if (memcmp(&snapshot.parent, &instance->ptpd_port_snapshot.parent, sizeof(snapshot.parent)) != 0)
		state_changed = true;

	/* If the 'steps removed' has changed, generate a state change to aid
	   sync instance selection. (The remaining characteristics are caught
	   above from the 'parent' substructure.) */
	if (snapshot.current.steps_removed != instance->ptpd_port_snapshot.current.steps_removed)
		state_changed = true;

	/* If the 'clustering score' has changed, generate a state change to aid
	   sync instance selection. */
	int current_clustering_score = instance->ptpd_port_private->rtOpts.clusteringEvaluator.calc_fn(
	        &instance->ptpd_port_private->rtOpts.clusteringEvaluator,
		ofm,
		instance->intf->clock);

	if (current_clustering_score != instance->clustering_score_snapshot) {
		INFO("%s: clustering score changed %d -> %d\n",
			 instance->config->hdr.name,
			 instance->clustering_score_snapshot,
			 current_clustering_score);
	}

	if (instance->clustering_score_snapshot != current_clustering_score) {
		state_changed = true;
	}

	/* If the port alarms have changed, generate a state change to the
	 * engine to ensure that the topology and state files get updated */
	if ((snapshot.port.alarms | instance->local_alarms) != ptp_get_alarms_snapshot(instance))
		state_changed = true;

	/* Record whether the leap second flags have changed. */
	if ((snapshot.time.leap59 != instance->ptpd_port_snapshot.time.leap59) ||
	    (snapshot.time.leap61 != instance->ptpd_port_snapshot.time.leap61))
		leap_second_changed = true;

	/* Check whether the offset has changed. */
	offset_changed = snapshot.current.offset_from_master != ofm;

	/* Store the new state */
	instance->ptpd_port_snapshot = snapshot;
	instance->local_alarms_snapshot = instance->local_alarms;
	instance->clustering_score_snapshot = current_clustering_score;

	/* Set the convergence threshold */
	if (bond_changed) {
		ptp_set_convergence_threshold(instance);
	}

	/* Update the convergence measure */
	ptp_convergence_update(instance);

	/* Update historical stats */
	ptp_stats_update(instance);

	/* Send data for clustering determination if a contributor */
	if (instance->ctrl_flags & SYNC_MODULE_CLUSTERING_DETERMINANT &&
	    offset_changed) {
		sfptpd_engine_clustering_input(module->engine,
					       instance->config->hdr.name,
					       instance->intf->clock,
					       ofm,
					       finitel(ofm) && ofm != 0.0L);
	}

	/* Send realtime stats update if anything changed */
	if (state_changed || bond_changed || instance_changed || offset_changed) {
		struct sfptpd_log_time time;
		sfptpd_log_get_time(&time);
		ptp_send_instance_rt_stats_update(module->engine, instance, time);
	}

	/* If the state has changed, send an event to the sync engine and
	 * update the UUID filter. Also send any status signaling messages
	 * required. */
	if (state_changed || bond_changed || instance_changed) {
		sfptpd_sync_instance_status_t status = { 0 };
		status.state = ptp_translate_state(snapshot.port.state);
		status.alarms = ptp_get_alarms_snapshot(instance);
		status.clock = instance->intf->clock;
		status.user_priority = instance->config->priority;
		status.clustering_score = current_clustering_score;
		sfptpd_time_float_ns_to_timespec(instance->ptpd_port_snapshot.current.offset_from_master,
						 &status.offset_from_master);

		status.local_accuracy = ptp_get_instance_accuracy(instance);
		ptp_translate_master_characteristics(instance, &status);

		if (state_changed || bond_changed) {
			sfptpd_engine_sync_instance_state_changed(instance->intf->module->engine,
								  sfptpd_thread_self(),
								  (struct sfptpd_sync_instance *) instance,
								  &status);
		}

		ptpd_publish_status(instance->ptpd_port_private,
				    status.alarms,
				    instance->ctrl_flags & SYNC_MODULE_SELECTED,
				    instance->synchronized,
				    bond_changed);
	}

	/* If the leap second state has changed, send the appropriate signal
	 * to the sync engine. Note that we only do this if we are the currently
	 * selected instance and we are a slave! */
	if (((instance->ctrl_flags & SYNC_MODULE_SELECTED) != 0) &&
	    (snapshot.port.state == PTPD_SLAVE) && leap_second_changed) {
		sfptpd_time_t guard_interval = 2 * snapshot.port.announce_interval;

		if (snapshot.time.leap59) {
			sfptpd_engine_schedule_leap_second(module->engine,
							   SFPTPD_LEAP_SECOND_59,
							   guard_interval);
		} else if (snapshot.time.leap61) {
			sfptpd_engine_schedule_leap_second(module->engine,
							   SFPTPD_LEAP_SECOND_61,
							   guard_interval);
		} else {			
			sfptpd_engine_cancel_leap_second(module->engine);
		}
	}

	/* If the grandmaster has changed, update the filtering */
	if (update_uuid_filter)
		ptp_timestamp_filtering_set_uuid(instance->intf, instance);

	return state_changed;
}


static void ptp_update_interface_state(struct sfptpd_ptp_intf *interface)
{
	struct sfptpd_ptp_instance *instance;
	struct ptpd_intf_fds fds;
	bool state_changed;
	int rc;

	assert(interface != NULL);

	state_changed = false;

	/* Bond changes are checked when a new link table becomes
	 * available and there is no other type of probing performed.
	 *
	 * Conceivably we could defer that handling to here, storing
	 * the new table and then for bonds calling
	 *   ptp_handle_bonding_interface_change(interface, &bond_changed)
	 */

	/* Get a snapshot of PTPD interface's fds */
	rc = ptpd_get_intf_fds(interface->ptpd_intf_private, &fds);
	if (rc != 0) {
		ERROR("ptp: failed to get PTPD interface fds, %s\n", strerror(rc));
		return;
	}

	for (instance = interface->instance_list; instance != NULL; instance = instance->next) {
		state_changed |= ptp_update_instance_state(instance,
							   interface->bond_changed);
	}

	/* If the sockets used for PTP traffic have changed (they get closed
	 * and reopened if a serious error occurs) then update the thread epoll
	 * set with the new values. */
	if (interface->bond_changed || state_changed) {
		ptp_update_sockets(interface->ptpd_intf_fds.event_sock,
				   fds.event_sock);
		ptp_update_sockets(interface->ptpd_intf_fds.general_sock,
				   fds.general_sock);
	}

	/* Store the new fds */
	interface->ptpd_intf_fds = fds;

	/* Clear bond changed flag */
	interface->bond_changed = false;
}


static struct sfptpd_ptp_instance *ptp_get_first_instance(sfptpd_ptp_module_t *ptp) {
	assert (ptp != NULL);

	if (ptp->intf_list != NULL) {
		return ptp->intf_list->instance_list;
	} else {
		return NULL;
	}
}

static struct sfptpd_ptp_instance *ptp_get_next_instance(struct sfptpd_ptp_instance *instance) {
	assert (instance != NULL);

	if (instance->next == NULL) {
		if (instance->intf->next == NULL) {
			return NULL;
		} else {
			return instance->intf->next->instance_list;
		}
	} else {
		return instance->next;
	}
}


static bool ptp_is_instance_valid(sfptpd_ptp_module_t *ptp,
				  struct sfptpd_ptp_instance *instance) {
	struct sfptpd_ptp_instance *ptr;

	assert(ptp != NULL);
	assert(instance != NULL);

	for (ptr = ptp_get_first_instance(ptp);
	     ptr != instance && ptr != NULL;
	     ptr = ptp_get_next_instance(ptr));

	return ptr == NULL ? false : true;
}


static void ptp_send_instance_rt_stats_update(struct sfptpd_engine *engine,
					      struct sfptpd_ptp_instance *instance,
					      struct sfptpd_log_time time)
{
	uint8_t *parent_id, *gm_id;
	sfptpd_time_t ofm_ns, owd_ns;
	struct sfptpd_ptp_bond_info *bond_info;

	assert(instance != NULL);
	assert(instance->ptpd_port_private != NULL);

	parent_id = instance->ptpd_port_snapshot.parent.clock_id;
	gm_id = instance->ptpd_port_snapshot.parent.grandmaster_id;

	bond_info = ptp_get_bond_info(instance);

	if (instance->ptpd_port_snapshot.port.state == PTPD_SLAVE) {
		ofm_ns = instance->ptpd_port_snapshot.current.offset_from_master;
		owd_ns = instance->ptpd_port_snapshot.current.one_way_delay;

		/* If PPS statistics logging is enabled, also output those */
		if (instance->config->pps_logging) {
			sfptpd_stats_pps_t pps_stats;
			ptp_log_pps_stats(engine, instance, &pps_stats);

			sfptpd_engine_post_rt_stats(engine, &time,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"gm", NULL, instance->intf->clock,
				(instance->ctrl_flags & SYNC_MODULE_SELECTED),
				false,
				instance->synchronized,
				ptp_get_alarms_snapshot(instance),
				STATS_KEY_OFFSET, ofm_ns,
				STATS_KEY_FREQ_ADJ, instance->ptpd_port_snapshot.current.frequency_adjustment,
				STATS_KEY_OWD, owd_ns,
				STATS_KEY_PARENT_ID, parent_id,
				STATS_KEY_GM_ID, gm_id,
				STATS_KEY_ACTIVE_INTF, bond_info->active_if,
				STATS_KEY_BOND_NAME, bond_info->bond_mode == SFPTPD_BOND_MODE_NONE ? NULL : bond_info->bond_if,
				STATS_KEY_PPS_OFFSET, (sfptpd_time_t)pps_stats.offset.last,
				STATS_KEY_BAD_PERIOD, pps_stats.bad_period_count,
				STATS_KEY_OVERFLOWS, pps_stats.overflow_count,
				STATS_KEY_P_TERM, instance->ptpd_port_snapshot.current.servo_p_term,
				STATS_KEY_I_TERM, instance->ptpd_port_snapshot.current.servo_i_term,
				STATS_KEY_END);
		} else {
			sfptpd_engine_post_rt_stats(engine, &time,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"gm", NULL, instance->intf->clock,
				(instance->ctrl_flags & SYNC_MODULE_SELECTED),
				false,
				instance->synchronized,
				ptp_get_alarms_snapshot(instance),
				STATS_KEY_OFFSET, ofm_ns,
				STATS_KEY_FREQ_ADJ, instance->ptpd_port_snapshot.current.frequency_adjustment,
				STATS_KEY_OWD, owd_ns,
				STATS_KEY_PARENT_ID, parent_id,
				STATS_KEY_GM_ID, gm_id,
				STATS_KEY_ACTIVE_INTF, bond_info->active_if,
				STATS_KEY_BOND_NAME, bond_info->bond_mode == SFPTPD_BOND_MODE_NONE ? NULL : bond_info->bond_if,
				STATS_KEY_P_TERM, instance->ptpd_port_snapshot.current.servo_p_term,
				STATS_KEY_I_TERM, instance->ptpd_port_snapshot.current.servo_i_term,
				STATS_KEY_END);
		}
	}
}


static void ptp_send_rt_stats_update(struct sfptpd_ptp_module *ptp,
				     struct sfptpd_log_time time)
{
	struct sfptpd_ptp_instance *instance;

	assert(ptp != NULL);

	for (instance = ptp_get_first_instance(ptp); instance; instance = ptp_get_next_instance(instance)) {
		ptp_send_instance_rt_stats_update(ptp->engine, instance, time);
	}
}


static int ptp_setup_discriminator(struct sfptpd_ptp_instance *instance) {
	struct sfptpd_ptp_module_config *config;

	assert(instance != NULL);
	config = instance->config;
	assert(config != NULL);

	if (config->ptpd_port.discriminator_name[0]) {
		const struct sfptpd_sync_instance_info *instance_info;
		struct sfptpd_clock *clock;

		instance_info = sfptpd_engine_get_sync_instance_by_name(instance->intf->module->engine,
									config->ptpd_port.discriminator_name);
		if (instance_info) {
			instance->discriminator_type = DISC_SYNC_INSTANCE;
			instance->discriminator.sync_instance = instance_info;
		} else {
			clock = sfptpd_clock_find_by_name(config->ptpd_port.discriminator_name);
			if (clock) {
				instance->discriminator_type = DISC_CLOCK;
				instance->discriminator.clock = clock;
			} else {
				CRITICAL("ptp %s: could not identify BMC discriminator %s\n",
					 SFPTPD_CONFIG_GET_NAME(instance->config),
					 config->ptpd_port.discriminator_name);
				return ENOENT;
			}
		}
	} else {
		instance->discriminator_type = DISC_NONE;
	}
	return 0;
}


static void ptp_on_get_status(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_sync_instance_status *status = &msg->u.get_status_resp.status;

	assert(ptp != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_ptp_instance *) msg->u.get_status_req.instance_handle;
	assert(instance);
	assert(ptp_is_instance_valid(ptp, instance));

	status->state = ptp_translate_state(instance->ptpd_port_snapshot.port.state);
	status->alarms = ptp_get_alarms_snapshot(instance);
	status->clock = instance->intf->clock;
	status->user_priority = instance->config->priority;
	status->local_accuracy = ptp_get_instance_accuracy(instance);

	/* The offset is only valid in the slave state */
	if (instance->ptpd_port_snapshot.port.state == PTPD_SLAVE) {
		sfptpd_time_float_ns_to_timespec(instance->ptpd_port_snapshot.current.offset_from_master,
						 &status->offset_from_master);
	} else {
		status->offset_from_master.tv_sec = 0;
		status->offset_from_master.tv_nsec = 0;
	}

	ptp_translate_master_characteristics(instance, status);

	SFPTPD_MSG_REPLY(msg);
}


static void ptp_on_control(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;
	struct sfptpd_ptp_instance *instance;

	assert(ptp != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_ptp_instance *) msg->u.control_req.instance_handle;
	assert(instance);
	assert(ptp_is_instance_valid(ptp, instance));

	ctrl_flags = instance->ctrl_flags;
	ctrl_flags &= ~msg->u.control_req.mask;
	ctrl_flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	if (ctrl_flags != instance->ctrl_flags)
		ptpd_control(instance->ptpd_port_private, ctrl_flags);

	/* Record the new control flags */
	instance->ctrl_flags = ctrl_flags;

	SFPTPD_MSG_REPLY(msg);
}


static void ptp_on_update_gm_info(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *ptr;
	struct sfptpd_grandmaster_info *info;
	uint8_t clock_class;
	ptpd_clock_accuracy_e clock_accuracy;
	unsigned int allan_variance;

	assert(ptp != NULL);
	assert(msg != NULL);

	info = &msg->u.update_gm_info_req.info;

	clock_class = ptp_translate_clock_class_to_ieee1588(info->clock_class);
	clock_accuracy = ptp_translate_accuracy_to_enum(info->accuracy);
	allan_variance = ptp_translate_allan_variance_to_ieee1588(info->allan_variance);

	/* Update all instances except the originator */
	for (ptr = ptp_get_first_instance(ptp); ptr != NULL; ptr = ptp_get_next_instance(ptr)) {
		if (ptr != (struct sfptpd_ptp_instance *)msg->u.update_gm_info_req.originator) {
			ptpd_update_gm_info(ptr->ptpd_port_private, info->remote_clock,
					    info->clock_id.id, clock_class,
					    info->time_source, clock_accuracy,
					    allan_variance, info->steps_removed,
					    info->time_traceable, info->freq_traceable);
		}
	}

	SFPTPD_MSG_FREE(msg);
}


static void ptp_on_update_leap_second(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *ptr;
	bool leap59, leap61;

	assert(ptp != NULL);
	assert(msg != NULL);

	leap59 = (msg->u.update_leap_second_req.type == SFPTPD_LEAP_SECOND_59);
	leap61 = (msg->u.update_leap_second_req.type == SFPTPD_LEAP_SECOND_61);

	/* Update all instances */
	for (ptr = ptp_get_first_instance(ptp); ptr != NULL; ptr = ptp_get_next_instance(ptr)) {
		ptpd_update_leap_second(ptr->ptpd_port_private, leap59, leap61);
	}

	SFPTPD_MSG_FREE(msg);
}


static void ptp_on_step_clock(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;

	assert(ptp != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_ptp_instance *) msg->u.step_clock_req.instance_handle;
	assert(instance != NULL);
	assert(ptp_is_instance_valid(ptp, instance));

	/* Step the clock and reset the servo */
	ptpd_step_clock(instance->ptpd_port_private, &msg->u.step_clock_req.offset);

	SFPTPD_MSG_REPLY(msg);
}


static void ptp_on_log_stats(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	assert(ptp != NULL);
	assert(msg != NULL);

	ptp_send_rt_stats_update(ptp, msg->u.log_stats_req.time);

	SFPTPD_MSG_FREE(msg);
}

static void ptp_write_ptp_nodes(FILE *stream,
				struct sfptpd_ptp_instance *instance)
{
	const char *format_node_string = "| %6s | %24s | %11s | %6s | %11s | %s\n";
	const char *format_node_data = "| %6s | %24s | %11d | %6d | %11d | %s\n";

	if (instance == NULL) {
		sfptpd_log_table_row(stream, true,
				     format_node_string,
				     "state",
				     "clock-id",
				     "port-number",
				     "domain",
				     "local-port",
				     "instance");
	} else {
		struct sfptpd_hash_table *table;
		struct sfptpd_stats_ptp_node *node;
		struct sfptpd_ht_iter iter;

		table = instance->ptpd_port_private->interface->nodeSet;
		node = sfptpd_stats_node_ht_get_first(table, &iter);

		while (node != NULL) {
			struct sfptpd_stats_ptp_node *next;

			next = sfptpd_stats_node_ht_get_next(&iter);
			sfptpd_log_table_row(stream, next == NULL,
					     format_node_data,
					     node->state,
					     node->clock_id_string,
					     node->port_number,
					     node->domain_number,
					     instance->ptpd_port_private->portIdentity.portNumber,
					     instance->config->hdr.name);
			node = next;
		}
	}
}


static void ptp_on_save_state(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_ptp_intf *interface;
	sfptpd_time_t ofm_ns, owd_ns;
	uint8_t *gm_id, *p_id;
	const char *format, *delay_mechanism;
	struct ptpd_port_snapshot *snapshot;
	unsigned int snapshot_alarms;
	char alarms[256], flags[256];
	bool hw_ts;
	struct sfptpd_log *nodes_log;

	assert(ptp != NULL);
	assert(msg != NULL);

	/* Open the stream for the ptp-nodes file and write the header line */
	nodes_log = sfptpd_log_open_ptp_nodes();
	if (nodes_log != NULL) {
		ptp_write_ptp_nodes(sfptpd_log_file_get_stream(nodes_log), NULL);
	}

	for (instance = ptp_get_first_instance(ptp); instance; instance = ptp_get_next_instance(instance)) {
		assert(instance->ptpd_port_private != NULL);

		const struct sfptpd_ptp_profile_def *profile = sfptpd_ptp_get_profile_def(instance->config->profile);

		/* Periodically write state and statistics to a file */
		snapshot = &instance->ptpd_port_snapshot;
		snapshot_alarms = snapshot->port.alarms | instance->local_alarms;
		sfptpd_sync_module_alarms_text(snapshot_alarms, alarms, sizeof(alarms));
		sfptpd_sync_module_ctrl_flags_text(instance->ctrl_flags, flags, sizeof(flags));

		ofm_ns = snapshot->current.offset_from_master;
		owd_ns = snapshot->current.one_way_delay;

		p_id = snapshot->parent.clock_id;
		gm_id = snapshot->parent.grandmaster_id;

		if (snapshot->port.delay_mechanism == PTPD_DELAY_MECHANISM_P2P)
			delay_mechanism = "peer-to-peer";
		else
			delay_mechanism = "end-to-end";

		hw_ts = ptp_is_instance_hw_timestamping(instance);

		switch (snapshot->port.state) {
		case PTPD_SLAVE:
			format = "instance: %s\n"
				"clock-name: %s\n"
				"clock-id: %s\n"
				"state: %s\n"
				"alarms: %s\n"
				"control-flags: %s\n"
				"interface: %s (%s)\n"
				"transport: %s\n"
				"profile: " SFPTPD_FORMAT_EUI48 " (%s) %s\n"
				"timestamping: %s\n"
				"offset-from-master: " SFPTPD_FORMAT_FLOAT "\n"
				"one-way-delay: " SFPTPD_FORMAT_FLOAT "\n"
				"freq-adjustment-ppb: " SFPTPD_FORMAT_FLOAT "\n"
				"in-sync: %d\n"
				"ptp-domain: %d\n"
				"steps-removed: %d\n"
				"parent-clock-id: " SFPTPD_FORMAT_EUI64 "\n"
				"parent-port-num: %d\n"
				"delay-mechanism: %s\n"
				"two-step: %s\n"
				"slave-only: %s\n"
				"grandmaster-id: " SFPTPD_FORMAT_EUI64 "\n"
				"grandmaster-clock-class: %d\n"
				"grandmaster-clock-accuracy: %d (<%0.0Lfns)\n"
				"grandmaster-bmc-priority1: %d\n"
				"grandmaster-bmc-priority2: %d\n"
				"timescale: %s\n"
				"current-utc-offset: %d\n"
				"leap-59: %d\n"
				"leap-61: %d\n"
				"clustering-score: %d\n"
				"diff-method: %s\n";

			sfptpd_log_write_state(instance->intf->clock,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				format,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				sfptpd_clock_get_long_name(instance->intf->clock),
				sfptpd_clock_get_hw_id_string(instance->intf->clock),
				ptp_state_text(snapshot->port.state, snapshot_alarms),
				alarms, flags,
				sfptpd_interface_get_name(instance->intf->bond_info.active_if),
				instance->intf->bond_info.logical_if,
				instance->intf->transport_name,
				profile->id[0], profile->id[1], profile->id[2],
				profile->id[3], profile->id[4], profile->id[5],
				profile->name, profile->version,
				hw_ts? "hw": "sw",
				ofm_ns, owd_ns,
				snapshot->current.frequency_adjustment,
				instance->synchronized,
				snapshot->port.domain_number,
				snapshot->current.steps_removed,
				p_id[0], p_id[1], p_id[2], p_id[3],
				p_id[4], p_id[5], p_id[6], p_id[7],
				snapshot->parent.port_num,
				delay_mechanism,
				snapshot->current.two_step? "yes": "no",
				snapshot->port.slave_only? "yes": "no",
				gm_id[0], gm_id[1], gm_id[2], gm_id[3],
				gm_id[4], gm_id[5], gm_id[6], gm_id[7],
				snapshot->parent.grandmaster_clock_class,
				snapshot->parent.grandmaster_clock_accuracy,
				ptp_translate_accuracy_to_float(snapshot->parent.grandmaster_clock_accuracy),
				snapshot->parent.grandmaster_priority1,
				snapshot->parent.grandmaster_priority2,
				snapshot->time.ptp_timescale? "tai": "utc",
				snapshot->time.current_utc_offset_valid?
				snapshot->time.current_utc_offset: 0,
				snapshot->time.leap59,
				snapshot->time.leap61,
				instance->clustering_score_snapshot,
				sfptpd_clock_get_diff_method(instance->intf->clock));
			break;

		case PTPD_MASTER:
		case PTPD_PASSIVE:
			format = "instance: %s\n"
				"clock-name: %s\n"
				"clock-id: %s\n"
				"state: %s\n"
				"alarms: %s\n"
				"control-flags: %s\n"
				"interface: %s (%s)\n"
				"transport: %s\n"
				"profile: " SFPTPD_FORMAT_EUI48 " (%s) %s\n"
				"timestamping: %s\n"
				"in-sync: %d\n"
				"ptp-domain: %d\n"
				"steps-removed: %d\n"
				"delay-mechanism: %s\n"
				"two-step: %s\n"
				"grandmaster-id: " SFPTPD_FORMAT_EUI64 "\n"
				"clock-class: %d\n"
				"clock-accuracy: %d (<%0.0Lfns)\n"
				"bmc-priority1: %d\n"
				"bmc-priority2: %d\n"
				"timescale: %s\n"
				"current-utc-offset: %d\n"
				"leap-59: %d\n"
				"leap-61: %d\n";

			sfptpd_log_write_state(instance->intf->clock,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				format,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				sfptpd_clock_get_long_name(instance->intf->clock),
				sfptpd_clock_get_hw_id_string(instance->intf->clock),
				ptp_state_text(snapshot->port.state, snapshot_alarms),
				alarms, flags,
				sfptpd_interface_get_name(instance->intf->bond_info.active_if),
				instance->intf->bond_info.logical_if,
				instance->intf->transport_name,
				profile->id[0], profile->id[1], profile->id[2],
				profile->id[3], profile->id[4], profile->id[5],
				profile->name, profile->version,
				hw_ts? "hw": "sw",
				instance->synchronized,
				snapshot->port.domain_number,
				snapshot->current.steps_removed,
				delay_mechanism,
				snapshot->current.two_step? "yes": "no",
				gm_id[0], gm_id[1], gm_id[2], gm_id[3],
				gm_id[4], gm_id[5], gm_id[6], gm_id[7],
				snapshot->parent.grandmaster_clock_class,
				snapshot->parent.grandmaster_clock_accuracy,
				ptp_translate_accuracy_to_float(snapshot->parent.grandmaster_clock_accuracy),
				snapshot->parent.grandmaster_priority1,
				snapshot->parent.grandmaster_priority2,
				snapshot->time.ptp_timescale? "tai": "utc",
				snapshot->time.current_utc_offset_valid?
				snapshot->time.current_utc_offset: 0,
				snapshot->time.leap59,
				snapshot->time.leap61);
			break;

		default:
			sfptpd_log_write_state(instance->intf->clock,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"instance: %s\n"
				"clock-name: %s\n"
				"clock-id: %s\n"
				"state: %s\n"
				"alarms: %s\n"
				"control-flags: %s\n"
				"interface: %s (%s)\n"
				"transport: %s\n"
				"timestamping: %s\n"
				"delay-mechanism: %s\n",
				SFPTPD_CONFIG_GET_NAME(instance->config),
				sfptpd_clock_get_long_name(instance->intf->clock),
				sfptpd_clock_get_hw_id_string(instance->intf->clock),
				ptp_state_text(snapshot->port.state, snapshot_alarms),
				alarms, flags,
				sfptpd_interface_get_name(instance->intf->bond_info.active_if),
				instance->intf->bond_info.logical_if,
				instance->intf->transport_name,
				hw_ts? "hw": "sw",
				delay_mechanism);
			break;
		}

		/* If we consider the clock to be in sync, save the frequency adjustment */
		if (instance->synchronized &&
		    (instance->ctrl_flags & SYNC_MODULE_CLOCK_CTRL)) {
			(void)sfptpd_clock_save_freq_correction(instance->intf->clock,
								instance->ptpd_port_snapshot.current.frequency_adjustment);
		}

		if (nodes_log != NULL) {
			ptp_write_ptp_nodes(sfptpd_log_file_get_stream(nodes_log), instance);
		}
	}

	sfptpd_log_file_close(nodes_log);

	/*Clear the tables*/
	for(interface = ptp->intf_list; interface; interface = interface->next) {
		sfptpd_ht_clear_entries(interface->ptpd_intf_private->nodeSet);
	}

	SFPTPD_MSG_FREE(msg);
}


static void ptp_on_write_topology(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;
	FILE *stream;
	char alarms[256];
	int steps_removed;
	ptpd_state_e state;
	uint8_t *gm, *p;
	bool boundary;
	char *steps_format;
	sfptpd_time_t ofm_ns;
	bool hw_ts;

	assert(ptp != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_ptp_instance *) msg->u.write_topology_req.instance_handle;

	assert(instance);
	assert(ptp_is_instance_valid(ptp, instance));

	assert(instance->intf->clock != NULL);

	/* This should only be called on selected instances */
	assert(instance->ctrl_flags & SYNC_MODULE_SELECTED);

	stream = msg->u.write_topology_req.stream;
	assert(stream != NULL);

	state = instance->ptpd_port_snapshot.port.state;

	p = instance->ptpd_port_snapshot.parent.clock_id;

	ofm_ns = instance->ptpd_port_snapshot.current.offset_from_master;

	hw_ts = ptp_is_instance_hw_timestamping(instance);

	/* If the parent clock ID is not the same as the grandmaster clock ID
	 * then there is an intermediary between the grandmaster and the slave
	 * e.g. one or more boundary clocks. In this case we want to draw the
	 * topology diagram to reflect this */
	if (memcmp(instance->ptpd_port_snapshot.parent.clock_id,
		   instance->ptpd_port_snapshot.parent.grandmaster_id,
		   sizeof(instance->ptpd_port_snapshot.parent.clock_id)) != 0) {
		/* If there is more than one boundary clock between us and the
		 * Grandmaster, indicate this on the diagram using a steps
		 * removed label. In this case we indicate the number of steps
		 * between the Grandmaster and our parent. */
		steps_removed = instance->ptpd_port_snapshot.current.steps_removed - 1;
		steps_format = (steps_removed >= 2)? "%d steps": NULL;
		gm = instance->ptpd_port_snapshot.parent.grandmaster_id;
		boundary = true;
	} else {
		/* This path is not necessary but placates -Werror=maybe-uninitialised */
		steps_removed = 0;
		steps_format = NULL;
		gm = (uint8_t [8]) {0, 0, 0, 0, 0, 0, 0, 0};
		boundary = false;
	}

	fprintf(stream, "====================\nstate: %s\n",
		ptp_state_text(state, ptp_get_alarms_snapshot(instance)));

	if (ptp_get_alarms_snapshot(instance) != 0) {
		sfptpd_sync_module_alarms_text( ptp_get_alarms_snapshot(instance),
						alarms, sizeof(alarms));
		fprintf(stream, "alarms: %s\n", alarms);
	}

	fprintf(stream,
		"interface: %s (%s)\n"
		"timestamping: %s\n"
		"====================\n\n",
		sfptpd_interface_get_name(instance->intf->bond_info.active_if),
		instance->intf->bond_info.logical_if, hw_ts? "hw": "sw");

	switch (state) {
	case PTPD_LISTENING:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "?");
		break;

	case PTPD_SLAVE:
	case PTPD_PASSIVE:
		sfptpd_log_topology_write_field(stream, true, "grandmaster");
		if (boundary) {
			sfptpd_log_topology_write_field(stream, true,
							SFPTPD_FORMAT_EUI64,
							gm[0], gm[1], gm[2], gm[3],
							gm[4], gm[5], gm[6], gm[7]);
			sfptpd_log_topology_write_1to1_connector(stream, false, true,
								 steps_format, steps_removed);
			sfptpd_log_topology_write_field(stream, true, "parent");
		}
		sfptpd_log_topology_write_field(stream, true,
						SFPTPD_FORMAT_EUI64 "/%d",
						p[0], p[1], p[2], p[3],
						p[4], p[5], p[6], p[7],
						instance->ptpd_port_snapshot.parent.port_num);
		if (instance->ptpd_port_snapshot.current.transparent_clock) {
			sfptpd_log_topology_write_1to1_connector(stream, false, true, NULL);
			sfptpd_log_topology_write_field(stream, true, "transparent");
			sfptpd_log_topology_write_field(stream, true, "clock");
		}
		if (state == PTPD_PASSIVE) {
			sfptpd_log_topology_write_1to1_connector(stream, false, true, "zzz");
		} else {
			sfptpd_log_topology_write_1to1_connector(stream, false, true,
								 SFPTPD_FORMAT_TOPOLOGY_FLOAT,
								 ofm_ns);
		}
		break;

	case PTPD_MASTER:
		sfptpd_log_topology_write_1to1_connector(stream, true, false, NULL);
		break;

	default:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "X");
		break;
	}

	sfptpd_log_topology_write_field(stream, true,
					sfptpd_clock_get_long_name(instance->intf->clock));
	sfptpd_log_topology_write_field(stream, true,
					sfptpd_clock_get_hw_id_string(instance->intf->clock));

	SFPTPD_MSG_REPLY(msg);
}


static void ptp_on_stats_end_period(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;

	assert(ptp != NULL);
	assert(msg != NULL);

	for (instance = ptp_get_first_instance(ptp); instance; instance = ptp_get_next_instance(instance)) {
		assert(instance->intf->clock != NULL);

		sfptpd_stats_collection_end_period(&instance->stats,
						   &msg->u.stats_end_period_req.time);

		/* Write the historical statistics to file */
		sfptpd_stats_collection_dump(&instance->stats, instance->intf->clock,
									SFPTPD_CONFIG_GET_NAME(instance->config));
	}

	/* Write remote monitoring stats */
	if (ptp->remote_monitor != NULL) {
		sfptpd_ptp_monitor_flush(ptp->remote_monitor);
	}

	SFPTPD_MSG_FREE(msg);
}


static void ptp_on_test_mode(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_instance *instance;
	sfptpd_ptp_module_config_t *config;
	enum sfptpd_test_id id;

	assert(ptp != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_ptp_instance *)msg->u.test_mode_req.instance_handle;
	assert(instance);
	assert(ptp_is_instance_valid(ptp, instance));

	assert(instance->ptpd_port_private != NULL);

	config = instance->config;
	id = msg->u.test_mode_req.id;

	switch (id) {
	case SFPTPD_TEST_ID_UTC_OFFSET:
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: UTC offset test mode can only be used in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		/* Modify the UTC offset by the value of param0. Reconfigure
		 * PTPD to use the new value */
		config->ptpd_port.timeProperties.currentUtcOffset += msg->u.test_mode_req.params[0];

		/* Compensate for the fact that the NIC clocks have real UTC,
		 * not fake UTC. */

		if (ptpd_test_set_utc_offset(instance->ptpd_port_private,
					     config->ptpd_port.timeProperties.currentUtcOffset,
					     msg->u.test_mode_req.params[0]) == 0) {
			NOTICE("test-mode: set UTC offset = %d\n",
			       config->ptpd_port.timeProperties.currentUtcOffset);
		}
		break;

	case SFPTPD_TEST_ID_TIMESTAMP_JITTER:
	{
		int max_jitter, interval;
		enum bad_timestamp_types type;

		type = (enum bad_timestamp_types) ((ptpd_test_get_bad_timestamp_type(instance->ptpd_port_private) + 1) % BAD_TIMESTAMP_TYPE_MAX);

		if (type == BAD_TIMESTAMP_TYPE_CORRUPTED) {
			interval = 120;
			max_jitter = 500000000;
		} else if (type == BAD_TIMESTAMP_TYPE_DEFAULT) {
			interval = 16;
			max_jitter = 50000000;
		} else if (type == BAD_TIMESTAMP_TYPE_MILD) {
			interval = 1;
			max_jitter = 10000;
		} else {
			interval = 16;
			max_jitter = 0;
		}

		/* When enabled, set timestamp jitter to random jitter of up to
		 * 50ms every sixteen packets */
		if (ptpd_test_set_bad_timestamp(instance->ptpd_port_private, type, interval, max_jitter) == 0) {
			NOTICE("test-mode: timestamp jitter set to type %d\n", type);
		}
	}
		break;

	case SFPTPD_TEST_ID_TRANSPARENT_CLOCK:
	{
		int max_correction;

		instance->test.transparent_clock = !instance->test.transparent_clock;
		max_correction = instance->test.transparent_clock? 1000000: 0;

		/* When enabled, transparent clock emulation has a random correction
		 * field up to 1ms. */
		if (ptpd_test_set_transparent_clock_emulation(instance->ptpd_port_private, max_correction) == 0) {
			NOTICE("test-mode: transparent clock emulation %s\n",
			       instance->test.transparent_clock?
			       "enabled with up to 1ms correction field": "disabled");
		}
	}
		break;

	case SFPTPD_TEST_ID_BOUNDARY_CLOCK_CHANGE:
	{
		uint8_t gm[8];
		uint32_t sr;

		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: Boundary clock test mode can only be used in "
				"PTP master mode\n", 
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		memcpy(gm, instance->ptpd_port_snapshot.parent.clock_id, sizeof(gm));

		/* Cycle through states:
		 *  state 0: gm = parent, sr = 0
		 *  state 1: gm != parent, sr = 0
		 *  state 2: gm != parent, sr = 1
		 *  state 3: gm != parent, sr = 2
		 */
		instance->test.boundary_clock_state++;
		if (instance->test.boundary_clock_state >= 4)
			instance->test.boundary_clock_state = 0;

		switch (instance->test.boundary_clock_state) {
		case 0: default: sr = 0; break;
		case 1: gm[0] ^= 0xff; sr = 0; break;
		case 2: gm[0] ^= 0xff; sr = 1; break;
		case 3: gm[0] ^= 0xff; sr = 2; break;
		}

		if (ptpd_test_set_boundary_clock_emulation(instance->ptpd_port_private, gm, sr) == 0) {
			NOTICE("test-mode: boundary clock emulation: gm %s "
			       "parent and steps removed = %d\n",
			       (instance->test.boundary_clock_state == 0)? "=": "!=", sr);
		}
	}
		break;

	case SFPTPD_TEST_ID_GRANDMASTER_CLOCK_CHANGE:
	{
		uint8_t class, accuracy, p1, p2;
		uint16_t oslv;
		
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: Grandmaster clock test mode can only be used "
				"in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		class = config->ptpd_port.clockQuality.clockClass;
		accuracy = (uint8_t)config->ptpd_port.clockQuality.clockAccuracy;
		oslv = config->ptpd_port.clockQuality.offsetScaledLogVariance;
		p1 = config->ptpd_port.priority1;
		p2 = config->ptpd_port.priority2;

		/* Cycle through states. In each case, change attribute by 2 steps:
		 *  state 0: default configuration
		 *  state 1: priority2 raised
		 *  state 2: offset scaled log variance reduced
		 *  state 3: accuracy increased
		 *  state 4: class improved
		 *  state 5: priority1 raised
		 */
		instance->test.grandmaster_clock_state++;
		if (instance->test.grandmaster_clock_state >= 6)
			instance->test.grandmaster_clock_state = 0;

		switch (instance->test.grandmaster_clock_state) {
		case 0: default: break;
		case 1: p2 -= 2; break;
		case 2: oslv -= 2; break;
		case 3: accuracy -= 2; break;
		case 4: class -= 2; break;
		case 5: p1 -= 2; break;
		}

		if (ptpd_test_change_grandmaster_clock(instance->ptpd_port_private, class, accuracy,
						       oslv, p1, p2) == 0) {
			NOTICE("test-mode: grandmaster clock change: class %d, "
			       "accuracy %d, o.s.l.v %d, priority1 %d, priority2 %d\n",
			       class, accuracy, oslv, p1, p2);
		}
	}
		break;

	case SFPTPD_TEST_ID_NO_ANNOUNCE_PKTS:
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: No Announce Packets test mode can only be used "
				"in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		instance->test.no_announce_pkts = !instance->test.no_announce_pkts;

		if (ptpd_test_pkt_suppression(instance->ptpd_port_private,
					      instance->test.no_announce_pkts,
					      instance->test.no_sync_pkts,
					      instance->test.no_follow_ups,
					      instance->test.no_delay_resps) == 0) {
			NOTICE("test-mode: no announce pkts: %sabled\n",
			       instance->test.no_announce_pkts? "en": "dis");
		}
		break;

	case SFPTPD_TEST_ID_NO_SYNC_PKTS:
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: No Sync Packets test mode can only be used "
				"in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		instance->test.no_sync_pkts = !instance->test.no_sync_pkts;

		if (ptpd_test_pkt_suppression(instance->ptpd_port_private,
					      instance->test.no_announce_pkts,
					      instance->test.no_sync_pkts,
					      instance->test.no_follow_ups,
					      instance->test.no_delay_resps) == 0) {
			NOTICE("test-mode: no sync pkts: %sabled\n",
			       instance->test.no_sync_pkts? "en": "dis");
		}
		break;

	case SFPTPD_TEST_ID_NO_FOLLOW_UPS:
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: No Follow Ups test mode can only be used "
				"in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}

		instance->test.no_follow_ups = !instance->test.no_follow_ups;

		if (ptpd_test_pkt_suppression(instance->ptpd_port_private,
					      instance->test.no_announce_pkts,
					      instance->test.no_sync_pkts,
					      instance->test.no_follow_ups,
					      instance->test.no_delay_resps) == 0) {
			NOTICE("test-mode: no follow ups: %sabled\n",
			       instance->test.no_follow_ups? "en": "dis");
		}
		break;

	case SFPTPD_TEST_ID_NO_DELAY_RESPS:
		if (instance->ptpd_port_snapshot.port.state != PTPD_MASTER) {
			WARNING("ptp %s: No Delay Responses test mode is generally only "
				"useful in PTP master mode\n",
				SFPTPD_CONFIG_GET_NAME(instance->config));
		}

		instance->test.no_delay_resps = !instance->test.no_delay_resps;

		if (ptpd_test_pkt_suppression(instance->ptpd_port_private,
					      instance->test.no_announce_pkts,
					      instance->test.no_sync_pkts,
					      instance->test.no_follow_ups,
					      instance->test.no_delay_resps) == 0) {
			NOTICE("test-mode: no delay resps: %sabled\n",
			       instance->test.no_delay_resps? "en": "dis");
		}
		break;

	default:
		break;
	}

fail:
	SFPTPD_MSG_FREE(msg);
}


static void ptp_on_link_table(sfptpd_ptp_module_t *ptp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_ptp_intf *interface;
	const struct sfptpd_link_table *link_table;
	bool bond_changed;
	int rc;

	assert(ptp != NULL);
	assert(msg != NULL);

	link_table = msg->u.link_table_req.link_table;
	SFPTPD_MSG_FREE(msg);

	sfptpd_link_table_free_copy(&ptp->link_table);
	if ((rc = sfptpd_link_table_copy(link_table, &ptp->link_table)) != 0) {
		sfptpd_thread_exit(rc);
		return;
	}

	for(interface = ptp->intf_list; interface; interface = interface->next) {

		/* Check for an interface change. If this fails it
		 * is a fatal error and we have to exit. */
		rc = ptp_handle_bonding_interface_change(interface, &bond_changed);
		if (rc != 0) {
			/* We can't carry on in this case */
			sfptpd_thread_exit(rc);
			return;
		}

		/* Set the flag to be picked up by the timer tick */
		if (!interface->bond_changed && bond_changed) {
			interface->bond_changed = true;
		}
	}

	sfptpd_engine_link_table_release(ptp->engine, link_table);
}


static bool ptp_measure_offset_from_discriminator(struct sfptpd_ptp_instance *instance,
						  sfptpd_time_t *result)
{
	struct timespec discrim_to_instance_lrc;
	bool discriminator_valid = false;
	int rc = 0;

	/* Find in current time offset for the BMC discriminator
	 * if one has been defined. */
	if (instance->discriminator_type == DISC_SYNC_INSTANCE) {
		sfptpd_sync_instance_status_t status = { 0 };

		/* When a sync instance is defined as the discriminator,
		   obtain the offset of the remote clock from this instance's LRC */
		rc = sfptpd_sync_module_get_status(instance->discriminator.sync_instance->module,
						   instance->discriminator.sync_instance->handle,
						   &status);
		if ((rc == 0) &&
		    ((status.offset_from_master.tv_sec != 0) ||
		     (status.offset_from_master.tv_nsec != 0))) {
			struct timespec discrim_lrc_to_instance_lrc;
			rc = sfptpd_clock_compare(status.clock,
						  instance->intf->clock,
						  &discrim_lrc_to_instance_lrc);
			sfptpd_time_subtract(&discrim_to_instance_lrc,
					     &discrim_lrc_to_instance_lrc,
					     &status.offset_from_master);
			discriminator_valid = true;
		}
	} else if (instance->discriminator_type == DISC_CLOCK) {
		/* When a local clock is defined as the discriminator,
		   use its offset from this instance's LRC */
		rc = sfptpd_clock_compare(instance->discriminator.clock,
					  instance->intf->clock,
					  &discrim_to_instance_lrc);
		if (rc == 0)
			discriminator_valid = true;
	}

	if (discriminator_valid) {
		TRACE_L5("ptp: measured offset from BMC discriminator to %s lrc of %22ld.%09ld\n",
			 SFPTPD_CONFIG_GET_NAME(instance->config),
			 discrim_to_instance_lrc.tv_sec,
			 discrim_to_instance_lrc.tv_nsec);
		*result = sfptpd_time_timespec_to_float_ns(&discrim_to_instance_lrc);
	} else if (instance->discriminator_type != DISC_NONE) {
		TRACE_L4("ptp: could not measure offset from BMC discriminator for %s%s%s\n",
			 SFPTPD_CONFIG_GET_NAME(instance->config),
			 rc ? ", " : "",
			 rc ? strerror(rc) : "");
	}

	return discriminator_valid;
}


static void ptp_on_timer(void *user_context, unsigned int id)
{
	struct sfptpd_ptp_intf *interface;
	struct sfptpd_ptp_instance *instance;
	sfptpd_ptp_module_t *ptp = (sfptpd_ptp_module_t *)user_context;

	assert(ptp != NULL);

	for(interface = ptp->intf_list; interface; interface = interface->next) {
		for (instance = interface->instance_list; instance; instance = instance->next) {
			sfptpd_time_t discrim_to_instance_lrc = 0.0L;
			bool discriminator_valid;

			/* Pass offset from BMC discriminator clock */
			discriminator_valid = ptp_measure_offset_from_discriminator(instance,
										    &discrim_to_instance_lrc);
			instance->ptpd_port_private->discriminator_valid = discriminator_valid;
			instance->ptpd_port_private->discriminator_offset = discrim_to_instance_lrc;

			/* Process the timer tick. Note that we pass the control
			 * flags into this function because the timer tick
			 * is responsible for restarting the instance if a
			 * fault has occurred. In this case the control flags
			 * need to be restored. */
			ptpd_timer_tick(instance->ptpd_port_private,
					instance->ctrl_flags);
		}

		/* update the state */
		ptp_update_interface_state(interface);
	}
}


/* Finalise the contents of an interface. There must be no extant
   instances on it. */
static void ptp_destroy_interface(struct sfptpd_ptp_module *ptp,
				  struct sfptpd_ptp_intf *interface) {
	struct sfptpd_ptp_intf **ptr;

	assert(ptp != NULL);
	assert(interface != NULL);
	assert(interface->instance_list == NULL);

	/* Deconfigure timestamping filtering on the interface */
	if (interface->start_successful) {
		ptp_timestamp_filtering_deconfigure_all(interface);
	}

	/* Remove from module */
	for (ptr = &ptp->intf_list; *ptr && *ptr != interface; ptr = &(*ptr)->next);
	assert(*ptr);
	*ptr = interface->next;

	/* Now we can free the interface */
	if (interface->start_successful && interface->ptpd_intf_private != NULL) {
		ptpd_interface_destroy(interface->ptpd_intf_private);
	}
	free(interface);
}


/* Destroy an instance. */
static void ptp_destroy_instance(struct sfptpd_ptp_module *ptp,
				 struct sfptpd_ptp_instance *instance) {
	struct sfptpd_ptp_intf *interface;
	struct sfptpd_ptp_instance **ptr;

	assert(instance != NULL);

	/* Delete the PTPD instance */
	if (instance->ptpd_port_private != NULL) {
		ptpd_port_destroy(instance->ptpd_port_private);
	}

	/* Free the statistics collection */
	sfptpd_stats_collection_free(&instance->stats);

	/* Depending how early this function is called, it's not guaranteed
	 * that we will have an interface. */
	interface = instance->intf;
	if (interface != NULL) {
		/* Remove from instance from the interface */
		for (ptr = &interface->instance_list; *ptr && *ptr != instance; ptr = &(*ptr)->next);
		assert(*ptr);
		*ptr = instance->next;

		/* If this was the last instance on the interface, then free
		 * the interface, just as it was implicitly created by the
		 * first instance */
		if (interface->instance_list == NULL)
			ptp_destroy_interface(ptp, interface);
	}

	/* Now we can free the instance */
	free(instance);
}


/* Finds an interface name and transport type. (Where an interface is used with
 * more than one transport there is an object for each transport.) The transport
 * argument is the unix addressing family AF_INET, AF_INET6, etc.
 */
static struct sfptpd_ptp_intf *ptp_find_interface_by_name_transport(sfptpd_ptp_module_t *ptp,
								    const char *name,
								    int transport) {
	struct sfptpd_ptp_intf *interface;

	/* Walk linked list, looking for the interface */
	for (interface = ptp->intf_list;
	     interface != NULL;
	     interface = interface->next) {

		if (strcmp(interface->defined_name, name) == 0 &&
		    transport == interface->representative_config->ptpd_intf.transportAF)
			break;
	}
	return interface;
}


static int ptp_ensure_interface_created(sfptpd_ptp_module_t *ptp,
					struct sfptpd_ptp_module_config *instance_config,
					struct sfptpd_ptp_intf **returned_interface) {

	struct sfptpd_ptp_intf *interface;
	const char *interface_name;
	int rc;

	assert(ptp != NULL);
	assert(instance_config != NULL);
	assert(returned_interface != NULL);

	interface_name = instance_config->interface_name;
	assert(interface_name != NULL);

	interface = ptp_find_interface_by_name_transport(ptp, interface_name,
							 instance_config->ptpd_intf.transportAF);
	if (interface != NULL) {
		goto success;
	}

	interface = (struct sfptpd_ptp_intf *) calloc(1, sizeof *interface);
	if (interface == NULL) {
		CRITICAL("ptp: could not allocate interface context for %s\n", interface_name);
		rc = ENOMEM;
		goto fail;
	}

	if (instance_config->ptpd_intf.transportAF == AF_INET) {
		interface->transport_name = "ipv4";
	} else if (instance_config->ptpd_intf.transportAF == AF_INET6) {
		interface->transport_name = "ipv6";
	} else {
		interface->transport_name = "invalid";
	}

	interface->module = ptp;
	interface->defined_name = interface_name;
	interface->representative_config = instance_config;
	interface->next = ptp->intf_list;
	ptp->intf_list = interface;

 success:
	*returned_interface = interface;
	return 0;

 fail:
	free(interface);
	return rc;
}


/** Validate a new instance that is proposed to be added to an interface.
 * @param proposed_intf the interface
 * @param proposed_instance the instance
 * @returns true if valid, otherwise false.
 */
static int ptp_validate_new_instance(struct sfptpd_ptp_intf *proposed_intf,
				     struct sfptpd_ptp_instance *proposed_instance) {

	struct sfptpd_ptp_instance *instance;

	assert(proposed_intf != NULL);
	assert(proposed_instance != NULL);

	/* Check for duplicate PTP domains on the same interface */
	for (instance = proposed_intf->instance_list; instance; instance = instance->next) {
		if ((instance != proposed_instance) &&
		    (instance->config->ptpd_port.domainNumber ==
		     proposed_instance->config->ptpd_port.domainNumber)) {
			CRITICAL("ptp %s: instance not valid for interface %s "
				 "because instance %s is already operating on "
				 "domain %d\n",
				 SFPTPD_CONFIG_GET_NAME(proposed_instance->config),
				 proposed_intf->defined_name,
				 SFPTPD_CONFIG_GET_NAME(instance->config),
				 proposed_instance->config->ptpd_port.domainNumber);
			return EBUSY;
		}
	}

	return 0;
}


static void ptp_destroy_instances(sfptpd_ptp_module_t *ptp) {
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_ptp_instance *next;

	for (instance = ptp_get_first_instance(ptp);
	     instance != NULL;
	     instance = next) {

		next = ptp_get_next_instance(instance);
		ptp_destroy_instance(ptp, instance);
	}
}


static int ptp_create_instances(struct sfptpd_config *config,
				sfptpd_ptp_module_t *ptp)
{
	sfptpd_ptp_module_config_t *instance_config;
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_ptp_intf *intf;
	int rc;

	assert(config != NULL);
	assert(ptp != NULL);
	assert(ptp->intf_list == NULL);

	/* Setting up initial state: find the first instance configuration */
	instance_config = (struct sfptpd_ptp_module_config *)
		sfptpd_config_category_first_instance(config,
						      SFPTPD_CONFIG_CATEGORY_PTP);

	/* Loop round available instance configurations */
	while (instance_config) {
		INFO("ptp %s: creating sync-instance\n",
		     SFPTPD_CONFIG_GET_NAME(instance_config));

		instance = (struct sfptpd_ptp_instance *)calloc(1, sizeof *instance);
		if (instance == NULL) {
			CRITICAL("ptp %s: failed to allocate sync instance memory\n",
				 SFPTPD_CONFIG_GET_NAME(instance_config));
			rc = ENOMEM;
			goto fail;
		}

		/* Populate instance state */
		instance->config = instance_config;

		/* Find/create interface state */
		rc = ptp_ensure_interface_created(ptp, instance_config, &intf);
		assert(rc == 0);
		instance->intf = intf;

		/* Insert in linked list */
		instance->next = intf->instance_list;
		intf->instance_list = instance;

		/* Check configuration constraints */
		rc = ptp_validate_new_instance(intf, instance);
		if (rc != 0)
			goto fail;

		/* Set default profile as appropriate */
		if (instance_config->profile == SFPTPD_PTP_PROFILE_UNDEF) {
			if (instance_config->ptpd_port.delayMechanism == PTPD_DELAY_MECHANISM_P2P)
				instance_config->profile = SFPTPD_PTP_PROFILE_DEFAULT_P2P;
			else
				instance_config->profile = SFPTPD_PTP_PROFILE_DEFAULT_E2E;
		}
		instance_config->ptpd_port.profile = sfptpd_ptp_get_profile_def(instance_config->profile);

		/* Set version-based defaults */
		if (instance_config->ptpd_port.ptp_version_minor < 1) {
			instance_config->ptpd_port.comm_caps_tlv_enabled = FALSE;
		}

		TRACE_L3("ptp %s: instance is %p\n",
			 SFPTPD_CONFIG_GET_NAME(instance_config),
			 instance);

		/* Get next configuration, if present */
		instance_config = (struct sfptpd_ptp_module_config *)
			sfptpd_config_category_next_instance(&instance_config->hdr);
	}

	return 0;

fail:
	ptp_destroy_instances(ptp);
	return rc;
}


static int ptp_validate_interface(sfptpd_ptp_module_t *ptp,
				  struct sfptpd_ptp_intf *suspect,
				  bool already_started)
{
	struct sfptpd_ptp_intf *intf;
	struct sfptpd_interface *interface;
	unsigned int i, j;

	assert(ptp != NULL);
	assert(suspect != NULL);

	/* If the interface is already started, then a second instance is using
	 * the same interface. We disallow this if running on a system that does
	 * not support so timestamping or one of the underlying physical
	 * interfaces is siena-based. */
	if (already_started) {
		for (i = 0; i < suspect->bond_info.num_physical_ifs; i++) {
			interface = suspect->bond_info.physical_ifs[i];

			if (sfptpd_interface_is_siena(interface)) {
				CRITICAL("ptp: more than one ptp instance "
					 "using physical interface %s. This "
					 "is not supported on Solarflare "
					 "SFN5322F and SFN6322F adapters\n",
					 sfptpd_interface_get_name(interface));
				return EBUSY;
			}
		}
	}

	/* Iterate through all the started interfaces and check that no other
	 * interface is using the one of the same physical interfaces. If it
	 * is, we only allow this if so timestamping is supported and the
	 * adapter is not siena-based.
	 *
	 * Yes this is ugly. The longer term the plan is to extend the
	 * sfptpd_interface class to include logical interfaces and express the
	 * bonding/vlan hierarchy through interface objects. At this point,
	 * usage of physical interfaces can be reference counted. */
	for (intf = ptp->intf_list; intf != NULL; intf = intf->next) {
		if ((intf != suspect) && (intf->start_attempted)) {
			for (i = 0; i < suspect->bond_info.num_physical_ifs; i++) {
				interface = suspect->bond_info.physical_ifs[i];

				for (j = 0; j < intf->bond_info.num_physical_ifs; j++) {
					if (interface == intf->bond_info.physical_ifs[j]) {
						if (sfptpd_interface_is_siena(interface)) {
							CRITICAL("ptp: more than one ptp instance using "
								 "physical interface %s. This is not "
								 "supported on Solarflare SFN5322F and "
								 "SFN6322F adapters\n",
								 sfptpd_interface_get_name(interface));
							return EBUSY;
						}
					}
				}
			}
		}
	}

	return 0;
}


static int ptp_ensure_interface_started(sfptpd_ptp_module_t *ptp,
					struct sfptpd_ptp_intf *interface) {

	int rc;

	assert(ptp != NULL);
	assert(interface != NULL);

	if (interface->start_attempted) {
		return ptp_validate_interface(ptp, interface, true);
	}

	/* We don't want to retry if there was an error the first time
	   we tried to be started. */
	interface->start_attempted = true;
	
	/* Parse the interface topology to work out if we are using VLANs and/or
	 * bonds */
	rc = ptp_parse_interface_topology(&interface->bond_info, interface->defined_name, &ptp->link_table);
	if (rc != 0) {
		CRITICAL("ptp: error parsing interface topology for %s (configured logical interface must exist), %s\n",
			 interface->defined_name, strerror(rc));
		return rc;
	}

	rc = ptp_validate_interface(ptp, interface, false);
	if (rc != 0)
		return rc;

	/* Determine and configure the PTP clock. */
	rc = ptp_configure_clock(interface);
	if (rc != 0) {
		CRITICAL("ptp: failed to configure clock for interface %s\n",
			 interface->defined_name);
		return rc;
	}

	interface->start_successful = true;
	return 0;
}


static int ptp_start_instance(struct sfptpd_ptp_instance *instance) {
	int rc = 0;
	struct sfptpd_ptp_module_config *config;
	struct sfptpd_ptp_intf *interface;

	config = instance->config;
	assert(config != NULL);

	/* Do any extra PTPD configuration that isn't handled as part of the
	 * default config or config file parsing */
	ptp_configure_ptpd(config);

	/* Register the PTP remote monitor with all instances if configured */
	if (config->remote_monitor) {
		struct ptpd_remote_stats_logger *logger = &config->ptpd_port.remoteStatsLogger;
		logger->log_rx_sync_timing_data_fn = sfptpd_ptp_monitor_update_rx_timing;
		logger->log_rx_sync_computed_data_fn = sfptpd_ptp_monitor_update_rx_computed;
		logger->log_tx_event_timestamps_fn = sfptpd_ptp_monitor_log_tx_timestamp;
		logger->log_slave_status_fn = sfptpd_ptp_monitor_update_slave_status;
		logger->context = instance->intf->module->remote_monitor;
	}

	/* Set up the critical stats logging callback */
	config->ptpd_port.criticalStatsLogger.log_fn = ptp_critical_stats_update;
	config->ptpd_port.criticalStatsLogger.private = instance;

        /* Set up the clustering evaluator callback */
	config->ptpd_port.clusteringEvaluator.calc_fn = sfptpd_engine_calculate_clustering_score;
	config->ptpd_port.clusteringEvaluator.comp_fn = sfptpd_engine_compare_clustering_guard_threshold;
	config->ptpd_port.clusteringEvaluator.private = instance->intf->module->engine;
	config->ptpd_port.clusteringEvaluator.instance_name = SFPTPD_CONFIG_GET_NAME(instance->config);

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. */
	instance->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Start the ptpd protocol on the interface if not already started for
	   another domain. */
	interface = ptp_find_interface_by_name_transport(instance->intf->module, config->interface_name,
							 config->ptpd_intf.transportAF);
	assert(interface);
	rc = ptp_ensure_interface_started(instance->intf->module, interface);
	if (rc != 0) {
		CRITICAL("ptp %s: could not start interface %s\n",
			 SFPTPD_CONFIG_GET_NAME(instance->config),
			 config->interface_name);
		goto fail;
	}

	if (instance->intf->bond_info.num_physical_ifs == 0)
		SYNC_MODULE_ALARM_SET(instance->local_alarms, NO_INTERFACE);

	rc = ptp_stats_init(instance);
	if (rc != 0) {
		CRITICAL("ptp %s: failed to create PTP stats\n",
			 SFPTPD_CONFIG_GET_NAME(instance->config));
		goto fail;
	}

	/* Initialise the sync module convergence and stats */
	ptp_convergence_init(instance);

	/* Set the convergence threshold */
	ptp_set_convergence_threshold(instance);

fail:
	return rc;
}


static int ptp_start_interface(struct sfptpd_ptp_intf *interface) {
	int rc = 0;
	int fd;

	/* The recorded file descriptors may be -1 if there is
	   no physical interface, e.g. because we have an empty
	   bond. This can be acceptable in a fault-tolerant
	   environment and is dealt with at a higher level
	   according to policy. */

	if ((fd = interface->ptpd_intf_fds.event_sock) != -1) {
		rc = sfptpd_thread_user_fd_add(fd, true, false);

		if (rc != 0) {
			CRITICAL("ptp: failed to add event socket to thread epoll set, %s\n",
				 strerror(rc));
			goto fail;
		}
	}

	if ((fd = interface->ptpd_intf_fds.general_sock) != -1) {
		rc = sfptpd_thread_user_fd_add(fd, true, false);

		if (rc != 0) {
			CRITICAL("ptp: failed to add general socket to thread epoll set, %s\n",
				 strerror(rc));
			goto fail;
		}
	}

 fail:
	return rc;
}


/* Set configuration items on the interface that depend on
   the configuration of all of the instances using them. */
static void ptp_intf_aggregate_instance_requirements(struct sfptpd_ptp_intf *interface) {
	struct sfptpd_ptp_instance *instance;
	struct ptpd_intf_config *intf_config;

	assert(interface);

	instance = interface->instance_list;

	if (instance != NULL) {
		assert(instance->config);
		
		intf_config = &instance->config->ptpd_intf;
		intf_config->multicast_needed = false;

		do {
			PortCommunicationCapabilities *caps = &instance->config->ptpd_port.comm_caps;
			if (caps->syncCapabilities & PTPD_COMM_MULTICAST_CAPABLE ||
			    caps->delayRespCapabilities & PTPD_COMM_MULTICAST_CAPABLE) {
				intf_config->multicast_needed = true;
			}
			instance = instance->next;
		} while (instance);
	}
}


static int ptp_on_startup(void *context)
{
	sfptpd_ptp_module_t *ptp = (sfptpd_ptp_module_t *)context;
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_ptp_intf *interface;
	sfptpd_ptp_module_config_t *config;
	int rc;
	/* Due to the way SO_TIMESTAMP is handled through the loopback interface,
	 * traffic could be received by the wrong instance in some cases.  Because
	 * of this, we refuse to run if there is more than one instance doing
	 * software timestamping.  See bug74320. */
	int sw_ts_instances = 0;

	assert(ptp != NULL);

	/* Find any instance to get the global configuration */
	instance = ptp_get_first_instance(ptp);
	assert(instance);
	config = instance->config;

	/* Start the remote monitor */
	if (config->remote_monitor) {
		ptp->remote_monitor = sfptpd_ptp_monitor_create();
	}

	for (instance = ptp_get_first_instance(ptp); instance; instance = ptp_get_next_instance(instance)) {
		rc = ptp_start_instance(instance);
		if (rc != 0) goto fail;
	}

	/* Initialise PTPD */
	rc = ptpd_init(&ptp->ptpd_global_private);
	if (rc != 0) {
		CRITICAL("ptp: failed to initialise PTPD, %s\n", strerror(rc));
		goto fail;
	}

	for (interface = ptp->intf_list; interface; interface = interface->next) {

		if (!interface->start_successful)
			continue;

		ptp_intf_aggregate_instance_requirements(interface);

		/* Create the PTPD interface instance */
		rc = ptpd_create_interface(&ptp_get_config_for_interface(interface)->ptpd_intf,
					   ptp->ptpd_global_private,
					   &interface->ptpd_intf_private);
		if (rc != 0) {
			CRITICAL("ptp: failed to create PTPD interface instance, %s\n", strerror(rc));
			goto fail;
		}

		for (instance = interface->instance_list; instance; instance = instance->next) {

			/* Create the PTPD port instance */
			rc = ptpd_create_port(&instance->config->ptpd_port,
					      interface->ptpd_intf_private,
					      &instance->ptpd_port_private);
			if (rc != 0) {
				CRITICAL("ptp: failed to create PTPD instance, %s\n", strerror(rc));
				goto fail;
			}

			/* Get an initial snapshot of PTPD's state so that we have something
			 * valid if queried */
			rc = ptpd_get_snapshot(instance->ptpd_port_private, &instance->ptpd_port_snapshot);
			if (rc != 0) {
				CRITICAL("ptp: failed to get PTPD state, %s\n", strerror(rc));
				goto fail;
			}

			/* TODO Low priority but resetting the PPS stats should
			 * only be done once per interface. */
			/* If we are logging PPS stats, reset them now */
			if (instance->config->pps_logging)
				ptp_pps_stats_init(instance);

			/* Count instances doing s/w timestamping via loopback */
			if (interface->ptpd_intf_private->tsMethod == TS_METHOD_SYSTEM) {
				sw_ts_instances++;
			}
		}
	}

	/* SWPTP-790: can't have more than a single instance doing SO_TIMESTAMP */
	if (sw_ts_instances > 1) {
		CRITICAL("ptp: multiple instances not compatible with software timestamping\n");
		rc = ENOTSUP;
		goto fail;
	}

	rc = sfptpd_thread_timer_create(PTP_TIMER_ID, CLOCK_MONOTONIC,
					ptp_on_timer, ptp);
	if (rc != 0) {
		CRITICAL("ptp: failed to create periodic timer, %s\n", strerror(rc));
		goto fail;
	}

	return 0;

fail:
	ptp_destroy_instances(ptp);

	return rc;
}


static void ptp_on_run(sfptpd_ptp_module_t *ptp)
{
	struct sfptpd_ptp_instance *instance;
	struct sfptpd_ptp_intf *interface;
	struct timespec interval;
	int rc;

	assert(ptp->timers_started == false);

	/* Set up the BMC discriminators, if configured.
	 * Deferred from on_startup because other instance references not available
	 * until now. */
	for (instance = ptp_get_first_instance(ptp); instance != NULL; instance = ptp_get_next_instance(instance))
		ptp_setup_discriminator(instance);

	interval.tv_sec = 0;
	interval.tv_nsec = PTP_TIMER_INTERVAL_NS;

	rc = sfptpd_thread_timer_start(PTP_TIMER_ID,
				       true, false, &interval);
	if (rc != 0) {
		CRITICAL("ptp: failed to start periodic timer, %s\n", strerror(rc));

		/* We can't carry on in this case */
		sfptpd_thread_exit(rc);
	}

	for (interface = ptp->intf_list; interface; interface = interface->next) {
		if (!interface->start_successful)
			continue;

		rc = ptpd_get_intf_fds(interface->ptpd_intf_private, &interface->ptpd_intf_fds);
		if (rc != 0) {
			ERROR("ptp: failed to get PTPD interface %s fds, %s\n",
			      instance->intf->bond_info.logical_if,
			      strerror(rc));
			continue;
		}

		rc = ptp_start_interface(interface);
		if (rc != 0)
			ERROR("ptp: failed to start interface %s, %s\n",
			      instance->intf->bond_info.logical_if,
			      strerror(rc));
	}

	ptp->timers_started = true;
}


static void ptp_on_shutdown(void *context)
{
	sfptpd_ptp_module_t *ptp = (sfptpd_ptp_module_t *)context;
	assert(ptp != NULL);

	ptp_destroy_instances(ptp);

	/* Delete the monitor */
	if (ptp->remote_monitor != NULL) {
		sfptpd_ptp_monitor_destroy(ptp->remote_monitor);
	}

	/* Delete the ptpd structures. At this point (after ptp_destroy_instances()
	 * above) there should only be the global context left to free. */
	ptpd_destroy(ptp->ptpd_global_private);

	/* Free copy of link table */
	sfptpd_link_table_free_copy(&ptp->link_table);

	/* Delete the sync module memory */
	free(ptp);
}


static void ptp_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	sfptpd_ptp_module_t *ptp = (sfptpd_ptp_module_t *)context;
	sfptpd_sync_module_msg_t *msg = (sfptpd_sync_module_msg_t *)hdr;

	assert(ptp != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		ptp_on_run(ptp);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_GET_STATUS:
		ptp_on_get_status(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_CONTROL:
		ptp_on_control(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_GM_INFO:
		ptp_on_update_gm_info(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_LEAP_SECOND:
		ptp_on_update_leap_second(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STEP_CLOCK:
		ptp_on_step_clock(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LOG_STATS:
		ptp_on_log_stats(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_SAVE_STATE:
		ptp_on_save_state(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY:
		ptp_on_write_topology(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		ptp_on_stats_end_period(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_TEST_MODE:
		ptp_on_test_mode(ptp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LINK_TABLE:
		ptp_on_link_table(ptp, msg);
		break;

	default:
		WARNING("ptp: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void ptp_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	struct sfptpd_ptp_intf *interface;
	sfptpd_ptp_module_t *ptp = (sfptpd_ptp_module_t *)context;
	bool event, general;
	unsigned int i;

	assert(ptp != NULL);
	assert(fds != NULL);

	for(interface = ptp->intf_list; interface; interface = interface->next) {
		event = false;
		general = false;
		for(i = 0; i < num_fds; i++) {
			if (fds[i] == interface->ptpd_intf_fds.event_sock)
				event = true;
			if (fds[i] == interface->ptpd_intf_fds.general_sock)
				general = true;
		}

		if (event || general) {
			/* Handle the ready sockets */
			ptpd_sockets_ready(interface->ptpd_intf_private,
					   event, general);

			/* check the state */
			ptp_update_interface_state(interface);
		}
	}
}


static const struct sfptpd_thread_ops ptp_thread_ops = 
{
	ptp_on_startup,
	ptp_on_shutdown,
	ptp_on_message,
	ptp_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_ptp_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_table_subscriber)
{
	sfptpd_ptp_module_t *ptp;
	struct sfptpd_ptp_instance *instance;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	TRACE_L3("ptp: creating sync-module\n");

	*sync_module = NULL;
	ptp = (sfptpd_ptp_module_t *)calloc(1, sizeof(*ptp));
	if (ptp == NULL) {
		CRITICAL("ptp: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	/* Keep a handle to the sync engine */
	ptp->engine = engine;

	/* Copy initial link table */
	*link_table_subscriber = true;
	if (sfptpd_link_table_copy(link_table, &ptp->link_table) != 0)
		goto fail1;

	/* Create all the sync instances */
	rc = ptp_create_instances(config, ptp);
	if (rc != 0)
		goto fail2;

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("ptp", &ptp_thread_ops, ptp, sync_module);
	if (rc != 0)
		goto fail3;

	/* If a buffer has been provided, populate the instance information */
	if (instances_info_buffer != NULL) {
		memset(instances_info_buffer, 0,
		       instances_info_entries * sizeof(*instances_info_buffer));

		for (instance = ptp_get_first_instance(ptp);
		     (instance != NULL) && (instances_info_entries > 0);
		     instance = ptp_get_next_instance(instance)) {
			instances_info_buffer->module = *sync_module;
			instances_info_buffer->handle = (struct sfptpd_sync_instance *) instance;
			instances_info_buffer->name = instance->config->hdr.name;
			instances_info_buffer++;
			instances_info_entries--;
		}
	}

	return 0;

fail3:
	ptp_destroy_instances(ptp);
fail2:
	sfptpd_link_table_free_copy(&ptp->link_table);
fail1:
	free(ptp);

	/* ENOENT is treated benignly by the caller but
	   is a genuine error condition for ptp */
	if (rc == ENOENT) {
		rc = EINVAL;
	}
	return rc;
}


/* fin */
