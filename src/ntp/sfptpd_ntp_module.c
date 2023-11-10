/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_ntp_module.c
 * @brief  NTP Synchronization Module
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netdb.h>

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
#include "sfptpd_ntpd_client.h"
#include "sfptpd_engine.h"

#include "sfptpd_ntp_module.h"


/****************************************************************************
 * Macros
 ****************************************************************************/

/* NTP component specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 1, x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 2, x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 3, x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 4, x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 5, x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 6, x, ##__VA_ARGS__)


/****************************************************************************
 * Types
 ****************************************************************************/

#define NTP_POLL_INTERVAL (250000000)

#define NTP_POLL_TIMER_ID (0)

enum ntp_mode {
	/* The NTP daemon may be running but should not be disciplining the
	 * system clock. */
	NTP_MODE_PASSIVE,
	/* The NTP daemon should be running and we have control of it. */
	NTP_MODE_ACTIVE
};

enum ntp_query_state {
	NTP_QUERY_STATE_SYS_INFO,
	NTP_QUERY_STATE_PEER_INFO,
	NTP_QUERY_STATE_SLEEP
};

enum ntp_stats_ids {
	NTP_STATS_ID_OFFSET,
	NTP_STATS_ID_SYNCHRONIZED
};

struct ntp_state {
	/* NTP module state */
	sfptpd_sync_module_state_t state;

	/* Alarms */
	sfptpd_sync_module_alarms_t alarms;

	/* Constraints */
	sfptpd_sync_module_constraints_t constraints;

	/* Unique information to identify the offset.
	   This is guaranteed to change each time
	   that NTP calculates a new offset from the selected peer.
	   The offset is invalid if 'valid' is false. */
	struct {
		struct sockaddr_storage peer;
		int pkts_received;
		bool valid;
	} __attribute__((packed)) offset_id_tuple;

	/* Information on currently selected peer */
	int selected_peer_idx;

	/* NTP daemon system info */
	struct sfptpd_ntpclient_sys_info sys_info;

	/* NTP daemon peer info */
	struct sfptpd_ntpclient_peer_info peer_info;

	/* Current offset from master or 0 if no peer selected */
	long double offset_from_master;

	/* Root dispersion of master or infinity if no peer selected */
	long double root_dispersion;

	/* Stratum of master or 0 if no peer selected */
	unsigned int stratum;

	/* Clustering evaluator */
	struct sfptpd_clustering_evaluator clustering_evaluator;

	/* Clustering score */
	int clustering_score;
};

typedef struct sfptpd_ntp_module {
	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Pointer to the configuration */
	struct sfptpd_ntp_module_config *config;

	/* How NTP is being used by sfptpd */
	enum ntp_mode mode;

	/* Which elements of the NTP daemon are enabled */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* NTP daemon query state. */
	enum ntp_query_state query_state;

	/* Time for next poll of the NTP daemon */
	struct timespec next_poll_time;

	/* Boolean indicating that the clock has been stepped and that the
	 * recorded NTP offset may not be correct. The offset will be
	 * invalidated until the NTP daemon next polls its peers */
	bool offset_unsafe;

	/* System time at which offset was last updated */
	struct timespec offset_timestamp;

	/* NTP module state */
	struct ntp_state state;

	/* NTP client container */
	struct sfptpd_ntpclient *client;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchonized to the master */
	bool synchronized;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;

	/* Stats collected in sync module */
	struct sfptpd_stats_collection stats;

} ntp_module_t;



/****************************************************************************
 * Constants
 ****************************************************************************/

static const struct sfptpd_stats_collection_defn ntp_stats_defns[] =
{
	{NTP_STATS_ID_OFFSET,       SFPTPD_STATS_TYPE_RANGE, "offset-from-peer", "ns", 0},
	{NTP_STATS_ID_SYNCHRONIZED, SFPTPD_STATS_TYPE_COUNT, "synchronized"}
};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/

static bool ntp_state_machine(ntp_module_t *ntp, struct ntp_state *state);
static void ntp_send_clustering_input(ntp_module_t *ntp);


/****************************************************************************
 * Configuration
 ****************************************************************************/

static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_ntp_module_config_t *ntp = (sfptpd_ntp_module_config_t *)section;
	int tokens, priority;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &priority);
	if (tokens != 1)
		return EINVAL;

	ntp->priority = (unsigned int)priority;
	return 0;
}


static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_ntp_module_config_t *ntp = (sfptpd_ntp_module_config_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	ntp->convergence_threshold = threshold;
	return 0;
}

static int parse_ntp_poll_interval(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	int tokens, interval;
	sfptpd_ntp_module_config_t *ntp = (sfptpd_ntp_module_config_t *)section;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%i", &interval);
	if (tokens != 1)
		return EINVAL;
	
	if (interval < 1) {
		CFG_ERROR(section, "invalid NTP poll interval %s. Minimum interval is 1 second\n",
		          params[0]);
		return ERANGE;
	}

	ntp->poll_interval = interval;
	return 0;
}

static int parse_ntp_key(struct sfptpd_config_section *section, const char *option,
			 unsigned int num_params, const char * const params[])
{
	sfptpd_ntp_module_config_t *ntp = (sfptpd_ntp_module_config_t *)section;
	assert(num_params == 2);

	ntp->key_id = strtoul(params[0], NULL, 0);
	if (ntp->key_id == 0) {
		CFG_ERROR(section, "ntp_key %d invalid. Non-zero value expected\n",
		          ntp->key_id);
		return ERANGE;
	}

	if (strlen(params[1]) >= sizeof(ntp->key_value)) {
		CFG_ERROR(section, "invalid NTP key value - maximum length is %zd characters\n",
			  sizeof(ntp->key_value) - 1);
		return ENOSPC;
	}

	sfptpd_strncpy(ntp->key_value, params[1], sizeof(ntp->key_value));

	return 0;
}

static const sfptpd_config_option_t ntp_config_options[] =
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
	{"ntp_poll_interval", "NUMBER",
		"Specifies the NTP daemon poll interval in seconds. Default value 1",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ntp_poll_interval},
	{"ntp_key", "ID VALUE",
		"NTP authentication key. Both ID and ascii key value must "
		"match a key configured in NTPD's keys file. The key value "
		"can be up to 31 characters long.",
		2, SFPTPD_CONFIG_SCOPE_INSTANCE, parse_ntp_key,
		.confidential = true},
};

static const sfptpd_config_option_set_t ntp_config_option_set =
{
	.description = "NTP Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_NTP,
	.num_options = sizeof(ntp_config_options)/sizeof(ntp_config_options[0]),
	.options = ntp_config_options
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

const char *ntp_state_text(sfptpd_sync_module_state_t state, unsigned int alarms)
{
	static const char *states_text[SYNC_MODULE_STATE_MAX] = {
		"ntp-listening",	/* SYNC_MODULE_STATE_LISTENING */
		"ntp-slave",		/* SYNC_MODULE_STATE_SLAVE */
		"ntp-master",		/* SYNC_MODULE_STATE_MASTER */
		"ntp-passive",		/* SYNC_MODULE_STATE_PASSIVE */
		"ntp-disabled",		/* SYNC_MODULE_STATE_DISABLED */
		"ntp-faulty",		/* SYNC_MODULE_STATE_FAULTY */
		"ntp-selection",	/* SYNC_MODULE_STATE_SELECTION */
	};

	assert(state < SYNC_MODULE_STATE_MAX);

	if ((state == SYNC_MODULE_STATE_SLAVE) && (alarms != 0))
		return "ntp-slave-alarm";

	return states_text[state];
}


static void ntp_convergence_init(ntp_module_t *ntp)
{
	long double threshold;

	assert(ntp != NULL);

	/* Initialise the convergence measure. */
	ntp->synchronized = false;
	sfptpd_stats_convergence_init(&ntp->convergence);

	/* Sets an appropriate convergence threshold.
	   Check if overriden by user. */
	threshold = ntp->config->convergence_threshold;

	/* Otherwise use a suitable value for NTP */
	if (threshold == 0) {
		threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_NTP;
	}

	sfptpd_stats_convergence_set_max_offset(&ntp->convergence, threshold);
}


static void ntp_convergence_update(ntp_module_t *ntp)
{
	struct timespec time;
	struct sfptpd_ntpclient_peer *peer;
	int rc;
	assert(ntp != NULL);

	rc = clock_gettime(CLOCK_MONOTONIC, &time);
	if (rc < 0) {
		ERROR("ntp: failed to get monotonic time, %s\n", strerror(errno));
	}

	/* If not in the slave state or we failed to get the time for some
	 * reason, reset the convergence measure. */
	if ((rc < 0) || (ntp->state.state != SYNC_MODULE_STATE_SLAVE)) {
		ntp->synchronized = false;
		sfptpd_stats_convergence_reset(&ntp->convergence);
	} else if ((ntp->state.alarms != 0) ||
		   ((ntp->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* If one or more alarms is triggered or timestamp processing
		 * is disabled, we consider the slave to be unsynchronized.
		 * However, don't reset the convergence measure as it is
		 * probably a temporary situation. */
		ntp->synchronized = false;
	} else {
		assert(ntp->state.selected_peer_idx != -1);
		peer = &ntp->state.peer_info.peers[ntp->state.selected_peer_idx];

		/* Update the synchronized state based on the current offset
		 * from master */
		ntp->synchronized = sfptpd_stats_convergence_update(&ntp->convergence,
								    time.tv_sec,
								    peer->offset);
	}
}


static void reset_offset_id(struct ntp_state *state)
{
	memset(&state->offset_id_tuple, '\0', sizeof state->offset_id_tuple);
}


static void set_offset_id(struct ntp_state *state, struct sfptpd_ntpclient_peer *peer)
{
	if (peer->remote_address_len == 0) {
		reset_offset_id(state);
	} else {
		reset_offset_id(state);
		assert(peer->remote_address_len < sizeof state->offset_id_tuple.peer);
		memcpy(&state->offset_id_tuple.peer,
		       &peer->remote_address,
		       peer->remote_address_len);
		state->offset_id_tuple.pkts_received = peer->pkts_received;
		state->offset_id_tuple.valid = true;
	}
}


static bool offset_ids_equal(struct ntp_state *state1, struct ntp_state *state2) {
	return (memcmp(&state1->offset_id_tuple,
		       &state2->offset_id_tuple,
		       sizeof state1->offset_id_tuple) == 0);
}


static bool offset_id_is_valid(struct ntp_state *state) {
	return state->offset_id_tuple.valid;
}


int ntp_stats_init(ntp_module_t *ntp)
{
	int rc;
	assert(ntp != NULL);
	
	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&ntp->stats, "ntp",
					    sizeof(ntp_stats_defns)/sizeof(ntp_stats_defns[0]),
					    ntp_stats_defns);
	return rc;
}


void ntp_stats_update(ntp_module_t *ntp)
{
	struct sfptpd_stats_collection *stats;
	struct sfptpd_ntpclient_peer *peer;
	assert(ntp != NULL);

	stats = &ntp->stats;

	/* The offset is only available if we are in slave mode */
	if (ntp->state.state == SYNC_MODULE_STATE_SLAVE) {
		assert(ntp->state.selected_peer_idx != -1);
		peer = &ntp->state.peer_info.peers[ntp->state.selected_peer_idx];
		/* Offset, frequency correction, one-way-delay */
		sfptpd_stats_collection_update_range(stats, NTP_STATS_ID_OFFSET, peer->offset,
						     ntp->offset_timestamp, true);
	} else {
		struct timespec now;
		sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &now);
		sfptpd_stats_collection_update_range(stats, NTP_STATS_ID_OFFSET, 0.0,
						     now, false);
	}

	sfptpd_stats_collection_update_count(stats, NTP_STATS_ID_SYNCHRONIZED, ntp->synchronized? 1: 0);
}


void ntp_parse_state(struct ntp_state *state, int rc, bool offset_unsafe)
{
	unsigned int i;
	bool candidates;
	struct sfptpd_ntpclient_peer *peer;
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
		SYNC_MODULE_CONSTRAINT_CLEAR(state->constraints, MUST_BE_SELECTED);
		SYNC_MODULE_CONSTRAINT_CLEAR(state->constraints, CANNOT_BE_SELECTED);
		state->sys_info.peer_address_len = 0;
		state->sys_info.clock_control_enabled = false;
		state->selected_peer_idx = -1;
		state->peer_info.num_peers = 0;
		reset_offset_id(state);
		return;
	}

	candidates = false;
	state->selected_peer_idx = -1;
	for (i = 0; i < state->peer_info.num_peers; i++) {
		peer = &state->peer_info.peers[i];

		/* Ignore ourselves */
		if (peer->self)
			continue;

		if (peer->selected) {
			if (state->selected_peer_idx != -1)
				WARNING("ntp: ntpd reporting more than one selected peer\n");
			else
				state->selected_peer_idx = i;
		}

		/* If we find a candidate or shortlisted node, set a flag */
		if (peer->candidate || peer->shortlist)
			candidates = true;
	}

	if (state->selected_peer_idx != -1)
		set_offset_id(state, &state->peer_info.peers[state->selected_peer_idx]);
	else
		reset_offset_id(state);

	/* We will only report being in the slave state if there is a selected
	 * peer and the offset is safe i.e. there has been an update since the
	 * clocks were stepped. */
	if ((state->selected_peer_idx != -1) && !offset_unsafe) {
		peer = &state->peer_info.peers[state->selected_peer_idx];

		state->state = SYNC_MODULE_STATE_SLAVE;
		state->offset_from_master = peer->offset;
		state->root_dispersion = peer->root_dispersion;
		state->stratum = peer->stratum;
	} else {
		state->state = candidates?
			SYNC_MODULE_STATE_SELECTION: SYNC_MODULE_STATE_LISTENING;
		state->offset_from_master = 0.0;
		state->root_dispersion = INFINITY;
		state->stratum = 0;
	}

	state->clustering_score =
		state->clustering_evaluator.calc_fn(
			&state->clustering_evaluator,
			state->offset_from_master,
			sfptpd_clock_get_system_clock());
}


int ntp_handle_clock_control_conflict(ntp_module_t *ntp, int error)
{
	struct sfptpd_config_general *gconf;

	gconf = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(ntp->config));
	CRITICAL("ntp: no capability to disable clock control\n");
	if (gconf->ignore_critical[SFPTPD_CRITICAL_CLOCK_CONTROL_CONFLICT]) {
		NOTICE("ptp: ignoring critical error by configuration\n");
		return 0;
	} else {
		NOTICE("configure \"ignore_critical: clock-control-conflict\" to allow sfptpd to start in spite of this condition\n");
		return error;
	}
}


static void ntp_send_instance_status(ntp_module_t *ntp, struct ntp_state *new_state)
{
	sfptpd_sync_instance_status_t status = { 0 };

	/* Send a status update to the sync engine to let it know
	 * that the state of the NTP module has changed. */
	status.state = new_state->state;
	status.alarms = new_state->alarms;
	status.constraints = new_state->constraints;
	status.clock = sfptpd_clock_get_system_clock();
	status.user_priority = ntp->config->priority;

	sfptpd_time_float_ns_to_timespec(new_state->offset_from_master,
					 &status.offset_from_master);
	status.local_accuracy = SFPTPD_ACCURACY_NTP;

	status.master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;
	status.master.accuracy = new_state->root_dispersion;
	status.master.allan_variance = NAN;
	status.master.time_traceable = false;
	status.master.freq_traceable = false;

	status.master.steps_removed = new_state->stratum;

	/* Report the clock class according to the state */
	if (status.state == SYNC_MODULE_STATE_SLAVE) {
		status.master.remote_clock = true;
		status.master.clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
		status.master.time_source = SFPTPD_TIME_SOURCE_NTP;

	} else {
		status.master.remote_clock = false;
		status.master.clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
		status.master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
	}

	status.clustering_score = new_state->clustering_score;

	sfptpd_engine_sync_instance_state_changed(ntp->engine,
						  sfptpd_thread_self(),
						  (struct sfptpd_sync_instance *)ntp,
						  &status);
}


static void ntp_send_rt_stats_update(ntp_module_t *ntp, struct sfptpd_log_time time)
{
	assert(ntp != NULL);

	if (ntp->mode == NTP_MODE_ACTIVE &&
	    ntp->state.state == SYNC_MODULE_STATE_SLAVE) {
		sfptpd_time_t offset = ntp->state.peer_info.peers[ntp->state.selected_peer_idx].offset;

		bool disciplining = (ntp->ctrl_flags & SYNC_MODULE_SELECTED) &&
			ntp->state.sys_info.clock_control_enabled;

		sfptpd_engine_post_rt_stats(ntp->engine,
		                     &time,
		                     SFPTPD_CONFIG_GET_NAME(ntp->config),
		                     "ntp", NULL, sfptpd_clock_get_system_clock(),
		                     disciplining, false,
		                     ntp->synchronized, ntp->state.alarms,
		                     STATS_KEY_OFFSET, offset,
		                     STATS_KEY_END);
	}
}


static void ntp_on_offset_id_change(ntp_module_t *ntp, struct ntp_state *new_state)
{
	DBG_L4("ntp: offset ID changed\n");

	if (ntp->offset_unsafe && !offset_id_is_valid(new_state)) {
		ntp->offset_unsafe = false;
		INFO("ntp: new ntpd offset detected\n");
		sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &ntp->offset_timestamp);
	}

	/* Send updated stats (offset) to the engine */
	struct sfptpd_log_time time;
	sfptpd_log_get_time(&time);
	ntp_send_rt_stats_update(ntp, time);
	ntp_send_clustering_input(ntp);
}


int ntp_configure_ntpd(ntp_module_t *ntp)
{
	int rc;
	struct sfptpd_ntp_module_config *config;
	assert(ntp != NULL);

	/* Set the NTP mode based on whether an instance of NTP has been
	 * defined. The semantics of the configuration for NTP are that if an
	 * instance of NTP has been defined, this implies that the daemon may
	 * use it to provide time. In this case the daemon requires control of
	 * the NTP daemon (NTP must be configured with a local key and key ID
	 * and this key must be provided to sfptpd). If an instance of NTP is
	 * not defined, the NTP daemon will be used in passive mode as a time
	 * reference only. In this case the NTP must not be disciplining the
	 * local system clock. */
	config = (struct sfptpd_ntp_module_config *)
		SFPTPD_CONFIG_CATEGORY_NEXT_INSTANCE(ntp->config);
	if (config != NULL) {
		/* If we have an instance use this as the NTP config. */
		ntp->config = config;
		ntp->mode = NTP_MODE_ACTIVE;
	} else {
		ntp->mode = NTP_MODE_PASSIVE;
	}

	/* Get the NTP configuration - this will either be the instance or the
	 * global configuration. */
	config = ntp->config;

	/* Make sure that if the NTP mode is 'active' then we have a key to
	 * authenticate transactions with the NTP daemon */
	if ((ntp->mode == NTP_MODE_ACTIVE) &&
	    ((config->key_id == 0) || (config->key_value[0] == '\0'))) {
		CRITICAL("ntp: active NTP instance created but no key supplied\n");
		rc = ntp_handle_clock_control_conflict(ntp, EINVAL);
		if (rc != 0)
			return rc;
	}

	/* Checking for systemd-timesyncd.
	 * ntpd and timesyncd seem to be mutually exclusive,
	 * the check for timesyncd comes first as otherwise it can fail. */
	{
		struct sfptpd_prog competitors[] = {
			{ "systemd-timesyncd" },
			{ NULL }
		};

		if (sfptpd_find_running_programs(competitors) != 0) {
			CRITICAL("ntp: systemd-timesyncd is running. sfptpd is incompatible "
				 "with systemd-timesyncd. Please disable it to continue\n");
			return EPROTONOSUPPORT;
		}
	}

	/* Create the NTP daemon clients to handle communications with NTPD */
	rc = sfptpd_ntpclient_create(&(ntp->client),
				     config->key_id, config->key_value);
	if (rc == ENOPROTOOPT) {
		WARNING("ntp: cannot communicate with NTP daemon. NTP daemon "
			"assumed not running\n");
		return 0;
	} else if (rc != 0) {
		CRITICAL("ntp: failed to create ntpd client, %s\n",
			 strerror(rc));
		return rc;
	}

	/* Assume that the NTP daemon is controlling the system clock until the
	 * NTP client tells us otherwise (Note: this is updated as part of the
	 * sys_info struct) */
	ntp->state.sys_info.clock_control_enabled = true;
		
	/* Try updating the system info. If this returns ECONNREFUSED then the
	 * daemon isn't running. Record the state as disabled and return. If
	 * the desired mode is Active then issue a warning. */
	rc = sfptpd_ntpclient_get_sys_info(ntp->client, &ntp->state.sys_info);
	if (rc != 0) {
		if (ntp->mode == NTP_MODE_ACTIVE)
			WARNING("ntp: configured to use NTP but ntpd is not running\n");
		if (rc != ECONNREFUSED)
			WARNING("failed to retrieve NTP system info, %s\n", strerror(rc));
		ntp_parse_state(&ntp->state, rc, ntp->offset_unsafe);
		return 0;
	}

	rc = sfptpd_ntpclient_get_peer_info(ntp->client, &ntp->state.peer_info);
	if (rc != 0) {
		ERROR("ntp: failed to retrieve ntpd peer info, %s\n", strerror(rc));
		goto fail;
	}

	/* If we are in active mode, make sure that the NTP daemon is not
	 * discipling the system clock. This will be updated later once a sync
	 * module instance is Selected as the reference. Note that this
	 * operation does NOT confirm that the NTPd authentication is working,
	 * if the NTPd (chrony) was launched with clock control disabled. */
	if (ntp->mode == NTP_MODE_ACTIVE && ntp->state.sys_info.clock_control_enabled) {
		rc = sfptpd_ntpclient_clock_control(ntp->client, false);
		if (rc != 0) {
			CRITICAL("ntp: failed to disable NTP clock control\n");
			SYNC_MODULE_CONSTRAINT_SET(ntp->state.constraints, MUST_BE_SELECTED);
			rc = ntp_handle_clock_control_conflict(ntp, EINVAL);
			if (rc != 0)
				return rc;
		} else {
			/* Update our local copy of the clock control status */
			ntp->state.sys_info.clock_control_enabled = false;
		}
	}

	/* At this point we have done what we can to get NTP into the right
	 * state.
	 *  - If clock control is enabled/unknown but we want it disabled then
	 *    we have to treat this as a fatal error, otherwise there would be
	 *    two entities adjusting the system clock.
	 *  - If clock control is disabled but we want it enabled this isn't
	 *    fatal but might result in unintended behaviour. Grumble and
	 *    carry on.
	 */
	if ((ntp->mode == NTP_MODE_PASSIVE) && ntp->state.sys_info.clock_control_enabled) {
		if (!sfptpd_clock_is_writable(sfptpd_clock_get_system_clock())) {
			INFO("ntp: sfptpd is configured to not discipline the system clock, ntpd may do so\n");
		} else {
			if (sfptpd_ntpclient_get_features(ntp->client)->get_clock_control)
				ERROR("ntp: ntpd is disciplining the system clock - cannot continue\n");
			else
				ERROR("ntp: ntpd may be disciplining the system clock - cannot continue\n");
			CRITICAL("ntp: failed to disable NTP clock control\n");
			SYNC_MODULE_CONSTRAINT_SET(ntp->state.constraints, MUST_BE_SELECTED);
			rc = ntp_handle_clock_control_conflict(ntp, EBUSY);
			if (rc != 0)
				return rc;
		}
	}

	/* Parse the various state information retrieved from the daemon */
	ntp_parse_state(&ntp->state, 0, ntp->offset_unsafe);
	INFO("ntp: currently in state %s\n", ntp_state_text(ntp->state.state, 0));

fail:
	return rc;
}

bool ntp_state_machine(ntp_module_t *ntp, struct ntp_state *new_state)
{
	int rc;
	bool update;
	struct timespec time_now, time_left;
	assert(ntp != NULL);
	assert(new_state != NULL);

	update = false;

	switch (ntp->query_state) {
	case NTP_QUERY_STATE_SYS_INFO:
		/* Get the system info, then move to the get peer state.
		 * Signal to the caller that the state may have changed */
		rc = sfptpd_ntpclient_get_sys_info(ntp->client, &new_state->sys_info);
		ntp_parse_state(new_state, rc, ntp->offset_unsafe);
		update = true;
		ntp->query_state = NTP_QUERY_STATE_PEER_INFO;
		break;

	case NTP_QUERY_STATE_PEER_INFO:
		/* Get the peer info and move to the sleep state. Signal to the
		 * caller that the state may have changed */
		rc = sfptpd_ntpclient_get_peer_info(ntp->client, &new_state->peer_info);
		ntp_parse_state(new_state, rc, ntp->offset_unsafe);
		update = true;
		ntp->query_state = NTP_QUERY_STATE_SLEEP;
		break;

	case NTP_QUERY_STATE_SLEEP:
		/* Check whether it's time to poll the NTP daemon again */
		(void)clock_gettime(CLOCK_MONOTONIC, &time_now);
		sfptpd_time_subtract(&time_left, &ntp->next_poll_time, &time_now);
		if (time_left.tv_sec < 0) {
			ntp->query_state = NTP_QUERY_STATE_SYS_INFO;
			ntp->next_poll_time.tv_sec += ntp->config->poll_interval;
		}
		/* This state always succeeds */
		rc = 0;
		break;
		
	default:
		assert(false);
		break;
	}

	return update;
}


static void ntp_handle_state_change(ntp_module_t *ntp, struct ntp_state *new_state)
{
	assert(ntp != NULL);
	assert(new_state != NULL);

	if (new_state->state != ntp->state.state) {
		INFO("ntp: changed state from %s to %s\n",
		     ntp_state_text(ntp->state.state, 0),
		     ntp_state_text(new_state->state, 0));

		switch (new_state->state) {
		case SYNC_MODULE_STATE_DISABLED:
			if (ntp->mode == NTP_MODE_ACTIVE)
				WARNING("ntp: ntpd no longer running\n");
			break;

		case SYNC_MODULE_STATE_FAULTY:
			ERROR("ntp: not able to communicate with ntpd\n");
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

	ntp_send_clustering_input(ntp);

	/* If we are in active mode i.e. NTP is running as a real instance
	 * of a sync module that can potentially control the system, we need to
	 * determine whether the state has changed and if so send a message to
	 * the engine. */
	if (ntp->mode != NTP_MODE_ACTIVE)
		return;

	if ((new_state->state != ntp->state.state) ||
	    (new_state->alarms != ntp->state.alarms) ||
	    (new_state->constraints != ntp->state.constraints) ||
	    (new_state->offset_from_master != ntp->state.offset_from_master) ||
	    (new_state->root_dispersion != ntp->state.root_dispersion) ||
	    (new_state->stratum != ntp->state.stratum)) {
		ntp_send_instance_status(ntp, new_state);
	}
}


static void ntp_send_clustering_input(ntp_module_t *ntp)
{
	assert(ntp != NULL);

	if (ntp->ctrl_flags & SYNC_MODULE_CLUSTERING_DETERMINANT) {
		sfptpd_time_t offset = ntp->state.peer_info.peers[ntp->state.selected_peer_idx].offset;

		sfptpd_engine_clustering_input(ntp->engine,
					       SFPTPD_CONFIG_GET_NAME(ntp->config),
					       sfptpd_clock_get_system_clock(),
					       offset,
					       finitel(offset) && offset != 0.0L &&
					       ntp->state.state == SYNC_MODULE_STATE_SLAVE);
	}
}


static void ntp_on_clock_control_change(ntp_module_t *ntp, struct ntp_state *new_state)
{
	int rc;
	bool clock_control;

	assert(ntp != NULL);
	assert(new_state != NULL);

	clock_control = ((ntp->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) != 0);

	if (new_state->sys_info.clock_control_enabled && !clock_control)
		CRITICAL("### ntpd is now disciplining the system clock! ###\n");

	if (!new_state->sys_info.clock_control_enabled && clock_control)
		WARNING("ntp: ntpd is no longer disciplining the system clock!\n");

	/* If we have control, try to stop NTP */
	if ((ntp->mode == NTP_MODE_ACTIVE) &&
	    (new_state->state != SYNC_MODULE_STATE_DISABLED)) {
		INFO("ntp: attempting to restore ntpd clock control state...\n");
		rc = sfptpd_ntpclient_clock_control(ntp->client, clock_control);
		if (rc == 0) {
			new_state->sys_info.clock_control_enabled = clock_control;
			SYNC_MODULE_CONSTRAINT_CLEAR(new_state->constraints, MUST_BE_SELECTED);
			SYNC_MODULE_CONSTRAINT_CLEAR(new_state->constraints, CANNOT_BE_SELECTED);
			INFO("ntp: successfully %sabled ntpd clock control\n",
			     clock_control? "en": "dis");
		} else {
			if (clock_control)
				SYNC_MODULE_CONSTRAINT_SET(new_state->constraints, CANNOT_BE_SELECTED);
			else
				SYNC_MODULE_CONSTRAINT_SET(new_state->constraints, MUST_BE_SELECTED);
			ERROR("ntp: failed to restore ntpd clock control state!\n");
		}
	}
}


static void ntp_on_get_status(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_sync_instance_status *status;

	assert(ntp != NULL);
	assert(msg != NULL);

	/* Note that we don't need to check whether a valid instance has been
	 * provided - there's always an instance of NTP. */

	status = &msg->u.get_status_resp.status;
	status->state = ntp->state.state;
	status->alarms = ntp->state.alarms;
	status->constraints = ntp->state.constraints;
	/* The reference clock for NTP is always the system clock */
	status->clock = sfptpd_clock_get_system_clock();
	status->user_priority = ntp->config->priority;

	sfptpd_time_float_ns_to_timespec(ntp->state.offset_from_master,
					 &status->offset_from_master);
	status->local_accuracy = SFPTPD_ACCURACY_NTP;

	status->clustering_score = ntp->state.clustering_score;

	status->master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;

	if (ntp->state.state == SYNC_MODULE_STATE_SLAVE) {
		status->master.remote_clock = true;
		status->master.clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
		status->master.time_source = SFPTPD_TIME_SOURCE_NTP;
		status->master.accuracy = ntp->state.root_dispersion;
		status->master.allan_variance = NAN;
		status->master.time_traceable = false;
		status->master.freq_traceable = false;
		status->master.steps_removed = ntp->state.stratum;
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


static void ntp_on_control(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	sfptpd_sync_module_ctrl_flags_t flags;
	int rc;

	assert(ntp != NULL);
	assert(msg != NULL);

	flags = ntp->ctrl_flags;

	/* We only allow control of NTP if the user configured an instance of
	 * the NTP daemon i.e. we are in the 'Active' mode.
	 * Note that we don't need to check whether a valid instance has been
	 * provided - there's always an instance of NTP. */
	if (ntp->mode != NTP_MODE_ACTIVE) {
		WARNING("ntp: cannot change control flags- no NTP instance\n");
		// TODO @bug64228 provide a generic error code in messages.
		SFPTPD_MSG_REPLY(msg);
		return;
	}

	/* Update the flags based on the message */
	flags &= ~msg->u.control_req.mask;
	flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	/* For the NTP sync module, only the clock control flag has meaning. */
	if ((flags ^ ntp->ctrl_flags) & SYNC_MODULE_CLOCK_CTRL) {
		bool clock_control = ((flags & SYNC_MODULE_CLOCK_CTRL) != 0);
		rc = sfptpd_ntpclient_clock_control(ntp->client, clock_control);
		if (rc == 0) {
			ntp->state.sys_info.clock_control_enabled = clock_control;
			SYNC_MODULE_CONSTRAINT_CLEAR(ntp->state.constraints, MUST_BE_SELECTED);
			SYNC_MODULE_CONSTRAINT_CLEAR(ntp->state.constraints, CANNOT_BE_SELECTED);
			DBG_L2("ntp: successfully %sabled ntpd clock control\n",
			       clock_control? "en": "dis");
		} else {
			ERROR("ntp: failed to change ntpd clock control, %s!\n",
			      strerror(rc));
			if (clock_control != ntp->state.sys_info.clock_control_enabled) {
				if (clock_control)
					SYNC_MODULE_CONSTRAINT_SET(ntp->state.constraints, CANNOT_BE_SELECTED);
				else
					SYNC_MODULE_CONSTRAINT_SET(ntp->state.constraints, MUST_BE_SELECTED);
			}
		}
	}

	ntp->ctrl_flags = flags;

	SFPTPD_MSG_REPLY(msg);
}


static void ntp_on_step_clock(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	assert(ntp != NULL);
	assert(msg != NULL);

	/* Invalidate offset until NTP next queries the peers. */
	ntp->offset_unsafe = true;
	INFO("ntp: clock step- ignoring ntpd offset until next update\n");

	SFPTPD_MSG_REPLY(msg);
}


static void ntp_on_log_stats(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	assert(ntp != NULL);
	assert(msg != NULL);

	ntp_send_rt_stats_update(ntp, msg->u.log_stats_req.time);
	ntp_send_clustering_input(ntp);

	SFPTPD_MSG_FREE(msg);
}


static void ntp_on_save_state(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_clock *clock;
	unsigned int num_candidates, i;
	char constraints[SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX];
	char alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];
	char flags[256];

	assert(ntp != NULL);
	assert(msg != NULL);

	sfptpd_sync_module_alarms_text(ntp->state.alarms, alarms, sizeof(alarms));
	sfptpd_sync_module_constraints_text(ntp->state.constraints, constraints, sizeof(constraints));
	sfptpd_sync_module_ctrl_flags_text(ntp->ctrl_flags, flags, sizeof(flags));

	clock = sfptpd_clock_get_system_clock();

	for (i = 0, num_candidates = 0; i < ntp->state.peer_info.num_peers; i++) {
		if (ntp->state.peer_info.peers[i].candidate)
			num_candidates++;
	}

	if (ntp->state.state == SYNC_MODULE_STATE_SLAVE) {
		char host[NI_MAXHOST] = "";
		struct sfptpd_ntpclient_peer *peer;
		int rc;

		peer = &ntp->state.peer_info.peers[ntp->state.selected_peer_idx];

		rc = getnameinfo((struct sockaddr *) &peer->remote_address,
				 peer->remote_address_len,
				 host, sizeof host,
				 NULL, 0, NI_NUMERICHOST);
		if (rc != 0) {
			DBG_L4("ntp: getnameinfo: %s\n", gai_strerror(rc));
		}

		sfptpd_log_write_state(clock,
			SFPTPD_CONFIG_GET_NAME(ntp->config),
			"instance: %s\n"
			"clock-name: %s\n"
			"state: %s\n"
			"alarms: %s\n"
			"constraints: %s\n"
			"control-flags: %s\n"
			"offset-from-peer: " SFPTPD_FORMAT_FLOAT "\n"
			"in-sync: %d\n"
			"selected-peer: %s\n"
			"num-peers: %d\n"
			"num-candidates: %d\n"
			"clustering-score: %d\n",
			SFPTPD_CONFIG_GET_NAME(ntp->config),
			sfptpd_clock_get_long_name(clock),
			ntp_state_text(ntp->state.state, 0),
			alarms, constraints, flags, peer->offset,
			ntp->synchronized,
			host,
			ntp->state.peer_info.num_peers,
			num_candidates,
			ntp->state.clustering_score);
	} else {
		sfptpd_log_write_state(clock,
			SFPTPD_CONFIG_GET_NAME(ntp->config),
			"instance: %s\n"
			"clock-name: %s\n"
			"state: %s\n"
			"alarms: %s\n"
			"constraints: %s\n"
			"control-flags: %s\n"
			"num-peers: %d\n"
			"num-candidates: %d\n",
			SFPTPD_CONFIG_GET_NAME(ntp->config),
			sfptpd_clock_get_long_name(clock),
			ntp_state_text(ntp->state.state, 0),
			alarms, constraints, flags,
			ntp->state.peer_info.num_peers,
			num_candidates);
	}

	SFPTPD_MSG_FREE(msg);
}

static void ntp_on_write_topology(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	FILE *stream;
	char alarms[256];
	struct sfptpd_clock *clock;
	char host[NI_MAXHOST] = "";
	struct sfptpd_ntpclient_peer *peer;
	int rc;

	assert(ntp != NULL);
	assert(msg != NULL);

	/* This should only be called on selected instances */
	assert(ntp->ctrl_flags & SYNC_MODULE_SELECTED);

	peer = &ntp->state.peer_info.peers[ntp->state.selected_peer_idx];

	rc = getnameinfo((struct sockaddr *) &peer->remote_address,
			 peer->remote_address_len,
			 host, sizeof host,
			 NULL, 0, NI_NUMERICHOST);
	if (rc != 0) {
		DBG_L4("ntp: getnameinfo: %s\n", gai_strerror(rc));
	}

	stream = msg->u.write_topology_req.stream;

	clock = sfptpd_clock_get_system_clock();

	fprintf(stream, "====================\nstate: %s\n",
		ntp_state_text(ntp->state.state, 0));

	if (ntp->state.alarms != 0) {
		sfptpd_sync_module_alarms_text(ntp->state.alarms, alarms, sizeof(alarms));
		fprintf(stream, "alarms: %s\n", alarms);
	}

	fprintf(stream, "====================\n\n");

	sfptpd_log_topology_write_field(stream, true, "ntp");

	switch (ntp->state.state) {
	case SYNC_MODULE_STATE_LISTENING:
	case SYNC_MODULE_STATE_SELECTION:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "?");
		break;

	case SYNC_MODULE_STATE_SLAVE:
		sfptpd_log_topology_write_field(stream, true, "selected-peer");
		sfptpd_log_topology_write_field(stream, true, "%s", host);
		sfptpd_log_topology_write_1to1_connector(stream, false, true,
							 SFPTPD_FORMAT_TOPOLOGY_FLOAT,
							 peer->offset);
		break;

	default:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "X");
		break;
	}

	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_long_name(clock));
	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_hw_id_string(clock));

	SFPTPD_MSG_REPLY(msg);
}


static void ntp_on_stats_end_period(ntp_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	assert(ntp != NULL);
	assert(msg != NULL);

	sfptpd_stats_collection_end_period(&ntp->stats,
					   &msg->u.stats_end_period_req.time);

	/* Write the historical statistics to file */
	sfptpd_stats_collection_dump(&ntp->stats, sfptpd_clock_get_system_clock(),
								 SFPTPD_CONFIG_GET_NAME(ntp->config));

	SFPTPD_MSG_FREE(msg);
}


static void ntp_on_run(ntp_module_t *ntp)
{
	struct timespec interval;
	int rc;

	interval.tv_sec = 0;
	interval.tv_nsec = NTP_POLL_INTERVAL;

	/* Start a single-shot timer for polling. This is rearmed
	   after the timer event has been handled because if polling
	   times out repeatedly events would get queued up if a
	   periodic timer were used. */
	rc = sfptpd_thread_timer_start(NTP_POLL_TIMER_ID,
				       false, false, &interval);
	if (rc != 0) {
		CRITICAL("ntp: failed to start poll timer, %s\n", strerror(rc));

		/* We can't carry on in this case */
		sfptpd_thread_exit(rc);
	}

	/* Determine the time when we should next poll the NTP daemon */
	(void)clock_gettime(CLOCK_MONOTONIC, &ntp->next_poll_time);
	ntp->query_state = NTP_QUERY_STATE_SYS_INFO;
	ntp->offset_unsafe = false;

	/* Send initial status */
	if (ntp->mode == NTP_MODE_ACTIVE) {
		ntp_on_offset_id_change(ntp, &ntp->state);
		ntp_send_instance_status(ntp, &ntp->state);
		ntp_stats_update(ntp);
	}
}


static void ntp_on_timer(void *user_context, unsigned int id)
{
	ntp_module_t *ntp = (ntp_module_t *)user_context;
	struct ntp_state new_state;
	bool update;
	struct timespec interval;
	int rc;

	assert(ntp != NULL);

	/* Progress the NTP state machine. We take a copy of the existing
	 * ntp state and this is updated by the state machine */
	new_state = ntp->state;
	update = ntp_state_machine(ntp, &new_state);

	if (update) {
		if (new_state.sys_info.clock_control_enabled !=
		    ntp->state.sys_info.clock_control_enabled)
			ntp_on_clock_control_change(ntp, &new_state);

		if (!offset_ids_equal(&new_state, &ntp->state))
			ntp_on_offset_id_change(ntp, &new_state);

		/* Handle changes in the state */
		ntp_handle_state_change(ntp, &new_state);
		
		/* Store the new state */
		ntp->state = new_state;

		/* Update the convergence criteria */
		ntp_convergence_update(ntp);

		/* Update historical stats */
		ntp_stats_update(ntp);
	}

	interval.tv_sec = 0;
	interval.tv_nsec = NTP_POLL_INTERVAL;

	rc = sfptpd_thread_timer_start(NTP_POLL_TIMER_ID,
				       false, false, &interval);
	if (rc != 0) {
		CRITICAL("ntp: failed to rearm poll timer, %s\n", strerror(rc));
		sfptpd_thread_exit(rc);
	}
}


static int ntp_on_startup(void *context)
{
	ntp_module_t *ntp = (ntp_module_t *)context;
	int rc;

	assert(ntp != NULL);

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. */
	ntp->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Configure the NTP daemon based on the configuration options */
	rc = ntp_configure_ntpd(ntp);
	if (rc != 0)
		goto fail;

	ntp_convergence_init(ntp);

	rc = ntp_stats_init(ntp);
	if (rc != 0) {
		CRITICAL("ntp: failed to create statistics collection, %s\n",
			 strerror(rc));
		goto fail;
	}

	/* Create a timer which will be used to poll NTP */
	rc = sfptpd_thread_timer_create(NTP_POLL_TIMER_ID, CLOCK_MONOTONIC,
					ntp_on_timer, ntp);
	if (rc != 0) {
		CRITICAL("ntp: failed to create poll timer, %s\n", strerror(rc));
		goto fail;
	}

	return 0;

fail:
	sfptpd_stats_collection_free(&ntp->stats);
	if (ntp->client != NULL)
		sfptpd_ntpclient_destroy(&(ntp->client));
	return rc;
}


static void ntp_on_shutdown(void *context)
{
	ntp_module_t *ntp = (ntp_module_t *)context;
	assert(ntp != NULL);

	sfptpd_ntpclient_destroy(&(ntp->client));

	sfptpd_stats_collection_free(&ntp->stats);

	/* Delete the sync module context */
	free(ntp);
}


static void ntp_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	ntp_module_t *ntp = (ntp_module_t *)context;
	sfptpd_sync_module_msg_t *msg = (sfptpd_sync_module_msg_t *)hdr;

	assert(ntp != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		ntp_on_run(ntp);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_GET_STATUS:
		ntp_on_get_status(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_CONTROL:
		ntp_on_control(ntp, msg);
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
		ntp_on_step_clock(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LOG_STATS:
		ntp_on_log_stats(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_SAVE_STATE:
		ntp_on_save_state(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY:
		ntp_on_write_topology(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		ntp_on_stats_end_period(ntp, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_TEST_MODE:
		/* This module doesn't have any test modes */
		SFPTPD_MSG_FREE(msg);
		break;

	default:
		WARNING("ntp: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void ntp_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	/* The NTP module doesn't use user file descriptors */
}


static const struct sfptpd_thread_ops ntp_thread_ops = 
{
	ntp_on_startup,
	ntp_on_shutdown,
	ntp_on_message,
	ntp_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void ntp_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_NTP);
	free(section);
}


static struct sfptpd_config_section *ntp_config_create(const char *name,
						       enum sfptpd_config_scope scope,
						       bool allows_instances,
						       const struct sfptpd_config_section *src)
{
	struct sfptpd_ntp_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_NTP));

	new = (struct sfptpd_ntp_module_config *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("ntp %s: failed to allocate memory for NTP configuration\n", name);
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
	} else {
		new->priority = SFPTPD_DEFAULT_PRIORITY;
		new->convergence_threshold = 0.0;
		new->poll_interval = 1;
		new->key_id = 0;
		new->key_value[0] = '\0';
	}

	/* If this is an implicitly created sync instance, give it the lowest
	   possible user priority. */
	if (name == NULL) {
		name = "ntp0";
		new->priority = INT_MAX;
	}

	SFPTPD_CONFIG_SECTION_INIT(new, ntp_config_create, ntp_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_NTP,
				   scope, allows_instances, name);

	return &new->hdr;
}


int sfptpd_ntp_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_ntp_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_ntp_module_config *)
		ntp_config_create(SFPTPD_NTP_MODULE_NAME,
				  SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	SFPTPD_CONFIG_SECTION_ADD(config, new);

	/* Register the configuration options */
	sfptpd_config_register_options(&ntp_config_option_set);
	return 0;
}


struct sfptpd_ntp_module_config *sfptpd_ntp_module_get_config(struct sfptpd_config *config)
{
	return (struct sfptpd_ntp_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_NTP);
}


void sfptpd_ntp_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name)
{
	/* For NTP no interface is required */
}


int sfptpd_ntp_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_subscribers)
{
	sfptpd_ntp_module_config_t *instance_config;
	ntp_module_t *ntp;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	DBG_L3("ntp: creating sync-module\n");

	*sync_module = NULL;
	ntp = (ntp_module_t *)calloc(1, sizeof(*ntp));
	if (ntp == NULL) {
		CRITICAL("ntp: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	/* Keep a handle to the sync engine */
	ntp->engine = engine;

	/* Find the NTP global configuration. If this doesn't exist then
	 * something has gone badly wrong. */
	ntp->config = sfptpd_ntp_module_get_config(config);
	if (ntp->config == NULL) {
		CRITICAL("ntp: failed to find NTP configuration\n");
		free(ntp);
		return ENOENT;
	}

	/* Find the nominal instance configuration */
	instance_config = (struct sfptpd_ntp_module_config *)
		sfptpd_config_category_first_instance(config,
						      SFPTPD_CONFIG_CATEGORY_NTP);

	/* Set up the clustering evaluator */
	ntp->state.clustering_evaluator.calc_fn = sfptpd_engine_calculate_clustering_score;
	ntp->state.clustering_evaluator.private = engine;
	ntp->state.clustering_evaluator.instance_name = instance_config->hdr.name;

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("ntp", &ntp_thread_ops, ntp, sync_module);
	if (rc != 0) {
		free(ntp);
		return rc;
	}

	/* If a buffer is provided write the instances into it */
	if ((instances_info_buffer != NULL) && (instances_info_entries >= 1)) {
		memset(instances_info_buffer, 0,
		       instances_info_entries * sizeof(*instances_info_buffer));
		instances_info_buffer->module = *sync_module;
		instances_info_buffer->handle = (struct sfptpd_sync_instance *)ntp;
		instances_info_buffer->name = instance_config->hdr.name;
	}

	return 0;
}


/* fin */
