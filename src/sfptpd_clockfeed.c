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

#include "sfptpd_clockfeed.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

#define MIN_POLL_PERIOD_LOG2 (-4)
#define CLOCK_POLL_TIMER_ID (0)

#define const_container_of(ptr, type, element) ((const type *)(((const char *) ptr) - offsetof(type, element)))


/****************************************************************************
 * Clock feed messages
 ****************************************************************************/

/* Macro used to define message ID values for clock feed messages */
#define CLOCKFEED_MSG(x) (SFPTPD_MSG_BASE_CLOCK_FEED + (x))

/* Add a clock source.
 * It is an asynchronous message without a reply.
 */
#define CLOCKFEED_MSG_ADD_CLOCK   CLOCKFEED_MSG(1)
struct clockfeed_add_clock {
	struct sfptpd_clock *clock;
	int poll_period_log2;
};

/* Remove a clock source.
 * It is an asynchronous message without a reply.
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
	struct sfptpd_clockfeed_shm *shm;
};

/* Unsubscribe from a clock source.
 * It is an synchronous message with no reply.
 */
#define CLOCKFEED_MSG_UNSUBSCRIBE   CLOCKFEED_MSG(4)
struct clockfeed_unsubscribe {
	struct sfptpd_clock *clock;
};

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
} engine_msg_t;

STATIC_ASSERT(sizeof(engine_msg_t) < SFPTPD_SIZE_GLOBAL_MSGS);


/****************************************************************************
 * Types
 ****************************************************************************/

struct clockfeed_source;

struct clockfeed_source {
	/* Pointer to clock source */
	struct sfptpd_clock *clock;

	/* Log2 of the period to poll this source */
	int poll_period_log2;

	/* Counters */
	uint64_t cycles;

	/* Samples */
	struct sfptpd_clockfeed_shm shm;

	/* Subscriber count */
	int refcount;

	/* Next source in list */
	struct clockfeed_source *next;
};

struct sfptpd_clockfeed {
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

	/* Linked list of removed clock sources */
	struct clockfeed_source *inactive;
};


/****************************************************************************
 * Global variables
 ****************************************************************************/

const static struct sfptpd_clockfeed *sfptpd_clockfeed = NULL;


/****************************************************************************
 * Function prototypes
 ****************************************************************************/



/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static void clockfeed_on_timer(void *user_context, unsigned int id)
{
	struct sfptpd_clockfeed *clockfeed = (struct sfptpd_clockfeed *)user_context;
	struct clockfeed_source *source;
	const uint32_t index_mask = (1 << SFPTPD_MAX_CLOCK_SAMPLES_LOG2) - 1;

	assert(clockfeed != NULL);

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
			sfclock_gettime(CLOCK_REALTIME, &record->system);

			if (record->rc == 0)
				sfptpd_time_add(&record->snapshot,
						&record->system,
						&diff);
			else
				sfptpd_time_zero(&record->snapshot);

			TRACE_L6("clockfeed %s: %llu: %llu: %d: "
				 SFPTPD_FMT_SFTIMESPEC " " SFPTPD_FMT_SFTIMESPEC "\n",
				 sfptpd_clock_get_short_name(source->clock),
				 source->cycles, source->shm.write_counter, record->rc,
				 SFPTPD_ARGS_SFTIMESPEC(record->system),
				 SFPTPD_ARGS_SFTIMESPEC(record->snapshot));

			source->shm.write_counter++;
		}
		source->cycles++;
	}
}


static int clockfeed_on_startup(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct sfptpd_timespec interval;
	uint64_t secs_fp32;
	int rc;

	assert(module != NULL);

	rc = sfptpd_thread_timer_create(CLOCK_POLL_TIMER_ID, CLOCK_MONOTONIC,
					clockfeed_on_timer, module);
	if (rc != 0)
		return rc;

	secs_fp32 = 0x8000000000000000ULL >> (31 - module->poll_period_log2);

	sfptpd_time_init(&interval, secs_fp32 >> 32,
			 ((secs_fp32 & 0xFFFFFFFFUL) * 1000000000UL) >> 32, 0);

	TRACE_L3("clockfeed: set poll interval to " SFPTPD_FMT_SFTIMESPEC "s\n",
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

static void clockfeed_on_add_clock(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(msg != NULL);

	source = calloc(1, sizeof *source);
	assert(source);

	/* Populate source */
	source->clock = msg->u.add_clock.clock;
	source->poll_period_log2 = msg->u.add_clock.poll_period_log2;

	if (source->poll_period_log2 < module->poll_period_log2) {
		ERROR("clockfeed: requested poll rate for %s (%d) exceeds "
		      "global limit of %d\n",
		      sfptpd_clock_get_short_name(source->clock),
		      source->poll_period_log2,
		      module->poll_period_log2);
		source->poll_period_log2 = module->poll_period_log2;
	}

	/* Add to linked list */
	source->next = module->active;
	module->active = source;

	INFO("clockfeed: added source: %s\n",
	      sfptpd_clock_get_short_name(source->clock));
}

static void clockfeed_on_remove_clock(struct sfptpd_clockfeed *module,
				      struct clockfeed_msg *msg)
{
	struct clockfeed_source **source;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.remove_clock.clock != NULL);

	for (source = &module->active;
	     *source && (*source)->clock != msg->u.remove_clock.clock;
	     source = &(*source)->next);

	if (*source == NULL) {
		ERROR("clockfeed: cannot remove inactive clock %s\n",
		      sfptpd_clock_get_short_name(msg->u.remove_clock.clock));
	} else {
		struct clockfeed_source *s = *source;

		*source = s->next;
		s->next = module->inactive;
		module->inactive = s;

		INFO("clockfeed: removed source: %s\n",
		     sfptpd_clock_get_short_name(s->clock));
	}
}

static void clockfeed_on_subscribe(struct sfptpd_clockfeed *module,
				   struct clockfeed_msg *msg)
{
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.subscribe_req.clock != NULL);

	for (source = module->active;
	     source && source->clock != msg->u.subscribe_req.clock;
	     source = source->next);

	if (source == NULL) {
		ERROR("clockfeed: non-existent clock subscribed to: %s\n",
		      sfptpd_clock_get_short_name(msg->u.subscribe_req.clock));
		msg->u.subscribe_resp.shm = NULL;
	} else {
		msg->u.subscribe_resp.shm = &source->shm;
		source->refcount++;
	}

	SFPTPD_MSG_REPLY(msg);
}

static void clockfeed_on_unsubscribe(struct sfptpd_clockfeed *module,
				     struct clockfeed_msg *msg)
{
	struct clockfeed_source *source;

	assert(module != NULL);
	assert(msg != NULL);
	assert(msg->u.unsubscribe.clock != NULL);

	for (source = module->active;
	     source && source->clock != msg->u.unsubscribe.clock;
	     source = source->next);

	if (source == NULL) {
		ERROR("clockfeed: non-existent clock unsubscribed from: %s\n",
		      sfptpd_clock_get_short_name(msg->u.unsubscribe.clock));
	} else {
		source->refcount--;

		assert(source->refcount >= 0);

		if (source->refcount == 0) {
			INFO("clockfeed: can free %s\n",
			     sfptpd_clock_get_short_name(source->clock));
		}
	}
}

static void clockfeed_on_shutdown(void *context)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;

	assert(module != NULL);
	assert(sfptpd_clockfeed == module);

	free(module);
	sfptpd_clockfeed = NULL;
}


static void clockfeed_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *)context;
	struct clockfeed_msg *msg = (struct clockfeed_msg *)hdr;

	assert(module != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		clockfeed_on_run(module);
		SFPTPD_MSG_FREE(msg);
		break;

	case CLOCKFEED_MSG_ADD_CLOCK:
		clockfeed_on_add_clock(module, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case CLOCKFEED_MSG_REMOVE_CLOCK:
		clockfeed_on_remove_clock(module, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	case CLOCKFEED_MSG_SUBSCRIBE:
		clockfeed_on_subscribe(module, msg);
		break;

	case CLOCKFEED_MSG_UNSUBSCRIBE:
		clockfeed_on_unsubscribe(module, msg);
		SFPTPD_MSG_FREE(msg);
		break;

	default:
		WARNING("clockfeed: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void clockfeed_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	struct sfptpd_clockfeed *module = (struct sfptpd_clockfeed *) context;

	assert(module != NULL);
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

struct sfptpd_clockfeed *sfptpd_clockfeed_create(struct sfptpd_thread **threadret)
{
	struct sfptpd_clockfeed *clockfeed;
	int rc;

	assert(threadret);
	assert(!sfptpd_clockfeed);

	TRACE_L3("clockfeed: creating service\n");

	*threadret = NULL;
	clockfeed = (struct sfptpd_clockfeed *) calloc(1, sizeof(*clockfeed));
	if (clockfeed == NULL) {
		CRITICAL("clockfeed: failed to allocate module memory\n");
		return NULL;
	}

	clockfeed->poll_period_log2 = MIN_POLL_PERIOD_LOG2;

	/* Create the service thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("clocks", &clockfeed_thread_ops, clockfeed, threadret);
	if (rc != 0) {
		free(clockfeed);
		errno = rc;
		return NULL;
	}

	clockfeed->thread = *threadret;
	sfptpd_clockfeed = clockfeed;
	return clockfeed;
}

void sfptpd_clockfeed_add_clock(struct sfptpd_clockfeed *clockfeed,
				struct sfptpd_clock *clock,
				int poll_period_log2)
{
	struct clockfeed_msg *msg;

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.add_clock, 0, sizeof(msg->u.add_clock));

	msg->u.add_clock.clock = clock;
	msg->u.add_clock.poll_period_log2 = poll_period_log2;

	(void)SFPTPD_MSG_SEND(msg, clockfeed->thread,
			      CLOCKFEED_MSG_ADD_CLOCK, false);
}

void sfptpd_clockfeed_remove_clock(struct sfptpd_clockfeed *clockfeed,
				   struct sfptpd_clock *clock)
{
	struct clockfeed_msg *msg;

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.remove_clock, 0, sizeof(msg->u.remove_clock));

	msg->u.remove_clock.clock = clock;

	(void)SFPTPD_MSG_SEND(msg, clockfeed->thread,
			      CLOCKFEED_MSG_REMOVE_CLOCK, false);
}

int sfptpd_clockfeed_subscribe(struct sfptpd_clock *clock,
			       const struct sfptpd_clockfeed_shm **shm)
{
	struct clockfeed_msg *msg;
	int rc;

	assert(sfptpd_clockfeed);
	assert(clock);
	assert(shm);

	/* The calling code has an easier life if it can treat a system
	 * clock, i.e. NULL feed, the same as a real one. */
	if (sfptpd_clock_is_system(clock)) {
		*shm = NULL;
		return 0;
	}

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return ENOMEM;
	}

	memset(&msg->u.subscribe_req, 0, sizeof(msg->u.subscribe_req));

	msg->u.subscribe_req.clock = clock;

	rc = SFPTPD_MSG_SEND_WAIT(msg, sfptpd_clockfeed->thread,
				  CLOCKFEED_MSG_SUBSCRIBE);
	if (rc == 0)
		*shm = msg->u.subscribe_resp.shm;

	return rc;
}

void sfptpd_clockfeed_unsubscribe(struct sfptpd_clock *clock)
{
	struct clockfeed_msg *msg;

	assert(sfptpd_clockfeed);
	assert(clock);

	if (sfptpd_clock_is_system(clock))
		return;

	msg = (struct clockfeed_msg *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	memset(&msg->u.unsubscribe, 0, sizeof(msg->u.unsubscribe));

	msg->u.unsubscribe.clock = clock;

	(void)SFPTPD_MSG_SEND(msg, sfptpd_clockfeed->thread,
			      CLOCKFEED_MSG_UNSUBSCRIBE, false);
}

static int clockfeed_compare_to_sys(const struct sfptpd_clockfeed_shm *shm,
				    struct sfptpd_timespec *diff,
				    struct sfptpd_timespec *t1,
				    struct sfptpd_timespec *t2)
	{
	const struct clockfeed_source *feed = const_container_of(shm, struct clockfeed_source, shm);
	const uint32_t index_mask = (1 << SFPTPD_MAX_CLOCK_SAMPLES_LOG2) - 1;
	const struct sfptpd_clockfeed_sample *sample;
	struct sfptpd_clock *clock;
	int writer1;
	int writer2;
	int rc = 0;

	sfptpd_time_zero(diff);

	TRACE_L5("clockfeed: comparing %s (%p shm) to sys\n",
		 sfptpd_clock_get_short_name(feed->clock), shm);

	clock = feed->clock;
	writer1 = shm->write_counter;

	if (writer1 == 0) {
		ERROR("clockfeed: no samples yet obtained from %s\n",
		      sfptpd_clock_get_short_name(clock));
		return EAGAIN;
	}

	sample = &shm->samples[(writer1 - 1) & index_mask];

	if (sample->rc != 0)
		return rc;

	sfptpd_time_subtract(diff, &sample->snapshot, &sample->system);

	/* Check for overrun */
	writer2 = shm->write_counter;
	if (writer2 >= writer1 + SFPTPD_MAX_CLOCK_SAMPLES - 1) {
		ERROR("clockfeed: last sample lost from %s while reading - reader too slow? %lld > %lld + %d\n",
		      sfptpd_clock_get_short_name(clock), writer2, writer1, SFPTPD_MAX_CLOCK_SAMPLES - 1);
		rc = ESTALE;
	}

	if (t1)
		*t1 = sample->snapshot;
	if (t2)
		*t2 = sample->system;

	return rc;
}

int sfptpd_clockfeed_compare(const struct sfptpd_clockfeed_shm *shm1,
			     const struct sfptpd_clockfeed_shm *shm2,
			     struct sfptpd_timespec *diff,
			     struct sfptpd_timespec *t1,
			     struct sfptpd_timespec *t2)
{
	const struct clockfeed_source *feed1 = const_container_of(shm1, struct clockfeed_source, shm);
	const struct clockfeed_source *feed2 = const_container_of(shm2, struct clockfeed_source, shm);
	struct sfptpd_timespec diff2;
	int rc = 0;

	sfptpd_time_zero(diff);

//	if (sfptpd_clock_is_deleted(clock1) || sfptpd_clock_is_deleted(clock2))
//		return ENOENT;

	TRACE_L5("clockfeed: comparing %s (%p shm) %s (%p shm)\n",
		 shm1 ? sfptpd_clock_get_short_name(feed1->clock) : "<>", shm1,
		 shm2 ? sfptpd_clock_get_short_name(feed2->clock) : "<>", shm2);

	if (shm1) {
		rc = clockfeed_compare_to_sys(shm1, diff, t1, shm2 ? NULL : t2);
	}
	if (shm2) {
		rc = clockfeed_compare_to_sys(shm2, &diff2, t2, shm1 ? NULL : t1);
		if (rc == 0)
			sfptpd_time_subtract(diff, diff, &diff2);
	}

	return rc;
}
