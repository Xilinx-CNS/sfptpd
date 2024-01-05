/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2024 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_crny_module.c
 * @brief  Chrony Synchronization Module
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
#include <regex.h>

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

#include "sfptpd_crny_module.h"
#include "sfptpd_crny_proto.h"


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

enum ntp_query_state {
	NTP_QUERY_STATE_SLEEP_DISCONNECTED,
	NTP_QUERY_STATE_SLEEP_CONNECTED,
	NTP_QUERY_STATE_CONNECT,
	NTP_QUERY_STATE_CONNECT_WAIT,
	NTP_QUERY_STATE_SYS_INFO,
	NTP_QUERY_STATE_SOURCE_COUNT,
	NTP_QUERY_STATE_SOURCE_DATUM,
	NTP_QUERY_STATE_NTP_DATUM,
};

enum ntp_query_event {
	NTP_QUERY_EVENT_NO_EVENT,
	NTP_QUERY_EVENT_RUN,
	NTP_QUERY_EVENT_TICK,
	NTP_QUERY_EVENT_TRAFFIC,
	NTP_QUERY_EVENT_CONN_LOST,
	NTP_QUERY_EVENT_REPLY_TIMEOUT,
};

enum ntp_stats_ids {
	NTP_STATS_ID_OFFSET,
	NTP_STATS_ID_SYNCHRONIZED
};


/* operation for clock control script */
enum chrony_clock_control_op {
	CRNY_CTRL_OP_NOP,
	CRNY_CTRL_OP_ENABLE,
	CRNY_CTRL_OP_DISABLE,
	CRNY_CTRL_OP_SAVE,
	CRNY_CTRL_OP_RESTORE,
	CRNY_CTRL_OP_RESTORENORESTART,
};

struct ntp_state {
	/* NTP module state */
	sfptpd_sync_module_state_t state;

	/* Alarms */
	sfptpd_sync_module_alarms_t alarms;

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

	/* Boolean indicating that the clock has been stepped and that the
	 * recorded NTP offset may not be correct. The offset will be
	 * invalidated until the NTP daemon next polls its peers */
	bool offset_unsafe;

	/* System time at which offset was last updated */
	struct timespec offset_timestamp;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchonized to the master */
	bool synchronized;

	/* Clustering evaluator */
	struct sfptpd_clustering_evaluator clustering_evaluator;

	/* Clustering score */
	int clustering_score;
};


struct crny_comm {

	/* Chrony command request */
	struct crny_cmd_request req;

	/* Chrony command response */
	struct crny_cmd_response resp;

	/* Chrony command address */
	struct sockaddr_un remote;

	/* Chrony command socket */
	int sock;

	/* Chrony command socket path */
	char unix_sock_path[112];
};


typedef struct sfptpd_crny_module {
	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Pointer to the configuration */
	struct sfptpd_crny_module_config *config;

	/* Which elements of the NTP daemon are enabled */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* Constraints */
	sfptpd_sync_module_constraints_t constraints;

	/* NTP daemon query state. */
	enum ntp_query_state query_state;
	int query_src_idx;

	/* Time for next poll of the NTP daemon */
	struct timespec next_poll_time;

	/* Time for control reply timeout */
	struct timespec reply_expiry_time;

	/* NTP module state */
	struct ntp_state state;

	/* next NTP module state */
	struct ntp_state next_state;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;

	/* Stats collected in sync module */
	struct sfptpd_stats_collection stats;

	/* Control communications */
	struct crny_comm crny_comm;

	/* Save state of clock control at chrony launch */
	bool chrony_state_saved;
	bool clock_control_at_save;

	/* Have currently blocked the system clock */
	bool have_blocked_sys;

	/* Whether we have entered the RUNning phase */
	bool running_phase;
} crny_module_t;


/****************************************************************************
 * Constants
 ****************************************************************************/

#define MODULE SFPTPD_CRNY_MODULE_NAME

#define NTP_POLL_INTERVAL (250000000)
#define NTP_POLL_TIMER_ID (0)

#define REPLY_TIMEOUT (1000000000)

static const struct sfptpd_stats_collection_defn ntp_stats_defns[] =
{
	{NTP_STATS_ID_OFFSET,       SFPTPD_STATS_TYPE_RANGE, "offset-from-peer", "ns", 0},
	{NTP_STATS_ID_SYNCHRONIZED, SFPTPD_STATS_TYPE_COUNT, "synchronized"}
};

static const char *query_state_names[] = {
	"SLEEP_DISCONNECTED",
	"SLEEP_CONNECTED",
	"CONNECT",
	"CONNECT_WAIT",
	"SYS_INFO",
	"SOURCE_COUNT",
	"SOURCE_DATUM",
	"NTP_DATUM",
};

static const char *query_event_names[] = {
	"NO_EVENT",
	"RUN",
	"TICK",
	"TRAFFIC",
	"CONN_LOST",
	"REPLY_TIMEOUT",
};

/****************************************************************************
 * Function prototypes
 ****************************************************************************/

static bool crny_state_machine(crny_module_t *ntp, enum ntp_query_event);
static void ntp_send_clustering_input(crny_module_t *ntp, struct ntp_state *state);


/****************************************************************************
 * Configuration
 ****************************************************************************/

static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *)section;
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
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *)section;
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
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *)section;
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


static int parse_clock_control(struct sfptpd_config_section *section,
			       const char *option,
			       unsigned int num_params,
			       const char * const params[])
{
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *)section;
	assert(num_params == 1);

	if (!strcmp(params[0], "off"))
		ntp->clock_control = false;
	else if (!strcmp(params[0], "on"))
		ntp->clock_control = true;
	else
		return EINVAL;

	return 0;
}


static int parse_control_script(struct sfptpd_config_section *section,
				const char *option,
				unsigned int num_params,
				const char * const params[])
{
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *)section;
	regex_t legacy_path;

	assert(num_params == 1);

	if (regcomp(&legacy_path, SFPTPD_CRNY_LEGACY_CONTROL_SCRIPT_PATTERN,
		    REG_EXTENDED | REG_NOSUB) != 0)
		return EBADMSG;

	if (regexec(&legacy_path, params[0], 0, NULL, 0) == 0) {
		sfptpd_strncpy(ntp->chronyd_script,
			       SFPTPD_CRNY_DEFAULT_CONTROL_SCRIPT,
			       sizeof ntp->chronyd_script);
		WARNING("crny: legacy chronyd_script path \"%s\" replaced with "
			"\"%s\"; please update configuration.\n",
			params[0], ntp->chronyd_script);
	} else {
		sfptpd_strncpy(ntp->chronyd_script, params[0],
			       sizeof ntp->chronyd_script);
	}

	regfree(&legacy_path);

	/* Implicitly enable clock control. */
	ntp->clock_control = true;

	return 0;
}


static const sfptpd_config_option_t ntp_config_options[] =
{
	{"priority", "<NUMBER>",
		"Relative priority of sync module instance. Smaller values have higher "
		"priority. The default 128.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_priority},
	{"sync_threshold", "<NUMBER>",
		"Threshold in nanoseconds of the offset from the clock source over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync (converged). The default is "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_NTP) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_sync_threshold},
	{"ntp_poll_interval", "NUMBER",
		"Specifies the NTP daemon poll interval in seconds. Default value 1",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_ntp_poll_interval},
	{"clock_control", "<off | on>",
		"Whether to invoke helper script to enable or disable chronyd "
		"clock control. Off by default.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false, parse_clock_control},
	{"control_script", "<filename>",
		"Specifes the path to a script which can be used to enable or "
		"disable chronyd clock control. If the legacy examples "
		"installation location is specified this will be replaced by "
		"the default location which is: "
		STRINGIFY(SFPTPD_CRNY_DEFAULT_CONTROL_SCRIPT),
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false, parse_control_script},
};

static int crny_validate_config(struct sfptpd_config_section *section)
{
	sfptpd_crny_module_config_t *ntp = (sfptpd_crny_module_config_t *) section;
	int rc = 0;

	assert(ntp != NULL);

	if (ntp->clock_control &&
	    access(ntp->chronyd_script, X_OK) != 0) {
		rc = errno;
		CFG_ERROR(section, "chronyd clock control requested but "
			  "specified control script \"%s\" is unusable: %s\n",
			  ntp->chronyd_script, strerror(rc));
	}

	return rc;
}

static const sfptpd_config_option_set_t ntp_config_option_set =
{
	.description = "Chrony Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_CRNY,
	.num_options = sizeof(ntp_config_options)/sizeof(ntp_config_options[0]),
	.options = ntp_config_options,
	.validator = crny_validate_config,
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

const char *crny_state_text(sfptpd_sync_module_state_t state, unsigned int alarms)
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


/* where x is the unsigned input and i is the index of the sign bit */
int32_t sfptpd_crny_tosigned(uint32_t x, int i){
	int32_t out = x & ((1 << i) - 1);
	if (x & (1 << i)) {
		/* if the sign bit is set, then negate the magnitude */
		out -= (1 << i);
	}
	return out;
}


/* convert chrony floating type to system floating type */
double sfptpd_crny_tofloat(uint32_t x){
	uint32_t exp, coef, coef_bitmask;
	/* exp is top 7 bits of float */
	exp = x >> 25;
	/* coef is bottom 25 bits of float */
	coef_bitmask = (1 << 25) - 1;
	coef = x & coef_bitmask;
	/* correct for signedness */
	int32_t exp_signed = sfptpd_crny_tosigned(exp, 6);
	int32_t coef_signed = sfptpd_crny_tosigned(coef, 24);
	/* do the computation */
	return coef_signed * pow(2.0L, exp_signed - 25);
}


int sfptpd_crny_addr_to_sockaddr(struct sockaddr_storage *addr,
				 socklen_t *length,
				 struct crny_addr *ip_addr)
{
	unsigned int addr_family = ntohs(ip_addr->addr_family);
	int rc = 0;

	if (addr_family == IP_V6) {
		struct sockaddr_in6 *sin6 = ((struct sockaddr_in6 *) addr);
		*length = sizeof *sin6;
		memset(sin6, '\0', sizeof *sin6);
		sin6->sin6_family = AF_INET6;
		memcpy(&sin6->sin6_addr, &ip_addr->addr_union.v6_addr,
		       sizeof(ip_addr->addr_union.v6_addr));
	} else if (addr_family == IP_V4) {
		struct sockaddr_in *sin = ((struct sockaddr_in *) addr);
		*length = sizeof *sin;
		memset(sin, '\0', sizeof *sin);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = ip_addr->addr_union.v4_addr;
	} else {
		if (addr_family != IP_UNSPEC)
			DBG_L6("crny: unexpected chrony address type %d\n",
				addr_family);
		*length = sizeof *addr;
		memset(addr, '0', sizeof *addr);
		addr->ss_family = AF_UNSPEC;
		rc = EINVAL;
	}

	return rc;
}

static void ntp_convergence_init(crny_module_t *ntp)
{
	long double threshold;

	assert(ntp != NULL);

	/* Initialise the convergence measure. */
	ntp->state.synchronized = false;
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


static bool ntp_convergence_update(crny_module_t *ntp, struct ntp_state *new_state)
{
	struct timespec time;
	struct sfptpd_ntpclient_peer *peer;
	int rc;
	assert(ntp != NULL);

	rc = clock_gettime(CLOCK_MONOTONIC, &time);
	if (rc < 0) {
		ERROR("crny: failed to get monotonic time, %s\n", strerror(errno));
	}

	/* If not in the slave state or we failed to get the time for some
	 * reason, reset the convergence measure. */
	if ((rc < 0) || (new_state->state != SYNC_MODULE_STATE_SLAVE)) {
		new_state->synchronized = false;
		sfptpd_stats_convergence_reset(&ntp->convergence);
	} else if ((new_state->alarms != 0) ||
		   ((ntp->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* If one or more alarms is triggered or timestamp processing
		 * is disabled, we consider the slave to be unsynchronized.
		 * However, don't reset the convergence measure as it is
		 * probably a temporary situation. */
		new_state->synchronized = false;
	} else {
		assert(new_state->selected_peer_idx != -1);
		peer = &new_state->peer_info.peers[new_state->selected_peer_idx];

		/* Update the synchronized state based on the current offset
		 * from master */
		new_state->synchronized = sfptpd_stats_convergence_update(&ntp->convergence,
									  time.tv_sec,
									  peer->offset);
	}

	return new_state->synchronized != ntp->state.synchronized;
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

static void chrony_req_initialize(struct crny_cmd_request* req, uint16_t cmd)
{
	memset(req, '\0', sizeof *req);
	*req = CMD_REQ_DEFAULT;
	req->randoms = rand();
	req->cmd1 = htons(cmd);
}

int crny_stats_init(crny_module_t *ntp)
{
	int rc;
	assert(ntp != NULL);

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&ntp->stats, "ntp",
					    sizeof(ntp_stats_defns)/sizeof(ntp_stats_defns[0]),
					    ntp_stats_defns);
	return rc;
}


void crny_stats_update(crny_module_t *ntp)
{
	struct sfptpd_stats_collection *stats;
	struct sfptpd_ntpclient_peer *peer;
	assert(ntp != NULL);

	stats = &ntp->stats;

	/* The offset is only available if we are in slave mode */
	if (ntp->state.state == SYNC_MODULE_STATE_SLAVE) {
		assert(ntp->state.selected_peer_idx != -1);
		assert(ntp->state.selected_peer_idx < SFPTPD_NTP_PEERS_MAX);
		assert(ntp->state.selected_peer_idx < ntp->state.peer_info.num_peers);
		peer = &ntp->state.peer_info.peers[ntp->state.selected_peer_idx];
		/* Offset, frequency correction, one-way-delay */
		sfptpd_stats_collection_update_range(stats, NTP_STATS_ID_OFFSET, peer->offset,
						     ntp->state.offset_timestamp, true);
	} else {
		struct timespec now;
		sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &now);
		sfptpd_stats_collection_update_range(stats, NTP_STATS_ID_OFFSET, 0.0,
						     now, false);
	}

	sfptpd_stats_collection_update_count(stats, NTP_STATS_ID_SYNCHRONIZED, ntp->state.synchronized? 1: 0);
}


void crny_parse_state(struct ntp_state *state, int rc, bool offset_unsafe)
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
		state->sys_info.peer_address_len = 0;
		state->sys_info.clock_control_enabled = false;
		state->selected_peer_idx = -1;
		state->peer_info.num_peers = 0;
		reset_offset_id(state);
		return;
	}

	candidates = false;
	state->selected_peer_idx = -1;
	assert(state->peer_info.num_peers <= SFPTPD_NTP_PEERS_MAX);
	for (i = 0; i < state->peer_info.num_peers; i++) {
		peer = &state->peer_info.peers[i];

		/* Ignore ourselves */
		if (peer->self)
			continue;

		if (peer->selected) {
			if (state->selected_peer_idx != -1)
				WARNING("crny: ntpd reporting more than one selected peer\n");
			else
				state->selected_peer_idx = i;
		}

		/* If we find a candidate or shortlisted node, set a flag */
		if (peer->candidate || peer->shortlist)
			candidates = true;
	}

	if (state->selected_peer_idx != -1) {
		assert(state->selected_peer_idx < SFPTPD_NTP_PEERS_MAX);
		assert(state->selected_peer_idx < state->peer_info.num_peers);
		set_offset_id(state, &state->peer_info.peers[state->selected_peer_idx]);
	} else {
		reset_offset_id(state);
	}

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

static void crny_close_socket(crny_module_t *ntp)
{
	if (ntp->crny_comm.sock != -1) {
		sfptpd_thread_user_fd_remove(ntp->crny_comm.sock);
		close(ntp->crny_comm.sock);
		ntp->crny_comm.sock = -1;
		unlink(ntp->crny_comm.unix_sock_path);
	}
}

static void block_clock(crny_module_t *ntp)
{
	if (!ntp->have_blocked_sys) {
		INFO("crny: blocking system clock\n");
		sfptpd_clock_set_blocked(sfptpd_clock_get_system_clock(), true);
		ntp->have_blocked_sys = true;
	}
}

static void unblock_clock(crny_module_t *ntp)
{
	if (ntp->have_blocked_sys) {
		INFO("crny: unblocking system clock\n");
		sfptpd_clock_set_blocked(sfptpd_clock_get_system_clock(), false);
		ntp->have_blocked_sys = false;
	}
}

static bool clock_control_at_launch(crny_module_t *ntp)
{
	pid_t pid;
	int fd;
	int rc;
	ssize_t sz;
	bool assume_absent = true;
	enum {
		OPT_START,
		OPT_MINUS,
		OPT_X,
		OPT_IGNORE,
	} state = OPT_START;

	struct sfptpd_prog chrony[] = {
		{ "chronyd" },
		{ NULL }
	};

	if (sfptpd_find_running_programs(chrony) == 0) {
		DBG_L6("crny: chrony static check: not running\n");
		goto finish;
	}

	DBG_L6("crny: chrony static check: running (%d, %s)\n",
		chrony[0].a_pid, chrony[0].a_program);

	pid = chrony[0].a_pid;

	/* Consider clock control to be enabled if chronyd is launched
	   without the '-x' option.
	 */

	char buf[PATH_MAX];

	rc = snprintf(buf, sizeof buf, "/proc/%d/cmdline", pid);
	assert(rc < sizeof buf);

	fd = open(buf, O_RDONLY);
	if (fd == -1)
		goto finish;

	while ((sz = read(fd, buf, sizeof buf)) > 0 && state != OPT_X) {
		int i;
		for (i = 0; i < sz && state != OPT_X; i++) {
			if (buf[i] == '\0') {
				state = OPT_START;
			} else if (state == OPT_START) {
				if (buf[i] == '-')
					state = OPT_MINUS;
				else
					state = OPT_IGNORE;
			} else if (state == OPT_MINUS) {
				if (buf[i] == 'x')
					state = OPT_X;
				else
					state = OPT_IGNORE;
			}
		}
	}

	if (sz != -1)
		assume_absent = false;

	close(fd);

finish:
	if (!assume_absent && strlen(ntp->config->chronyd_script) == 0) {
		if (state == OPT_X) {
			SYNC_MODULE_CONSTRAINT_SET(ntp->constraints, CANNOT_BE_SELECTED);
			SYNC_MODULE_CONSTRAINT_CLEAR(ntp->constraints, MUST_BE_SELECTED);
		} else {
			SYNC_MODULE_CONSTRAINT_SET(ntp->constraints, MUST_BE_SELECTED);
			SYNC_MODULE_CONSTRAINT_CLEAR(ntp->constraints, CANNOT_BE_SELECTED);
		}
	} else {
		SYNC_MODULE_CONSTRAINT_CLEAR(ntp->constraints, MUST_BE_SELECTED);
		SYNC_MODULE_CONSTRAINT_CLEAR(ntp->constraints, CANNOT_BE_SELECTED);
	}

	return assume_absent || state == OPT_X ? 0 : 1;
}

int crny_configure_ntpd(crny_module_t *ntp)
{
	struct sfptpd_crny_module_config *config;
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
	config = (struct sfptpd_crny_module_config *)
		SFPTPD_CONFIG_CATEGORY_NEXT_INSTANCE(ntp->config);
	if (config != NULL) {
		/* If we have an instance use this as the NTP config. */
		ntp->config = config;
	} else {
		/* This is where the ntp module determines active/passive but
		   let's make this unrelated to config presence for crny. */
	}

	/* Get the NTP configuration - this will either be the instance or the
	 * global configuration. */
	config = ntp->config;

	/* Checking for systemd-timesyncd.
	 * ntpd and timesyncd seem to be mutually exclusive,
	 * the check for timesyncd comes first as otherwise it can fail. */
	{
		struct sfptpd_prog competitors[] = {
			{ "systemd-timesyncd" },
			{ NULL }
		};

		if (sfptpd_find_running_programs(competitors) != 0) {
			CRITICAL("crny: systemd-timesyncd is running. sfptpd is incompatible "
				 "with systemd-timesyncd. Please disable it to continue\n");
			return EPROTONOSUPPORT;
		}
	}

	snprintf(ntp->crny_comm.unix_sock_path, 100, "/var/run/chrony/chronyc.%d.sock",
		 (int)getpid());

	/* Assume that the NTP daemon is controlling the system clock until the
	 * NTP client tells us otherwise (Note: this is updated as part of the
	 * sys_info struct) */
	if ((ntp->state.sys_info.clock_control_enabled = clock_control_at_launch(ntp)))
		block_clock(ntp);

	return 0;
}


static int issue_request(crny_module_t *ntp)
{
	int rc;
	struct crny_comm *comm = &ntp->crny_comm;
	struct crny_cmd_request *req = &comm->req;
	const struct timespec timeout = { .tv_sec = REPLY_TIMEOUT / 1000000000L,
					  .tv_nsec = REPLY_TIMEOUT % 1000000000L };

	assert(ntp != NULL);

	if (comm->sock < 0) {
		return ENOTCONN;
	}

	DBG_L6("crny: req(ver=%d, pkt=%d, cmd=%d, attempt=%d, seq=%d)\n",
	       req->header[0], req->header[1],
	       ntohs(req->cmd1), ntohs(req->ignore), req->randoms);

	/* Determine the timeout for this request */
	(void)clock_gettime(CLOCK_MONOTONIC, &ntp->reply_expiry_time);
	sfptpd_time_add(&ntp->reply_expiry_time, &ntp->reply_expiry_time, &timeout);

	rc = send(comm->sock, req, sizeof(*req), 0);
	if (rc == -1) {
		if (errno == ENOTCONN || errno == ECONNREFUSED) {
			ERROR("crny: control connection disconnected, %s\n",
			      strerror(errno));
			crny_close_socket(ntp);
			return ENOTCONN;
		} else {
			ERROR("crny: error sending cmd request to chronyd, %s\n",
			      strerror(errno));
			return errno;
		}
	}

	return 0;
}


static int check_reply(struct crny_cmd_request *request,
		       struct crny_cmd_response *reply,
		       uint16_t expect)

{
	uint16_t req_cmd = ntohs(request->cmd1);
	uint32_t req_seq = request->randoms;
	uint16_t status = ntohs(reply->status);
	uint16_t cmd = ntohs(reply->cmd);
	uint16_t packet = ntohs(reply->reply);
	uint32_t seq = reply->seq_id;
	int rc = 0;

	if (status != 0) {
		DBG_L4("crny: unsuccessful chrony "
		       "response status %d for command %d\n",
			 status, req_cmd);
		rc = rc == 0 ? EPROTO : rc;
	}
	if (seq != req_seq) {
		DBG_L4("crny: sequence number in response (%x) "
		       "does not match sequence number in request (%x)\n",
		       seq, req_seq);
		rc = rc == 0 ? EPROTO : rc;
	}
	if (packet != expect) {
		DBG_L6("crny: unexpected response type "
		       "%d to command %d, expected %d\n",
		       packet, req_cmd, expect);
		rc = rc == 0 ? EPROTO : rc;
	}
	if (cmd != req_cmd) {
		DBG_L6("crny: response command field (%d) does "
		       "not match command %d\n",
		       cmd, req_cmd);
		rc = rc == 0 ? EPROTO : rc;
	}

	return rc;
}


static int issue_get_sys_info(crny_module_t *ntp)
{
	int rc;

	/* Start from previous state */
	ntp->next_state = ntp->state;

	/* Generate tracking request */
	chrony_req_initialize(&ntp->crny_comm.req, CRNY_REQ_TRACKING_STATE);

	/* Allocate space for receiving reply */
	rc = issue_request(ntp);
	if (rc != 0) {
		DBG_L6("crny: get-sys-info: chrony_send_recv failed, %s\n",
		       strerror(errno));
	}

	return rc;
}


static int handle_get_sys_info(crny_module_t *ntp)
{
	int rc;
	struct crny_comm *comm = &ntp->crny_comm;
	struct crny_cmd_request *req = &comm->req;
	struct crny_cmd_response *reply = &comm->resp;
	struct sfptpd_ntpclient_sys_info sys_info;
	struct ntp_state *next_state = &ntp->next_state;

	assert(ntp != NULL);

	rc = check_reply(req, reply, CRNY_RESP_TRACKING_STATE);
	if (rc != 0) {
		DBG_L6("crny: get-sys-info: invalid reply, %s\n",
		       strerror(errno));
		return rc;
	}

	/* ref_id of 0x7f7f0101L means LOCAL == 127.127.1.1. 0x4C4F434C == LOCL also means local */
	uint32_t ref_id = ntohl(*(uint32_t*)reply->data);
	DBG_L6("crny: get-sys-info: tracking ref id: %08lX\n", ref_id);
	if (ref_id == REF_ID_UNSYNC){
		/* if the ref_id is null then we likely don't have any other info either
		so we should just return an error code */
		DBG_L4("crny: get-sys-info: peer not contactable\n");
		return EAGAIN;
	}
	else if (ref_id == REF_ID_LOCAL || ref_id == REF_ID_LOCL){
		DBG_L6("crny: get-sys-info: peer is local\n");
	}

	struct crny_addr *ip_addr = (struct crny_addr *)((uint32_t *)reply->data + 1);

	if (ip_addr->addr_family == 0){
		DBG_L6("crny: get-sys-info: tracked source does not "
		       "have a network address.\n");
	} else {
		char host[NI_MAXHOST];

		sfptpd_crny_addr_to_sockaddr(&sys_info.peer_address,
					     &sys_info.peer_address_len,
					     ip_addr);

		int rc = getnameinfo((struct sockaddr *) &sys_info.peer_address,
				     sys_info.peer_address_len,
				     host, sizeof host,
				     NULL, 0, NI_NUMERICHOST);
		DBG_L6("crny: get-sys-info: selected-peer-address: %s\n",
		       rc == 0 ? host : gai_strerror(rc));
	}

	bool clock_control = clock_control_at_launch(ntp);
	sys_info.clock_control_enabled = clock_control;

	next_state->sys_info = sys_info;

	return 0;
}

int issue_get_source_count(crny_module_t *ntp)
{
	chrony_req_initialize(&ntp->crny_comm.req, CRNY_REQ_GET_NUM_SOURCES);
	return issue_request(ntp);
}

int handle_get_source_count(crny_module_t *ntp)
{
	int rc;
	struct crny_comm *comm = &ntp->crny_comm;
	struct crny_cmd_request *req = &comm->req;
	struct crny_cmd_response *reply = &comm->resp;
	struct ntp_state *next_state = &ntp->next_state;

	rc = check_reply(req, reply, CRNY_RESP_NUM_SOURCES);
	if (rc != 0) {
		DBG_L6("crny: get-peer-info: invalid reply, %s\n",
		       strerror(errno));
		return rc;
	}

	/* Get number of sources from reply*/
	int32_t num_sources = ntohl(*(int*)reply->data);

	if (num_sources > SFPTPD_NTP_PEERS_MAX) {
		num_sources = SFPTPD_NTP_PEERS_MAX;
		DBG_L4("crny: get-peer-info: too many peers - summary limited to %d peers\n",
		       num_sources);
	}
	next_state->peer_info.num_peers = num_sources;

	return 0;
}

int issue_get_source_datum(crny_module_t *ntp)
{
	struct sfptpd_ntpclient_peer *peer = &ntp->next_state.peer_info.peers[ntp->query_src_idx];

	memset(peer, '\0', sizeof *peer);
	chrony_req_initialize(&ntp->crny_comm.req, CRNY_REQ_SOURCE_DATA_ITEM);
	*((int32_t *) &ntp->crny_comm.req.cmd2) = htonl(ntp->query_src_idx);

	return issue_request(ntp);
}

int handle_get_source_datum(crny_module_t *ntp)
{
	int rc;
	struct crny_addr *ip_addr;
	struct crny_comm *comm = &ntp->crny_comm;
	struct crny_cmd_request *req = &comm->req;
	struct crny_cmd_response *reply = &comm->resp;
	struct ntp_state *next_state = &ntp->next_state;
	struct sfptpd_ntpclient_peer *peer = &next_state->peer_info.peers[ntp->query_src_idx];

	rc = check_reply(req, reply, CRNY_RESP_SOURCE_DATA_ITEM);
	if (rc != 0) {
		DBG_L6("crny: get-peer%d-info: invalid reply, %s\n",
		       ntp->query_src_idx, strerror(errno));
		return ENOENT;
	}

	/* Grab the state field from source data reply */
	struct crny_source *src_data = (struct crny_source *)(&reply->data);
	enum crny_state_code state = ntohs(src_data->state);
	enum crny_src_mode_code mode = ntohs(src_data->mode);

	DBG_L6("crny: get-peer%d-info: mode %d state %d\n",
	       ntp->query_src_idx, mode, state);

	peer->selected = (state == CRNY_STATE_SYSPEER);
	peer->shortlist = (state == CRNY_STATE_CANDIDATE);
	peer->self = (mode == CRNY_SRC_MODE_REF);

	if (mode == CRNY_SRC_MODE_REF) {
		DBG_L6("crny: get-peer%d-info: source is a reference clock\n", ntp->query_src_idx);
		/* No peer information will be avaliable via NTPDATA request */
		return ENOENT;
	}

	/* Go straight on to populate following NTPDATA request */
	chrony_req_initialize(&ntp->crny_comm.req, CRNY_REQ_NTP_DATA);

	/* Specify which record we want ntpdata to fetch by giving it
	   the ip address of the peer.
	   Copy directly from reply into request. */
	ip_addr = &src_data->ip_addr;
	if (ip_addr->addr_family == 0) {
		DBG_L6("crny: get-peer%d-info: address family unspecified in tracking reply.\n", ntp->query_src_idx);
		return ENOENT;
	}
	memcpy(&ntp->crny_comm.req.cmd2, ip_addr, sizeof(*ip_addr));

	return 0;
}

int issue_get_ntp_datum(crny_module_t *ntp)
{
	/* Request already populated when source request handled */
	return issue_request(ntp);
}

int handle_get_ntp_datum(crny_module_t *ntp)
{
	int rc;
	struct crny_comm *comm = &ntp->crny_comm;
	struct crny_cmd_request *req = &comm->req;
	struct crny_cmd_response *reply = &comm->resp;
	struct ntp_state *next_state = &ntp->next_state;
	struct sfptpd_ntpclient_peer *peer = &next_state->peer_info.peers[ntp->query_src_idx];

	struct crny_ntpdata *answer = (struct crny_ntpdata *)(&reply->data);
	rc = check_reply(req, reply, CRNY_RESP_NTP_DATA);
	if (rc != 0) {
		DBG_L6("crny: get-chrony-peer%d-info: invalid reply, %s\n",
		       ntp->query_src_idx, strerror(errno));
	} else {
		sfptpd_crny_addr_to_sockaddr(&peer->remote_address,
					     &peer->remote_address_len,
					     &answer->remote_ip);
		sfptpd_crny_addr_to_sockaddr(&peer->local_address,
					     &peer->local_address_len,
					     &answer->local_ip);
		peer->pkts_sent = ntohl(answer->total_sent);
		peer->pkts_received = ntohl(answer->total_received);
		peer->stratum = answer->stratum;
		peer->candidate = (answer->mode == CRNY_NTPDATA_MODE_SERVER);
		peer->offset = sfptpd_crny_tofloat(ntohl(answer->offset)) * -1.0e9; /*do we negate the offset? */
		peer->root_dispersion = sfptpd_crny_tofloat(ntohl(answer->root_dispersion)) * 1.0e9;
	}

	/* Success */
	return 0;
}


static int crny_resolve(crny_module_t *ntp)
{
	assert(ntp);

	strcpy(ntp->crny_comm.remote.sun_path, CRNY_CONTROL_SOCKET_PATH);

	/* check if chronyd is running */
	if (access(ntp->crny_comm.remote.sun_path, F_OK) != 0) {
		DBG_L4("crny: nonexistent path %s, %s. Is chronyd running?\n",
		       ntp->crny_comm.remote.sun_path, strerror(errno));
		return errno;
	} else {
		return 0;
	}
}

static int crny_connect(crny_module_t *ntp)
{
	assert(ntp);
	assert(ntp->crny_comm.sock == -1);

	int rc;
	struct crny_comm *comm = &ntp->crny_comm;
	struct sockaddr_un local_sockaddr;

	comm->remote.sun_family = AF_UNIX;
	local_sockaddr.sun_family = AF_UNIX;
	if (snprintf(local_sockaddr.sun_path, sizeof (local_sockaddr.sun_path),
	    "%s", ntp->crny_comm.unix_sock_path) >=
		sizeof (local_sockaddr.sun_path)) {
		ERROR("crny: Unix socket path %s too long\n",
		      comm->unix_sock_path);
		return errno;
	}

	comm->sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (comm->sock < 0) {
		ERROR("crny: could not create Unix socket, %s\n",
			strerror(rc = errno));
		return rc;
	}

	int flags = fcntl(comm->sock, F_GETFD);
	if (flags == -1) {
		ERROR("crny: fcntl(F_GETFD) failed : %s\n",
		      strerror(rc = errno));
		goto cleanup;
	}

	flags |= FD_CLOEXEC;

	if (fcntl(comm->sock, F_SETFD, flags) < 0) {
		ERROR("crny: fcntl(F_SETFD) failed : %s\n",
		      strerror(rc = errno));
		goto cleanup;
	}

	if (fcntl(comm->sock, F_SETFL, O_NONBLOCK)) {
		ERROR("crny: fcntl() could not set O_NONBLOCK: %s\n",
		      strerror(rc = errno));
		goto cleanup;
	}

	/* Bind the local socket to the path we just specified */
	/* Note: we need to unlink before bind, in case the socket wasn't cleaned up last time */
	unlink(ntp->crny_comm.unix_sock_path);
	if (bind(comm->sock, &local_sockaddr, sizeof (local_sockaddr)) < 0) {
		ERROR("crny: Could not bind Unix socket to %s : %s\n",
		      ntp->crny_comm.unix_sock_path, strerror(rc = errno));
		goto cleanup2;
	}

	/* You need to chmod 0666 the socket otherwise pselect will time out. */
	if (chmod(comm->unix_sock_path, 0666) < 0) {
		ERROR("crny: Could not chmod %s : %s\n",
		      ntp->crny_comm.unix_sock_path, strerror(rc = errno));
		goto cleanup2;
	}

	/* Connect the socket */
	if (connect(comm->sock, &comm->remote, sizeof (comm->remote)) < 0) {
		rc = errno;
		if (rc != EINPROGRESS) {
			ERROR("crny: could not connect socket to address %s, %s\n",
			      comm->unix_sock_path, strerror(rc));
			goto cleanup2;
		}
	} else {
		rc = 0;
	}
	sfptpd_thread_user_fd_add(comm->sock, true, false);

	return rc;

cleanup2:
	unlink(ntp->crny_comm.unix_sock_path);
cleanup:
	close(comm->sock);
	comm->sock = -1;
	return rc;
}

static bool crny_state_machine(crny_module_t *ntp,
				  enum ntp_query_event event)
{
	int rc;
	bool update = false;
	bool disconnect = false;
	struct timespec time_now, time_left;
	struct ntp_state *next_state = &ntp->next_state;
	enum ntp_query_state next_query_state = ntp->query_state;

	assert(ntp != NULL);

	if (event == NTP_QUERY_EVENT_NO_EVENT) {
		update = false;
		goto finish;
	}

	if (event == NTP_QUERY_EVENT_CONN_LOST) {
		crny_close_socket(ntp);
		update = true;
		next_query_state = NTP_QUERY_STATE_SLEEP_DISCONNECTED;
		goto finish;
	}

	if (event == NTP_QUERY_EVENT_TICK &&
	    ntp->query_state != NTP_QUERY_STATE_CONNECT &&
	    ntp->query_state != NTP_QUERY_STATE_SLEEP_DISCONNECTED &&
	    ntp->query_state != NTP_QUERY_STATE_SLEEP_CONNECTED) {
		struct timespec now;

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if (sfptpd_time_cmp(&now, &ntp->reply_expiry_time) >= 0)
			event = NTP_QUERY_EVENT_REPLY_TIMEOUT;
	}

	switch (ntp->query_state) {
	case NTP_QUERY_STATE_CONNECT:
		rc = crny_connect(ntp);
		if (rc == 0) {
			if (issue_get_sys_info(ntp) != 0)
				disconnect = true;
			else
				next_query_state = NTP_QUERY_STATE_SYS_INFO;
		} else if (rc == EINPROGRESS) {
			next_query_state = NTP_QUERY_STATE_CONNECT_WAIT;
		} else {
			crny_parse_state(next_state, ENOPROTOOPT, next_state->offset_unsafe);
			next_query_state = NTP_QUERY_STATE_SLEEP_DISCONNECTED;
		}
		break;

	case NTP_QUERY_STATE_CONNECT_WAIT:
		if (event == NTP_QUERY_EVENT_TRAFFIC) {
			int val;
			socklen_t sz = sizeof val;

			rc = getsockopt(ntp->crny_comm.sock, SOL_SOCKET, SO_ERROR, &val, &sz);
			if (rc != 0 || val != 0 || issue_get_sys_info(ntp) != 0)
				disconnect = true;
			else
				next_query_state = NTP_QUERY_STATE_SYS_INFO;
		} else if (event == NTP_QUERY_EVENT_REPLY_TIMEOUT) {
			next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
		}
		break;

	case NTP_QUERY_STATE_SYS_INFO:
		if (event == NTP_QUERY_EVENT_TRAFFIC) {
			rc = handle_get_sys_info(ntp);
			if (issue_get_source_count(ntp) != 0)
				disconnect = true;
			else
				next_query_state = NTP_QUERY_STATE_SOURCE_COUNT;
		} else if (event == NTP_QUERY_EVENT_REPLY_TIMEOUT) {
			next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
		}
		break;

	case NTP_QUERY_STATE_SOURCE_COUNT:
		if (event == NTP_QUERY_EVENT_TRAFFIC) {
			rc = handle_get_source_count(ntp);
			if (next_state->peer_info.num_peers > 0) {
				ntp->query_src_idx = 0;
				if (issue_get_source_datum(ntp) != 0)
					disconnect = true;
				else
					next_query_state = NTP_QUERY_STATE_SOURCE_DATUM;
			} else {
				next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
			}
		} else if (event == NTP_QUERY_EVENT_REPLY_TIMEOUT) {
			next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
		}
		break;

	case NTP_QUERY_STATE_SOURCE_DATUM:
		if (event ==  NTP_QUERY_EVENT_TRAFFIC) {
			rc = handle_get_source_datum(ntp);
			if (rc == ENOENT) {
				if (++ntp->query_src_idx == next_state->peer_info.num_peers) {
					crny_parse_state(next_state, 0, next_state->offset_unsafe);
					update = true;
					sfptpd_ntpclient_print_peers(&next_state->peer_info, MODULE);
					next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
				} else if (issue_get_source_datum(ntp) != 0)
					disconnect = true;
			} else if (issue_get_ntp_datum(ntp) != 0)
				disconnect = true;
			else
				next_query_state = NTP_QUERY_STATE_NTP_DATUM;
		} else if (event == NTP_QUERY_EVENT_REPLY_TIMEOUT) {
			next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
		}
		break;

	case NTP_QUERY_STATE_NTP_DATUM:
		if (event == NTP_QUERY_EVENT_TRAFFIC) {
			rc = handle_get_ntp_datum(ntp);
			if (++ntp->query_src_idx == next_state->peer_info.num_peers) {
				crny_parse_state(next_state, 0, next_state->offset_unsafe);
				update = true;
				sfptpd_ntpclient_print_peers(&next_state->peer_info, MODULE);
				next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
			} else {
				if (issue_get_source_datum(ntp) != 0)
					disconnect = true;
				else
					next_query_state = NTP_QUERY_STATE_SOURCE_DATUM;
			}
		} else if (event == NTP_QUERY_EVENT_REPLY_TIMEOUT) {
			next_query_state = NTP_QUERY_STATE_SLEEP_CONNECTED;
		}
		break;

	case NTP_QUERY_STATE_SLEEP_DISCONNECTED:
		/* Check whether it's time to poll the NTP daemon again */
		(void)clock_gettime(CLOCK_MONOTONIC, &time_now);
		sfptpd_time_subtract(&time_left, &ntp->next_poll_time, &time_now);
		if (time_left.tv_sec < 0 || event == NTP_QUERY_EVENT_RUN) {
			if (crny_resolve(ntp) == 0)
				next_query_state = NTP_QUERY_STATE_CONNECT;
			else
				crny_parse_state(next_state, ENOPROTOOPT, next_state->offset_unsafe);
			ntp->next_poll_time.tv_sec += ntp->config->poll_interval;
		}
		break;

	case NTP_QUERY_STATE_SLEEP_CONNECTED:
		/* Check whether it's time to poll the NTP daemon again */
		(void)clock_gettime(CLOCK_MONOTONIC, &time_now);
		sfptpd_time_subtract(&time_left, &ntp->next_poll_time, &time_now);
		if (time_left.tv_sec < 0) {
			if (issue_get_sys_info(ntp) != 0) {
				disconnect = true;
			} else {
				next_query_state = NTP_QUERY_STATE_SYS_INFO;
				ntp->next_poll_time.tv_sec += ntp->config->poll_interval;
			}
		}
		break;
		
	default:
		assert(false);
		break;
	}

	if (disconnect) {
		crny_close_socket(ntp);
		update = true;
		next_query_state = NTP_QUERY_STATE_SLEEP_DISCONNECTED;
	}

finish:
	DBG_L6("crny: state %s --%s--> %s (%s)\n",
	       query_state_names[ntp->query_state],
	       query_event_names[event],
	       query_state_names[next_query_state],
	       update ? "update" : "no update");

	if (ntp->next_state.state != ntp->state.state)
		update = true;

	ntp->query_state = next_query_state;
	return update;
}


static bool ntp_handle_state_change(crny_module_t *ntp,
				    struct ntp_state *new_state,
				    sfptpd_sync_instance_status_t *status_out)
{
	sfptpd_sync_instance_status_t status = { 0 };

	assert(ntp != NULL);
	assert(new_state != NULL);
	assert(status_out != NULL);

	if (new_state->state != ntp->state.state) {
		INFO("crny: changed state from %s to %s\n",
		     crny_state_text(ntp->state.state, 0),
		     crny_state_text(new_state->state, 0));

		switch (new_state->state) {
		case SYNC_MODULE_STATE_DISABLED:
			WARNING("crny: ntpd no longer running\n");
			break;

		case SYNC_MODULE_STATE_FAULTY:
			ERROR("crny: not able to communicate with ntpd\n");
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

	if ((new_state->state != ntp->state.state) ||
	    (new_state->alarms != ntp->state.alarms) ||
	    (new_state->offset_from_master != ntp->state.offset_from_master) ||
	    (new_state->root_dispersion != ntp->state.root_dispersion) ||
	    (new_state->stratum != ntp->state.stratum)) {

		/* Send a status update to the sync engine to let it know
		 * that the state of the NTP module has changed. */
		status.state = new_state->state;
		status.alarms = new_state->alarms;
		status.constraints = ntp->constraints;
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

		*status_out = status;

		return true;
	} else {
		return false;
	}
}


static void ntp_send_rt_stats_update(crny_module_t *ntp,
				     struct sfptpd_log_time time,
				     struct ntp_state *new_state)
{
	assert(ntp != NULL);

	if (new_state->state == SYNC_MODULE_STATE_SLAVE) {
		sfptpd_time_t offset = new_state->peer_info.peers[new_state->selected_peer_idx].offset;

		bool disciplining = new_state->sys_info.clock_control_enabled;

		sfptpd_engine_post_rt_stats(ntp->engine,
					    &time,
					    SFPTPD_CONFIG_GET_NAME(ntp->config),
					    "ntp", NULL, sfptpd_clock_get_system_clock(),
					    disciplining, false,
					    new_state->synchronized, new_state->alarms,
					    STATS_KEY_OFFSET, offset,
					    STATS_KEY_END);
	}
}


static void ntp_send_clustering_input(crny_module_t *ntp, struct ntp_state *state)
{
	assert(ntp != NULL);

	if (ntp->ctrl_flags & SYNC_MODULE_CLUSTERING_DETERMINANT) {
		sfptpd_time_t offset = state->peer_info.peers[state->selected_peer_idx].offset;

		sfptpd_engine_clustering_input(ntp->engine,
					       SFPTPD_CONFIG_GET_NAME(ntp->config),
					       sfptpd_clock_get_system_clock(),
					       offset,
					       finitel(offset) && offset != 0.0L &&
					       state->state == SYNC_MODULE_STATE_SLAVE);
	}
}


static char *clock_control_op_name(enum chrony_clock_control_op op) {
	if (op == CRNY_CTRL_OP_NOP)
		return " nop";
	else if (op == CRNY_CTRL_OP_SAVE)
		return " save";
	else if (op == CRNY_CTRL_OP_RESTORE)
		return " restore";
	else if (op == CRNY_CTRL_OP_RESTORENORESTART)
		return " restorenorestart";
	else if (op == CRNY_CTRL_OP_ENABLE)
		return " enable";
	else
		return " disable";
}

#define CLOCK_CONTROL_MIN_INTERVAL (0) /* min time between running script in seconds */
static int do_clock_control(crny_module_t *ntp,
			    enum chrony_clock_control_op op_req)
{
	static struct timespec last_changed;
	struct timespec now, delta;
	enum chrony_clock_control_op op_do;
	bool clock_control;
	bool have_control = strlen(ntp->config->chronyd_script) != 0;
	char *command;
	char *action;
	int len, total;
	int status;
	int rc;

	if (!have_control)
		return ENOSYS; /* Functionality not available */

	clock_control = clock_control_at_launch(ntp);

	if ((op_req == CRNY_CTRL_OP_ENABLE && clock_control) ||
	    (op_req == CRNY_CTRL_OP_DISABLE && !clock_control))
		op_do = CRNY_CTRL_OP_NOP;
	else
		op_do = op_req;

	if (op_req == CRNY_CTRL_OP_RESTORE && clock_control == ntp->clock_control_at_save)
		op_do = CRNY_CTRL_OP_RESTORENORESTART;

	action = clock_control_op_name(op_do);
	DBG_L6("crny: chrony_clock_control(op_req = %s, op_do = %s)\n",
	       clock_control_op_name(op_req), action);

	if (op_do == CRNY_CTRL_OP_NOP)
		return 0;

	if (op_do == CRNY_CTRL_OP_SAVE)
		ntp->clock_control_at_save = clock_control;

	if (!(last_changed.tv_sec == 0 && last_changed.tv_nsec == 0) &&
	    op_do != CRNY_CTRL_OP_RESTORE &&
	    op_do != CRNY_CTRL_OP_RESTORENORESTART) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &now);
		sfptpd_time_subtract(&delta, &now, &last_changed);
		if (delta.tv_sec < CLOCK_CONTROL_MIN_INTERVAL) {
			INFO("crny: chrony_clock_control - return EAGAIN as delta = %d s\n",
			     delta.tv_sec);
			return EAGAIN; /* too soon since we last changed */
		}
	}

	len = strlen(ntp->config->chronyd_script);
	total = len + strlen(action) + 1;
	command = calloc(1, total);
	if (!command)
		return ENOMEM;
	strcpy(command, ntp->config->chronyd_script);
	strcpy(command + len, action);

	INFO("crny: invoking clock control script '%s'\n", command);

	/* If we have a valid socket close it */
	if (op_do == CRNY_CTRL_OP_ENABLE ||
	    op_do == CRNY_CTRL_OP_DISABLE ||
	    op_do == CRNY_CTRL_OP_RESTORE) {
		crny_close_socket(ntp);
	}

	/* run the command */
	status = system(command);
	if (status == -1 || !WIFEXITED(status))
		rc = ECHILD;
	else
		rc = WEXITSTATUS(status);
	if (rc != 0)
		ERROR("crny: clock control script failed, %s\n",
			strerror(rc));

	if (op_do != CRNY_CTRL_OP_SAVE)
		clock_gettime(CLOCK_MONOTONIC_RAW, &last_changed);
	free(command);
	return rc;
}

static int crny_clock_control(crny_module_t *ntp,
			 bool enable)
{
	if (!ntp->chrony_state_saved) {
		do_clock_control(ntp, CRNY_CTRL_OP_SAVE);
		ntp->chrony_state_saved = true;
	}

	return do_clock_control(ntp, enable ? CRNY_CTRL_OP_ENABLE : CRNY_CTRL_OP_DISABLE);
}

static void ntp_on_clock_control_change(crny_module_t *ntp, struct ntp_state *new_state)
{
	int rc;
	bool clock_control;

	assert(ntp != NULL);
	assert(new_state != NULL);

	/* This function is called when the state of the daemon has been
	 * detected as changed. This is likely to be in the period after a
	 * deliberate change of control as that control settles or if something
	 * external happens to chronyd.
	 */

	if (new_state->sys_info.clock_control_enabled)
		block_clock(ntp);
	else
		unblock_clock(ntp);

	clock_control = ((ntp->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) != 0);

	if (new_state->sys_info.clock_control_enabled && !clock_control)
		CRITICAL("### chronyd is now disciplining the system clock! ###\n");

	if (!new_state->sys_info.clock_control_enabled && clock_control)
		WARNING("crny: chronyd is no longer disciplining the system clock!\n");

	/* If we have control, try to stop NTP */
	if (new_state->sys_info.clock_control_enabled != clock_control &&
	    (new_state->state != SYNC_MODULE_STATE_DISABLED)) {
		INFO("crny: attempting to restore chronyd clock control state...\n");
		rc = crny_clock_control(ntp, clock_control);
		if (rc == 0) {
			new_state->sys_info.clock_control_enabled = clock_control;
			INFO("crny: successfully %sabled chronyd clock control\n",
			     clock_control? "en": "dis");
		} else {
			ERROR("crny: failed to restore chronyd clock control!\n");
		}
	}
}


static void ntp_on_offset_id_change(crny_module_t *ntp, struct ntp_state *new_state)
{
	DBG_L3("crny: offset ID changed\n");

	if (new_state->offset_unsafe && !offset_id_is_valid(new_state)) {
		new_state->offset_unsafe = false;
		INFO("crny: new ntpd offset detected\n");
		sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &new_state->offset_timestamp);
	}
}


static void ntp_on_get_status(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_sync_instance_status *status;

	assert(ntp != NULL);
	assert(msg != NULL);

	/* Note that we don't need to check whether a valid instance has been
	 * provided - there's always an instance of NTP. */

	status = &msg->u.get_status_resp.status;
	status->state = ntp->state.state;
	status->alarms = ntp->state.alarms;
	status->constraints = ntp->constraints;
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


static void ntp_on_control(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	sfptpd_sync_module_ctrl_flags_t flags;
	bool have_control = strlen(ntp->config->chronyd_script) != 0;
	int rc;

	assert(ntp != NULL);
	assert(msg != NULL);

	flags = ntp->ctrl_flags;

	/* Update the flags based on the message */
	flags &= ~msg->u.control_req.mask;
	flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	/* For the NTP sync module, only the clock control flag has meaning. */
	if (ntp->running_phase &&
	    (flags ^ ntp->ctrl_flags) & SYNC_MODULE_CLOCK_CTRL) {
		bool clock_control = ((flags & SYNC_MODULE_CLOCK_CTRL) != 0);
		bool clock_controlling = clock_control_at_launch(ntp);

		if (!!clock_control != !!clock_controlling) {
			/* Check if we can actually effect any control. */
			if (!have_control) {
				WARNING("crny: cannot change control flags - no control script specified\n");
				flags ^= SYNC_MODULE_CLOCK_CTRL;
				// TODO @bug64228 provide a generic error code in messages.
			} else {
				rc = crny_clock_control(ntp, clock_control);
				if (rc == 0) {
					INFO("crny: %sabled chronyd clock control\n",
					       clock_control? "en": "dis");
				} else {
				ERROR("crny: failed to change chronyd clock control, %s!\n",
				      strerror(rc));
				}
			}
		}
	}

	ntp->ctrl_flags = flags;
	SFPTPD_MSG_REPLY(msg);
}


static void ntp_on_step_clock(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	assert(ntp != NULL);
	assert(msg != NULL);

	/* Invalidate offset until NTP next queries the peers. */
	ntp->state.offset_unsafe = true;
	INFO("crny: clock step- ignoring ntp offset until next update\n");

	SFPTPD_MSG_REPLY(msg);
}


static void ntp_on_log_stats(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	assert(ntp != NULL);
	assert(msg != NULL);

	ntp_send_rt_stats_update(ntp, msg->u.log_stats_req.time, &ntp->state);
	ntp_send_clustering_input(ntp, &ntp->state);

	SFPTPD_MSG_FREE(msg);
}


static void ntp_on_save_state(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_clock *clock;
	unsigned int num_candidates, i;
	char constraints[SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX];
	char alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];
	char flags[256];

	assert(ntp != NULL);
	assert(msg != NULL);

	sfptpd_sync_module_alarms_text(ntp->state.alarms, alarms, sizeof(alarms));
	sfptpd_sync_module_constraints_text(ntp->constraints, constraints, sizeof(constraints));
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
			DBG_L4("crny: getnameinfo: %s\n", gai_strerror(rc));
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
			crny_state_text(ntp->state.state, 0),
			alarms, constraints, flags, peer->offset,
			ntp->state.synchronized,
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
			crny_state_text(ntp->state.state, 0),
			alarms, constraints, flags,
			ntp->state.peer_info.num_peers,
			num_candidates);
	}

	SFPTPD_MSG_FREE(msg);
}

static void ntp_on_write_topology(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
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
		DBG_L4("crny: getnameinfo: %s\n", gai_strerror(rc));
	}

	stream = msg->u.write_topology_req.stream;

	clock = sfptpd_clock_get_system_clock();

	fprintf(stream, "====================\nstate: %s\n",
		crny_state_text(ntp->state.state, 0));

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


static void ntp_on_stats_end_period(crny_module_t *ntp, sfptpd_sync_module_msg_t *msg)
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


static void update_state(crny_module_t *ntp)
{
	struct ntp_state *new_state = &ntp->next_state;
	sfptpd_sync_instance_status_t status = { 0 };
	bool any_change = false;
	bool status_change;

	assert(ntp != NULL);

	/* Handle a change in clock control state */
	if (new_state->sys_info.clock_control_enabled !=
	    ntp->state.sys_info.clock_control_enabled) {
		ntp_on_clock_control_change(ntp, new_state);
		any_change = true;
	}

	/* Handle changes in the state */
	if ((status_change = ntp_handle_state_change(ntp, new_state, &status)))
		any_change = true;

	/* Update the convergence criteria */
	if (ntp_convergence_update(ntp, new_state))
		any_change = true;

	/* Handle a change in either the source ID or offset */
	if (!offset_ids_equal(new_state, &ntp->state)) {
		ntp_on_offset_id_change(ntp, new_state);
		any_change = true;
	}

	if (any_change) {
		/* Send updated stats (offset) to the engine */
		struct sfptpd_log_time time;
		sfptpd_log_get_time(&time);
		ntp_send_rt_stats_update(ntp, time, new_state);

		/* Send clustering input to the engine */
		ntp_send_clustering_input(ntp, new_state);
	}

	/* Store the new state */
	ntp->state = *new_state;

	/* Send changes to the egine */
	if (status_change) {
		sfptpd_engine_sync_instance_state_changed(ntp->engine,
							  sfptpd_thread_self(),
							  (struct sfptpd_sync_instance *)ntp,
							  &status);
	}

	/* Update historical stats */
	crny_stats_update(ntp);
}


static void ntp_on_run(crny_module_t *ntp)
{
	struct timespec interval;
	int rc;
	bool have_control = strlen(ntp->config->chronyd_script) != 0;

	assert(ntp);

	ntp->running_phase = true;
	interval.tv_sec = 0;
	interval.tv_nsec = NTP_POLL_INTERVAL;

	/* Start a single-shot timer for polling. This is rearmed
	   after the timer event has been handled because if polling
	   times out repeatedly events would get queued up if a
	   periodic timer were used. */
	rc = sfptpd_thread_timer_start(NTP_POLL_TIMER_ID,
				       false, false, &interval);
	if (rc != 0) {
		CRITICAL("crny: failed to start poll timer, %s\n", strerror(rc));

		/* We can't carry on in this case */
		sfptpd_thread_exit(rc);
	}

	/* Determine the time when we should next poll the NTP daemon */
	(void)clock_gettime(CLOCK_MONOTONIC, &ntp->next_poll_time);
	ntp->query_state = NTP_QUERY_STATE_SLEEP_DISCONNECTED;
	ntp->state.offset_unsafe = false;

	rc = EOPNOTSUPP;
	if (ntp->ctrl_flags & SYNC_MODULE_CLOCK_CTRL &&
	    !clock_control_at_launch(ntp)) {
		if (have_control)
			rc = crny_clock_control(ntp, true);
		if (!have_control || rc != 0) {
			WARNING("crny: no capability to enable clock control, %s\n",
				strerror(rc));
			ntp->ctrl_flags &= ~SYNC_MODULE_CLOCK_CTRL;
		}
	} else if ((ntp->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) == 0 &&
		    clock_control_at_launch(ntp)) {
		if (have_control)
			rc = crny_clock_control(ntp, false);
		if ((!have_control || rc != 0) &&
		    sfptpd_clock_get_discipline(sfptpd_clock_get_system_clock())) {
			struct sfptpd_config_general *gconf;

			gconf = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(ntp->config));
			rc = rc == 0 ? EOPNOTSUPP : rc;
			CRITICAL("crny: no capability to disable clock control, %s\n", strerror(rc));

			if (gconf->ignore_critical[SFPTPD_CRITICAL_CLOCK_CONTROL_CONFLICT]) {
				NOTICE("ptp: ignoring critical error by configuration\n");
			} else {
				NOTICE("configure \"ignore_critical: clock-control-conflict\" to allow sfptpd to start in spite of this condition\n");
				sfptpd_thread_exit(rc);
			}
		}
	}

	/* Kick off the state machine */
	ntp->next_state = ntp->state;
	if (crny_state_machine(ntp, NTP_QUERY_EVENT_RUN))
		update_state(ntp);
}


static void ntp_on_timer(void *user_context, unsigned int id)
{
	crny_module_t *ntp = (crny_module_t *)user_context;
	struct timespec interval;
	int rc;

	assert(ntp != NULL);

	/* Progress the NTP state machine. We take a copy of the existing
	 * ntp state and this is updated by the state machine */
	if (crny_state_machine(ntp, NTP_QUERY_EVENT_TICK))
		update_state(ntp);

	interval.tv_sec = 0;
	interval.tv_nsec = NTP_POLL_INTERVAL;

	rc = sfptpd_thread_timer_start(NTP_POLL_TIMER_ID,
				       false, false, &interval);
	if (rc != 0) {
		CRITICAL("crny: failed to rearm poll timer, %s\n", strerror(rc));
		sfptpd_thread_exit(rc);
	}
}


static int ntp_on_startup(void *context)
{
	crny_module_t *ntp = (crny_module_t *)context;
	int rc;

	assert(ntp != NULL);

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. */
	ntp->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Configure the NTP daemon based on the configuration options */
	rc = crny_configure_ntpd(ntp);
	if (rc != 0)
		goto fail;

	ntp_convergence_init(ntp);

	rc = crny_stats_init(ntp);
	if (rc != 0) {
		CRITICAL("crny: failed to create statistics collection, %s\n",
			 strerror(rc));
		goto fail;
	}

	/* Create a timer which will be used to poll NTP */
	rc = sfptpd_thread_timer_create(NTP_POLL_TIMER_ID, CLOCK_MONOTONIC,
					ntp_on_timer, ntp);
	if (rc != 0) {
		CRITICAL("crny: failed to create poll timer, %s\n", strerror(rc));
		goto fail;
	}

	if (crny_resolve(ntp) != 0)
		ntp->state.state = SYNC_MODULE_STATE_DISABLED;

	ntp->next_state = ntp->state;

	return 0;

fail:
	sfptpd_stats_collection_free(&ntp->stats);
	return rc;
}


static void ntp_on_shutdown(void *context)
{
	crny_module_t *ntp = (crny_module_t *)context;
	assert(ntp != NULL);

	crny_close_socket(ntp);
	sfptpd_stats_collection_free(&ntp->stats);

	/* Delete the sync module context */
	free(ntp);
}


static void ntp_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	crny_module_t *ntp = (crny_module_t *)context;
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
		WARNING("crny: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void crny_do_io(crny_module_t *ntp)
{
	enum ntp_query_event event = NTP_QUERY_EVENT_NO_EVENT;
	int rc;
	struct crny_comm *comm = &ntp->crny_comm;

	assert(ntp != NULL);

	rc = recv(comm->sock, &comm->resp, sizeof(comm->resp), 0);
	if (rc < 0)
		rc = -errno;

	if (rc >= 8) {
		DBG_L6("crny: resp(ver=%d, pkt=%d, cmd=%d, seq=%d)\n",
		       comm->resp.header[0], comm->resp.header[1],
		       ntohs(comm->resp.cmd), comm->resp.seq_id);
		event = NTP_QUERY_EVENT_TRAFFIC;
	} else if (rc >= 0) {
		ERROR("crny: useless reply received from chronyd\n");
	} else if (rc == -EAGAIN || rc == -EINTR) {
		/* Ignore wakeup */
		DBG_L6("crny: fd woken up, %s\n", strerror(rc));
	} else {
		ERROR("crny: chrony: error receiving reply from chronyd, %s\n",
		      strerror(rc));
		event = NTP_QUERY_EVENT_CONN_LOST;
	}

	/* Progress the NTP state machine. */
	if (crny_state_machine(ntp, event))
		update_state(ntp);
}


static void ntp_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	int i;
	crny_module_t *ntp = (crny_module_t *) context;

	assert(ntp != NULL);

	for (i = 0; i < num_fds; i++) {
		if (ntp->crny_comm.sock == fds[i]) {
			crny_do_io(ntp);
		}
	}
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
	assert(section->category == SFPTPD_CONFIG_CATEGORY_CRNY);
	free(section);
}


static struct sfptpd_config_section *ntp_config_create(const char *name,
						       enum sfptpd_config_scope scope,
						       bool allows_instances,
						       const struct sfptpd_config_section *src)
{
	struct sfptpd_crny_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_CRNY));

	new = (struct sfptpd_crny_module_config *)calloc(1, sizeof(*new));
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
		new->clock_control = false;
		sfptpd_strncpy(new->chronyd_script,
			       SFPTPD_CRNY_DEFAULT_CONTROL_SCRIPT,
			       sizeof new->chronyd_script);
	}

	/* If this is an implicitly created sync instance, give it the lowest
	   possible user priority. */
	if (name == NULL) {
		name = "crny0";
		new->priority = INT_MAX;
	}

	SFPTPD_CONFIG_SECTION_INIT(new, ntp_config_create, ntp_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_CRNY,
				   scope, allows_instances, name);

	return &new->hdr;
}


int sfptpd_crny_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_crny_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_crny_module_config *)
		ntp_config_create(MODULE,
				  SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	SFPTPD_CONFIG_SECTION_ADD(config, new);

	/* Register the configuration options */
	sfptpd_config_register_options(&ntp_config_option_set);
	return 0;
}


struct sfptpd_crny_module_config *sfptpd_crny_module_get_config(struct sfptpd_config *config)
{
	return (struct sfptpd_crny_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_CRNY);
}


void sfptpd_crny_module_set_default_interface(struct sfptpd_config *config,
						 const char *interface_name)
{
	/* For NTP no interface is required */
}


int sfptpd_crny_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_subscribers)

{
	sfptpd_crny_module_config_t *instance_config;
	crny_module_t *ntp;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	TRACE_L3("crny: creating sync-module\n");

	*sync_module = NULL;
	ntp = (crny_module_t *)calloc(1, sizeof(*ntp));
	if (ntp == NULL) {
		CRITICAL("crny: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	/* Keep a handle to the sync engine */
	ntp->engine = engine;

	/* Initialise state */
	ntp->crny_comm.sock = -1;

	/* Find the NTP global configuration. If this doesn't exist then
	 * something has gone badly wrong. */
	ntp->config = sfptpd_crny_module_get_config(config);
	if (ntp->config == NULL) {
		CRITICAL("crny: failed to find NTP configuration\n");
		free(ntp);
		return ENOENT;
	}

	/* Find the nominal instance configuration */
	instance_config = (struct sfptpd_crny_module_config *)
		sfptpd_config_category_first_instance(config,
						      SFPTPD_CONFIG_CATEGORY_CRNY);

	/* Set up the clustering evaluator */
	ntp->state.clustering_evaluator.calc_fn = sfptpd_engine_calculate_clustering_score;
	ntp->state.clustering_evaluator.private = engine;
	ntp->state.clustering_evaluator.instance_name = instance_config->hdr.name;

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("crny", &ntp_thread_ops, ntp, sync_module);
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
