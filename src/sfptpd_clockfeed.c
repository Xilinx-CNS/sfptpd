/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_clockfeed.c
 * @brief  Feed of clock differences/timestamps
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

#include "sfptpd_app.h"
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
#include "sfptpd_sync_module.h"
#include "sfptpd_multicast.h"

#include "sfptpd_clockfeed.h"


/****************************************************************************
 * Constants and macros
 ****************************************************************************/

#define MODULE "clocks"
#define PREFIX MODULE ": "

/* Clock feed component specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 1, PREFIX x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 2, PREFIX x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 3, PREFIX x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 4, PREFIX x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 5, PREFIX x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, 6, PREFIX x, ##__VA_ARGS__)
#define DBG_LX(sev, x, ...)  TRACE(SFPTPD_COMPONENT_ID_CLOCKS, sev, PREFIX x, ##__VA_ARGS__)

#define CLOCKFEED_MODULE_MAGIC     0xC10CFEED0030D01EULL
#define CLOCKFEED_SOURCE_MAGIC     0xC10CFEED00005005ULL
#define CLOCKFEED_SHM_MAGIC        0xC10CFEED00005443ULL
#define CLOCKFEED_SUBSCRIBER_MAGIC 0xC10CFEED50B5C1BEULL
#define CLOCKFEED_DELETED_MAGIC    0xD0D00EC5C10CFEEDULL

#define CLOCK_POLL_TIMER_ID (0)

#define MAX_CLOCK_SAMPLES_LOG2 (4)
#define MAX_CLOCK_SAMPLES      (1 << MAX_CLOCK_SAMPLES_LOG2)

#define MAX_EVENT_SUBSCRIBERS (4)

/* Stats ids */
enum clockfeed_stats_ids {
	CLOCKFEED_STATS_ID_NUM_CLOCKS,
};

static const struct sfptpd_stats_collection_defn clockfeed_stats_defns[] =
{
	{CLOCKFEED_STATS_ID_NUM_CLOCKS, SFPTPD_STATS_TYPE_RANGE, "num-clocks", NULL, 0},
};

/****************************************************************************
 * Clock feed messages
 ****************************************************************************/

/* Macro used to define message ID values for clock feed messages */
#define CLOCKFEED_MSG(x) SFPTPD_CLOCKFEED_MSG(x)

/* Add a clock source.
 * It is a synchronous message.
 */
#define CLOCKFEED_MSG_ADD_CLOCK   CLOCKFEED_MSG(1)
struct clockfeed_add_clock {
	struct sfptpd_clock *clock;
	int poll_period_log2;
};

/* Remove a clock source.
 * It is a synchronous message.
 */
#define CLOCKFEED_MSG_REMOVE_CLOCK   CLOCKFEED_MSG(2)
struct clockfeed_remove_clock {
	struct sfptpd_clock *clock;
};

/* Subscribe to a clock source.
 * It is a synchronous message with a reply.
 */
#define CLOCKFEED_MSG_SUBSCRIBE   CLOCKFEED_MSG(3)
struct clockfeed_subscribe_req {
	struct sfptpd_clock *clock;
};
struct clockfeed_subscribe_resp {
	struct sfptpd_clockfeed_sub *sub;
};

/* Unsubscribe from a clock source.
 * It is a synchronous message.
 */
#define CLOCKFEED_MSG_UNSUBSCRIBE   CLOCKFEED_MSG(4)
struct clockfeed_unsubscribe {
	struct sfptpd_clockfeed_sub *sub;
};

/* Notification that a cycle of processing all ready clock feeds has
 * been completed. This value is defined in the public header file.
 * It is an asynchronous message with no reply. To be multicast.
 */
#define CLOCKFEED_MSG_SYNC_EVENT   SFPTPD_CLOCKFEED_MSG_SYNC_EVENT

/* Next message code to be allocated. Avoid overlapping with above
 * message code that is defined in the header file.
 */
#define CLOCKFEED_MSG_NEXT_UNALLOCATED   CLOCKFEED_MSG(6)

/* Union of all clock feed messages
 * @hdr Standard message header
 * @u Union of message payloads
 */
typedef struct clockfeed_msg {
	sfptpd_msg_hdr_t hdr;
	union {
		struct clockfeed_add_clock add_clock;
		struct clockfeed_remove_clock remove_clock;
		struct clockfeed_subscribe_req subscribe_req;
		struct clockfeed_subscribe_resp subscribe_resp;
		struct clockfeed_unsubscribe unsubscribe;
	} u;
} sfptpd_clockfeed_msg_t;

static_assert(sizeof(sfptpd_clockfeed_msg_t) < SFPTPD_SIZE_GLOBAL_MSGS,
	      "message fits into global pool entry");


/****************************************************************************
 * Types
 ****************************************************************************/

struct clockfeed_source;

struct clockfeed_shm {
	struct sfptpd_clockfeed_sample samples[MAX_CLOCK_SAMPLES];
	uint64_t magic;
	uint64_t write_counter;
};

struct sfptpd_clockfeed_sub {
	uint64_t magic;

	/* Read-only reference to source info and SHM */
	struct clockfeed_source *source;

	/* Sample counter for last read sample */
	int64_t read_counter;

	/* Minimum counter for next read sample */
	int64_t min_counter;

	/* Flags */
	bool have_max_age:1;
	bool have_max_age_diff:1;

	/* Maximum age of sample */
	struct sfptpd_timespec max_age;

	/* Maximum age difference of samples */
	struct sfptpd_timespec max_age_diff;

	/* Linked list of subscribers to source */
	struct sfptpd_clockfeed_sub *next;
};

struct clockfeed_source {
	uint64_t magic;

	/* Pointer to clock source */
	struct sfptpd_clock *clock;

	/* Log2 of the period to poll this source */
	int poll_period_log2;

	/* Counters */
	uint64_t cycles;

	/* Samples */
	struct clockfeed_shm shm;

	/* Subscribers */
	struct sfptpd_clockfeed_sub *subscribers;

	/* Next source in list */
	struct clockfeed_source *next;

	/* Is inactive */
	bool inactive;
};

struct sfptpd_clockfeed {
	uint64_t magic;

	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Clock feed Thread */
	struct sfptpd_thread *thread;

	/* Log2 of the period to poll overall */
	int poll_period_log2;

	/* Whether we have entered the RUNning phase */
	bool running_phase;

	/* Linked list of live clock sources */
	struct clockfeed_source *active;

	/* Linked list of removed (zombie) clock sources */
	struct clockfeed_source *inactive;

	/* Clock feed statistics */
	struct sfptpd_stats_collection stats;
};


/****************************************************************************
 * Global variables
 ****************************************************************************/



/****************************************************************************
 * Function prototypes
 ****************************************************************************/



/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static void clockfeed_dump_state(struct sfptpd_clockfeed *clockfeed, int sev)
{
	struct sfptpd_clockfeed_sub *subscriber;
	struct clockfeed_source *source;
	int i;

	DBG_LX(sev, "dumping state:\n");
	for (i = 0; i < 2; i ++) {
		const char *which[] = { "active", "inactive" };
		DBG_LX(sev, " %s sources:\n", which[i]);
		for (source = (i == 0 ? clockfeed->active : clockfeed->inactive); source; source = source->next) {
			DBG_LX(sev, "  - clock %s\n", sfptpd_clock_get_short_name(source->clock));
			DBG_LX(sev, "     write_counter %d\n", source->shm.write_counter);
			DBG_LX(sev, "     subscribers:\n");
			for (subscriber = source->subscribers; subscriber; subscriber = subscriber->next) {
				DBG_LX(sev, "    - subscriber %p\n", subscriber);
				DBG_LX(sev, "       read_counter %d\n", subscriber->read_counter);
				DBG_LX(sev, "       min_counter %d\n", subscriber->min_counter);
			}
		}
	}
}

static void clockfeed_send_sync_event(struct sfptpd_clockfeed *clockfeed)
{
	sfptpd_clockfeed_msg_t msg;

	assert(clockfeed != NULL);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	SFPTPD_MSG_INIT(msg);
	SFPTPD_MULTICAST_SEND(&msg,
			      SFPTPD_CLOCKFEED_MSG_SYNC_EVENT,
			      SFPTPD_MSG_POOL_LOCAL, false);
}

static void clockfeed_reap_zombies(struct sfptpd_clockfeed *module,
				   struct clockfeed_source *source)
{
	assert(module);
	assert(source);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source->inactive && source->subscribers == NULL) {
		struct clockfeed_source **nextp;

		DBG_L3("removing source %s\n",
			 sfptpd_clock_get_short_name(source->clock));

		for (nextp = &module->inactive;
		     *nextp && (*nextp != source);
		     nextp = &(*nextp)->next)
			assert((*nextp)->magic == CLOCKFEED_SOURCE_MAGIC);

		assert(*nextp == source);

		*nextp = source->next;
		source->magic = CLOCKFEED_DELETED_MAGIC;
		free(source);
	}
}

/* This is the key function of the clock feed component. Periodically sample
 * all clock differences (against the system clock) for all interesting clocks.
 * These may have different cadences configured (internally - this is not used
 * at present).
 *
 * Snapshots of the clocks are stored in a lock-free circular buffer
 * structure for consumption in another thread via helper functions. Typically
 * only the last snapshot available will be consumed but additional samples
 * are present to help avoid losing samples through contention and in case
 * future consumers can benefit from out-of-date history.
 */
static void clockfeed_on_timer(void *user_context, unsigned int id)
{
	struct sfptpd_clockfeed *clockfeed = (struct sfptpd_clockfeed *)user_context;
	const uint32_t index_mask = (1 << MAX_CLOCK_SAMPLES_LOG2) - 1;
	struct sfptpd_timespec realtime = { 0, 0 };
	struct clockfeed_source *source;
	int sources_count;

	assert(clockfeed != NULL);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	sources_count = 0;
	for (source = clockfeed->active; source; source = source->next) {
		const int cadence = source->poll_period_log2 - clockfeed->poll_period_log2;
		const uint64_t cadence_mask = (1 << cadence) - 1;

		if ((source->cycles & cadence_mask) == 0) {
			uint32_t index = source->shm.write_counter & index_mask;
			struct sfptpd_clockfeed_sample *record = &source->shm.samples[index];
			struct sfptpd_timespec diff;

			record->seq = source->shm.write_counter;
			record->rc = sfptpd_clock_compare(source->clock,
						  sfptpd_clock_get_system_clock(),
						  &diff);

			sfclock_gettime(CLOCK_MONOTONIC, &record->mono);
			sfclock_gettime(CLOCK_REALTIME, &realtime);
			record->system = realtime;

			if (record->rc == 0)
				sfptpd_time_add(&record->snapshot,
						&record->system,
						&diff);
			else
				sfptpd_time_zero(&record->snapshot);

			DBG_L6("%s: %llu: %llu: %d: "
			       SFPTPD_FMT_SFTIMESPEC " " SFPTPD_FMT_SFTIMESPEC "\n",
			       sfptpd_clock_get_short_name(source->clock),
			       source->cycles, source->shm.write_counter, record->rc,
			       SFPTPD_ARGS_SFTIMESPEC(record->system),
			       SFPTPD_ARGS_SFTIMESPEC(record->snapshot));

			source->shm.write_counter++;
		}
		source->cycles++;
		sources_count++;
	}

	if (realtime.sec == 0)
		sfclock_gettime(CLOCK_REALTIME, &realtime);

	sfptpd_stats_collection_update_range(&clockfeed->stats,
					     CLOCKFEED_STATS_ID_NUM_CLOCKS,
					     sources_count, realtime, true);

	clockfeed_send_sync_event(clockfeed);
}

static int clockfeed_on_startup(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct sfptpd_timespec interval;
	uint64_t secs_fp32;
	int rc;

	assert(module != NULL);

	sfptpd_multicast_publish(SFPTPD_CLOCKFEED_MSG_SYNC_EVENT);
	sfptpd_multicast_subscribe(SFPTPD_APP_MSG_DUMP_TABLES);

	/* Create a message pool for sending end-of-scan sync messages */
	rc = sfptpd_thread_alloc_msg_pool(SFPTPD_MSG_POOL_LOCAL,
					  MAX_EVENT_SUBSCRIBERS,
					  sizeof(struct clockfeed_msg));
	if (rc != 0)
		return rc;

	rc = sfptpd_thread_timer_create(CLOCK_POLL_TIMER_ID, CLOCK_MONOTONIC,
					clockfeed_on_timer, module);
	if (rc != 0)
		return rc;

	secs_fp32 = 0x8000000000000000ULL >> (31 - module->poll_period_log2);

	sfptpd_time_init(&interval, secs_fp32 >> 32,
			 ((secs_fp32 & 0xFFFFFFFFUL) * 1000000000UL) >> 32, 0);

	DBG_L1("poll interval to " SFPTPD_FMT_SFTIMESPEC "s\n",
	       SFPTPD_ARGS_SFTIMESPEC(interval));

	rc = sfptpd_thread_timer_start(CLOCK_POLL_TIMER_ID,
				       true, false, &interval);
	if (rc != 0)
		return rc;

	return 0;
}

static void clockfeed_on_run(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;

	assert(module != NULL);

	module->running_phase = true;
}

static void clockfeed_on_dump_tables(void *context, sfptpd_app_msg_t *msg)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;

	assert(module != NULL);

	clockfeed_dump_state(module, 0);
}

static void clockfeed_on_add_clock(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);

	DBG_L3("received add_clock message\n");

	source = calloc(1, sizeof *source);
	assert(source);

	/* Populate source */
	source->magic = CLOCKFEED_SOURCE_MAGIC;
	source->shm.magic = CLOCKFEED_SHM_MAGIC;
	source->clock = msg->u.add_clock.clock;
	source->poll_period_log2 = msg->u.add_clock.poll_period_log2;

	if (source->poll_period_log2 < module->poll_period_log2) {
		ERROR(PREFIX "clockfeed: requested poll rate for %s (%d) exceeds "
		      "global limit of %d\n",
		      sfptpd_clock_get_short_name(source->clock),
		      source->poll_period_log2,
		      module->poll_period_log2);
		source->poll_period_log2 = module->poll_period_log2;
	}

	/* Add to linked list */
	source->next = module->active;
	module->active = source;

	DBG_L1("added source %s with log2 sync interval %d\n",
		sfptpd_clock_get_short_name(source->clock),
		source->poll_period_log2);

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_remove_clock(struct sfptpd_clockfeed *module,
				      struct clockfeed_msg *msg)
{
	struct clockfeed_source **source;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);
	assert(msg->u.remove_clock.clock != NULL);

	DBG_L3("received remove_clock message\n");

	for (source = &module->active;
	     *source && (*source)->clock != msg->u.remove_clock.clock;
	     source = &(*source)->next)
		assert((*source)->magic == CLOCKFEED_SOURCE_MAGIC);

	if (*source == NULL) {
		DBG_L4("ignoring request to remove inactive clock %s\n",
		      sfptpd_clock_get_short_name(msg->u.remove_clock.clock));
	} else {
		struct clockfeed_source *s = *source;

		*source = s->next;
		s->next = module->inactive;
		s->inactive = true;
		module->inactive = s;

		DBG_L4("marked source inactive: %s\n",
			 sfptpd_clock_get_short_name(s->clock));

		clockfeed_reap_zombies(module, s);
	}

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_subscribe(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct sfptpd_clockfeed_sub *subscriber;
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.subscribe_req.clock != NULL);

	DBG_L3("received subscribe message\n");

	for (source = module->active;
	     source && source->clock != msg->u.subscribe_req.clock;
	     source = source->next)
		assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source == NULL)
		for (source = module->inactive;
		     source && source->clock != msg->u.subscribe_req.clock;
		     source = source->next)
			assert(source->magic == CLOCKFEED_SOURCE_MAGIC);

	if (source == NULL) {
		ERROR("clockfeed: non-existent clock subscribed to: %s\n",
		      sfptpd_clock_get_short_name(msg->u.subscribe_req.clock));
		msg->u.subscribe_resp.sub = NULL;
	} else {
		subscriber = calloc(1, sizeof *subscriber);
		assert(subscriber);

		if (source->inactive)
			WARNING("clockfeed: subscribed to inactive source\n");

		subscriber->magic = CLOCKFEED_SUBSCRIBER_MAGIC;
		subscriber->source = source;
		subscriber->read_counter = -1;
		subscriber->min_counter = -1;
		subscriber->next = source->subscribers;
		source->subscribers = subscriber;

		msg->u.subscribe_resp.sub = subscriber;
	}

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_unsubscribe(struct sfptpd_clockfeed *module,
				     struct clockfeed_msg *msg)
{
	struct sfptpd_clockfeed_sub **nextp, *s, *sub;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.unsubscribe.sub != NULL);

	DBG_L3("received unsubscribe message\n");

	sub = msg->u.unsubscribe.sub;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	for (nextp = &sub->source->subscribers;
	     (s = *nextp) && s != sub;
	     nextp = &(s->next));

	if (s == NULL) {
		ERROR(PREFIX "non-existent clock subscription\n");
	} else {
		*nextp = s->next;
	}

	clockfeed_reap_zombies(module, sub->source);
	sub->magic = CLOCKFEED_DELETED_MAGIC;
	free(sub);

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_shutdown(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct clockfeed_source **source;
	struct clockfeed_source *s;
	int count;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);

	DBG_L2("shutting down\n");

	sfptpd_multicast_unsubscribe(SFPTPD_APP_MSG_DUMP_TABLES);
	sfptpd_multicast_unpublish(SFPTPD_CLOCKFEED_MSG_SYNC_EVENT);
	clockfeed_dump_state(module, 5);

	/* Mark all sources inactive */
	count = 0;
	for (source = &module->active; *source; source = &(*source)->next) {
		s = *source;
		assert(s->magic == CLOCKFEED_SOURCE_MAGIC);
		assert(!s->inactive);
		s->inactive = true;
		count++;
	}

	/* Move active list onto inactive list */
	*source = module->inactive;
	module->inactive = module->active;
	module->active = NULL;
	DBG_L4("inactivated all %d active sources\n", count);

	/* Reap zombies */
	for (s = module->inactive; s; s = s->next)
		clockfeed_reap_zombies(module, s);


	if (module->inactive)
		WARNING("clockfeed: clock source subscribers remaining on shutdown\n");

	clockfeed_dump_state(module, module->inactive ? 0 : 5);
	sfptpd_stats_collection_free(&module->stats);

	module->magic = CLOCKFEED_DELETED_MAGIC;
	free(module);
}

static void clockfeed_on_stats_end_period(struct sfptpd_clockfeed *module, sfptpd_sync_module_msg_t *msg)
{
	assert(module != NULL);
	assert(msg != NULL);

	sfptpd_stats_collection_end_period(&module->stats,
					   &msg->u.stats_end_period_req.time);

	/* Write the historical statistics to file */
	sfptpd_stats_collection_dump(&module->stats, NULL, "clocks");

	SFPTPD_MSG_FREE(msg);
}

static void clockfeed_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct clockfeed_msg *msg = (struct clockfeed_msg *)hdr;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		clockfeed_on_run(module);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_APP_MSG_DUMP_TABLES:
		clockfeed_on_dump_tables(module, (sfptpd_app_msg_t *) msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		clockfeed_on_stats_end_period(module, (sfptpd_sync_module_msg_t *) msg);
		break;

	case CLOCKFEED_MSG_ADD_CLOCK:
		clockfeed_on_add_clock(module, msg);
		break;

	case CLOCKFEED_MSG_REMOVE_CLOCK:
		clockfeed_on_remove_clock(module, msg);
		break;

	case CLOCKFEED_MSG_SUBSCRIBE:
		clockfeed_on_subscribe(module, msg);
		break;

	case CLOCKFEED_MSG_UNSUBSCRIBE:
		clockfeed_on_unsubscribe(module, msg);
		break;

	default:
		WARNING("clockfeed: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void clockfeed_on_user_fds(void *context, unsigned int num_fds,
				 struct sfptpd_thread_readyfd events[])
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *) context;

	assert(module != NULL);
	assert(module->magic == CLOCKFEED_MODULE_MAGIC);
}


static const struct sfptpd_thread_ops clockfeed_thread_ops =
{
	clockfeed_on_startup,
	clockfeed_on_shutdown,
	clockfeed_on_message,
	clockfeed_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_clockfeed *sfptpd_clockfeed_create(struct sfptpd_thread **threadret,
						 int min_poll_period_log2)
{
	struct sfptpd_clockfeed *clockfeed;
	int rc;

	assert(threadret);

	DBG_L3("creating service\n");

	*threadret = NULL;
	clockfeed = (struct sfptpd_clockfeed *) calloc(1, sizeof(*clockfeed));
	if (clockfeed == NULL) {
		CRITICAL(PREFIX "failed to allocate module memory\n");
		return NULL;
	}

	clockfeed->poll_period_log2 = min_poll_period_log2;

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&clockfeed->stats, "clockfeed",
					    sizeof(clockfeed_stats_defns)/sizeof(clockfeed_stats_defns[0]),
					    clockfeed_stats_defns);
	if (rc != 0) {
		errno = rc;
		goto fail;
	}

	/* Create the service thread - the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("clocks", &clockfeed_thread_ops, clockfeed, threadret);
	if (rc != 0) {
		errno = rc;
		goto fail;
	}

	clockfeed->magic = CLOCKFEED_MODULE_MAGIC;
	clockfeed->thread = *threadret;
	return clockfeed;

fail:
	free(clockfeed);
	return NULL;
}

void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2)
{
	struct clockfeed_msg *msg;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.add_clock, 0, sizeof(msg->u.add_clock));

	msg->u.add_clock.clock = clock;
	msg->u.add_clock.poll_period_log2 = poll_period_log2;

	SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
			     CLOCKFEED_MSG_ADD_CLOCK);
}

void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock)
{
	struct clockfeed_msg *msg;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.remove_clock, 0, sizeof(msg->u.remove_clock));

	msg->u.remove_clock.clock = clock;

	SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
			     CLOCKFEED_MSG_REMOVE_CLOCK);
}

int sfptpd_clockfeed_subscribe(struct sfptpd_clockfeed *clockfeed,
			       struct sfptpd_clock *clock,
			       struct sfptpd_clockfeed_sub **sub)
{
	struct clockfeed_msg *msg;
	int rc;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);
	assert(clock);
	assert(sub);

	/* The calling code has an easier life if it can treat a system
	 * clock, i.e. NULL feed, the same as a real one. */
	if (sfptpd_clock_is_system(clock)) {
		*sub = NULL;
		return 0;
	}

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return ENOMEM;
	}

	memset(&msg->u.subscribe_req, 0, sizeof(msg->u.subscribe_req));

	msg->u.subscribe_req.clock = clock;

	rc = SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
				  CLOCKFEED_MSG_SUBSCRIBE);
	if (rc == 0) {
		assert(msg->u.subscribe_resp.sub);
		assert(msg->u.subscribe_resp.sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);
		*sub = msg->u.subscribe_resp.sub;
	}

	return rc;
}

void sfptpd_clockfeed_unsubscribe(struct sfptpd_clockfeed *clockfeed,
				 struct sfptpd_clockfeed_sub *subscriber)
{
	struct clockfeed_msg *msg;

	assert(clockfeed);
	assert(clockfeed->magic == CLOCKFEED_MODULE_MAGIC);

	if (subscriber == NULL)
		return;

	assert(subscriber->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.unsubscribe, 0, sizeof(msg->u.unsubscribe));

	msg->u.unsubscribe.sub = subscriber;

	SFPTPD_MSG_SEND_WAIT(msg, clockfeed->thread,
			     CLOCKFEED_MSG_UNSUBSCRIBE);
}

static int clockfeed_compare_to_sys(struct sfptpd_clockfeed_sub *sub,
				    struct sfptpd_timespec *diff,
				    struct sfptpd_timespec *t1,
				    struct sfptpd_timespec *t2,
				    struct sfptpd_timespec *mono_time)
{
	const struct clockfeed_shm *shm = &sub->source->shm;
	const uint32_t index_mask = (1 << MAX_CLOCK_SAMPLES_LOG2) - 1;
	const struct sfptpd_clockfeed_sample *sample;
	struct sfptpd_clock *clock;
	struct sfptpd_timespec now_mono;
	struct sfptpd_timespec age;
	int writer1;
	int writer2;
	int rc = 0;

	sfptpd_time_zero(diff);

	DBG_L5("consumer: comparing %s (%p shm) to sys\n",
		sfptpd_clock_get_short_name(sub->source->clock), shm);

	clock = sub->source->clock;
	writer1 = shm->write_counter;

	if (sub->source->inactive)
		return EOWNERDEAD;

	if (!sfptpd_clock_is_active(clock))
		return ENOENT;

	if (writer1 == 0) {
		ERROR(PREFIX "no samples yet obtained from %s\n",
		      sfptpd_clock_get_short_name(clock));
		return EAGAIN;
	}

	sample = &shm->samples[(writer1 - 1) & index_mask];

	if (sample->rc != 0)
		return rc;

	sfptpd_time_subtract(diff, &sample->snapshot, &sample->system);

	/* Check for overrun */
	writer2 = shm->write_counter;
	if (writer2 >= writer1 + MAX_CLOCK_SAMPLES - 1) {
		WARNING(PREFIX "%s: last sample lost while reading - reader too slow? %lld > %lld + %d\n",
		        sfptpd_clock_get_short_name(clock), writer2, writer1, MAX_CLOCK_SAMPLES - 1);
		return ENODATA;
	}

	/* Check for old sample when new one requested */
	if (writer1 < sub->min_counter) {
		WARNING(PREFIX "%s: old sample (%d) when fresh one (%d) requested\n",
		        sfptpd_clock_get_short_name(clock), writer1, sub->min_counter);
		return ESTALE;
	}
	if (sub->have_max_age) {
		rc = sfclock_gettime(CLOCK_MONOTONIC, &now_mono);
		if (rc != 0)
			return EAGAIN;
		sfptpd_time_subtract(&age, &now_mono, &sample->mono);
		if (sfptpd_time_cmp(&age, &sub->max_age) > 0) {
			WARNING(PREFIX "%s: sample too old\n",
				sfptpd_clock_get_short_name(clock));
			return ESTALE;
		}
	}
	if (t1)
		*t1 = sample->snapshot;
	if (t2)
		*t2 = sample->system;
	if (mono_time)
		*mono_time = sample->mono;
	if (rc == 0) {
		sub->read_counter = writer1;
	}

	return rc;
}

int sfptpd_clockfeed_compare(struct sfptpd_clockfeed_sub *sub1,
			     struct sfptpd_clockfeed_sub *sub2,
			     struct sfptpd_timespec *diff,
			     struct sfptpd_timespec *t1,
			     struct sfptpd_timespec *t2,
			     struct sfptpd_timespec *mono)
{
	const struct clockfeed_source *feed1 = sub1 ? sub1->source : NULL;
	const struct clockfeed_source *feed2 = sub2 ? sub2->source : NULL;
	const struct clockfeed_shm *shm1 = sub1 ? &feed1->shm : NULL;
	const struct clockfeed_shm *shm2 = sub2 ? &feed2->shm : NULL;
	const struct sfptpd_timespec *max_age_diff = NULL;
	struct sfptpd_timespec diff2;
	struct sfptpd_timespec mono1;
	struct sfptpd_timespec mono2;
	int rc = 0;

	sfptpd_time_zero(diff);

	if (sub1 && sub2) {
		if (sub1->have_max_age_diff)
			max_age_diff = &sub1->max_age_diff;
		if (sub2->have_max_age_diff && (!max_age_diff || sfptpd_time_is_greater_or_equal(max_age_diff, &sub2->max_age_diff)))
			max_age_diff = &sub2->max_age_diff;
		if (!mono && max_age_diff)
			mono = &mono1;
	}

	DBG_L6("consumer: comparing %s (%p shm) to %s (%p shm)\n",
		shm1 ? sfptpd_clock_get_short_name(feed1->clock) : "<sys>", shm1,
		shm2 ? sfptpd_clock_get_short_name(feed2->clock) : "<sys>", shm2);

	if (sub1) {
		rc = clockfeed_compare_to_sys(sub1, diff, t1, sub2 ? NULL : t2, mono);
	}
	if (rc == 0 && sub2) {
		rc = clockfeed_compare_to_sys(sub2, &diff2, t2, sub1 ? NULL : t1, mono ? &mono2 : NULL);
		if (rc == 0) {
			sfptpd_time_subtract(diff, diff, &diff2);
			if (mono && sfptpd_time_is_greater_or_equal(mono, &mono2))
				*mono = mono2;
		}
	}

	if (rc == 0 && max_age_diff) {
		struct sfptpd_timespec age_diff;

		if (sfptpd_time_is_greater_or_equal(&mono2, mono))
			sfptpd_time_subtract(&age_diff, &mono2, mono);
		else
			sfptpd_time_subtract(&age_diff, mono, &mono2);

		if (sfptpd_time_is_greater_or_equal(&age_diff, max_age_diff)) {
			WARNING("%s-%s: to big an age difference between samples\n",
				sfptpd_clock_get_short_name(feed1->clock),
				sfptpd_clock_get_short_name(feed2->clock));
			return ESTALE;
		}
	}

	return rc;
}

void sfptpd_clockfeed_require_fresh(struct sfptpd_clockfeed_sub *sub)
{
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	DBG_L6("%s: updating minimum read counter from %d to %d\n",
		sfptpd_clock_get_short_name(sub->source->clock),
		sub->min_counter, sub->read_counter + 1);

	sub->min_counter = sub->read_counter + 1;
}

void sfptpd_clockfeed_set_max_age(struct sfptpd_clockfeed_sub *sub,
				  const struct sfptpd_timespec *max_age) {
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	sub->have_max_age = true;
	sub->max_age = *max_age;
}

void sfptpd_clockfeed_set_max_age_diff(struct sfptpd_clockfeed_sub *sub,
				       const struct sfptpd_timespec *max_age_diff) {
	if (!sub)
		return;

	assert(sub->magic == CLOCKFEED_SUBSCRIBER_MAGIC);

	sub->have_max_age_diff = true;
	sub->max_age_diff = *max_age_diff;
}

void sfptpd_clockfeed_stats_end_period(struct sfptpd_clockfeed *module,
				       struct sfptpd_timespec *time)
{
	sfptpd_sync_module_stats_end_period(module->thread, time);
}
