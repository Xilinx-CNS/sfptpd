/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

#ifdef HAVE_GPS

/**
 * @file   sfptpd_gps_module.c
 * @brief  GPS Synchronization Module
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <gps.h>

#include "sfptpd_app.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_clock.h"
#include "sfptpd_thread.h"
#include "sfptpd_interface.h"
#include "sfptpd_statistics.h"
#include "sfptpd_time.h"
#include "sfptpd_engine.h"

#include "sfptpd_gps_module.h"


/****************************************************************************
 * Types
 ****************************************************************************/

struct gps_instance;

enum gps_stats_ids {
	GPS_STATS_ID_OFFSET,
	GPS_STATS_ID_SYNCHRONIZED,
	GPS_STATS_ID_SATELLITES_USED,
	GPS_STATS_ID_SATELLITES_SEEN,
};

struct gps_state {
	/* NTP module state */
	sfptpd_sync_module_state_t state;

	/* Alarms */
	sfptpd_sync_module_alarms_t alarms;

	/* Unique information to identify the offset.
	   This is guaranteed to change each time
	   that NTP calculates a new offset from the selected peer.
	   The offset is invalid if 'valid' is false. */
	struct {
		struct sfptpd_timespec offset_timestamp;
		bool valid;
	} __attribute__((packed)) offset_id_tuple;

	/* Current offset from master or 0 if no peer selected */
	long double offset_from_master;

	/* Time accuracy estimate (ns) */
	long double est_accuracy;

	/* Have fix */
	bool fix;

	/* Number of satellites in fix */
	int sats_used;
	int sats_seen;

	/* Stratum of master or 0 if no peer selected */
	unsigned int stratum;

	/* Boolean indicating that the clock has been stepped and that the
	 * recorded NTP offset may not be correct. The offset will be
	 * invalidated until the NTP daemon next polls its peers */
	bool offset_unsafe;

	/* GPS time at which offset was last updated */
	struct sfptpd_timespec offset_gps_timestamp;

	/* System time at which offset was last updated */
	struct sfptpd_timespec offset_timestamp;

	/* Cached log time */
	struct sfptpd_log_time log_time;

	/* PPS quantisation error */
	long pps_quant_err_ps;
	struct sfptpd_timespec pps_quant_err_pulse;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchonized to the master */
	bool synchronized;

	/* Clustering score */
	int clustering_score;

	/* Clustering evaluator */
	struct sfptpd_clustering_evaluator clustering_evaluator;
};

struct gps_module {
	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Whether we have entered the RUNning phase */
	bool running_phase;

	/* Linked list of instances */
	struct gps_instance *instances;
};

struct gps_instance {
	/* Pointer to next instance in linked list */
	struct gps_instance *next;

	/* Pointer to sync module */
	struct gps_module *module;

	/* Pointer to the configuration */
	struct sfptpd_gps_module_config *config;

	/* Which elements of the GPS daemon are enabled */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* Time for next poll of the GPS daemon */
	struct sfptpd_timespec next_poll_time;

	/* Time for control reply timeout */
	struct sfptpd_timespec reply_expiry_time;

	/* GPS module state */
	struct gps_state state;

	/* next GPS module state */
	struct gps_state next_state;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;

	/* Stats collected in sync module */
	struct sfptpd_stats_collection stats;

	/* fd for gpsd */
	int gpsd_fd;

	/* gps state */
	struct gps_data_t gps_data;

	/* Constraints */
	sfptpd_sync_module_constraints_t constraints;
};

/****************************************************************************
 * Constants
 ****************************************************************************/

#define MODULE SFPTPD_GPS_MODULE_NAME

static const struct sfptpd_stats_collection_defn gps_stats_defns[] =
{
	{GPS_STATS_ID_OFFSET,            SFPTPD_STATS_TYPE_RANGE, "offset-from-peer", "ns", 0},
	{GPS_STATS_ID_SYNCHRONIZED,      SFPTPD_STATS_TYPE_COUNT, "synchronized"},
	{GPS_STATS_ID_SATELLITES_USED,   SFPTPD_STATS_TYPE_RANGE, "satellites-used"},
	{GPS_STATS_ID_SATELLITES_SEEN,   SFPTPD_STATS_TYPE_RANGE, "satellites-seen"},
};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/



/****************************************************************************
 * Configuration
 ****************************************************************************/

static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_gps_module_config_t *gps = (sfptpd_gps_module_config_t *)section;
	int tokens, priority;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &priority);
	if (tokens != 1)
		return EINVAL;

	gps->priority = (unsigned int)priority;
	return 0;
}


static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_gps_module_config_t *gps = (sfptpd_gps_module_config_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	gps->convergence_threshold = threshold;
	return 0;
}


static int parse_gpsd(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[])
{
	sfptpd_gps_module_config_t *gps = (sfptpd_gps_module_config_t *)section;
	assert(num_params <= 2);

	gps->gpsd = true;
	sfptpd_strncpy(gps->gpsd_host, params[0], sizeof gps->gpsd_host);
	sfptpd_strncpy(gps->gpsd_serv, params[1], sizeof gps->gpsd_serv);

	return 0;
}


static const sfptpd_config_option_t gps_config_options[] =
{
	{"priority", "<NUMBER>",
		"Relative priority of sync module instance. Smaller values have higher "
		"priority. The default 128.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_priority},
	{"sync_threshold", "<NUMBER>",
		"Threshold in nanoseconds of the offset from the clock source over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync (converged). The default is "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_NTP) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_threshold},
	{"gpsd", "[<HOST> [<PORT>]]",
		"Host and port for gpsd. The default is the shared memory inteface.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_gpsd},
};

static const sfptpd_config_option_set_t gps_config_option_set =
{
	.description = "GPS Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_GPS,
	.num_options = sizeof(gps_config_options)/sizeof(gps_config_options[0]),
	.options = gps_config_options
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

const char *gps_state_text(sfptpd_sync_module_state_t state, unsigned int alarms)
{
	static const char *states_text[SYNC_MODULE_STATE_MAX] = {
		"gps-listening",	/* SYNC_MODULE_STATE_LISTENING */
		"gps-slave",		/* SYNC_MODULE_STATE_SLAVE */
		"gps-master",		/* SYNC_MODULE_STATE_LISTENING */
		"gps-passive",		/* SYNC_MODULE_STATE_LISTENING */
		"gps-disabled",		/* SYNC_MODULE_STATE_DISABLED */
		"gps-faulty",		/* SYNC_MODULE_STATE_FAULTY */
		"gps-selection",	/* SYNC_MODULE_STATE_SELECTION */
	};

	assert(state < SYNC_MODULE_STATE_MAX);

	if ((state == SYNC_MODULE_STATE_SLAVE) && (alarms != 0))
		return "gps-slave-alarm";

	return states_text[state];
}


static void gps_convergence_init(struct gps_instance *gps)
{
	long double threshold;

	assert(gps != NULL);

	/* Initialise the convergence measure. */
	gps->state.synchronized = false;
	sfptpd_stats_convergence_init(&gps->convergence);

	/* Sets an appropriate convergence threshold.
	   Check if overriden by user. */
	threshold = gps->config->convergence_threshold;

	/* Otherwise use a suitable value for GPS */
	if (threshold == 0) {
		threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_GPS;
	}

	sfptpd_stats_convergence_set_max_offset(&gps->convergence, threshold);
}


static bool gps_convergence_update(struct gps_instance *gps, struct gps_state *new_state)
{
	struct sfptpd_timespec time;
	int rc;
	assert(gps != NULL);

	rc = sfclock_gettime(CLOCK_MONOTONIC, &time);
	if (rc < 0) {
		ERROR("gps: failed to get monotonic time, %s\n", strerror(errno));
	}

	/* If not in the slave state or we failed to get the time for some
	 * reason, reset the convergence measure. */
	if ((rc < 0) || (new_state->state != SYNC_MODULE_STATE_SLAVE)) {
		new_state->synchronized = false;
		sfptpd_stats_convergence_reset(&gps->convergence);
	} else if ((new_state->alarms != 0) ||
		   ((gps->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* If one or more alarms is triggered or timestamp processing
		 * is disabled, we consider the slave to be unsynchronized.
		 * However, don't reset the convergence measure as it is
		 * probably a temporary situation. */
		new_state->synchronized = false;
	} else {
		/* write something akin to selected peer to state */

		/* Update the synchronized state based on the current offset
		 * from master */
		new_state->synchronized = sfptpd_stats_convergence_update(&gps->convergence,
									  time.sec,
									  /* TODO */ 0);
	}

	return new_state->synchronized != gps->state.synchronized;
}


static void reset_offset_id(struct gps_state *state)
{
	memset(&state->offset_id_tuple, '\0', sizeof state->offset_id_tuple);
}


static void set_offset_id(struct gps_state *state)
{
	reset_offset_id(state);
	state->offset_id_tuple.offset_timestamp = state->offset_timestamp;
	state->offset_id_tuple.valid = true;
}


static bool offset_ids_equal(struct gps_state *state1, struct gps_state *state2) {
	return (memcmp(&state1->offset_id_tuple,
		       &state2->offset_id_tuple,
		       sizeof state1->offset_id_tuple) == 0);
}


static bool offset_id_is_valid(struct gps_state *state) {
	return state->offset_id_tuple.valid;
}


static int gps_stats_init(struct gps_instance *gps)
{
	int rc;

	assert(gps != NULL);

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&gps->stats, "gps",
					    sizeof(gps_stats_defns)/sizeof(gps_stats_defns[0]),
					    gps_stats_defns);
	return rc;
}


void gps_stats_update(struct gps_instance *gps)
{
	struct sfptpd_stats_collection *stats;
	struct sfptpd_timespec now;

	assert(gps != NULL);

	stats = &gps->stats;
	sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &now);

	/* The offset is only available if we are in slave mode */
	if (gps->state.state == SYNC_MODULE_STATE_SLAVE) {
		/* Offset, frequency correction, one-way-delay */
		sfptpd_stats_collection_update_range(stats, GPS_STATS_ID_OFFSET,gps->state.offset_from_master,
						     gps->state.offset_timestamp, true);
	} else {
		sfptpd_stats_collection_update_range(stats, GPS_STATS_ID_OFFSET, 0.0,
						     now, false);
	}

	sfptpd_stats_collection_update_count(stats, GPS_STATS_ID_SYNCHRONIZED, gps->state.synchronized ? 1: 0);
	sfptpd_stats_collection_update_range(stats, GPS_STATS_ID_SATELLITES_USED, gps->state.sats_used, now, true);
	sfptpd_stats_collection_update_range(stats, GPS_STATS_ID_SATELLITES_SEEN, gps->state.sats_seen, now, true);
}


void gps_parse_state(struct gps_state *state, int rc, bool offset_unsafe)
{
	assert(state != NULL);

	/* We derive the current state using the following rules in this order:
	 *   - If there is no one listening on the NTP socket, we conclude that
	 *     the daemon is not running
	 *   - If we get any other error while communicating with the daemon,
	 *     we are in the faulty state
	 *   - If we have a selected peer and clock control is enabled then we
	 *     are in the slave state
	 *   - If we have a selected peer and clock control is disabled then we
	 *     are in the monitor state
	 *   - If any peers are candidates or shortlisted we are in the
	 *     selection state
	 *   - Otherwise, we are in the listening state
	 */
	if (rc != 0) {
		if (rc == ENOPROTOOPT)
			state->state = SYNC_MODULE_STATE_DISABLED;
		else if (rc == EAGAIN)
			state->state = SYNC_MODULE_STATE_LISTENING;
		else
			state->state = SYNC_MODULE_STATE_FAULTY;
		reset_offset_id(state);
	}

	//offsetid
	if (state->fix && !offset_unsafe)
		set_offset_id(state);
	else
		reset_offset_id(state);

	/* We will only report being in the slave state if there is a selected
	 * peer and the offset is safe i.e. there has been an update since the
	 * clocks were stepped. */
	if (state->fix && !offset_unsafe) {
		state->state = SYNC_MODULE_STATE_SLAVE;
	} else {
		state->state = state->sats_seen > 0 ?
			SYNC_MODULE_STATE_SELECTION: SYNC_MODULE_STATE_LISTENING;
		sfptpd_time_zero(&state->offset_gps_timestamp);
		sfptpd_time_zero(&state->offset_timestamp);
		state->offset_from_master = 0.0L;
		state->stratum = 0;
	}

	state->clustering_score =
		state->clustering_evaluator.calc_fn(
			&state->clustering_evaluator,
			state->offset_from_master,
			sfptpd_clock_get_system_clock());
}


int gps_configure_gpsd(struct gps_instance *gps)
{
	int rc;

	assert(gps != NULL);

	if (!gps->config->gpsd) {
		CRITICAL("gps %s: needs gpsd configuration\n",
			 SFPTPD_CONFIG_GET_NAME(gps->config));
		return EINVAL;
	}

	rc = gps_open(gps->config->gpsd_host,
		      gps->config->gpsd_serv,
		      &gps->gps_data);

	if (rc != 0) {
		CRITICAL("gps %s: error opening, %s\n",
			 SFPTPD_CONFIG_GET_NAME(gps->config),
			 gps_errstr(rc));
		return ENOSYS;
	}

	return 0;
}

static void gps_send_rt_stats_update(struct gps_instance *gps,
				     struct sfptpd_log_time time,
				     struct gps_state *new_state)
{
	if (new_state->state == SYNC_MODULE_STATE_SLAVE) {
		sfptpd_time_t offset = new_state->offset_from_master;

		bool disciplining = false;

		sfptpd_engine_post_rt_stats(gps->module->engine,
					    &new_state->log_time,
					    SFPTPD_CONFIG_GET_NAME(gps->config),
					    "gps", NULL, sfptpd_clock_get_system_clock(),
					    disciplining, false,
					    new_state->synchronized, new_state->alarms,
					    STATS_KEY_OFFSET, offset,
					    STATS_KEY_END);
	}
}


static void gps_send_clustering_input(struct gps_instance *gps, struct gps_state *state)
{
	assert(gps != NULL);

	if (gps->ctrl_flags & SYNC_MODULE_CLUSTERING_DETERMINANT) {
		sfptpd_time_t offset = state->offset_from_master;

		sfptpd_engine_clustering_input(gps->module->engine,
					       SFPTPD_CONFIG_GET_NAME(gps->config),
					       sfptpd_clock_get_system_clock(),
					       offset,
					       finitel(offset) && offset != 0.0L &&
					       state->state == SYNC_MODULE_STATE_SLAVE);
	}
}


static void gps_on_offset_id_change(struct gps_instance *gps, struct gps_state *new_state)
{
	TRACE_L4("gps: offset ID changed\n");

	if (new_state->offset_unsafe && !offset_id_is_valid(new_state)) {
		new_state->offset_unsafe = false;
		INFO("gps: new gps offset detected\n");
		sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &new_state->offset_timestamp);
	}
}

static bool gps_is_instance_in_list(struct gps_module *module,
				    struct gps_instance *gps) {
	struct gps_instance *ptr;

	assert(module);
	assert(gps);

	/* Walk linked list, looking for the clock */
	for (ptr = module->instances;
	     ptr && ptr != gps;
	     ptr = ptr->next);

	return (ptr == NULL) ? false : true;
}

static void gps_on_get_status(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_sync_instance_status *status;
	struct gps_instance *gps;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.get_status_req.instance_handle != NULL);

	gps = (struct gps_instance *) msg->u.get_status_req.instance_handle;
	assert(gps);
	assert(gps_is_instance_in_list(module, gps));

	status = &msg->u.get_status_resp.status;
	status->state = gps->state.state;
	status->alarms = gps->state.alarms;
	/* The reference clock for GPS is always the system clock */
	status->clock = sfptpd_clock_get_system_clock();
	status->user_priority = gps->config->priority;
	status->constraints = gps->constraints;

	sfptpd_time_float_ns_to_timespec(gps->state.offset_from_master,
					 &status->offset_from_master);
	status->local_accuracy = SFPTPD_ACCURACY_GPS;

	status->clustering_score = gps->state.clustering_score;

	status->master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;

	if (gps->state.state == SYNC_MODULE_STATE_SLAVE) {
		status->master.remote_clock = true;
		status->master.clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
		status->master.time_source = SFPTPD_TIME_SOURCE_GPS;
		status->master.accuracy = gps->state.est_accuracy;
		status->master.allan_variance = NAN;
		status->master.time_traceable = false;
		status->master.freq_traceable = false;
		status->master.steps_removed = gps->state.stratum;
	} else {
		status->master.remote_clock = false;
		status->master.clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
		status->master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
		status->master.accuracy = INFINITY;
		status->master.allan_variance = NAN;
		status->master.time_traceable = false;
		status->master.freq_traceable = false;
		status->master.steps_removed = 0;
	}

	SFPTPD_MSG_REPLY(msg);
}


static void gps_on_control(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	sfptpd_sync_module_ctrl_flags_t flags;
	struct gps_instance *gps;

	assert(msg != NULL);
	assert(msg->u.control_req.instance_handle != NULL);

	gps = (struct gps_instance *)msg->u.control_req.instance_handle;
	assert(gps);
	assert(gps_is_instance_in_list(module, gps));

	flags = gps->ctrl_flags;

	/* Update the flags based on the message */
	flags &= ~msg->u.control_req.mask;
	flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	gps->ctrl_flags = flags;
	SFPTPD_MSG_REPLY(msg);
}


static void gps_on_step_clock(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	assert(module != NULL);
	assert(msg != NULL);

	/* Invalidate offset until GPS next queries the peers. */
	//TODO:gps->state.offset_unsafe = true;
	INFO("gps: clock step- ignoring gps offset until next update\n");

	SFPTPD_MSG_REPLY(msg);
}


static void gps_on_log_stats(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	struct gps_instance *gps;

	assert(module != NULL);
	assert(msg != NULL);

	for (gps = module->instances; gps; gps = gps->next) {
		gps_send_rt_stats_update(gps, msg->u.log_stats_req.time, &gps->state);
		gps_send_clustering_input(gps, &gps->state);
	}

	SFPTPD_MSG_FREE(msg);
}

static void gps_on_save_state(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	struct gps_instance *gps;
	struct sfptpd_clock *clock;
	char constraints[SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX];
	char alarms[256], flags[256];

	assert(module != NULL);
	assert(msg != NULL);

	clock = sfptpd_clock_get_system_clock();

	for (gps = module->instances; gps; gps = gps->next) {
		sfptpd_sync_module_alarms_text(gps->state.alarms, alarms, sizeof(alarms));
		sfptpd_sync_module_constraints_text(gps->constraints, constraints, sizeof(constraints));
		sfptpd_sync_module_ctrl_flags_text(gps->ctrl_flags, flags, sizeof(flags));
		sfptpd_log_write_state(clock,
			SFPTPD_CONFIG_GET_NAME(gps->config),
			"instance: %s\n"
			"clock-name: %s\n"
			"state: %s\n"
			"alarms: %s\n"
			"constraints: %s\n"
			"control-flags: %s\n"
			"offset-from-master: " SFPTPD_FORMAT_FLOAT "\n"
			"in-sync: %d\n"
			"num-satellites: %d/%d\n",
			SFPTPD_CONFIG_GET_NAME(gps->config),
			sfptpd_clock_get_long_name(clock),
			gps_state_text(gps->state.state, 0),
			alarms, constraints, flags,
			gps->state.offset_from_master,
			gps->state.synchronized,
			gps->state.sats_used, gps->state.sats_seen);
	}
	SFPTPD_MSG_FREE(msg);
}

static void gps_on_write_topology(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	FILE *stream;
	char alarms[256];
	struct sfptpd_clock *clock;
	char host[NI_MAXHOST] = "";
	struct gps_instance *gps;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.write_topology_req.instance_handle != NULL);

	stream = msg->u.write_topology_req.stream;
	gps = (struct gps_instance *) msg->u.write_topology_req.instance_handle;
	clock = sfptpd_clock_get_system_clock();

	/* This should only be called on selected instances */
	assert(gps->ctrl_flags & SYNC_MODULE_SELECTED);

	fprintf(stream, "====================\nstate: %s\n",
		gps_state_text(gps->state.state, 0));

	if (gps->state.alarms != 0) {
		sfptpd_sync_module_alarms_text(gps->state.alarms, alarms, sizeof(alarms));
		fprintf(stream, "alarms: %s\n", alarms);
	}

	fprintf(stream, "====================\n\n");

	sfptpd_log_topology_write_field(stream, true, "gps");

	switch (gps->state.state) {
	case SYNC_MODULE_STATE_LISTENING:
	case SYNC_MODULE_STATE_SELECTION:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "?");
		break;

	case SYNC_MODULE_STATE_SLAVE:
		sfptpd_log_topology_write_field(stream, true, "gps");
		sfptpd_log_topology_write_field(stream, true, "%s", host);
		sfptpd_log_topology_write_1to1_connector(stream, false, true,
							 SFPTPD_FORMAT_TOPOLOGY_FLOAT,
							 gps->state.offset_from_master);
		break;

	default:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "X");
		break;
	}

	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_long_name(clock));
	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_hw_id_string(clock));

	SFPTPD_MSG_REPLY(msg);
}


static void gps_on_stats_end_period(struct gps_module *module, sfptpd_sync_module_msg_t *msg)
{
	struct gps_instance *gps;

	assert(module != NULL);
	assert(msg != NULL);

	for (gps = module->instances; gps; gps = gps->next) {
		sfptpd_stats_collection_end_period(&gps->stats,
						   &msg->u.stats_end_period_req.time);

		/* Write the historical statistics to file */
		sfptpd_stats_collection_dump(&gps->stats, sfptpd_clock_get_system_clock(),
					     SFPTPD_CONFIG_GET_NAME(gps->config));
	}

	SFPTPD_MSG_FREE(msg);
}

static void update_gpsd_fd(struct gps_instance *gps)
{
	if (gps->gps_data.gps_fd != gps->gpsd_fd) {
		INFO("gps: new gpsd fd %d\n", gps->gps_data.gps_fd);
		if (gps->gpsd_fd != -1)
			sfptpd_thread_user_fd_remove(gps->gpsd_fd);
		if (gps->gps_data.gps_fd != -1)
			sfptpd_thread_user_fd_add(gps->gps_data.gps_fd, true, false);
		gps->gpsd_fd = gps->gps_data.gps_fd;
	}
}

static bool gps_handle_state_change(struct gps_instance *gps,
				    struct gps_state *new_state,
				    sfptpd_sync_instance_status_t *status_out)
{
	sfptpd_sync_instance_status_t status = { 0 };
	bool status_changed;

	assert(gps != NULL);
	assert(new_state != NULL);
	assert(status_out != NULL);

	if (new_state->state != gps->state.state) {
		INFO("gps: changed state from %s to %s\n",
		     gps_state_text(gps->state.state, 0),
		     gps_state_text(new_state->state, 0));

		switch (new_state->state) {
		case SYNC_MODULE_STATE_DISABLED:
			WARNING("gps: gpsd no longer running\n");
			break;

		case SYNC_MODULE_STATE_FAULTY:
			ERROR("gps: not able to communicate with gpsd\n");
			break;

		case SYNC_MODULE_STATE_MASTER:
		case SYNC_MODULE_STATE_LISTENING:
		case SYNC_MODULE_STATE_SELECTION:
		case SYNC_MODULE_STATE_SLAVE:
			/* Nothing to do here */
			break;

		case SYNC_MODULE_STATE_PASSIVE:
		default:
			assert(false);
			break;
		}
	}

	status_changed =((new_state->state != gps->state.state) ||
			 (new_state->alarms != gps->state.alarms) ||
			 (new_state->stratum != gps->state.stratum) ||
			 (new_state->offset_from_master != 0 && gps->state.offset_from_master == 0));

	if (status_changed ||
	    new_state->offset_from_master != gps->state.offset_from_master) {

		/* Send a status update to the sync engine to let it know
		 * that the state of the GPS module has changed. */
		status.state = new_state->state;
		status.alarms = new_state->alarms;
		status.clock = sfptpd_clock_get_system_clock();
		status.user_priority = gps->config->priority;
		status.constraints = gps->constraints;

		sfptpd_time_float_ns_to_timespec(new_state->offset_from_master,
						 &status.offset_from_master);
		status.local_accuracy = SFPTPD_ACCURACY_GPS;

		status.master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;
		status.master.accuracy = new_state->est_accuracy;
		status.master.allan_variance = NAN;
		status.master.time_traceable = false;
		status.master.freq_traceable = false;

		status.master.steps_removed = new_state->stratum;

		/* Report the clock class according to the state */
		if (status.state == SYNC_MODULE_STATE_SLAVE) {
			status.master.remote_clock = true;
			status.master.clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
			status.master.time_source = SFPTPD_TIME_SOURCE_GPS;

		} else {
			status.master.remote_clock = false;
			status.master.clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
			status.master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
		}

		status.clustering_score = new_state->clustering_score;

		*status_out = status;
	}

	return status_changed;
}


static void update_state(struct gps_instance *gps)
{
	struct gps_state *new_state = &gps->next_state;
	sfptpd_sync_instance_status_t status = { 0 };
	bool any_change = false;
	bool status_change;

	assert(gps != NULL);

	/* Handle changes in the state */
	if ((status_change = gps_handle_state_change(gps, new_state, &status)))
		any_change = true;

	/* Update the convergence criteria */
	if (gps_convergence_update(gps, new_state))
		any_change = true;

	/* Handle a change in either the source ID or offset */
	if (!offset_ids_equal(new_state, &gps->state)) {
		gps_on_offset_id_change(gps, new_state);
		any_change = true;
	}

	if (any_change) {
		/* Send updated stats (offset) to the engine */
		struct sfptpd_log_time time;
		sfptpd_log_get_time(&time);
		gps_send_rt_stats_update(gps, time, new_state);

		/* Send clustering input to the engine */
		gps_send_clustering_input(gps, new_state);
	}

	/* Store the new state */
	gps->state = *new_state;

	/* Send changes to the engine */
	if (status_change) {
		sfptpd_engine_sync_instance_state_changed(gps->module->engine,
							  sfptpd_thread_self(),
							  (struct sfptpd_sync_instance *)gps,
							  &status);
	}

	/* Update historical stats */
	gps_stats_update(gps);
}


static void gps_on_run(struct gps_module *module)
{
	struct gps_instance *gps;

	assert(module);

	for (gps = module->instances; gps; gps = gps->next) {
		update_gpsd_fd(gps);
		if (gps->gpsd_fd != -1) {
			gps_stream(&gps->gps_data, WATCH_TIMING | WATCH_PPS | WATCH_ENABLE | WATCH_JSON, NULL);
		}
	}

	module->running_phase = true;
}


static int gps_init_instance(struct gps_instance *gps)
{
	int rc;

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. */
	gps->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Set up the clustering evaluator */
	gps->state.clustering_evaluator.calc_fn = sfptpd_engine_calculate_clustering_score;
	gps->state.clustering_evaluator.private = gps->module->engine;
	gps->state.clustering_evaluator.instance_name = gps->config->hdr.name;

	rc = gps_configure_gpsd(gps);
	if (rc != 0)
		goto fail;

	gps_convergence_init(gps);

	rc = gps_stats_init(gps);
	if (rc != 0) {
		CRITICAL("gps: failed to create statistics collection, %s\n",
			 strerror(rc));
		goto fail;
	}
	return 0;

fail:
	sfptpd_stats_collection_free(&gps->stats);
	return rc;
}

static void gps_close_instance(struct gps_instance *gps)
{
	gps_stream(&gps->gps_data, WATCH_DISABLE, NULL);
	gps_close(&gps->gps_data);
}

static int gps_on_startup(void *context)
{
	struct gps_module *module = (struct gps_module *)context;
	struct gps_instance *gps;
	int rc = 0;

	assert(module != NULL);

	for (gps = module->instances; rc == 0 && gps; gps = gps->next) {
		rc = gps_init_instance(gps);
		if (rc != 0) break;
	}

	return rc;
}

static void gps_on_shutdown(void *context)
{
	struct gps_module *module = (struct gps_module *)context;
	struct gps_instance *gps;

	assert(module != NULL);

	for (gps = module->instances; gps; gps = gps->next) {
		gps_close_instance(gps);
		sfptpd_stats_collection_free(&gps->stats);
	}

	/* Delete the sync module context */
	free(gps);
}


static void gps_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	struct gps_module *gps = (struct gps_module *)context;
	sfptpd_sync_module_msg_t *msg = (sfptpd_sync_module_msg_t *)hdr;

	assert(gps != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		gps_on_run(gps);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_GET_STATUS:
		gps_on_get_status(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_CONTROL:
		gps_on_control(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_GM_INFO:
		/* This module doesn't use this message */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_LEAP_SECOND:
		/* This module doesn't use this message */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STEP_CLOCK:
		gps_on_step_clock(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LOG_STATS:
		gps_on_log_stats(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_SAVE_STATE:
		gps_on_save_state(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY:
		gps_on_write_topology(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		gps_on_stats_end_period(gps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_TEST_MODE:
		/* This module doesn't have any test modes */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LINK_TABLE:
		/* This module doesn't react to networking reconfiguration */
		SFPTPD_MSG_FREE(msg);
		break;

	default:
		WARNING("gps: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}

static bool gps_state_machine(struct gps_instance *gps, int read_rc)
{
        struct gps_data_t *gps_data = &gps->gps_data;
        struct gps_state *next_state = &gps->next_state;
	gps_mask_t set = gps_data->set;

	assert(gps != NULL);

	memcpy(next_state, &gps->state, sizeof *next_state);

	if (read_rc < 0) {
		gps_parse_state(next_state, read_rc, next_state->offset_unsafe);
		return next_state->state != gps->state.state;
	}

	TRACE_L5("gps: update: %X%s%s%s%s%s%s%s%s\n", set,
		 set & ONLINE_SET ? " online" : "",
		 set & TIME_SET ? " time" : "",
		 set & TIMERR_SET ? " timerr" : "",
		 set & SATELLITE_SET ? " satellite" : "",
		 set & STATUS_SET ? " status" : "",
		 set & MODE_SET ? " mode" : "",
		 set & TOFF_SET ? " toff" : "",
		 set & OSCILLATOR_SET ? " osc" : "");

	if (gps_data->set & SATELLITE_SET) {
		TRACE_L5("gps: SATELLITE num_sats %d/%d\n",
			 gps_data->satellites_used, gps_data->satellites_visible);
		next_state->sats_used = gps_data->satellites_used;
		next_state->sats_seen = gps_data->satellites_visible;
	}

	if (gps_data->set & STATUS_SET) {
		TRACE_L4("gps: STATUS fix %d status %d\n",
			 gps_data->fix.mode, gps_data->fix.status);
		next_state->fix = gps_data->fix.mode >= MODE_2D;
		/* Not useful: && gps_data->fix.status != STATUS_NO_FIX; */
		TRACE_L5("gps: STATUS terr %lfs\n",
			 gps_data->fix.ept);
		next_state->est_accuracy = gps_data->fix.ept * 1.0E9;
		TRACE_L5("gps: STATUS co-ordinates %lf/%lf\n",
			 gps_data->fix.latitude, gps_data->fix.longitude);
		sfptpd_time_from_std_floor(&next_state->offset_gps_timestamp,
				     &gps_data->fix.time);
	}

	if (next_state->fix && gps_data->set & TOFF_SET) {
		struct sfptpd_timespec diff;

		TRACE_L5("gps: %s"
			 " real " SFPTPD_FORMAT_TIMESPEC
			 " clock " SFPTPD_FORMAT_TIMESPEC "\n",
			 (gps_data->set & TOFF_SET) ? "TOFF" : "toff",
			 gps_data->pps.real.tv_sec, gps_data->pps.real.tv_nsec,
			 gps_data->pps.clock.tv_sec, gps_data->pps.clock.tv_nsec);

		sfptpd_time_from_std_floor(&next_state->offset_gps_timestamp,
					  &gps_data->pps.real);
		sfptpd_time_from_std_floor(&next_state->offset_timestamp,
					  &gps_data->pps.clock);
		sfptpd_time_subtract(&diff,
				     &next_state->offset_timestamp,
				     &next_state->offset_gps_timestamp);
		next_state->offset_from_master = sfptpd_time_timespec_to_float_ns(&diff);
	}

	gps_parse_state(next_state, read_rc, next_state->offset_unsafe);

	return (next_state->state != gps->state.state ||
		next_state->sats_used != gps->state.sats_used ||
		next_state->sats_seen != gps->state.sats_seen ||
		next_state->offset_from_master != gps->state.offset_from_master);
}


static void gps_do_io(struct gps_instance *gps)
{
	int rc;

	assert(gps != NULL);

	rc = gps_read(&gps->gps_data, NULL, 0);
	if (rc < 0) {
		if (errno == -EAGAIN || errno == -EINTR) {
		/* Ignore wakeup */
			TRACE_L6("gps: fd woken up, %s\n", strerror(rc));
			return;
		} else {
			TRACE_L4("gps: read: %s\n", strerror(errno));
		}
	} else if (rc > 0) {
		TRACE_L6("gps: data(sz=%d)\n", rc);
	}

	/* Progress the GPS state machine. */
	if (gps_state_machine(gps, rc))
		update_state(gps);
}

static void gps_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	struct gps_module *module = (struct gps_module *) context;
	struct gps_instance *gps;
	int i;

	assert(module != NULL);

	for (i = 0; i < num_fds; i++) {
		for (gps = module->instances; gps; gps = gps->next) {
			if (gps->gpsd_fd == fds[i]) {
				gps_do_io(gps);
			}
		}
	}
}


static const struct sfptpd_thread_ops gps_thread_ops =
{
	gps_on_startup,
	gps_on_shutdown,
	gps_on_message,
	gps_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void gps_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_GPS);
	free(section);
}


static struct sfptpd_config_section *gps_config_create(const char *name,
						       enum sfptpd_config_scope scope,
						       bool allows_instances,
						       const struct sfptpd_config_section *src)
{
	struct sfptpd_gps_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_GPS));

	new = (struct sfptpd_gps_module_config *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("gps %s: failed to allocate memory for GPS configuration\n", name);
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
	} else {
		new->priority = SFPTPD_DEFAULT_PRIORITY;
		new->convergence_threshold = 0.0;
	}

	/* If this is an implicitly created sync instance, give it the lowest
	   possible user priority. */
	if (name == NULL) {
		name = "gps0";
		new->priority = INT_MAX;
	}

	SFPTPD_CONFIG_SECTION_INIT(new, gps_config_create, gps_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_GPS,
				   scope, allows_instances, name);

	return &new->hdr;
}


int sfptpd_gps_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_gps_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_gps_module_config *)
		gps_config_create(MODULE,
				  SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	SFPTPD_CONFIG_SECTION_ADD(config, new);

	/* Register the configuration options */
	sfptpd_config_register_options(&gps_config_option_set);
	return 0;
}


struct sfptpd_gps_module_config *sfptpd_gps_module_get_config(struct sfptpd_config *config)
{
	return (struct sfptpd_gps_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_GPS);
}


void sfptpd_gps_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name)
{
	/* For GPS no interface is required */
}
static void gps_destroy_instances(struct gps_module *module) {
	struct gps_instance *next;
	struct gps_instance *gps;

	next = module->instances;
	module = NULL;

	for (gps = next; gps; gps = next) {
		next = gps->next;
		free(gps);
	}
}

static int gps_create_instances(struct sfptpd_config *configs,
				struct gps_module *module)
{
	sfptpd_gps_module_config_t *config;
	struct gps_instance *gps;
	struct gps_instance **next;

	assert(configs != NULL);
	assert(module != NULL);
	assert(module->instances == NULL);

	next = &module->instances;
	for (config = (struct sfptpd_gps_module_config *)
		sfptpd_config_category_first_instance(configs,
			SFPTPD_CONFIG_CATEGORY_GPS);
	     config;
	     config = (struct sfptpd_gps_module_config *)
		sfptpd_config_category_next_instance(&config->hdr)) {

		INFO("gps %s: creating sync-instance\n",
		     SFPTPD_CONFIG_GET_NAME(config));

		gps = (struct gps_instance *) calloc(1, sizeof *gps);
		if (gps == NULL) {
			CRITICAL("gps %s: failed to allocate sync instance\n",
				 SFPTPD_CONFIG_GET_NAME(config));
			gps_destroy_instances(module);
			return ENOMEM;
		}
		gps->module = module;
		gps->config = config;
		gps->gpsd_fd = -1;
		SYNC_MODULE_CONSTRAINT_SET(gps->constraints, CANNOT_BE_SELECTED);

		*next = gps;
		next = &gps->next;
	}
	return 0;
}

int sfptpd_gps_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_table_subscriber)
{
	struct gps_module *gps;
	struct gps_instance *instance;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	TRACE_L3("gps: creating sync-module\n");

	*sync_module = NULL;
	gps = (struct gps_module *) calloc(1, sizeof(*gps));
	if (gps == NULL) {
		CRITICAL("gps: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	/* Keep a handle to the sync engine */
	gps->engine = engine;

	/* Create all the sync instances */
	rc = gps_create_instances(config, gps);
	if (rc != 0)
		goto fail;

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("gps", &gps_thread_ops, gps, sync_module);
	if (rc != 0) {
		free(gps);
		return rc;
	}

	/* If a buffer is provided write the instances into it */
	if ((instances_info_buffer != NULL) && (instances_info_entries >= 1)) {
		memset(instances_info_buffer, 0,
		       instances_info_entries * sizeof(*instances_info_buffer));

		for (instance = gps->instances;
		     (instance != NULL) && (instances_info_entries > 0);
		     instance = instance->next) {
			instances_info_buffer->module = *sync_module;
			instances_info_buffer->handle = (struct sfptpd_sync_instance *) instance;
			instances_info_buffer->name = instance->config->hdr.name;
			instances_info_buffer++;
			instances_info_entries--;
		}
	}

	return 0;
fail:
	free(gps);
	return rc;
}

#endif
