/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

/**
 * @file   sfptpd_engine.c
 * @brief  Main engine of sfptpd application
 */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sfptpd_app.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_thread.h"
#include "sfptpd_message.h"
#include "sfptpd_misc.h"
#include "sfptpd_engine.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_bic.h"
#include "sfptpd_clock.h"
#include "sfptpd_servo.h"
#include "sfptpd_constants.h"
#include "sfptpd_thread.h"
#include "sfptpd_time.h"
#include "sfptpd_instance.h"
#include "sfptpd_interface.h"
#include "sfptpd_statistics.h"
#include "sfptpd_ntp_module.h"
#include "sfptpd_ptp_module.h"
#include "sfptpd_pps_module.h"
#include "sfptpd_freerun_module.h"
#include "sfptpd_crny_module.h"
#include "sfptpd_netlink.h"
#include "sfptpd_multicast.h"
#include "sfptpd_clockfeed.h"


/****************************************************************************
 * Engine Messages
 ****************************************************************************/

/* Macro used to define message ID values for engine messages */
#define ENGINE_MSG(x) (SFPTPD_MSG_BASE_ENGINE + (x))

/* Message to signal to engine to step all clocks to the current offset.
 * This is typically sent to the Engine module when a SIGUSR1 signal is
 * sent to sfptpd. It is an asynchronous message without a reply.
 */
#define ENGINE_MSG_STEP_CLOCKS   ENGINE_MSG(1)

/* Message to signal to the engine that a sync module has changed state.
 * The message contains the updated status. This is an asynchronous
 * message without a reply.
 * @sync_module Handle of relevant sync module
 * @sync_instance Handle of sync instance that changed state
 * @status Updated sync module status
 */
#define ENGINE_MSG_SYNC_INSTANCE_STATE_CHANGED   ENGINE_MSG(2)
struct engine_sync_instance_state_changed {
	struct sfptpd_thread *sync_module;
	struct sfptpd_sync_instance *sync_instance;
	struct sfptpd_sync_instance_status status;
};

/* Message to schedule a leap second for the end of the current day
 * (midnight UTC). This is an asynchronous message without a reply.
 * @type Type of leap second
 * @guard_interval Guard interval to use either side of leap second in seconds.
 */
#define ENGINE_MSG_SCHEDULE_LEAP_SECOND   ENGINE_MSG(3)
struct engine_schedule_leap_second {
	enum sfptpd_leap_second_type type;
	long double guard_interval;
};

/* Message to signal to engine to hange selected sync instance
 */
#define ENGINE_MSG_SELECT_INSTANCE   ENGINE_MSG(4)
struct engine_select_instance
{
	char name [SFPTPD_CONFIG_SECTION_NAME_MAX];
};

/* Message to cancel the currently scheduled leap second (if any). This is an
 * asynchronous message without a reply.
 */
#define ENGINE_MSG_CANCEL_LEAP_SECOND   ENGINE_MSG(5)

/* Configure a test mode - the exact behaviour varies accoriding to
 * the mode selected. This is an asynchronous message without a reply.
 * @mode Test mode to be configured
 * @params Test mode specific parameters
 */
#define ENGINE_MSG_CONFIGURE_TEST_MODE   ENGINE_MSG(6)
struct engine_configure_test_mode
{
	enum sfptpd_test_id mode;
	int params[3];
};

/* Request a realtime stats entry to be sent */
#define ENGINE_MSG_RT_STATS_ENTRY ENGINE_MSG(8)

/* Message to signal to engine to facilitate log rotation.
 * This is typically sent to the Engine module when a SIGHUP signal is
 * sent to sfptpd. It is an asynchronous message without a reply.
 */
#define ENGINE_MSG_LOG_ROTATE   ENGINE_MSG(9)

/* Post status information needed for clustering determination.
 */
#define ENGINE_MSG_CLUSTERING_INPUT   ENGINE_MSG(10)

/** Message to notify the release of a link table.
 */
#define ENGINE_MSG_LINK_TABLE_RELEASE ENGINE_MSG(11)
struct engine_link_table_release {
	const struct sfptpd_link_table *link_table;
};


/* Union of all engine messages
 * @hdr Standard message header
 * @u Union of message payloads
 */
typedef struct engine_msg {
	sfptpd_msg_hdr_t hdr;
	union {
		struct engine_sync_instance_state_changed sync_instance_state_changed;
		struct engine_schedule_leap_second schedule_leap_second;
		struct engine_configure_test_mode configure_test_mode;
		struct engine_select_instance select_instance;
		struct sfptpd_clustering_input clustering_input;
		struct engine_link_table_release link_table_release;
	} u;
} engine_msg_t;

STATIC_ASSERT(sizeof(engine_msg_t) < SFPTPD_SIZE_GLOBAL_MSGS);


/* Message to carry realtime stats entry.
 * These will be allocated from a different pool as the messages above.
 * @hdr Standard message header
 * @stats Actual stats contents
 */
struct rt_stats_msg {
	sfptpd_msg_hdr_t hdr;
	struct sfptpd_sync_instance_rt_stats_entry stats;
};
/* This assert has to be here because sfptpd_instance.h
 * and sfptpd_engine.h don't reference each other. */
STATIC_ASSERT(STATS_KEY_END < 8 * sizeof(((struct sfptpd_sync_instance_rt_stats_entry*)0)->stat_present));


/****************************************************************************
 * Types and Structures
 ****************************************************************************/

/* Timer identities */
enum engine_timer_ids {
	ENGINE_TIMER_LOG_STATS,
	ENGINE_TIMER_STATS_PERIOD_END,
	ENGINE_TIMER_SAVE_STATE,
	ENGINE_TIMER_LEAP_SECOND,
	ENGINE_TIMER_SELECTION_HOLDOFF,
	ENGINE_TIMER_NETLINK_RESCAN,
	ENGINE_TIMER_NETLINK_COALESCE,
};

/* Leap second states */
enum leap_second_state {
	LEAP_SECOND_STATE_IDLE,
	LEAP_SECOND_STATE_SCHEDULED,
	LEAP_SECOND_STATE_ACTIVE_PRE,
	LEAP_SECOND_STATE_ACTIVE_POST,
	LEAP_SECOND_STATE_TEST
};

/* Reasons for netlink flow control */
#define NL_XOFF_SPACE (1 << 1)
#define NL_XOFF_COALESCE (1 << 2)

struct sfptpd_engine {
	/* Pointers to overall and general configuration */
	struct sfptpd_config *config;
	struct sfptpd_config_general *general_config;

	/* Engine thread */
	struct sfptpd_thread *thread;

	/* Clock feed service */
	struct sfptpd_clockfeed *clockfeed;
	struct sfptpd_thread *clockfeed_thread;

	/* Leap second data */
	struct {
		/* Leap second state */
		enum leap_second_state state;

		/* Type of leap second that is occurring */
		enum sfptpd_leap_second_type type;

		/* Time that scheduled leap second will occur */
		struct sfptpd_timespec time;

		/* Guard interval to apply around leap second */
		struct sfptpd_timespec guard_interval;
	} leap_second;

	/* Sync modules (fixed-size array) */
	struct sfptpd_thread *sync_modules[SFPTPD_CONFIG_CATEGORY_MAX];
	struct sfptpd_thread *link_subscriber[SFPTPD_CONFIG_CATEGORY_MAX];

	/* Sync instances (array dimensioned at startup) */
	struct sync_instance_record *sync_instances;
	int num_sync_instances;

	/* Current candidate sync instance for selection */
	struct sync_instance_record *candidate;

	/* Selected sync instance */
	struct sync_instance_record *selected;

	/* Discriminator sync instance for clustering */
	struct sync_instance_record *clustering_discriminator;

	/* Time instance last changed */
	struct sfptpd_timespec last_instance_change;

	/* Pointer to the currently selected local reference clock */
	struct sfptpd_clock *lrc;

	/* Total number of servos allocated and number currently active */
	unsigned int total_servos;
	unsigned int active_servos;

	/* Set of local clock servos used to slave clocks to the LRC */
	struct sfptpd_servo **servos;
	sfptpd_sync_module_alarms_t *servo_prev_alarms;

	/* Netlink state */
	struct sfptpd_nl_state *netlink_state;
	const struct sfptpd_link_table *link_table_prev;
	const struct sfptpd_link_table *link_table;
	int link_subscribers;
	int netlink_xoff;
};


/* Used when serialising text output */
const char *RT_STATS_KEY_NAMES[] = {
	[STATS_KEY_OFFSET] = "offset",
	[STATS_KEY_FREQ_ADJ] = "freq-adj",
	[STATS_KEY_OWD] = "one-way-delay",
	[STATS_KEY_PARENT_ID] = "parent-id",
	[STATS_KEY_GM_ID] = "gm-id",
	[STATS_KEY_PPS_OFFSET] = "pps-offset",
	[STATS_KEY_BAD_PERIOD] = "pps-bad-periods",
	[STATS_KEY_OVERFLOWS] = "pps-overflows",
	[STATS_KEY_ACTIVE_INTF] = "active-interface",
	[STATS_KEY_BOND_NAME] = "bond-interface",
	[STATS_KEY_P_TERM] = "p-term",
	[STATS_KEY_I_TERM] = "i-term",
	[STATS_KEY_M_TIME] = "m-time",
	[STATS_KEY_S_TIME] = "s-time",
};

STATIC_ASSERT(sizeof(RT_STATS_KEY_NAMES)/sizeof(*RT_STATS_KEY_NAMES) == STATS_KEY_END);

/****************************************************************************
 * Function prototypes
 ****************************************************************************/

static void on_synchronize(void *user_context);
static void on_log_stats(void *user_context, unsigned int timer_id);
static void on_save_state(void *user_context, unsigned int timer_id);
static void on_stats_period_end(void *user_context, unsigned int timer_id);
static void on_leap_second_timer(void *user_context, unsigned int timer_id);
static void on_selection_holdoff_timer(void *user_context, unsigned int timer_id);
static void on_netlink_rescan_timer(void *user_context, unsigned int timer_id);
static void on_netlink_coalesce_timer(void *user_context, unsigned int timer_id);

struct engine_timer_defn {
	enum engine_timer_ids timer_id;
	clockid_t clock_id;
	sfptpd_thread_on_timer_fn expiry_fn;
};

static const struct engine_timer_defn engine_timer_defns[] =
{
	{ENGINE_TIMER_LOG_STATS,	 CLOCK_MONOTONIC, on_log_stats},
	{ENGINE_TIMER_SAVE_STATE,	 CLOCK_MONOTONIC, on_save_state},
	{ENGINE_TIMER_STATS_PERIOD_END,  CLOCK_MONOTONIC, on_stats_period_end},
	{ENGINE_TIMER_LEAP_SECOND,       CLOCK_REALTIME,  on_leap_second_timer},
	{ENGINE_TIMER_SELECTION_HOLDOFF, CLOCK_MONOTONIC, on_selection_holdoff_timer},
	{ENGINE_TIMER_NETLINK_RESCAN,    CLOCK_MONOTONIC, on_netlink_rescan_timer},
	{ENGINE_TIMER_NETLINK_COALESCE,  CLOCK_MONOTONIC, on_netlink_coalesce_timer},
};



/****************************************************************************
 * Local Functions
 ****************************************************************************/

/* Find the local sync instance record for a given sync instance handle.
   @param engine The engine
   @param handle The sync instance handle
   @returns The sync instance record or NULL if not found.
*/
static struct sync_instance_record *get_sync_instance_record_by_handle(struct sfptpd_engine *engine,
								       struct sfptpd_sync_instance *handle)
{
	struct sync_instance_record *instance;
	int i;

	for (i = 0; i < engine->num_sync_instances; i++) {
		instance = &engine->sync_instances[i];
		if (instance->info.handle == handle) {
			return instance;
		}
	}

	return NULL;
}

/* Find the local sync instance record for a given sync instance name.
   @param engine The engine
   @param name The sync instance name
   @returns The sync instance record or NULL if not found.
*/
static struct sync_instance_record *get_sync_instance_record_by_name(struct sfptpd_engine *engine,
								     const char *name)
{
	struct sync_instance_record *instance;
	int i;

	for (i = 0; i < engine->num_sync_instances; i++) {
		instance = &engine->sync_instances[i];
		if (0 == strcmp(name, instance->info.name)) {
			return instance;
		}
	}

	return NULL;
}


static void change_sync_instance_flags(struct sfptpd_engine *engine,
				       sfptpd_sync_module_ctrl_flags_t value,
				       sfptpd_sync_module_ctrl_flags_t mask)
{
	unsigned int i;
	int rc;
	assert(engine != NULL);

	/* Re-enable timestamp processing on all sync-instances */
	for (i = 0; i < engine->num_sync_instances; i++) {
		rc = sfptpd_sync_module_control(engine->sync_instances[i].info.module,
						engine->sync_instances[i].info.handle,
						value, mask);
		if (rc != 0) {
			ERROR("failed to change control flags to %x (mask %x)"
			      "on sync instance %s, %s\n",
			      value, mask, engine->sync_instances[i].info.name,
			      strerror(rc));
		}
	}
}


static void set_sync_instance_test_mode(struct sfptpd_engine *engine,
					enum sfptpd_test_id id, int param0,
					int param1, int param2)
{
	unsigned int i;
	assert(engine != NULL);

	/* Set test mode on all instances */
	for (i = 0; i < engine->num_sync_instances; i++) {
		sfptpd_sync_module_test_mode(engine->sync_instances[i].info.module,
					     engine->sync_instances[i].info.handle,
					     id, param0, param1, param2);
	}
}


static void update_leap_second_status(struct sfptpd_engine *engine,
				      enum sfptpd_leap_second_type leap_second_type)
{
	unsigned int i;

	assert(engine != NULL);
	assert(leap_second_type < SFPTPD_LEAP_SECOND_MAX);

	/* Re-enable timestamp processing on all sync-instances */
	for (i = 0; i < SFPTPD_CONFIG_CATEGORY_MAX; i++) {
		if (engine->sync_modules[i] != NULL) {
			sfptpd_sync_module_update_leap_second(engine->sync_modules[i],
							      leap_second_type);
		}
	}
}


static void reconfigure_servos(struct sfptpd_engine *engine,
			       sfptpd_sync_instance_status_t *sync_module_status)
{
	struct sfptpd_clock **active;
	struct sfptpd_clock *clock;
	size_t num_active = 0;
	unsigned int clock_idx;
	unsigned int idx;

	assert(engine != NULL);
	assert(sync_module_status != NULL);

	assert(sync_module_status->clock != NULL);
	engine->lrc = sync_module_status->clock;

	/* For each clock that is not the LRC, configure a servo to slave the
	 * clock to the LRC */
	active = sfptpd_clock_get_active_snapshot(&num_active);
	idx = 0;
	for (clock_idx = 0; clock_idx < num_active; clock_idx++) {
		clock = active[clock_idx];
		if (sfptpd_clock_get_discipline(clock) && (clock != engine->lrc)) {
			/* We should always have enough servos */
			assert(idx < engine->total_servos);

			sfptpd_servo_set_clocks(engine->servos[idx],
						engine->lrc, clock);
			idx++;
		}
	}
	sfptpd_clock_free_active_snapshot(active);

	/* Record the number of active servos */
	engine->active_servos = idx;
	TRACE_L3("total servos %d, active servos %d, lrc %s\n",
		 engine->total_servos, engine->active_servos,
		 sfptpd_clock_get_short_name(engine->lrc));
}


static void destroy_servos(struct sfptpd_engine *engine)
{
	unsigned int i;

	if (engine->servos != NULL) {
		for (i = 0; i < engine->total_servos; i++) {
			if (engine->servos[i] != NULL) {
				sfptpd_servo_destroy(engine->servos[i]);
				engine->servos[i] = NULL;
			}
		}

		free(engine->servos);
		engine->servos = NULL;
	}
	if (engine->servo_prev_alarms != NULL) {
		free(engine->servo_prev_alarms);
		engine->servo_prev_alarms = NULL;
	}

	engine->total_servos = 0;
	engine->active_servos = 0;
}


static int create_servos(struct sfptpd_engine *engine, struct sfptpd_config *config)
{
	struct sfptpd_servo *servo;
	int rc;
	unsigned int i;

	assert(engine != NULL);
	assert(config != NULL);

	/* Record the total number of servos we could need. */
	engine->total_servos = sfptpd_clock_get_total() + SFPTPD_EXTRA_SERVOS_FOR_HOTPLUGGING;
	engine->active_servos = 0;

	TRACE_L3("maximum servos required %d (including %d spare for hotplugging)\n",
		 engine->total_servos,
		 SFPTPD_EXTRA_SERVOS_FOR_HOTPLUGGING);
	if (engine->total_servos > 0) {
		engine->servos = (struct sfptpd_servo **)calloc(engine->total_servos,
								sizeof(void *));
		engine->servo_prev_alarms = (sfptpd_sync_module_alarms_t *)calloc(engine->total_servos,
									          sizeof(sfptpd_sync_module_alarms_t));
		if (engine->servos == NULL) {
			CRITICAL("failed to allocate servos\n");
			return ENOMEM;
		}
	}

	/* Allocate the required number of servos */
	rc = 0;
	for (i = 0; i < engine->total_servos; i++) {
		servo = sfptpd_servo_create(engine->clockfeed, config, i);
		if (servo == NULL) {
			CRITICAL("failed to allocate servo\n");
			rc = ENOMEM;
			break;
		}
		engine->servos[i] = servo;
	}

	/* If not successful, tidy up the mess */
	if (rc != 0)
		destroy_servos(engine);

	return rc;
}


static int create_timers(struct sfptpd_engine *engine)
{
	struct sfptpd_timespec interval;
	unsigned int i;
	int rc;

	assert(engine != NULL);
	assert(engine->config != NULL);

	/* First, create the timers */
	for (i = 0; i < sizeof(engine_timer_defns)/sizeof(engine_timer_defns[0]); i++) {
		rc = sfptpd_thread_timer_create(engine_timer_defns[i].timer_id,
						engine_timer_defns[i].clock_id,
						engine_timer_defns[i].expiry_fn,
						engine);
		if (rc != 0) {
			CRITICAL("failed to create timer %d for engine, %s\n",
				 engine_timer_defns[i].timer_id, strerror(rc));
			return rc;
		}
	}

	/* Start the stats logging timer */
	sfptpd_time_from_s(&interval, SFPTPD_STATISTICS_LOGGING_INTERVAL);
	rc = sfptpd_thread_timer_start(ENGINE_TIMER_LOG_STATS,
				       true, false, &interval);
	if (rc != 0) {
		CRITICAL("failed to start stats logging timer, %s\n", strerror(rc));
		return rc;
	}

	/* Start a long-term stats collection timer to go off every minute */
	sfptpd_time_from_s(&interval, SFPTPD_STATS_COLLECTION_INTERVAL);
	rc = sfptpd_thread_timer_start(ENGINE_TIMER_STATS_PERIOD_END,
				       true, false, &interval);
	if (rc != 0) {
		CRITICAL("failed to start stats collection timer, %s\n", strerror(rc));
		return rc;
	}

	/* Start the state save timer */
	sfptpd_time_from_s(&interval, SFPTPD_STATE_SAVE_INTERVAL);
	rc = sfptpd_thread_timer_start(ENGINE_TIMER_SAVE_STATE,
				       true, false, &interval);
	if (rc != 0) {
		CRITICAL("failed to start state save timer, %s\n", strerror(rc));
		return rc;
	}

	/* If in manual-startup mode and the initial sync instance is not the
	 * best by automatic selection, kick off the holdoff timer. */
	if (engine->general_config->selection_holdoff_interval != 0 &&
	    engine->general_config->selection_policy.strategy == SFPTPD_SELECTION_STRATEGY_MANUAL_STARTUP &&
	    engine->candidate != engine->selected) {

		sfptpd_time_from_s(&interval, engine->general_config->selection_holdoff_interval);

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_SELECTION_HOLDOFF,
					       false, false, &interval);
		if (rc != 0) {
			CRITICAL("failed to start selection holdoff timer, %s\n",
				 strerror(rc));
			return rc;
		}
	}

	/* Start the netlink rescan timer */
	if (engine->general_config->netlink_rescan_interval != 0) {

		sfptpd_time_from_s(&interval, engine->general_config->netlink_rescan_interval);

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_NETLINK_RESCAN,
					       true, false, &interval);
		if (rc != 0) {
			CRITICAL("failed to start netlink rescan timer, %s\n",
				 strerror(rc));
			return rc;
		}
	}

	return 0;
}


static void write_state(struct sfptpd_engine *engine)
{
	int i;

	/* Save the state of the sync modules */
	for (i = 0; i < SFPTPD_CONFIG_CATEGORY_MAX; i++) {
		if (engine->sync_modules[i] != NULL) {
			sfptpd_sync_module_save_state(engine->sync_modules[i]);
		}
	}

	/* For each of the servos, save the state */
	for (i = 0; i < engine->active_servos; i++)
		sfptpd_servo_save_state(engine->servos[i]);
}


static void write_topology(struct sfptpd_engine *engine)
{
	struct sfptpd_log *log;
	FILE *stream;
	unsigned int i, num_servos;
	struct sync_instance_record *sync_instance;

	assert(engine != NULL);

	sync_instance = engine->selected;
	if (sync_instance == NULL) {
		WARNING("cannot write topology with no selected sync instance\n");
		return;
	}

	log = sfptpd_log_open_topology();
	if (log == NULL)
		return;
	stream = sfptpd_log_file_get_stream(log);

	num_servos = engine->active_servos;

	/* Pass the topology file handle to the sync module to fill in */
	sfptpd_sync_module_write_topology(sync_instance->info.module,
					  sync_instance->info.handle,
					  stream);

	if (num_servos > 0) {
		/* Write topology for secondary servos:
		 * 1-to-n connector start */
		sfptpd_log_topology_write_1ton_connector_start(stream, num_servos, false);
		/* Write ns offset for each servo */
		for (i = 0; i < num_servos; i++)
			sfptpd_servo_write_topology_offset(engine->servos[i], stream);
		fputc('\n', stream);
		/* Finish the 1-to-n connector */
		sfptpd_log_topology_write_1ton_connector_end(stream, num_servos, true);
		/* Write the interface name for each servo clock */
		for (i = 0; i < num_servos; i++)
			sfptpd_servo_write_topology_clock_name(engine->servos[i], stream);
		fputc('\n', stream);
		/* Write the MAC address for each servo clock */
		for (i = 0; i < num_servos; i++)
			sfptpd_servo_write_topology_clock_hw_id(engine->servos[i], stream);
		fputc('\n', stream);
	}

	sfptpd_log_file_close(log);
}


static void write_sync_instances(struct sfptpd_engine *engine)
{
	const char *header[] = { "R", "instance", "S", "M", "X", "state", "O", "A", "priority", "C", "gm class", "accuracy", "allan var", "steps" };
	const char *format_header = "| %2s | %-12s%1s | %1s%1s |%-9s %1s | %1s | %8s | %1s | %-11s | %8s | %9s | %5s |\n";
	const char *format_record = "| %2d | %-12s%1s | %c%c |%-9s %1d | %1s | %8.3g | %1d | %-11s | %8.3llg | %9.3llg | %5d |\n";
	const struct sfptpd_selection_policy *policy;
	struct sfptpd_log *log;
	FILE *stream;
	unsigned int i;
	struct sync_instance_record *record;

	assert(engine != NULL);

	log = sfptpd_log_open_sync_instances();
	if (log == NULL)
		return;
	stream = sfptpd_log_file_get_stream(log);

	/* Write table header */
	sfptpd_log_table_row(stream, true, format_header,
			     header[0], header[1], header[2], header[3],
			     header[4], header[5], header[6], header[7],
			     header[8], header[9], header[10], header[11],
			     header[12], header[13]);

	/* Write table records */
	for (i = 0; i < engine->num_sync_instances; i++) {
		char constraint;

		record = engine->sync_instances + i;
		if (SYNC_MODULE_CONSTRAINT_TEST(record->status.constraints, MUST_BE_SELECTED))
			constraint = 'm';
		else if (SYNC_MODULE_CONSTRAINT_TEST(record->status.constraints, CANNOT_BE_SELECTED))
			constraint = 'c';
		else
			constraint = '-';

		sfptpd_log_table_row(stream, i == engine->num_sync_instances - 1, format_record,
				     record->rank,
				     record->info.name,
				     record == engine->selected ? "*" : " ",
				     record->selected ? 'M' : '-',
				     constraint,
				     sync_module_state_text[record->status.state],
				     sfptpd_state_priorities[record->status.state],
				     record->status.alarms == 0 ? " " : "A",
				     (double) record->status.user_priority,
				     record->status.clustering_score,
				     sfptpd_clock_class_text(record->status.master.clock_class),
				     record->status.master.accuracy + record->status.local_accuracy,
				     record->status.master.allan_variance,
				     record->status.master.steps_removed);
	}

	fprintf(stream,
		"\nKey: R = rank  S = selected  M = manual  O = state order  "
		"A = alarms  C = clustering score\n"
		"     X = external constraint (m = must-be-selected  c = cannot-be-selected) \n");

	fprintf(stream,
		"\nSelection policy:\n");

	policy = &engine->general_config->selection_policy;
	for (i = 0; i < SELECTION_RULE_MAX; i++) {
		enum sfptpd_selection_rule rule = policy->rules[i];

		if (rule == SELECTION_RULE_END)
			break;
		assert(rule < SELECTION_RULE_MAX);

		fprintf(stream, " %i : %s\n", i, sfptpd_selection_rule_names[rule]);
	}

	sfptpd_log_file_close(log);
}


static void write_interfaces(void)
{
	const char *format_interface_string = "| %16s | %8s | %21s | %17s |\n";
	const char *format_interface_data = "| %16s | %8s | %21s | %17s |\n";
	const char *ts_caps[4] = {"-", "sw", "hw", "hw & sw"};
	struct sfptpd_log *log;
	FILE *stream;
	struct sfptpd_interface *interface;
	struct sfptpd_db_query_result query_result;
	const char *ptp_caps, *rx_ts_caps;
	int i;

	log = sfptpd_log_open_interfaces();
	if (log == NULL)
		return;
	stream = sfptpd_log_file_get_stream(log);

	sfptpd_log_table_row(stream, true, format_interface_string,
			     "interface",
			     "ptp-caps",
			     "pkt-timestamping-caps",
			     "mac-address");

	query_result = sfptpd_interface_get_all_snapshot();

	for (i = 0; i < query_result.num_records; i++) {
		struct sfptpd_interface **intfp = query_result.record_ptrs[i];
		interface = *intfp;

		/* This is slightly naughty. It's safe in the sense that it
		 * can't cause a crash/corruption etc but might no longer be
		 * correct if the capabilities are extended in the future. */
		ptp_caps = ts_caps[sfptpd_interface_ptp_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_ALL];
		rx_ts_caps = ts_caps[sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_ALL];

		sfptpd_log_table_row(stream,
				     (i + 1 == query_result.num_records),
				     format_interface_data,
				     sfptpd_interface_get_name(interface),
				     ptp_caps, rx_ts_caps,
				     sfptpd_interface_get_mac_string(interface));
	}

	query_result.free(&query_result);

	sfptpd_log_file_close(log);
}


static void propagate_grandmaster_info(struct sfptpd_engine *engine,
				       struct sfptpd_sync_instance_info *info,
				       struct sfptpd_grandmaster_info *master) {
	enum sfptpd_config_category type;

	assert(engine != NULL);
	assert(info != NULL);
	assert(master != NULL);

	TRACE_L2("new grandmaster info: instance = %s, id = "
		 "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx, "
		 "remote = %s, clock class = %s, time source = %s, accuracy = %Lf, steps removed = %d\n",
		 info->name,
		 master->clock_id.id[0], master->clock_id.id[1],
		 master->clock_id.id[2], master->clock_id.id[3],
		 master->clock_id.id[4], master->clock_id.id[5],
		 master->clock_id.id[6], master->clock_id.id[7],
		 master->remote_clock? "yes": "no",
		 sfptpd_clock_class_text(master->clock_class),
		 sfptpd_clock_time_source_text(master->time_source),
		 master->accuracy, master->steps_removed);

	/* Propagate any grandmaster information */
	for (type = 0; type < SFPTPD_CONFIG_CATEGORY_MAX; type++) {
		struct sfptpd_thread *module = engine->sync_modules[type];
		if (module != NULL) {
			sfptpd_sync_module_update_gm_info(module, info->handle, master);
		}
	}
}


/* Select a new sync instance
   @param engine The engine
   @param the_new The new sync instance
   @returns Zero on success otherwise an error code.
*/
static int select_sync_instance(struct sfptpd_engine *engine,
				struct sync_instance_record *the_new)
{
	struct sync_instance_record *the_old;
	struct sfptpd_timespec time_now;
	struct sfptpd_timespec time_last_instance = { 0 };
	int rc;
	bool lrc_change;
	struct sfptpd_log_time log_time;

	assert(engine != NULL);
	assert(the_new != NULL);

	the_old = engine->selected;

	/* If the new and old instances are the same, there is nothing to do. */
	if (the_new == the_old)
		return 0;

	sfptpd_log_get_time(&log_time);

	/* Is the LRC changing? */
	lrc_change = (the_new->status.clock != engine->lrc);

	/* Stop the old instance from doing things. */
	if (the_old != NULL) {
		rc = sfptpd_sync_module_control(the_old->info.module,
						the_old->info.handle,
						0,
						SYNC_MODULE_SELECTED | SYNC_MODULE_CLOCK_CTRL);
		if (rc != 0) {
			CRITICAL("failed to deselect sync instance %s, %s\n",
				 the_old->info.name, strerror(rc));
			return rc;
		}

		/* bug73223/73674 : at startup we have engine->thread == NULL, which will cause
		 * an assert. We should set this variable in thread_create before startup. */
		if (engine->thread != NULL)
			sfptpd_sync_module_log_stats(the_old->info.module, &log_time);
	}

	/* Change the engine's record */
	engine->selected = the_new;

	/* Start the new instance doing things. */
	rc = sfptpd_sync_module_control(the_new->info.module,
					the_new->info.handle,
					SYNC_MODULE_SELECTED | SYNC_MODULE_CLOCK_CTRL,
					SYNC_MODULE_SELECTED | SYNC_MODULE_CLOCK_CTRL);
	if (rc != 0) {
		CRITICAL("failed to select sync instance %s, %s\n",
			 the_new->info.name, strerror(rc));
		return rc;
	}

	/* bug73223/73674 : at startup we have engine->thread == NULL, which will cause
	 * an assert. We should set this variable in thread_create before startup. */
	if (engine->thread != NULL)
		sfptpd_sync_module_log_stats(the_new->info.module, &log_time);

	/* Update the GM info */
	propagate_grandmaster_info(engine,
				   &engine->selected->info,
				   &(engine->selected->status.master));

	/* Reconfigure the servos if necessary */
	if (lrc_change)
		reconfigure_servos(engine, &(engine->selected->status));

	/* Write the updated topology and state. */
	write_topology(engine);
	write_state(engine);
	write_sync_instances(engine);

	/* Record the time of this change and print how long the previous
	 * instance was selected for. */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &time_now);
	if (the_old != NULL) {
		sfptpd_time_subtract(&time_last_instance, &time_now, &engine->last_instance_change);
	}
	engine->last_instance_change = time_now;

	INFO("selected sync instance %s (%s was active for " SFPTPD_FORMAT_FLOAT "s)\n",
	     the_new->info.name,
	     (the_old == NULL ? "[none]" : the_old->info.name),
	     sfptpd_time_timespec_to_float_ns(&time_last_instance) / ONE_BILLION);

	return 0;
}


static void write_rt_stats_log(struct sfptpd_log_time *time,
			       struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	char *comma = "";

	assert(entry != NULL);

	sfptpd_log_stats("%s [%s%s%s%s", time->time,
		       entry->instance_name ? entry->instance_name : "",
		       entry->instance_name ? ":" : "",
		       entry->clock_master ? sfptpd_clock_get_short_name(entry->clock_master) : entry->source,
		       entry->is_blocked ? "-#" : (entry->is_disciplining ? "->" : "--")
			);

	if (entry->active_intf != NULL)
		sfptpd_log_stats("%s(%s)", sfptpd_clock_get_short_name(entry->clock_slave),
						 sfptpd_interface_get_name(entry->active_intf));
	else
		sfptpd_log_stats("%s", sfptpd_clock_get_long_name(entry->clock_slave));

	sfptpd_log_stats("], "); /* To maintain backwards compatibility the comma var is actually useless */

	#define FLOAT_STATS_OUT(k, v, red) \
		if (entry->stat_present & (1 << k)) { \
			if (red) \
				sfptpd_log_stats("%s%s: " SFPTPD_FORMAT_FLOAT_RED, comma, RT_STATS_KEY_NAMES[k], v); \
			else \
				sfptpd_log_stats("%s%s: " SFPTPD_FORMAT_FLOAT, comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ", "; \
		}
	#define INT_STATS_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			sfptpd_log_stats("%s%s: %d", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ", "; \
		}
	#define EUI64_STATS_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			sfptpd_log_stats("%s%s: " SFPTPD_FORMAT_EUI64, comma, RT_STATS_KEY_NAMES[k], \
						v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); \
			comma = ", "; \
		}

	bool alarm_red = sfptpd_log_isatty() && entry->alarms != 0;

	FLOAT_STATS_OUT(STATS_KEY_OFFSET, entry->offset, alarm_red);
	FLOAT_STATS_OUT(STATS_KEY_FREQ_ADJ, entry->freq_adj, 0);
	sfptpd_log_stats("%sin-sync: %s", comma, entry->is_in_sync ? "1" : "0");
	comma = ", ";
	FLOAT_STATS_OUT(STATS_KEY_OWD, entry->one_way_delay, alarm_red);
	EUI64_STATS_OUT(STATS_KEY_PARENT_ID, entry->parent_id);
	EUI64_STATS_OUT(STATS_KEY_GM_ID, entry->gm_id);
	FLOAT_STATS_OUT(STATS_KEY_PPS_OFFSET, entry->pps_offset, 0);
	INT_STATS_OUT(STATS_KEY_BAD_PERIOD, entry->bad_period_count);
	INT_STATS_OUT(STATS_KEY_OVERFLOWS, entry->overflow_count);

	#undef FLOAT_STATS_OUT
	#undef INT_STATS_OUT
	#undef EUI64_STATS_OUT

	sfptpd_log_stats("\n");
}


static void write_rt_stats_json(FILE* json_stats_fp,
				struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	char* comma = "";
	char ftime[24];
	size_t len = 0;

	assert(json_stats_fp != NULL);
	assert(entry != NULL);

	#define LPRINTF(...) { \
		int _ret = fprintf(__VA_ARGS__); \
		if (_ret < 0) { \
			 TRACE_L4("error writing json stats, %s\n", \
				  strerror(errno)); \
			 return; \
		} \
		len += _ret; \
	}

	LPRINTF(json_stats_fp, "{\"instance\":\"%s\",\"time\":\"%s\","
		"\"clock-master\":{\"name\":\"%s\"",
		entry->instance_name ? entry->instance_name : "",
		entry->time.time, entry->clock_master ?
			sfptpd_clock_get_long_name(entry->clock_master) : entry->source);

	/* Add clock time */
	if (entry->clock_master != NULL) {
		if (entry->has_m_time) {
			sfptpd_secs_t secs = entry->time_master.sec;
			sfptpd_local_strftime(ftime, (sizeof ftime) - 1, "%Y-%m-%d %H:%M:%S", &secs);
			LPRINTF(json_stats_fp, ",\"time\":\"%s.%09" PRIu32 "\"",
				ftime, entry->time_master.nsec);
		}

		/* Extra info about clock interface, mostly useful when using bonds */
		if (entry->clock_master != sfptpd_clock_get_system_clock())
			LPRINTF(json_stats_fp, ",\"primary-interface\":\"%s\"",
				sfptpd_interface_get_name(
					sfptpd_clock_get_primary_interface(entry->clock_master)));
	}

	/* Slave clock info */
	LPRINTF(json_stats_fp, "},\"clock-slave\":{\"name\":\"%s\"",
		sfptpd_clock_get_long_name(entry->clock_slave));
	if (entry->has_s_time) {
		sfptpd_secs_t secs = entry->time_slave.sec;
		sfptpd_local_strftime(ftime, (sizeof ftime) - 1, "%Y-%m-%d %H:%M:%S", &secs);
		LPRINTF(json_stats_fp, ",\"time\":\"%s.%09" PRIu32 "\"",
			ftime, entry->time_slave.nsec);
	}

	/* Extra info about clock interface, mostly useful when using bonds */
	if (entry->clock_slave != sfptpd_clock_get_system_clock())
		 LPRINTF(json_stats_fp, ",\"primary-interface\":\"%s\"",
				 sfptpd_interface_get_name(
					 sfptpd_clock_get_primary_interface(entry->clock_slave)));

	LPRINTF(json_stats_fp, "},\"is-disciplining\":%s,\"in-sync\":%s,"
			       "\"alarms\":[",
			entry->is_disciplining ? "true" : "false",
			entry->is_in_sync ? "true" : "false");

	/* Alarms */
	len += sfptpd_sync_module_alarms_stream(json_stats_fp, entry->alarms, ",");

	LPRINTF(json_stats_fp, "],\"stats\":{");

	/* Print those stats which are present */
	#define FLOAT_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":%Lf", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define INT_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":%d", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define STRING_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":\"%s\"", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define EUI64_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":\"" SFPTPD_FORMAT_EUI64 "\"", \
					comma, RT_STATS_KEY_NAMES[k], \
					v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); \
			comma = ","; \
		}

	FLOAT_JSON_OUT(STATS_KEY_OFFSET, entry->offset);
	FLOAT_JSON_OUT(STATS_KEY_FREQ_ADJ, entry->freq_adj);
	FLOAT_JSON_OUT(STATS_KEY_OWD, entry->one_way_delay);
	EUI64_JSON_OUT(STATS_KEY_PARENT_ID, entry->parent_id);
	EUI64_JSON_OUT(STATS_KEY_GM_ID, entry->gm_id);
	STRING_JSON_OUT(STATS_KEY_ACTIVE_INTF, sfptpd_interface_get_name(entry->active_intf));
	STRING_JSON_OUT(STATS_KEY_BOND_NAME, entry->bond_name);
	FLOAT_JSON_OUT(STATS_KEY_PPS_OFFSET, entry->pps_offset);
	INT_JSON_OUT(STATS_KEY_BAD_PERIOD, entry->bad_period_count);
	INT_JSON_OUT(STATS_KEY_OVERFLOWS, entry->overflow_count);
	FLOAT_JSON_OUT(STATS_KEY_P_TERM, entry->p_term);
	FLOAT_JSON_OUT(STATS_KEY_I_TERM, entry->i_term);

	#undef FLOAT_JSON_OUT
	#undef INT_JSON_OUT
	#undef STRING_JSON_OUT
	#undef EUI64_JSON_OUT

	/* Close json object and flush stream */
	LPRINTF(json_stats_fp, "}}\n");

	#undef LPRINTF

	sfptpd_log_rt_stats_written(len, entry->alarms != 0);
}


/****************************************************************************
 * Engine event handlers
 ****************************************************************************/

static void on_cancel_leap_second(struct sfptpd_engine *engine)
{
	/* If the leap second is in progress, it can't be cancelled */
	if (engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_PRE) {
		WARNING("can't cancel leap second - already in progress!\n");
		return;
	}

	/* If a leap second is scheduled or we are in the guard interval after
	 * it has occurred, unschedule it and resume synchronize operations. */
	if ((engine->leap_second.state == LEAP_SECOND_STATE_SCHEDULED) ||
	    (engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_POST)) {
		/* Clear the leap second on all clocks that support this
		 * function (system clock only) */
		sfptpd_clock_schedule_leap_second(SFPTPD_LEAP_SECOND_NONE);
	}

	/* This covers the scheduled, active post and test states */
	if (engine->leap_second.state != LEAP_SECOND_STATE_IDLE) {
		/* Stop the leap second timer and go back to the inactive state */
		sfptpd_thread_timer_stop(ENGINE_TIMER_LEAP_SECOND);
		engine->leap_second.state = LEAP_SECOND_STATE_IDLE;

		/* Clear the leap second guard flag and re-enable timestamp
		 * processing on all sync-instances */
		change_sync_instance_flags(engine, 0,
					   SYNC_MODULE_LEAP_SECOND_GUARD);
		change_sync_instance_flags(engine, SYNC_MODULE_TIMESTAMP_PROCESSING,
					   SYNC_MODULE_TIMESTAMP_PROCESSING);

		/* Update leap second status in all sync modules. */
		update_leap_second_status(engine, SFPTPD_LEAP_SECOND_NONE);

		NOTICE("leap second cancelled/complete\n");
	}
}


static void on_schedule_leap_second(struct sfptpd_engine *engine,
				    enum sfptpd_leap_second_type type,
				    long double guard_interval)
{
	struct sfptpd_timespec now, expiry_time;
	struct sfptpd_config_general *general_cfg;
	int rc;

	assert(engine != NULL);
	assert((type == SFPTPD_LEAP_SECOND_61) || (type == SFPTPD_LEAP_SECOND_59));
	assert(guard_interval > 0.0);
	
	general_cfg = engine->general_config;

	/* Enforce a minimum and maximums for the guard interval */
	if (guard_interval < SFPTPD_LEAP_SECOND_GUARD_INTERVAL_MIN)
		guard_interval = SFPTPD_LEAP_SECOND_GUARD_INTERVAL_MIN;
	if (guard_interval > SFPTPD_LEAP_SECOND_GUARD_INTERVAL_MAX)
		guard_interval = SFPTPD_LEAP_SECOND_GUARD_INTERVAL_MAX;

	/* If a leap second is scheduled, unschedule it! */
	if ((engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_PRE) ||
	    (engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_POST)) {
		WARNING("can't schedule leap second - one already in progress!\n");
		return;
	}

	/* If there is a leap second already scheduled or we are in the test
	 * state, cancel it */
	if ((engine->leap_second.state == LEAP_SECOND_STATE_SCHEDULED) ||
	    (engine->leap_second.state == LEAP_SECOND_STATE_TEST)) {
		on_cancel_leap_second(engine);
	}

	/* Work out when the leap second will occur */
	if (sfclock_gettime(CLOCK_REALTIME, &now) < 0) {
		ERROR("Failed to get realtime time, %s\n", strerror(errno));
	} else {
		/* Work out when the end of the current day is as an absolute
		 * UTC time in seconds */
		sfptpd_time_from_s(&engine->leap_second.time, now.sec - (now.sec % 86400) + 86400);

		/* If this is a leap second 59, then the second is deleted one
		 * second before midnight i.e. the day is one second shorter. */
		if (type == SFPTPD_LEAP_SECOND_59)
			engine->leap_second.time.sec--;

		/* Record the leap second type and the guard interval */
		engine->leap_second.type = type;
		sfptpd_time_float_s_to_timespec(guard_interval,
						&engine->leap_second.guard_interval);

		/* Set up the leap second timer for the leap second due time
		 * minus a guard interval during which time synchronization of
		 * the clocks will stop. Note that if application of the guard
		 * interval takes us into the past, the timer will expiry
		 * immediately. */
		sfptpd_time_subtract(&expiry_time, &engine->leap_second.time,
				     &engine->leap_second.guard_interval);

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_LEAP_SECOND,
					       false, true, &expiry_time);
		if (rc != 0) {
			ERROR("failed to start leap second timer, %s\n",
			      strerror(rc));
			return;
		}

		/* If clock stepping is enabled, schedule the leap second.
		 * Note that only the system clock supports this. */
		if ((general_cfg->clocks.control == SFPTPD_CLOCK_CTRL_SLEW_AND_STEP) ||
		    ((general_cfg->clocks.control == SFPTPD_CLOCK_CTRL_STEP_FORWARD) &&
		     (type == SFPTPD_LEAP_SECOND_61)))
			sfptpd_clock_schedule_leap_second(type);

		/* Update leap second status in all sync modules. */
		update_leap_second_status(engine, type);

		/* Go to the scheduled state */
		engine->leap_second.state = LEAP_SECOND_STATE_SCHEDULED;
		char ftime[8];
		sfptpd_secs_t leap_second_time = (sfptpd_secs_t)sfptpd_time_timespec_to_float_s(&engine->leap_second.time);
		sfptpd_local_strftime(ftime, sizeof(ftime), "%H:%M", &leap_second_time);
		NOTICE("leap second %s scheduled for UTC midnight (local time: %s)\n",
		       (type == SFPTPD_LEAP_SECOND_61)? "61": "59", ftime);
	}
}


static void on_test_adjust_frequency(struct sfptpd_engine *engine,
				     int amount)
{
	long double adjustment = (long double) amount;

	assert(engine != NULL);

	if (engine->lrc != NULL) {
		NOTICE("adjusting local reference clock (%s) frequency by %Lf ppb\n",
		       sfptpd_clock_get_short_name(engine->lrc),
		       adjustment);
		sfptpd_clock_adjust_frequency(engine->lrc, adjustment);
	} else {
		ERROR("no reference clock to adjust\n");
	}
}


static void on_test_leap_second(struct sfptpd_engine *engine,
				enum sfptpd_leap_second_type type)
{
	struct sfptpd_timespec now;
	int rc;
	struct sync_instance_record *instance = engine->selected;

	assert(instance != NULL);

	if (!engine->general_config->test_mode)
		return;

	/* If a leap second is scheduled then refuse the test request */
	if ((engine->leap_second.state != LEAP_SECOND_STATE_IDLE) &&
	    (engine->leap_second.state != LEAP_SECOND_STATE_TEST)) {
		WARNING("won't set up leap second - genuine leap second scheduled!\n");
		return;
	}

	/* If there is a leap second test set up, cancel it */
	if (engine->leap_second.state == LEAP_SECOND_STATE_TEST)
		on_cancel_leap_second(engine);

	/* If we have only been asked to cancel the leap second then we've
	 * finished. */
	if (type == SFPTPD_LEAP_SECOND_NONE)
		return;

	/* Work out when the leap second will occur */
	if (sfclock_gettime(CLOCK_REALTIME, &now) < 0) {
		ERROR("Failed to get realtime time, %s\n", strerror(errno));
	} else {
		/* Work out when the end of the current day is as an
		 * absolute UTC time in seconds */
		sfptpd_time_from_s(&engine->leap_second.time, now.sec - (now.sec % 86400) + 86400);

		/* If this is a leap second 59, then the second is
		 * deleted one second before midnight i.e. the day is
		 * one second shorter. */
		if (type == SFPTPD_LEAP_SECOND_59)
			engine->leap_second.time.sec--;

		/* If we are testing leap seconds then the master has to be
		 * serving the atomic timescale. Setting the timer for midnight
		 * system time is correct because this is when the leap second
		 * actually occurs. */

		/* Record the leap second type */
		engine->leap_second.type = type;

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_LEAP_SECOND, false,
					       true, &engine->leap_second.time);
		if (rc != 0) {
			ERROR("failed to start leap second timer, %s\n",
			      strerror(rc));
			return;
		}

		/* Update leap second status in all sync modules. */
		update_leap_second_status(engine, type);

		char ftime[8];
		sfptpd_secs_t leap_second_time = (sfptpd_secs_t)sfptpd_time_timespec_to_float_s(&engine->leap_second.time);
		sfptpd_local_strftime(ftime, sizeof(ftime), "%H:%M", &leap_second_time);
		INFO("leap second %s test at UTC midnight (local time: %s)\n",
		     (type == SFPTPD_LEAP_SECOND_61)? "61": "59", ftime);

		/* Go to the test state */
		engine->leap_second.state = LEAP_SECOND_STATE_TEST;
	}
}


static void on_leap_second_timer(void *user_context, unsigned int timer_id)
{
	struct sfptpd_timespec expiry_time;
	int rc;
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	struct sync_instance_record *instance = engine->selected;

	assert(engine != NULL);
	assert(instance != NULL);

	switch (engine->leap_second.state) {
	case LEAP_SECOND_STATE_SCHEDULED:
		/* The scheduling timer indicates that the leap second is
		 * imminent. Restart the timer for the moment of the leap
		 * second event and change state. */
		expiry_time = engine->leap_second.time;
		rc = sfptpd_thread_timer_start(ENGINE_TIMER_LEAP_SECOND, false,
					       true, &expiry_time);
		if (rc != 0) {
			ERROR("failed to restart leap second timer, %s\n",
			      strerror(rc));
			engine->leap_second.state = LEAP_SECOND_STATE_IDLE;
			return;
		}
		engine->leap_second.state = LEAP_SECOND_STATE_ACTIVE_PRE;

		/* Set the leap second guard flag and disable timestamp
		 * processing on all sync-instances */
		change_sync_instance_flags(engine, 0,
					   SYNC_MODULE_TIMESTAMP_PROCESSING);
		change_sync_instance_flags(engine, SYNC_MODULE_LEAP_SECOND_GUARD,
					   SYNC_MODULE_LEAP_SECOND_GUARD);

		NOTICE("leap second %s imminent. Suspending timestamp processing\n",
		       (engine->leap_second.type == SFPTPD_LEAP_SECOND_61)? "61": "59");
		break;

	case LEAP_SECOND_STATE_ACTIVE_PRE:
		if ((engine->general_config->clocks.control == SFPTPD_CLOCK_CTRL_SLEW_AND_STEP) ||
		    ((engine->general_config->clocks.control == SFPTPD_CLOCK_CTRL_STEP_FORWARD) &&
		     (engine->leap_second.type == SFPTPD_LEAP_SECOND_59))) {
			NOTICE("leap second now: stepping clocks %s by one second\n",
			       (engine->leap_second.type == SFPTPD_LEAP_SECOND_59)?
			       "forward": "backward");

			/* The leap second occurs now. If configured to do so,
			 * step the clock of each servo. Restart the timer to
			 * end the post leap second guard interval. */
			sfptpd_clock_leap_second_now(engine->leap_second.type);
		} else {
			NOTICE("leap second now: clocks will be slewed %s by one second\n",
			       (engine->leap_second.type == SFPTPD_LEAP_SECOND_59)?
			       "forward": "backward");
		}

		/* Update leap second status in all sync modules indicating
		 * that the leap second has happened. */
		engine->leap_second.type = SFPTPD_LEAP_SECOND_NONE;
		update_leap_second_status(engine, SFPTPD_LEAP_SECOND_NONE);

		sfptpd_time_add(&expiry_time, &engine->leap_second.time,
				&engine->leap_second.guard_interval);
		rc = sfptpd_thread_timer_start(ENGINE_TIMER_LEAP_SECOND, false,
					       true, &expiry_time);
		if (rc != 0) {
			ERROR("failed to restart leap second timer, %s\n",
			      strerror(rc));
			engine->leap_second.state = LEAP_SECOND_STATE_IDLE;
			return;
		}
		engine->leap_second.state = LEAP_SECOND_STATE_ACTIVE_POST;
		break;

	case LEAP_SECOND_STATE_ACTIVE_POST:
		/* The end of the guard interval. Complete the leap second */
		on_cancel_leap_second(engine);
		break;

	case LEAP_SECOND_STATE_TEST:
		/* Use a test mode to change the UTC offset in all
		 * (appropriate) sync instances. */
		set_sync_instance_test_mode(engine,
					    SFPTPD_TEST_ID_UTC_OFFSET,
					    (engine->leap_second.type == SFPTPD_LEAP_SECOND_61)? 1: -1,
					    0, 0);

		/* Call cancel to tidy up. This will update the leap second
		 * status of the sync modules. */
		on_cancel_leap_second(engine);
		break;

	case LEAP_SECOND_STATE_IDLE:
	default:
		/* Shouldn't normally get this - ignore it */
		break;
	}
}


static int engine_set_netlink_polling(struct sfptpd_engine *engine, bool poll)
{
	int rc = 0;
	int fd;
	int get_fd_state;

	get_fd_state = 0;
	do {
		fd = sfptpd_netlink_get_fd(engine->netlink_state, &get_fd_state);
		if (fd != -1) {
			if (poll) {
				if ((rc = sfptpd_thread_user_fd_add(fd, true, false)))
					CRITICAL("engine: failed to add netlink socket to thread epoll set, %s\n",
						 strerror(rc));
			} else {
				if ((rc = sfptpd_thread_user_fd_remove(fd)))
					CRITICAL("engine: failed to remove netlink socket from thread epoll set, %s\n",
						 strerror(rc));
			}
		}
	} while (fd != -1 && (rc == 0 || !poll));

	return rc;
}

static void engine_handle_new_link_table(struct sfptpd_engine *engine, int version)
{
	struct sfptpd_clock **clocks_before;
	struct sfptpd_clock **clocks_after;
	size_t num_clocks_before;
	size_t num_clocks_after;

	int rc, rows, i, j;
	bool new_link_table = false;
	bool reconfigure = false;

	assert(engine != NULL);

	clocks_before = sfptpd_clock_get_active_snapshot(&num_clocks_before);

	while (version > 0) {
		TRACE_L3("engine: link changes - new table version %d\n", version);

		new_link_table = true;
		engine->link_table_prev = engine->link_table;
		rows = sfptpd_netlink_get_table(engine->netlink_state, version, &engine->link_table);
		assert(rows == engine->link_table->count);

		if (engine->link_table_prev == NULL) {
			sfptpd_clock_free_active_snapshot(clocks_before);
			return;
		}

		for (i = 0; i < engine->link_table->count; i++) {
			const struct sfptpd_link *link = engine->link_table->rows + i;

			assert(link->event != SFPTPD_LINK_DOWN);

			if (link->event == SFPTPD_LINK_UP ||
			    link->event == SFPTPD_LINK_CHANGE) {
				rc = sfptpd_interface_hotplug_insert(link);

				if (rc == 0) {
					reconfigure = true;
				}
			}
		}

		for (i = 0; engine->link_table_prev &&
		            i < engine->link_table_prev->count; i++) {
			const struct sfptpd_link *link = engine->link_table_prev->rows + i;
			int intf_i = link->if_index;
			for (j = 0; j < engine->link_table->count; j++) {
				int intf_j = engine->link_table->rows[j].if_index;
				if (intf_i == intf_j)
					break;
			}
			if (j == engine->link_table->count) {
				rc = sfptpd_interface_hotplug_remove(link);

				if (rc == 0) {
					reconfigure = true;
				}
			}
		}

		version = sfptpd_netlink_release_table(engine->netlink_state,
						       engine->link_table_prev->version,
						       engine->link_subscribers + 1);
		if (engine->netlink_xoff & NL_XOFF_SPACE) {
			engine->netlink_xoff &= ~NL_XOFF_SPACE;
			NOTICE("engine: resuming netlink polling\n"),
			rc = engine_set_netlink_polling(engine, true);
		}
	}

	if (version < 0)
		ERROR("engine: servicing netlink responses, %s\n", strerror(-version));


	/* Reflect hot-plugged clocks in clock feeds */
	clocks_after = sfptpd_clock_get_active_snapshot(&num_clocks_after);
	for (i = 0, j = 0; i < num_clocks_before || j < num_clocks_after;) {
		struct sfptpd_clock *clock_a = i < num_clocks_before ? clocks_before[i] : NULL;
		struct sfptpd_clock *clock_b = j < num_clocks_after ? clocks_after[j] : NULL;

		if (clock_a && (!clock_b || clock_b > clock_a)) {
			TRACE_L3("engine: clock %s hot unplugged\n",
				 sfptpd_clock_get_short_name(clock_a));
			sfptpd_clockfeed_remove_clock(engine->clockfeed, clock_a);
			i++;
		} else if (clock_b && (!clock_a || clock_a > clock_b)) {
			TRACE_L3("engine: clock %s hot plugged\n",
				 sfptpd_clock_get_short_name(clock_b));
			sfptpd_clockfeed_add_clock(engine->clockfeed, clock_b,
						   engine->general_config->clocks.sync_interval);
			j++;
		} else {
			/* Clock present before and after change */
			i++, j++;
		}
	}
	sfptpd_clock_free_active_snapshot(clocks_after);
	sfptpd_clock_free_active_snapshot(clocks_before);

	if (reconfigure) {
		TRACE_L3("engine: reconfiguring slave servos after interface hotplugging\n");
		reconfigure_servos(engine, &(engine->selected->status));
	}

	if (new_link_table) {
		/* Send new link table to subscribing sync modules */
		for (i = 0; i < engine->link_subscribers; i++) {
			assert(i < SFPTPD_CONFIG_CATEGORY_MAX);
			assert(engine->link_subscriber[i] != NULL);

			sfptpd_sync_module_link_table(engine->link_subscriber[i], engine->link_table);
		}
	}
}


static void engine_on_user_fds(void *context, unsigned int num_fds,
			       struct sfptpd_thread_event fd[])
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)context;
	struct sfptpd_timespec interval;
	int rc;

	assert(engine != NULL);

	if ((engine->netlink_xoff & NL_XOFF_COALESCE) == 0 &&
	    engine->general_config->netlink_coalesce_ms != 0) {

		/* Start the netlink coalesce timer */
		sfptpd_time_from_ns(&interval, 1000000 *
				    ((uint64_t)engine->general_config->netlink_coalesce_ms));

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_NETLINK_COALESCE,
					       false, false, &interval);
		if (rc != 0) {
			ERROR("failed to start netlink coalesce timer, %s\n",
			      strerror(rc));
		} else {
			TRACE_L5("engine: netlink coalesce timer started\n");
			engine->netlink_xoff |= NL_XOFF_COALESCE;
		}
	} else if (num_fds == 0) {
		/* This is how coalesce timer expiry is indicated. */

		TRACE_L5("engine: netlink coalesce timer expired\n");
		engine->netlink_xoff &= ~NL_XOFF_COALESCE;
	}

	rc = sfptpd_netlink_service_fds(engine->netlink_state,
					engine->link_subscribers + 1,
					engine->netlink_xoff != 0);
	if (rc > 0) {
		engine_handle_new_link_table(engine, rc);
	} else if (rc == -EAGAIN) {
		NOTICE("engine: suspending netlink polling until table freed\n");
		engine->netlink_xoff |= NL_XOFF_SPACE;
		rc = engine_set_netlink_polling(engine, false);
	} else if (rc < 0) {
		ERROR("engine: servicing netlink fds, %s\n", strerror(rc));
	}
}


static void on_netlink_rescan_timer(void *user_context, unsigned int timer_id)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	int rc;

	rc = sfptpd_netlink_scan(engine->netlink_state);
	if (rc != 0) {
		ERROR("engine: netlink rescan, %s\n",
		      strerror(rc));
	}
}


static void on_netlink_coalesce_timer(void *user_context, unsigned int timer_id)
{
	engine_on_user_fds(user_context, 0, NULL);
}


static void on_synchronize(void *user_context)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	struct sfptpd_timespec time;
	int i;

	assert(engine != NULL);

	/* If a leap second is due then we suspend synchronization for
	 * a small number of seconds before and after to allow the various
	 * clocks to step and update.
	 */
	if ((engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_PRE) ||
	    (engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_POST)) {
		TRACE_L6("synchronization suspended during leap second\n");
		return;
	}

	/* Get the current monotonic time for the servo algorithm */
	if (sfclock_gettime(CLOCK_MONOTONIC, &time) < 0) {
		ERROR("failed to get monotonic time, %s\n", strerror(errno));
	} else {
		/* Prepare the servos */
		for (i = 0; i < engine->active_servos; i++)
			sfptpd_servo_prepare(engine->servos[i]);

		/* Run the slave servos */
		for (i = 0; i < engine->active_servos; i++) {
			sfptpd_sync_module_alarms_t prev_alarms;
			sfptpd_sync_module_alarms_t alarms;
			const char *servo_name;

			(void)sfptpd_servo_synchronize(engine, engine->servos[i], &time);

			prev_alarms = engine->servo_prev_alarms[i];
			alarms = sfptpd_servo_get_alarms(engine->servos[i],
							 &servo_name);
			if (prev_alarms != alarms) {
				char old_alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];
				char new_alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];

				sfptpd_sync_module_alarms_text(prev_alarms,
		                                               old_alarms, sizeof old_alarms);
		                sfptpd_sync_module_alarms_text(alarms,
		                                               new_alarms, sizeof new_alarms);

		                NOTICE("%s: alarms changed: %s -> %s\n",
		                       servo_name, old_alarms, new_alarms);

				engine->servo_prev_alarms[i] = alarms;
			}
		}
	}
}

void sfptpd_engine_post_rt_stats_simple(struct sfptpd_engine *engine, struct sfptpd_servo *servo){
	struct sfptpd_log_time logtime;
	sfptpd_log_get_time(&logtime);

	struct sfptpd_servo_stats stats = sfptpd_servo_get_stats(servo);

	sfptpd_engine_post_rt_stats(engine,
					&logtime,
					stats.servo_name,
					"servo", stats.clock_master, stats.clock_slave,
					stats.disciplining, stats.blocked, stats.in_sync, stats.alarms,
					STATS_KEY_FREQ_ADJ, stats.freq_adj,
					STATS_KEY_P_TERM, stats.p_term,
					STATS_KEY_I_TERM, stats.i_term,
					STATS_KEY_OFFSET, stats.offset,
					STATS_KEY_M_TIME, stats.time_master,
					STATS_KEY_S_TIME, stats.time_slave,
					STATS_KEY_END);
}

static void on_log_stats(void *user_context, unsigned int timer_id)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	struct sfptpd_log_time time;
	struct sync_instance_record *instance;
	int i;

	assert(engine != NULL);

	/* If a leap second is due then we suspend synchronization for
	 * a small number of seconds before and after to allow the various
	 * clocks to step and update.
	 */
	if ((engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_PRE) ||
	    (engine->leap_second.state == LEAP_SECOND_STATE_ACTIVE_POST)) {
		TRACE_L6("stats logging suspended during leap second\n");
		return;
	}

	/* We pass the current time into the logging functions
	 * so that each batch of stats has exactly the same time.
	 * This allow the output to be processed more easily */
	sfptpd_log_get_time(&time);

	/* Iterate through all the instances and print stats if available */
	for (i = 0; i < engine->num_sync_instances; i++) {
		instance = &engine->sync_instances[i];

		/* Print any stats present */
		if (instance->latest_rt_stats.instance_name != NULL) {
			write_rt_stats_log(&time, &instance->latest_rt_stats);

			/* If the sync instance is no longer measuring itself
			 * against remote reference then erase saved stats. */
			if (instance->status.state != SYNC_MODULE_STATE_SLAVE &&
			    instance->status.state != SYNC_MODULE_STATE_PASSIVE)
				instance->latest_rt_stats.instance_name = NULL;
		}
	}

	/* For each of the servos, dump stats */
	for (i = 0; i < engine->active_servos; i++) {
		sfptpd_engine_post_rt_stats_simple(engine, engine->servos[i]);

		/* Update NIC clock with the current sync status */
		sfptpd_servo_update_sync_status(engine->servos[i]);
	}

	write_topology(engine);
	write_sync_instances(engine);
	sfptpd_log_rt_stats_written(0, true);
}


static void on_save_state(void *user_context, unsigned int timer_id)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	assert(engine != NULL);
	write_state(engine);
}


static void on_stats_period_end(void *user_context, unsigned int timer_id)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	struct sfptpd_timespec time;
	enum sfptpd_config_category type;
	int i;

	assert(engine != NULL);

	/* Get the current monotonic time for the servo algorithm */
	if (sfclock_gettime(CLOCK_REALTIME, &time) < 0) {
		ERROR("failed to get monotonic time, %s\n", strerror(errno));
	} else {
		/* Request stats collection from each sync module */
		for (type = 0; type < SFPTPD_CONFIG_CATEGORY_MAX; type++) {
			struct sfptpd_thread *module = engine->sync_modules[type];
			if (module != NULL) {
				sfptpd_sync_module_stats_end_period(module,
								    &time);
			}
		}

		/* For each of the servos, update the stats history */
		for (i = 0; i < engine->active_servos; i++) {
			sfptpd_servo_stats_end_period(engine->servos[i], &time);
		}

		/* Write out clock feed stats */
		if (engine->clockfeed != NULL) {
			sfptpd_clockfeed_stats_end_period(engine->clockfeed,
							  &time);
		}
	}
}


static void on_selection_holdoff_timer(void *user_context, unsigned int timer_id)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)user_context;
	assert(engine != NULL);

	if (engine->candidate != NULL) {
		select_sync_instance(engine, engine->candidate);
		engine->candidate = NULL;
	}
}


static void on_thread_exit(struct sfptpd_engine *engine,
			   sfptpd_msg_thread_exit_notify_t *msg)
{
	CRITICAL("fatal error from sync module %p, %s\n",
		 msg->thread,
		 strerror(msg->exit_code));
	sfptpd_thread_exit(msg->exit_code);
}


static void on_step_clocks(struct sfptpd_engine *engine)
{
	struct sync_instance_record *sync_instance;
	struct sfptpd_sync_instance *handle;
	struct sfptpd_sync_instance_status status;
	struct sfptpd_timespec servo_offset;
	struct sfptpd_timespec zero = sfptpd_time_null();
	int rc, i;

	assert(engine != NULL);

	if (engine->general_config->clocks.control == SFPTPD_CLOCK_CTRL_NO_ADJUST) {
		NOTICE("step clocks signal blocked by \"clock-control no-adjust\"\n");
		return;
	}

	sync_instance = engine->selected;
	if (sync_instance == NULL) {
		WARNING("cannot step clocks with no selected sync instance\n");
		return;
	}

	handle = sync_instance->info.handle;

	/* Get the current offset from the sync module (if it is a freerun
	 * module or in a master mode or unknown this will return 0. Step
	 * the sync module by this amount and then each of the slave servos
	 * by the combined sync module offset and slave servo offset */
	rc = sfptpd_sync_module_get_status(sync_instance->info.module, handle, &status);
	if (rc == 0) {
		sfptpd_sync_module_step_clock(sync_instance->info.module,
					      handle,
					      &status.offset_from_master);

		/* For each of the servos, get the servo offset and add it to
		 * the sync module offset. Then step the servo by this amount */
		for (i = 0; i < engine->active_servos; i++) {
			struct sfptpd_servo_stats stats = sfptpd_servo_get_stats(engine->servos[i]);
			if (SYNC_MODULE_ALARM_TEST(stats.alarms, CLOCK_NEAR_EPOCH)) {
				WARNING("%s slave clock %s not stepped because master clock %s is near epoch.\n",
					stats.servo_name, sfptpd_clock_get_long_name(stats.clock_slave),
					sfptpd_clock_get_long_name(stats.clock_master));
				continue;
			}

			sfptpd_servo_get_offset_from_master(engine->servos[i],
							    &servo_offset);

			sfptpd_time_add(&servo_offset, &servo_offset,
					&status.offset_from_master);

			(void)sfptpd_servo_step_clock(engine->servos[i], &servo_offset);
		}

		/* We also need to tell NTP sync modules that the clock has
		 * been stepped so that they ignore the NTP offset until the
		 * next reading from the NTP daemon. Note that the actual
		 * offset is not relevent in this case as the NTP module is not
		 * in charge of disciplining the clock */
		for (i = 0; i < SFPTPD_CONFIG_CATEGORY_MAX; i++) {
			if ((1 << i) & SFPTPD_SYNC_MODULE_IS_NTP)
				sfptpd_sync_module_step_clock(engine->sync_modules[i],
							      NULL, &zero);
		}
	}
}


static void on_sync_instance_state_changed(struct sfptpd_engine *engine,
					   struct sfptpd_thread *module,
					   struct sfptpd_sync_instance *instance,
					   sfptpd_sync_instance_status_t *status)
{
	struct sync_instance_record *instance_record;
	struct sync_instance_record *new_candidate;
	struct sfptpd_timespec interval;
	int rc;
	bool state_written = false;

	assert(engine != NULL);
	assert(module != NULL);
	assert(instance != NULL);
	assert(status != NULL);

	instance_record = get_sync_instance_record_by_handle(engine, instance);
	assert(instance_record != NULL);

	/* Is this for the currently selected instance. If so then we potentially
	 * need to update the GM info and reconfigure the servos. */
	if (instance_record == engine->selected) {
		/* If the clock characteristics have changed, ensure that all
		 * master instances know about it. */
		if (!sfptpd_sync_module_gm_info_equal(&instance_record->status.master,
						      &status->master)) {
			propagate_grandmaster_info(engine, &instance_record->info,
						   &status->master);
		}

		/* If the clock has changed, we need to reconfigure the servos.
		 * We also update the state of all instances and the topology. */
		if (status->clock != engine->lrc) {
			TRACE_L3("local reference clock changed- reconfiguring slave servos\n");
			reconfigure_servos(engine, status);
			write_state(engine);
			write_topology(engine);
			write_sync_instances(engine);
			state_written = true;
		}
	}

	/* If we haven't already written the state and the state of this sync
	 * instance has changed, save the new state. */
	if (!state_written &&
	    ((instance_record->status.state != status->state) ||
	     (instance_record->status.alarms != status->alarms))) {
		sfptpd_sync_module_save_state(module);
	}

	if (instance_record->status.alarms != status->alarms) {
		char old_alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];
		char new_alarms[SYNC_MODULE_ALARM_ALL_TEXT_MAX];

		sfptpd_sync_module_alarms_text(instance_record->status.alarms,
					       old_alarms, sizeof old_alarms);
		sfptpd_sync_module_alarms_text(status->alarms,
					       new_alarms, sizeof new_alarms);

		NOTICE("%s: alarms changed: %s -> %s\n",
		       instance_record->info.name, old_alarms, new_alarms);
	}

	/* Update the status of this sync instance and then re-evaluate the best
	 * instance */
	instance_record->status = *status;
	new_candidate = sfptpd_bic_choose(&engine->general_config->selection_policy,
					  engine->sync_instances,
					  engine->num_sync_instances,
					  engine->candidate == NULL ? engine->selected : engine->candidate);
	assert (NULL != new_candidate);

	/* If we have no current candidate and proposed candidate is the
	 * currently selected instance or the proposed candidate is the current
	 * candidate then no further action is necessary. */
	if ((new_candidate == engine->selected) && (engine->candidate == NULL))
		return;
	if (new_candidate == engine->candidate)
		return;

	/* If the selection holdoff is disabled, select the new instance
	 * immediately and return. */
	if (engine->general_config->selection_holdoff_interval == 0) {
		(void)select_sync_instance(engine, new_candidate);
		return;
	}

	/* If we don't currently have a candidate or the proposed candidate is
	 * the currently selected instance then start/restart the selection
	 * holdoff timer. */
	if ((engine->candidate == NULL) || (engine->selected == new_candidate)) {
		/* Stop and restart the selection holdoff timer */
		sfptpd_time_from_s(&interval, engine->general_config->selection_holdoff_interval);

		rc = sfptpd_thread_timer_stop(ENGINE_TIMER_SELECTION_HOLDOFF);
		if (rc != 0) {
			CRITICAL("failed to stop selection holdoff timer, %s\n",
				 strerror(rc));
		}

		rc = sfptpd_thread_timer_start(ENGINE_TIMER_SELECTION_HOLDOFF,
					       false, false, &interval);
		if (rc != 0) {
			CRITICAL("failed to start selection holdoff timer, %s\n",
				 strerror(rc));
		}
	}

	/* Save the new candidate */
	engine->candidate = new_candidate;

	rc = sfptpd_thread_timer_get_time_left(ENGINE_TIMER_SELECTION_HOLDOFF, &interval);
	if (rc != 0) {
		CRITICAL("failed to read remaining time for selection holdoff timer, %s\n",
			 strerror(rc));
	}

	/* Round seconds up as usually we will get a result of the x.99 form. */
	if (interval.nsec > ONE_BILLION/2) {
		interval.sec++;
		interval.nsec = 0;
	}

	if (new_candidate == engine->selected) {
		INFO("canceled switch away from sync instance %s as its rank recovered\n",
		     new_candidate->info.name, interval.sec, engine->selected->info.name);
	} else {
		INFO("will switch to sync instance %s in %d seconds if %s does not recover\n",
		     new_candidate->info.name, interval.sec, engine->selected->info.name);
	}
}


static void on_select_instance(struct sfptpd_engine *engine,
			       const char *name) {

	if (engine->general_config->selection_policy.strategy == SFPTPD_SELECTION_STRATEGY_MANUAL) {
		struct sync_instance_record *selected_instance;
		assert(engine != NULL);
		assert(name != NULL);

		selected_instance = get_sync_instance_record_by_name(engine, name);
		if (NULL == selected_instance) {
			WARNING("Sync instance '%s' not found - can't select\n", name);
			return;
		}
		sfptpd_bic_select_instance(engine->sync_instances,
					   engine->num_sync_instances,
					   selected_instance);

		/* Select the instance */
		(void)select_sync_instance(engine, selected_instance);
	} else {
		WARNING("Sync policy is automatic, not manual, selection of '%s' ignored\n", name);
	}
}


static void on_configure_test_mode(struct sfptpd_engine *engine,
				   engine_msg_t *msg)
{
	assert(engine != NULL);
	assert(msg != NULL);

	/* We only allow test functions to be used if test-mode is enabled
	 * in the configuration file */
	if (!engine->general_config->test_mode)
		return;

	switch (msg->u.configure_test_mode.mode) {
	case SFPTPD_TEST_ID_LEAP_SECOND_CANCEL:
		on_test_leap_second(engine, SFPTPD_LEAP_SECOND_NONE);
		break;

	case SFPTPD_TEST_ID_LEAP_SECOND_61:
		on_test_leap_second(engine, SFPTPD_LEAP_SECOND_61);
		break;

	case SFPTPD_TEST_ID_LEAP_SECOND_59:
		on_test_leap_second(engine, SFPTPD_LEAP_SECOND_59);
		break;

	case SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_CANCEL:
		sfptpd_engine_schedule_leap_second(engine, SFPTPD_LEAP_SECOND_NONE, 12.0);
		break;

	case SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_61:
		sfptpd_engine_schedule_leap_second(engine, SFPTPD_LEAP_SECOND_61, 12.0);
		break;

	case SFPTPD_TEST_ID_LOCAL_LEAP_SECOND_59:
		sfptpd_engine_schedule_leap_second(engine, SFPTPD_LEAP_SECOND_59, 12.0);
		break;

	case SFPTPD_TEST_ID_ADJUST_FREQUENCY:
		on_test_adjust_frequency(engine, msg->u.configure_test_mode.params[0]);
		break;

	default:
		/* All other test modes propagated to all sync instances */
		set_sync_instance_test_mode(engine,
					    msg->u.configure_test_mode.mode,
					    msg->u.configure_test_mode.params[0],
					    msg->u.configure_test_mode.params[1],
					    msg->u.configure_test_mode.params[2]);
	}
}


static void on_rt_stats_entry(struct sfptpd_engine *engine,
			      struct rt_stats_msg *msg)
{
	assert(engine != NULL);
	assert(engine->general_config != NULL);
	assert(msg != NULL);

	/* Store latest stats */
	struct sync_instance_record *record;
	record = get_sync_instance_record_by_name(engine, msg->stats.instance_name);
	if (record != NULL)
		record->latest_rt_stats = msg->stats;
	else /* This will happen for servos */
		write_rt_stats_log(&msg->stats.time, &msg->stats);

	/* Write to json_stats */
	FILE* stream = sfptpd_log_get_rt_stats_out_stream();
	if (stream != NULL)
		write_rt_stats_json(stream, &msg->stats);
}


static void on_log_rotate(struct sfptpd_engine *engine)
{
	assert(engine != NULL);

	sfptpd_log_rotate(engine->config);
}


static void on_clustering_input(struct sfptpd_engine *engine,
				engine_msg_t *msg)
{
	assert(engine != NULL);
	assert(engine->general_config != NULL);
	assert(msg != NULL);

	/* Store latest stats */
	struct sync_instance_record *record;
	record = get_sync_instance_record_by_name(engine, msg->u.clustering_input.instance_name);
	if (record != NULL)
		record->latest_clustering_input = msg->u.clustering_input;
}


static void on_link_table_release(struct sfptpd_engine *engine,
				  engine_msg_t *msg)
{
	int rc;

	assert(engine != NULL);
	assert(engine->general_config != NULL);
	assert(msg != NULL);

	rc = sfptpd_netlink_release_table(engine->netlink_state,
					  msg->u.link_table_release.link_table->version,
					  engine->link_subscribers + 1);

	if (rc > 0)
		engine_handle_new_link_table(engine, rc);
	else if (rc < 0)
		ERROR("engine: releasing link table, %s\n", strerror(-rc));

	if (engine->netlink_xoff & NL_XOFF_SPACE) {
		engine->netlink_xoff &= ~NL_XOFF_SPACE;
		NOTICE("engine: resuming netlink polling\n");
		rc = engine_set_netlink_polling(engine, true);
	}
}


static void on_servo_pid_adjust(struct sfptpd_engine *engine,
				sfptpd_servo_msg_t *msg)
{
	int i;

	assert(engine != NULL);
	assert(msg != NULL);

	if (!(msg->u.pid_adjust.servo_type_mask & SFPTPD_SERVO_TYPE_LOCAL))
		return;

	for (i = 0; i < engine->active_servos; i++) {
		sfptpd_servo_pid_adjust(engine->servos[i],
					msg->u.pid_adjust.kp,
					msg->u.pid_adjust.ki,
					msg->u.pid_adjust.kd,
					msg->u.pid_adjust.reset);
	}
}


static void engine_on_shutdown(void *context)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)context;
	struct sfptpd_clock **clocks;
	size_t num_clocks;
	int module;

	assert(engine != NULL);

	sfptpd_multicast_unsubscribe(SFPTPD_SERVO_MSG_PID_ADJUST);
	sfptpd_multicast_unsubscribe(SFPTPD_CLOCKFEED_MSG_SYNC_EVENT);

	/* Remove clock feeds */
	clocks = sfptpd_clock_get_active_snapshot(&num_clocks);
	while (num_clocks--)
		sfptpd_clockfeed_remove_clock(engine->clockfeed, clocks[num_clocks]);
	sfptpd_clock_free_active_snapshot(clocks);

	/* Now free up the resources */
	for (module = 0; module < SFPTPD_CONFIG_CATEGORY_MAX; module++) {
		if (engine->sync_modules[module] != NULL) {
			sfptpd_sync_module_destroy(engine->sync_modules[module]);
			engine->sync_modules[module] = NULL;
		}
	}
	if (engine->sync_instances != NULL) {
		free(engine->sync_instances);
		engine->sync_instances = NULL;
		engine->num_sync_instances = 0;
	}
	destroy_servos(engine);

	if (engine->clockfeed_thread != NULL) {
		sfptpd_thread_destroy(engine->clockfeed_thread);
		engine->clockfeed_thread = NULL;
	}

	/* Ownership of netlink state reverts to main */
}

/* Create a sync module. This is a helper function for engine_on_startup(),
   called once per available sync module.
   @param engine The engine
   @param config The parent configuration object
   @param type The config category number corresponding to the sync module
   @param next_instance_index a pointer into the engine's sync instance
   record array
   @returns Zero on success, otherwise an error code */
static int create_sync_module(struct sfptpd_engine *engine,
			      struct sfptpd_config *config,
			      enum sfptpd_config_category type,
			      int next_instance_index) {

	struct sfptpd_sync_instance_info *infos = NULL;
	int instances = sfptpd_config_category_count_instances(config, type);
	int i;
	int rc;
	bool link_subscriber = false;

	infos = NULL;
	if (instances > 0) {
		infos = (struct sfptpd_sync_instance_info *) calloc(instances, sizeof *infos);
	}

	TRACE_L3("sync module %d, instances %d\n", type, instances);
	rc = sfptpd_sync_module_create(type, config, engine,
				       engine->sync_modules + type,
				       infos, instances,
				       engine->link_table,
				       &link_subscriber);
	if (rc == ENOENT) {
		/* Not a sync module; nothing to do. */
		rc = 0;
		goto fail;
	}

	if (rc != 0) {
		if (rc != EREPORTED)
			CRITICAL("failed to create sync module %s, %s\n",
				 sfptpd_sync_module_name(type), strerror(rc));
		rc = EREPORTED;
		goto fail;
	}

	TRACE_L3("created sync module %s\n", sfptpd_sync_module_name(type));

	if (link_subscriber) {
		engine->link_subscriber[engine->link_subscribers++] = engine->sync_modules[type];
		TRACE_L3("subscribed sync module %s to link table updates\n",
			 sfptpd_sync_module_name(type));
	}

	/* Iterate through instances */
	for (i = 0; i < instances; i++) {
		struct sync_instance_record *record =
			&engine->sync_instances[next_instance_index + i];

		/* Copy information from the module */
		record->info = infos[i];

		/* Initialise information for the engine */
		record->status.state = SYNC_MODULE_STATE_MAX;
		record->status.alarms = 0;

		TRACE_L3("created sync instance %s\n", record->info.name);
	}

fail:
	free(infos);
	return rc;
}


static int engine_on_startup(void *context)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)context;
	struct sfptpd_config *config;
	int rc;
	enum sfptpd_config_category type;
	int all_instances;
	int i;
	struct sync_instance_record *bic_instance;

	assert(engine != NULL);
	assert(engine->config != NULL);

	config = engine->config;

	engine->clockfeed = sfptpd_clockfeed_create(&engine->clockfeed_thread,
						    engine->general_config->clocks.sync_interval);
	if (engine->clockfeed == NULL) {
		rc = errno;
		CRITICAL("could not start clock feed, %s\n", strerror(rc));
		goto fail;
	}

	/* Register clocks with clock feed */
	{
		struct sfptpd_clock **active;
		struct sfptpd_clock *clock;
		size_t num_active;
		int idx;

		active = sfptpd_clock_get_active_snapshot(&num_active);
		for (idx = 0; idx < num_active; idx++) {
		     clock = active[idx];
			if (clock != sfptpd_clock_get_system_clock())
				sfptpd_clockfeed_add_clock(engine->clockfeed, clock,
							   engine->general_config->clocks.sync_interval);
		}
		sfptpd_clock_free_active_snapshot(active);
	}

	/* Count potential sync instances, create storage for them and
	   initialise shadow state */
	all_instances = 0;
	for (type = 0; type < SFPTPD_CONFIG_CATEGORY_MAX; type++) {
		all_instances += sfptpd_config_category_count_instances(config, type);
	}
	engine->sync_instances = (struct sync_instance_record *)calloc(all_instances, sizeof *engine->sync_instances);
	if (engine->sync_instances == NULL) {
		rc = ENOMEM;
		CRITICAL("failed to allocate array for sync instances\n");
		goto fail;
	}

	/* Create all the sync module types */
	all_instances = 0;
	for (type = 0; type < SFPTPD_CONFIG_CATEGORY_MAX; type++) {
		int instances = sfptpd_config_category_count_instances(config, type);
		/* If there are instances of this sync module or this is the NTP
		 * sync module type then create the sync module. */
		if ((instances != 0) || (type == SFPTPD_CONFIG_CATEGORY_NTP)) {
			rc = create_sync_module(engine, config, type, all_instances);
			if (rc != 0)
				goto fail;
		}
		all_instances += instances;
	}

	/* Now we have all the selectable sync instances */
	engine->num_sync_instances = all_instances;
	if (all_instances == 0) {
		CRITICAL("no sync instances created\n");
		rc = ENOENT;
		goto fail;
	}

	rc = create_servos(engine, config);
	if (rc != 0) {
		CRITICAL("failed to create clock servos, %s\n", strerror(rc));
		goto fail;
	}

	/* 4 messages per producer of stats */
	int rt_stats_msg_pool_size = (all_instances + engine->total_servos) << 2;
	rc = sfptpd_thread_alloc_msg_pool(SFPTPD_MSG_POOL_RT_STATS,
					  rt_stats_msg_pool_size, sizeof(struct rt_stats_msg));
	if (rc != 0) {
		CRITICAL("failed to create realtime stats message pool, %s\n",
			 strerror(rc));
		goto fail;
	}

	rc = sfptpd_multicast_subscribe(SFPTPD_SERVO_MSG_PID_ADJUST);
	if (rc != 0) {
		CRITICAL("failed to subscribe to servo message multicasts, %s\n",
			 strerror(rc));
		goto fail;
	}

	/* Get the status from all the sync instances */
	for (i = 0; i < all_instances; i++) {
		struct sync_instance_record *sync_instance = engine->sync_instances + i;
		sfptpd_sync_instance_status_t sync_instance_status = { 0 };

		if (sync_instance->info.handle == NULL)
			goto fail; /* Happens if startup failed, e.g. due to bug74320. */

		/* Set initial clustering scores to configured default */
		sync_instance_status.clustering_score =
			engine->general_config->clustering_score_without_discriminator;

		rc = sfptpd_sync_module_get_status(sync_instance->info.module,
						   sync_instance->info.handle,
						   &sync_instance_status);
		if (rc != 0) {
			CRITICAL("failed to get status from sync module, %s\n", strerror(rc));
			goto fail;
		}
		sync_instance->status = sync_instance_status;

	}

	char* d_name = engine->general_config->clustering_discriminator_name;
	if (d_name[0]) {
		engine->clustering_discriminator = get_sync_instance_record_by_name(engine, d_name);
		if (engine->clustering_discriminator == NULL) {
			CRITICAL("Error: could not find discriminator %s\n",
				 engine->general_config->clustering_discriminator_name);
		} else {
			rc = sfptpd_sync_module_control(engine->clustering_discriminator->info.module,
							engine->clustering_discriminator->info.handle,
							SYNC_MODULE_CLUSTERING_DETERMINANT,
							SYNC_MODULE_CLUSTERING_DETERMINANT);

			/* Ensure discriminator instance defaults to a good clustering
			   score to avoid pathological outcomes. */
			engine->clustering_discriminator->status.clustering_score = 1;
		}
	}

	/* Find out which is the best instance, but don't select it just yet.
	 * Must do this after gathering initial status since BIC requires a valid
	 * status.
	 */
	bic_instance = sfptpd_bic_choose(&engine->general_config->selection_policy,
					 engine->sync_instances, engine->num_sync_instances, NULL);
	assert (NULL != bic_instance);

	if (engine->general_config->selection_policy.strategy == SFPTPD_SELECTION_STRATEGY_AUTOMATIC) {
		/* In automatic mode, we just select the BIC and we're done */
		rc = select_sync_instance(engine, bic_instance);
		if (rc != 0) {
			CRITICAL("failed to select initial sync instance, %s\n", strerror(rc));
			goto fail;
		}
	} else {
		/* Manual and manual-startup modes have a user-provided instance. */
		struct sync_instance_record *initial_instance;
		initial_instance = get_sync_instance_record_by_name(engine, engine->general_config->initial_sync_instance);
		if (NULL == initial_instance) {
			CRITICAL("Can't find initial sync instance '%s'\n", 
				 engine->general_config->initial_sync_instance);
			rc = ENOENT;
			goto fail;
		}
		NOTICE("Selecting initial sync instance '%s'\n",
			engine->general_config->initial_sync_instance);

		/* In manual mode, we want to tell the BIC that the instance is manually selected */
		if (engine->general_config->selection_policy.strategy == SFPTPD_SELECTION_STRATEGY_MANUAL) {
			sfptpd_bic_select_instance(engine->sync_instances, engine->num_sync_instances, initial_instance);
		}

		/* In both manual modes, we initially select the user-supplied instance */
		rc = select_sync_instance(engine, initial_instance);
		if (rc != 0) {
			CRITICAL("failed to select initial sync instance, %s\n", strerror(rc));
			goto fail;
		}

		/* In manual-startup mode, we revert to automatic mode after the holdoff timer expires */
		if (bic_instance != initial_instance &&
		    engine->general_config->selection_policy.strategy == SFPTPD_SELECTION_STRATEGY_MANUAL_STARTUP) {
			engine->candidate = bic_instance;
			INFO("sync instance %s is a candidate for selection\n",
			     bic_instance->info.name);
		}
	}

	/* Write the interfaces file */
	write_interfaces();

	return 0;

fail:
	engine_on_shutdown(context);
	return rc;
}


static void on_run(struct sfptpd_engine *engine)
{
	enum sfptpd_config_category type;
	int rc;

	assert(engine != NULL);

	/* Register for clock feed events */
	rc = sfptpd_multicast_subscribe(SFPTPD_CLOCKFEED_MSG_SYNC_EVENT);
	if (rc != 0) {
		CRITICAL("failed to subscribe to clock feed sync events, %s\n",
			 strerror(rc));
		goto fail;
	}

	/* Create the timers */
	rc = create_timers(engine);
	if (rc != 0) {
		CRITICAL("failed to create sync engine timers, %s\n", strerror(rc));
		goto fail;
	}

	rc = engine_set_netlink_polling(engine, true);
	if (rc != 0) {
		CRITICAL("could not start netlink polling\n");
		goto fail;
	}

	/* Propagate GO! signal to the sync modules */
	for (type = 0; type < SFPTPD_CONFIG_CATEGORY_MAX; type++) {
		int instances = sfptpd_config_category_count_instances(engine->config, type);
		if ((instances != 0) || (type == SFPTPD_CONFIG_CATEGORY_NTP)) {
			TRACE_L3("engine: sending RUN message to sync module type %d\n",
				 type);
			sfptpd_app_run(engine->sync_modules[type]);
		}
	}

fail:
	if (rc != 0)
		sfptpd_thread_exit(rc);
}


static void engine_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	struct sfptpd_engine *engine = (struct sfptpd_engine *)context;
	engine_msg_t *msg = (engine_msg_t *)hdr;

	assert(engine != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_MSG_ID_THREAD_EXIT_NOTIFY:
		on_thread_exit(engine, (sfptpd_msg_thread_exit_notify_t *)hdr);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_APP_MSG_RUN:
		on_run(engine);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_STEP_CLOCKS:
		on_step_clocks(engine);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_SYNC_INSTANCE_STATE_CHANGED:
		on_sync_instance_state_changed(engine,
					       msg->u.sync_instance_state_changed.sync_module,
					       msg->u.sync_instance_state_changed.sync_instance,
					       &msg->u.sync_instance_state_changed.status);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_SCHEDULE_LEAP_SECOND:
		on_schedule_leap_second(engine,
					msg->u.schedule_leap_second.type,
					msg->u.schedule_leap_second.guard_interval);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_SELECT_INSTANCE:
		on_select_instance(engine, 
				   msg->u.select_instance.name);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_CANCEL_LEAP_SECOND:
		on_cancel_leap_second(engine);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_CONFIGURE_TEST_MODE:
		on_configure_test_mode(engine, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_RT_STATS_ENTRY:
		on_rt_stats_entry(engine, (struct rt_stats_msg*)hdr);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_LOG_ROTATE:
		on_log_rotate(engine);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_CLUSTERING_INPUT:
		on_clustering_input(engine, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case ENGINE_MSG_LINK_TABLE_RELEASE:
		on_link_table_release(engine, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SERVO_MSG_PID_ADJUST:
		on_servo_pid_adjust(engine, (sfptpd_servo_msg_t *) msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_CLOCKFEED_MSG_SYNC_EVENT:
		on_synchronize(engine);
		SFPTPD_MSG_FREE(msg);
		break;

	default:
		WARNING("engine: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
	}
}


static const struct sfptpd_thread_ops engine_thread_ops =
{
	engine_on_startup,
	engine_on_shutdown,
	engine_on_message,
	engine_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_engine_create(struct sfptpd_config *config,
			 struct sfptpd_engine **engine,
			 struct sfptpd_nl_state *netlink,
			 const struct sfptpd_link_table *initial_link_table)
{
	struct sfptpd_engine *new;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);

	new = (struct sfptpd_engine *)calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("failed to allocate memory for sync engine\n");
		*engine = NULL;
		return ENOMEM;
	}

	/* Store pointers to the config for use by the engine thread */
	new->config = config;
	new->general_config = sfptpd_general_config_get(config);
	new->netlink_state = netlink;
	new->link_table = initial_link_table;

	rc = sfptpd_thread_create("engine", &engine_thread_ops, new, &new->thread);
	if (rc != 0) {
		if (rc != EREPORTED)
			CRITICAL("couldn't create sync engine thread, %s\n", strerror(rc));
		rc = EREPORTED;
		goto fail1;
	}
	TRACE_L2("sync engine created successfully\n");

	TRACE_L3("main: sending RUN message to engine\n");
	sfptpd_app_run(new->thread);

	*engine = new;
	return 0;

fail1:
	free(new);
	*engine = NULL;
	return rc;
}


void sfptpd_engine_destroy(struct sfptpd_engine *engine)
{
	int rc;

	if (engine != NULL) {
		rc = sfptpd_thread_destroy(engine->thread);

		/* Finally free the memory. Note that this is done after
		 * deleting the thread as the reference to the thread is inside
		 * the engine data. */
		if (rc == 0)
			free(engine);
	}
}


struct sfptpd_thread *sfptpd_engine_get_ntp_module(struct sfptpd_engine *engine)
{
	struct sfptpd_prog chrony[] = {
		{ "chronyd" },
		{ NULL }
	};

	assert(engine != NULL);

	return engine->sync_modules[sfptpd_find_running_programs(chrony)
		? SFPTPD_CONFIG_CATEGORY_CRNY
		: SFPTPD_CONFIG_CATEGORY_NTP];
}


void sfptpd_engine_step_clocks(struct sfptpd_engine *engine)
{
	engine_msg_t *msg;

	assert(engine != NULL);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_STEP_CLOCKS, false);
}

void sfptpd_engine_select_instance(struct sfptpd_engine *engine, const char *new_instance)
{
	engine_msg_t *msg;

	assert(engine != NULL);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}
	strncpy(msg->u.select_instance.name, new_instance, sizeof (msg->u.select_instance.name) - 1);

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_SELECT_INSTANCE, false);
}

void sfptpd_engine_sync_instance_state_changed(struct sfptpd_engine *engine,
					       struct sfptpd_thread *sync_module,
					       struct sfptpd_sync_instance *sync_instance,
					       struct sfptpd_sync_instance_status *status)
{
	engine_msg_t *msg;

	assert(engine != NULL);
	assert(sync_module != NULL);
	assert(status != NULL);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.sync_instance_state_changed.sync_module = sync_module;
	msg->u.sync_instance_state_changed.sync_instance = sync_instance;
	msg->u.sync_instance_state_changed.status = *status;
	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_SYNC_INSTANCE_STATE_CHANGED, false);
}

void sfptpd_engine_link_table_release(struct sfptpd_engine *engine, const struct sfptpd_link_table *link_table)
{
	engine_msg_t *msg;

	assert(engine != NULL);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.link_table_release.link_table = link_table;

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_LINK_TABLE_RELEASE, false);
}

static bool offset_valid(const struct sfptpd_timespec *offset_from_master)
{
	return !(sfptpd_time_is_zero(offset_from_master));
}

/*
	For discriminator option:
		Return clustering_score_without_discriminator if discriminator 
                	OR sync instance offset is invalid.
		Return 1 if both discriminator AND sync instance have time AND
			sync instance is within threshold of discriminator.
		Return 1 if the candidate is also the discriminator reference.
		Return 0 if sync instance is outside discriminator threshold. 
		Return 0 if discriminator mode clustering is not used.
*/
int sfptpd_engine_calculate_clustering_score(struct sfptpd_clustering_evaluator *evaluator,
					       sfptpd_time_t offset_from_master,
					       struct sfptpd_clock *instance_clock)
{	
	struct sfptpd_engine *engine;
	struct sfptpd_clustering_input *discriminator;
	int default_score;
	struct sfptpd_timespec instance_ofm;
	struct sfptpd_timespec discriminator_ofm;

	assert(evaluator);
	assert(evaluator->private);

	engine = evaluator->private;

	if (engine->general_config->clustering_mode != SFPTPD_CLUSTERING_MODE_DISCRIMINATOR ||
	    engine->clustering_discriminator == NULL)
		return 0;

	if (strcmp(evaluator->instance_name, engine->clustering_discriminator->info.name) == 0)
		return 1;

	default_score = engine->general_config->clustering_score_without_discriminator;
	discriminator = &engine->clustering_discriminator->latest_clustering_input;

	sfptpd_time_float_ns_to_timespec(discriminator->offset_from_master,
					 &discriminator_ofm);
	if (!discriminator->offset_valid) {
		TRACE_L5("clustering: offset invalid for clustering determinant %s: using default clustering score %d\n",
		         engine->clustering_discriminator->info.name,
			 default_score);
		return default_score;
	}

	sfptpd_time_float_ns_to_timespec(offset_from_master,
					 &instance_ofm);
	if ( !offset_valid(&instance_ofm)) {
		TRACE_L6("clustering: offset invalid for clustering candidate %s: using default clustering score %d\n",
			 evaluator->instance_name,
			 default_score);
		return default_score;
	}

	/* clock valid and discriminator valid */
	/* compare clock against discriminator */
	struct sfptpd_timespec discrim_lrc_to_instance_lrc;
	struct sfptpd_timespec discrim_to_instance_lrc;
	struct sfptpd_timespec discrim_to_instance;

	/* First calculate (d_lrc - i_lrc) */
	sfptpd_clock_compare(discriminator->clock,
			     instance_clock,
			     &discrim_lrc_to_instance_lrc);

	/* Now calculate (d_lrc - i_lrc) - (d_lrc - d_gm) */
	sfptpd_time_subtract(&discrim_to_instance_lrc,
			     &discrim_lrc_to_instance_lrc,
			     &discriminator_ofm);

	/* Now calculate (d_lrc - i_lrc) - (d_lrc - d_gm) + (i_lrc - i_gm) */
	/* = d_lrc - i_lrc - d_lrc + d_gm + i_lrc - i_gm */
	/* = d_gm - i_gm */
	sfptpd_time_add(&discrim_to_instance,
			&discrim_to_instance_lrc,
			&instance_ofm);

	sfptpd_time_t diff_i = sfptpd_time_timespec_to_float_ns(&discrim_to_instance);

	TRACE_L6("clustering: %s remote clock is " SFPTPD_FORMAT_FLOAT
		 "ns from discriminator remote clock\n",
		 evaluator->instance_name,
		 diff_i);
	diff_i = sfptpd_time_abs(diff_i);
	return (diff_i < engine->general_config->clustering_discriminator_threshold ? 1 : 0);
}


/*
        Returns true if the clustering score is outside of the clustering guard threshold.
*/
bool sfptpd_engine_compare_clustering_guard_threshold(struct sfptpd_clustering_evaluator *evaluator, int clustering_score)
{
        struct sfptpd_engine *engine = evaluator->private;
        if (!engine->general_config->clustering_guard_enabled) {
                return false;
        }
	return clustering_score < engine->general_config->clustering_guard_threshold;
}


void sfptpd_engine_schedule_leap_second(struct sfptpd_engine *engine,
					enum sfptpd_leap_second_type type,
					long double guard_interval)
{
	engine_msg_t *msg;

	assert(engine != NULL);
	assert((type == SFPTPD_LEAP_SECOND_61) || (type == SFPTPD_LEAP_SECOND_59));
	assert(guard_interval > 0.0);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.schedule_leap_second.type = type;
	msg->u.schedule_leap_second.guard_interval = guard_interval;
	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_SCHEDULE_LEAP_SECOND, false);
}


void sfptpd_engine_cancel_leap_second(struct sfptpd_engine *engine)
{
	engine_msg_t *msg;

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_CANCEL_LEAP_SECOND, false);
}


void sfptpd_engine_test_mode(struct sfptpd_engine *engine,
			     enum sfptpd_test_id test_id,
			     int param0, int param1, int param2)
{
	engine_msg_t *msg;

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.configure_test_mode.mode = test_id;
	msg->u.configure_test_mode.params[0] = param0;
	msg->u.configure_test_mode.params[1] = param1;
	msg->u.configure_test_mode.params[2] = param2;
	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_CONFIGURE_TEST_MODE, false);
}


void sfptpd_engine_post_rt_stats(struct sfptpd_engine *engine,
		struct sfptpd_log_time *time,
		const char *instance_name,
		const char *source,
		const struct sfptpd_clock *clock_master,
		const struct sfptpd_clock *clock_slave,
		bool disciplining,
		bool blocked,
		bool in_sync,
		sfptpd_sync_module_alarms_t alarms,
		...)
{
	assert(engine != NULL);
	assert(time != NULL);
	assert(instance_name != NULL);
	assert(source != NULL || clock_master != NULL);
	assert(clock_slave != NULL);

	va_list ap;
	const uint8_t *ptr_eui64;
	struct rt_stats_msg *msg = (struct rt_stats_msg*)sfptpd_msg_alloc(
		SFPTPD_MSG_POOL_RT_STATS, false);

	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("rt_stats");
		return;
	}

	memset(&msg->stats, 0, sizeof(msg->stats));
	msg->stats.time = *time;
	msg->stats.instance_name = instance_name;
	msg->stats.source = source;
	msg->stats.clock_master = clock_master;
	msg->stats.clock_slave = clock_slave;
	msg->stats.is_disciplining = disciplining;
	msg->stats.is_blocked = blocked;
	msg->stats.is_in_sync = in_sync;
	msg->stats.alarms = alarms;
	msg->stats.stat_present = 0;

	va_start(ap, alarms);
	while (true) {
		int key = va_arg(ap, int);

		if (key == STATS_KEY_END)
			break;
		assert(key < STATS_KEY_END);

		switch (key) {
		case STATS_KEY_OFFSET:
			msg->stats.offset = va_arg(ap, sfptpd_time_t);
			break;
		case STATS_KEY_FREQ_ADJ:
			msg->stats.freq_adj = va_arg(ap, sfptpd_time_t);
			break;
		case STATS_KEY_OWD:
			msg->stats.one_way_delay = va_arg(ap, sfptpd_time_t);
			break;
		case STATS_KEY_PARENT_ID:
			ptr_eui64 = va_arg(ap, uint8_t *);
			if (ptr_eui64 == NULL)
				continue;
			memcpy(msg->stats.parent_id, ptr_eui64, sizeof msg->stats.parent_id);
			break;
		case STATS_KEY_GM_ID:
			ptr_eui64 = va_arg(ap, uint8_t *);
			if (ptr_eui64 == NULL)
				continue;
			memcpy(msg->stats.gm_id, ptr_eui64, sizeof msg->stats.gm_id);
			break;
		case STATS_KEY_PPS_OFFSET:
			msg->stats.pps_offset = va_arg(ap, sfptpd_time_t);
			break;
		case STATS_KEY_BAD_PERIOD:
			msg->stats.bad_period_count = va_arg(ap, int);
			break;
		case STATS_KEY_OVERFLOWS:
			msg->stats.overflow_count = va_arg(ap, int);
			break;
		case STATS_KEY_ACTIVE_INTF:
			msg->stats.active_intf = va_arg(ap, struct sfptpd_interface *);
			if (msg->stats.active_intf == NULL)
				continue;
			break;
		case STATS_KEY_BOND_NAME:
			msg->stats.bond_name = va_arg(ap, char *);
			if (msg->stats.bond_name == NULL)
				continue;
			break;
		case STATS_KEY_P_TERM:
			msg->stats.p_term = va_arg(ap, long double);
			break;
		case STATS_KEY_I_TERM:
			msg->stats.i_term = va_arg(ap, long double);
			break;
		case STATS_KEY_M_TIME:
			msg->stats.time_master = va_arg(ap, struct sfptpd_timespec);
			msg->stats.has_m_time = true;
			break;
		case STATS_KEY_S_TIME:
			msg->stats.time_slave = va_arg(ap, struct sfptpd_timespec);
			msg->stats.has_s_time = true;
			break;
		default:
			CRITICAL("sfptpd_engine_post_rt_stats: unknown key given (%d)\n", key);
		}

		msg->stats.stat_present |= (1 << key);
	}
	va_end(ap);

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_RT_STATS_ENTRY, false);
}


void sfptpd_engine_log_rotate(struct sfptpd_engine *engine)
{
	engine_msg_t *msg;

	assert(engine != NULL);

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_LOG_ROTATE, false);
}


void sfptpd_engine_clustering_input(struct sfptpd_engine *engine,
				    const char *instance_name,
				    struct sfptpd_clock *lrc,
				    sfptpd_time_t offset_from_master,
				    bool offset_valid)
{
	engine_msg_t *msg;

	msg = (engine_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.clustering_input, 0, sizeof(msg->u.clustering_input));

	msg->u.clustering_input.clock = lrc;
	msg->u.clustering_input.instance_name = instance_name;
	msg->u.clustering_input.offset_from_master = offset_from_master;
	msg->u.clustering_input.offset_valid = offset_valid;

	(void)SFPTPD_MSG_SEND(msg, engine->thread,
			      ENGINE_MSG_CLUSTERING_INPUT, false);
}


const struct sfptpd_sync_instance_info *sfptpd_engine_get_sync_instance_by_name(struct sfptpd_engine *engine,
										const char *name)
{
	struct sync_instance_record *record;

	record = get_sync_instance_record_by_name(engine, name);
	return record ? &record->info : NULL;
}


struct sfptpd_clockfeed *sfptpd_engine_get_clockfeed(struct sfptpd_engine *engine)
{
	assert(engine);

	return engine->clockfeed;
}


/* fin */
