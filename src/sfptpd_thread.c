/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2024 Xilinx, Inc. */

/**
 * @file   sfptpd_thread.c
 * @brief  Posix threads with messaging and events
 */

#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/ioctl.h>

#include "sfptpd_logging.h"
#include "sfptpd_thread.h"
#include "sfptpd_time.h"


/****************************************************************************
 * Types & Defines
 ****************************************************************************/

/* Threading library specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 1, x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 2, x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 3, x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 4, x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 5, x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_THREADING, 6, x, ##__VA_ARGS__)

#define SFPTPD_MSG_TRACE(x, ...) DBG_L4("msg_trace %s " x, thread_get_name(), ##__VA_ARGS__)

/* Maximum number of events we handle in the epoll */
#define SFPTPD_THREAD_MAX_EPOLL_EVENTS (32)

#define SFPTPD_EVENT_MAGIC  (0x75315eed)
#define SFPTPD_THREAD_MAGIC (0xf00dface)
#define SFPTPD_ZOMBIE_MAGIC (0x203b111e)
#define SFPTPD_DEAD_MAGIC   (0xdead7ead)

/* Maximum time we will wait for a child thred to exit before giving up. */
#define SFPTPD_JOIN_TIMEOUT       (1000000)
#define SFPTPD_JOIN_POLL_INTERVAL (10000)

/* If a timer expires more times than this without ticking, warn the user.
 * bug77937: This value should be 1 but due to the way the PTP thread blocks
 * when collecting transmit timestamps it is possible to miss two timers in
 * normal operation if transmit timestamping is not working. */
#define TIMER_EXPIRIES_WARN_THRESH (2)

enum thread_event_type {
	THREAD_EVENT_TIMER,
	THREAD_EVENT_EVENT,
};

/* This is a helper structure around a standard posix pipe. */
struct sfptpd_pipe
{
	/* Read and write indices for pipe */
#define PIPE_READ_IDX (0)
#define PIPE_WRITE_IDX (1)

	/* Size of items to be fed through pipe */
	size_t item_size;

	/* Pipe file descriptors */
	int fds[2];
};


#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ (F_LINUX_SPECIFIC_BASE + 7)
#endif

#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ (F_LINUX_SPECIFIC_BASE + 8)
#endif


/* User event structure */
struct sfptpd_event
{
	/* Magic number and link to next event */
	uint32_t magic;
	struct sfptpd_event *next;

	/* Type of event */
	enum thread_event_type type;

	/* User assigned ID for the event */
	sfptpd_event_id_t id;

	/* Event file descriptor */
	int fd;

	/* Function to call when timer occurs */
	sfptpd_thread_on_event_fn on_event;

	/* User context to pass in to event fn */
	void *user_context;
};


/* Message queue */
struct sfptpd_queue
{
	/* Pipe used to implement queue */
	struct sfptpd_pipe pipe;
};


/* Message pool */
struct sfptpd_pool
{
	/* Name of message pool */
	const char *name;

	/* Queue containing free messages */
	struct sfptpd_queue free;

	/* Message memory */
	uint8_t *buffer;

	pthread_mutex_t stats_lock;
	int free_count;
	int used_count;
};


/* Thread context */
struct sfptpd_thread
{
	/* Magic number and link to the next thread object */
	uint32_t magic;
	struct sfptpd_thread *next;
	struct sfptpd_thread *next_zombie;

	/* Textual name for thread */
	const char *name;

	/* Handle of parent thread */
	struct sfptpd_thread *parent;

	/* Posix thread handle */
	pthread_t pthread;

	/* Message used to signal that a thread has completed the startup
	 * stage. This is used to send the response only part of a send-wait. */
	sfptpd_msg_thread_startup_status_t startup_status;

	/* User handlers and context. Note that the signal handler is only used
	 * by the root thread. */
	struct sfptpd_thread_ops ops;
	sfptpd_thread_on_signal_fn on_signal;
	void *user_context;

	/* Epoll instance */
	int epoll_fd;

	/* Exit event */
	int exit_event_fd;

	/* Signal file descriptor */
	int signal_fd;

	/* Used to record thread exit code */
	int exit_errno;

	/* Local message pool */
	struct sfptpd_pool msg_pool;

	/* Queue used to receive general messages */
	struct sfptpd_queue queue_general;

	/* Queue used only to handle replies to blocking sends */
	struct sfptpd_queue queue_wait_reply;

	/* Events, including timers */
	struct sfptpd_event *event_list;
};


/* Threading library context */
struct sfptpd_thread_lib
{
	/* Key used for storing per-thread context */
	pthread_key_t key;

	/* Used to restore signal mask when signal library exits */
	sigset_t original_signal_set;

	/* Policy on when to reap zombie threads */
	enum sfptpd_thread_zombie_policy zombie_policy;

	/* Global message pool */
	struct sfptpd_pool global_msg_pool;

	/* Realtime stats message pool */
	struct sfptpd_pool rt_stats_msg_pool;

	/* Handle of root thread i.e. the process entry thread */
	struct sfptpd_thread *root_thread;

	/* Thread list */
	struct sfptpd_thread *thread_list;

	/* Zombie list */
	struct sfptpd_thread *zombie_list;
};


/****************************************************************************
 * Constant Data
 ****************************************************************************/


/****************************************************************************
 * Local Data
 ****************************************************************************/

static struct sfptpd_thread_lib sfptpd_thread_lib;


/****************************************************************************
 * Prototypes
 ****************************************************************************/

static const char *thread_get_name(void);


/****************************************************************************
 * Pipe
 ****************************************************************************/

static int pipe_size(const struct sfptpd_pipe *pipe)
{
	int sz = fcntl(pipe->fds[PIPE_READ_IDX], F_GETPIPE_SZ);
	if (sz == -1)
		DBG_L1("thread %s: failed to get pipe size, %s.\n",
		       thread_get_name(), strerror(errno));
	return sz;
}


static int pipe_set_size(const struct sfptpd_pipe *pipe, int size)
{
	int rc = fcntl(pipe->fds[0], F_SETPIPE_SZ, size);
	/* F_SETPIPE_SZ returns new pipe capacity, not 0 as stated in
	 * older documentation. */
	if (rc < size) {
		if (rc < 0) {
			ERROR("thread %s: failed to set pipe %d/%d size to %d, %s\n",
			      thread_get_name(), pipe->fds[0], pipe->fds[1], size,
			      strerror(errno));
			rc = errno;
		} else {
			/* Unexpected, sanity check only. */
			ERROR("thread %s: failed to set pipe %d/%d size to %d, got %d\n",
			      thread_get_name(), pipe->fds[0], pipe->fds[1], size, rc);
			rc = ENOSPC;
		}
	} else {
		SFPTPD_MSG_TRACE("thread %s: set pipe %d/%d size to %d\n",
		                 thread_get_name(), pipe->fds[0], pipe->fds[1], rc);
		rc = 0;
	}
	return rc;
}


static int pipe_resize_for_queue(const struct sfptpd_pipe *pipe, int max_queue_size)
{
	int pagesz = sysconf(_SC_PAGESIZE);
	int pipe_pages_req;

	/* SWPTP-902: Writes to pipe will block after 1 whole pipe's worth of
	 * data have been written if the first page of the pipe has not been
	 * completely drained by reader.  To avoid this, ensure pipe is at least
	 * one page bigger than size relating to maximum queue level. If unknown
	 * (0 size), assume it will queue no more than 1 page of data. */
	if (max_queue_size == 0)
		max_queue_size = pagesz;
	pipe_pages_req = 1 + (max_queue_size + pagesz - 1) / pagesz;

	int pipesz = pipe_size(pipe);
	/* SWPTP-911: older kernels (e.g. rh6) don't support this call. At
	 * least we tried... */
	if (pipesz < 0) {
		static bool warned = false;
		if (!warned) {
			INFO("F_GETPIPE_SZ is not supported by your kernel. Sfptpd may hang if your pipes are configured to be backed by a single page (SWPTP-902).\n");
			warned = true;
		}
		return 0;
	}

	if (pipesz < pipe_pages_req * pagesz) {
		int rc = pipe_set_size(pipe, pipe_pages_req * pagesz);
		if (rc != 0) {
			ERROR("thread %s: failed to resize pipe, queue needs %d pages, rc=%d\n",
			      thread_get_name(), pipe_pages_req, rc);
			return rc;
		}
	}
	return 0;
}


static int pipe_create(struct sfptpd_pipe *new, size_t item_size, bool writes_block)
{
	int rc, flags;
	assert(new != NULL);
	assert(item_size != 0);

	rc = pipe(new->fds);
	if (rc != 0) {
		CRITICAL("thread %s: failed to create pipe, %s\n",
			 thread_get_name(), strerror(errno));
		return errno;
	}

	/* Clear the blocking flag for the read end of the pipe. */
	flags = fcntl(new->fds[PIPE_READ_IDX], F_GETFL, 0);
	if (flags == -1) {
		ERROR("thread %s: failed to get pipe read flags, %s\n",
		      thread_get_name(), strerror(errno));
		goto fail;
	}

	flags |= O_NONBLOCK;
	if (fcntl(new->fds[PIPE_READ_IDX], F_SETFL, flags) == -1) {
		ERROR("thread %s: failed to set pipe read flags, %s\n",
		      thread_get_name(), strerror(errno));
		goto fail;
	}

	/* If requested, clear the blocking flag for the write end of the pipe. */
	if (!writes_block) {
		flags = fcntl(new->fds[PIPE_WRITE_IDX], F_GETFL, 0);
		if (flags == -1) {
			ERROR("thread %s: failed to get pipe write flags, %s\n",
			      thread_get_name(), strerror(errno));
			goto fail;
		}

		flags |= O_NONBLOCK;
		if (fcntl(new->fds[PIPE_WRITE_IDX], F_SETFL, flags) == -1) {
			ERROR("thread %s: failed to set pipe write flags, %s\n",
			      thread_get_name(), strerror(errno));
			goto fail;
		}
	}

	new->item_size = item_size;
	SFPTPD_MSG_TRACE("thread %s: created pipe %d/%d size %d\n",
	                 thread_get_name(),
	                 new->fds[PIPE_READ_IDX], new->fds[PIPE_WRITE_IDX],
	                 pipe_size(new));
	return 0;

fail:
	close(new->fds[PIPE_READ_IDX]);
	close(new->fds[PIPE_WRITE_IDX]);
	return errno;
}


static void pipe_destroy(struct sfptpd_pipe *pipe)
{
	close(pipe->fds[PIPE_READ_IDX]);
	close(pipe->fds[PIPE_WRITE_IDX]);
}


static int pipe_write(struct sfptpd_pipe *pipe, const void *data, size_t count)
{
	ssize_t bytes_written;

	assert(pipe != NULL);
	assert(data != NULL);
	assert(count == pipe->item_size);

	/* Write the data to the pipe */
	bytes_written = write(pipe->fds[PIPE_WRITE_IDX], data, count);
	if (bytes_written != count) {
		/* We expect to write all the data or nothing. */
		if (bytes_written >= 0)
			errno = ERANGE;
		DBG_L1("thread %s: failed to write to pipe, %zd, %s\n",
		       thread_get_name(), bytes_written, strerror(errno));
		return errno;
	}

	return 0;
}


static int pipe_read(struct sfptpd_pipe *pipe, void *buffer, size_t count,
		     bool wait)
{
	int rc;
	ssize_t bytes_read;
	fd_set read_fds;

	assert(pipe != NULL);
	assert(buffer != NULL);
	assert(count == pipe->item_size);

	/* If we are waiting for the pipe to contain something, select until
	 * the pipe is non empty. */
	if (wait) {
		FD_ZERO(&read_fds);
		FD_SET(pipe->fds[PIPE_READ_IDX], &read_fds);

		/* Select until we don't get interrupted. Note that this could
		 * in theory make the application fail to handle signals
		 * correctly if we get stuck here due to app misbehaviour. In
		 * practise this is very unlikely. */
		do {
			rc = select(pipe->fds[PIPE_READ_IDX] + 1, &read_fds,
				    NULL, NULL, NULL);
		} while ((rc == -1) && (errno == EINTR));

		/* If we got an error, then it's serious, return */
		if (rc == -1) {
			ERROR("thread %s: error waiting for pipe, %s\n",
			      thread_get_name(), strerror(errno));
			return errno;
		}
	}

	bytes_read = read(pipe->fds[PIPE_READ_IDX], buffer, count);

	/* If we haven't waited and there are no messages available this is an
	 * expected error. */
	if (!wait && (bytes_read == -1) && (errno == EAGAIN))
		return errno;

	if (bytes_read != count) {
		/* We expect to read all the data or nothing */
		if (bytes_read >= 0)
			errno = ERANGE;
		WARNING("thread %s: failed to read from pipe, %zd, %s\n",
			thread_get_name(), bytes_read, strerror(errno));
		return errno;
	}

	return 0;
}


static int pipe_get_read_fd(struct sfptpd_pipe *pipe)
{
	assert(pipe != NULL);
	return pipe->fds[PIPE_READ_IDX];
}


/****************************************************************************
 * Message Queues
 ****************************************************************************/

static int queue_create(struct sfptpd_queue *queue)
{
	int rc;
	assert(queue != NULL);
	rc = pipe_create(&queue->pipe, sizeof(sfptpd_msg_hdr_t *), true);
	if (rc == 0)
		rc = pipe_resize_for_queue(&queue->pipe, 0);
	return rc;
}


static void queue_destroy(struct sfptpd_queue *queue, bool free_messages)
{
	sfptpd_msg_hdr_t *msg;

	assert(queue != NULL);

	if (free_messages) {
		while (pipe_read(&queue->pipe, &msg, sizeof(msg), false) == 0) {
			/* Free the message */
			sfptpd_msg_free(msg);
		}
	}

	pipe_destroy(&queue->pipe);
}


static int queue_send(struct sfptpd_queue *queue, sfptpd_msg_hdr_t *msg)
{
	int rc;

	assert(queue != NULL);
	assert(msg != NULL);

	rc = pipe_write(&queue->pipe, &msg, sizeof(msg));
	if (rc != 0) {
		WARNING("thread %s: failed to send to queue, %s\n",
			thread_get_name(), strerror(rc));
		return errno;
	}

	return 0;
}


static int queue_receive(struct sfptpd_queue *queue, sfptpd_msg_hdr_t **msg,
			 bool wait)
{
	int rc;

	assert(queue != NULL);
	assert(msg != NULL);

	rc = pipe_read(&queue->pipe, msg, sizeof(*msg), wait);
	if (rc != 0) {
		if (wait || (rc != EAGAIN))
			WARNING("thread %s: failed to receive from queue, %s\n",
				thread_get_name(), strerror(rc));
		return errno;
	}

	return 0;
}


static int queue_get_read_fd(struct sfptpd_queue *queue)
{
	assert(queue != NULL);
	return pipe_get_read_fd(&queue->pipe);
}


/****************************************************************************
 * Message Pools
 ****************************************************************************/

static int pool_allocate(struct sfptpd_pool *pool,
			 unsigned int num_msgs, unsigned int msg_size)
{
	uint8_t *buf;
	unsigned int i;
	int rc;
	struct sfptpd_msg_hdr *msg;

	assert(pool != NULL);
	assert(num_msgs != 0);
	assert(msg_size >= sizeof(sfptpd_msg_hdr_t));
	
	if (pool->buffer != NULL) {
		ERROR("thread %s: message pool %s already allocated\n",
		      thread_get_name(), pool->name);
		return EALREADY;
	}

	rc = pipe_resize_for_queue(&pool->free.pipe, num_msgs * sizeof(msg));
	if (rc != 0) {
		CRITICAL("thread %s message pool %s: failed to resize pipe\n",
		         thread_get_name(), pool->name);
		return rc;
	}

	/* Round the message size to the next word boundary */
	msg_size = (msg_size + 3) & ~3;

	pool->buffer = (uint8_t *)calloc(num_msgs, msg_size);
	if (pool->buffer == NULL) {
		CRITICAL("thread %s message pool %s: failed to allocate memory for messages\n",
			 thread_get_name(), pool->name);
		return ENOMEM;
	}

	SFPTPD_MSG_TRACE("pool %s create @ %p len %u pipe %d/%d\n",
			pool->name, pool->buffer, num_msgs * msg_size,
			pool->free.pipe.fds[0], pool->free.pipe.fds[1]);

	/* Initialise each message and add to the free queue */
	for (i = 0, buf = pool->buffer; i < num_msgs; i++) {
		msg = (sfptpd_msg_hdr_t *)buf;
		sfptpd_msg_init(msg, msg_size - sizeof(sfptpd_msg_hdr_t));
		msg->free = &pool->free;
		msg->pool = pool;
		rc = queue_send(&pool->free, msg);
		assert(rc == 0);
		pool->free_count++;

		buf += msg_size;
	}

	DBG_L2("thread %s message pool %s: allocated pool with %d messages of size %d\n",
	       thread_get_name(), pool->name, num_msgs, msg_size);
	return 0;
}


static int pool_create(struct sfptpd_pool *pool, const char *name,
		       unsigned int num_msgs, unsigned int msg_size)
{
	int rc;
	assert(pool != 0);
	assert(name != 0);

	/* Set message pool name */
	pool->name = name;
	pool->buffer = NULL;
	pthread_mutex_init(&pool->stats_lock, NULL);
	pool->free_count = 0;
	pool->used_count = 0;


	rc = queue_create(&pool->free);
	if (rc != 0) {
		CRITICAL("thread %s: failed to create message pool %s, %s\n",
			 thread_get_name(), name, strerror(rc));
		return rc;
	}

	if (num_msgs == 0) {
		DBG_L3("thread %s: created empty message pool %s, pipe %d/%d\n",
		       thread_get_name(), name,
		       pool->free.pipe.fds[0], pool->free.pipe.fds[1]);
		return 0;
	}

	rc = pool_allocate(pool, num_msgs, msg_size);
	if (rc != 0) {
		queue_destroy(&pool->free, false);
		return rc;
	}

	DBG_L2("thread %s: created message pool %s, pipe %d/%d\n",
	       thread_get_name(), name,
	       pool->free.pipe.fds[0], pool->free.pipe.fds[1]);
	return 0;
}


static void pool_destroy(struct sfptpd_pool *pool)
{
	assert(pool != NULL);

	/* Deleting the queue but don't free (send to free queue) messages
	 * queued on it. */
	queue_destroy(&pool->free, false);

	pthread_mutex_destroy(&pool->stats_lock);

	if (pool->buffer != NULL) {
		free(pool->buffer);
		pool->buffer = NULL;
	}

	DBG_L3("thread %s: destroyed message pool %s\n",
	       thread_get_name(), pool->name);
}


static int pool_receive(struct sfptpd_pool *pool, sfptpd_msg_hdr_t **msg,
			bool wait)
{
	assert(pool != NULL);
	return queue_receive(&pool->free, msg, wait);
}


/****************************************************************************
 * Event Functions
 ****************************************************************************/

static int event_get_id(struct sfptpd_event *event)
{
	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);
	return event->id;
}


static int event_get_fd(struct sfptpd_event *event)
{
	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);
	return event->fd;
}


static void event_set_next(struct sfptpd_event *event, struct sfptpd_event *next)
{
	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);
	event->next = next;
}


static struct sfptpd_event *event_get_next(struct sfptpd_event *event)
{
	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);
	return event->next;
}


static struct sfptpd_event *event_find_by_id(struct sfptpd_thread *thread,
					     sfptpd_event_id_t event_id)
{
	struct sfptpd_event *event;

	assert(thread);

	for (event = thread->event_list; event != NULL; event = event_get_next(event))
		if (event_get_id(event) == event_id)
			return event;

	return NULL;
}


static struct sfptpd_event *thread_event_find_by_id(sfptpd_event_id_t event_id)
{
	return event_find_by_id(sfptpd_thread_self(), event_id);
}


static int thread_event_check_type(struct sfptpd_event *event,
				   enum thread_event_type type)
{
	if (event == NULL)
		return ENOENT;
	else if (event->type != type)
		return EINVAL;
	else
		return 0;
}


static const char *thread_event_type(enum thread_event_type type)
{
	switch (type) {
	case THREAD_EVENT_TIMER:
		return "timer";
	case THREAD_EVENT_EVENT:
		return "event";
	default:
		return "invalid";
	}
}


static int event_create(sfptpd_event_id_t event_id,
			enum thread_event_type event_type,
			clockid_t clock_id,
			sfptpd_thread_on_event_fn on_event,
			void *user_context,
			struct sfptpd_event **event)
{
	struct sfptpd_event *new;
	const char *type;

	assert(on_event != NULL);
	assert(event != NULL);

	type = thread_event_type(event_type);
	new = (struct sfptpd_event *)calloc(1, sizeof(*new));
	if (new == NULL)
		return ENOMEM;

	if (event_type == THREAD_EVENT_TIMER) {
		new->fd = timerfd_create(clock_id, TFD_NONBLOCK);
	} else {
		assert(event_type == THREAD_EVENT_EVENT);
		new->fd = eventfd(0, EFD_NONBLOCK);
	}
	if (new->fd < 0) {
		ERROR("thread %s %s %d: failed to create %sfd, %s\n",
		      thread_get_name(), type, event_id, type, strerror(errno));
		goto fail1;
	}

	new->magic = SFPTPD_EVENT_MAGIC;
	new->next = NULL;
	new->id = event_id;
	new->on_event = on_event;
	new->user_context = user_context;
	new->type = event_type;

	DBG_L3("thread %s: created %s %d with fd %d\n",
	       thread_get_name(), type, event_id, new->fd);
	*event = new;
	return 0;

fail1:
	free(new);
	*event = NULL;
	return errno;
}


static void event_destroy(struct sfptpd_event *event)
{
	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);

	DBG_L3("thread %s %s %d: destroyed\n",
	       thread_get_name(),
	       thread_event_type(event->type), event->id);

	/* Close the timer handle and free the memory */
	(void)close(event->fd);

	/* Clear the magic value and free the memory */
	event->magic = 0;
	free(event);
}


static int thread_event_create(unsigned int event_id,
			       enum thread_event_type event_type,
			       clockid_t clock_id,
			       sfptpd_thread_on_event_fn on_event,
			       void *user_context)
{
	struct sfptpd_thread *self;
	struct sfptpd_event *source;
	struct epoll_event event;
	const char *type;
	int rc;

	assert(on_event != NULL);

	type = thread_event_type(event_type);
	self = sfptpd_thread_self();

	/* Fail if the timer already exists. */
	if (thread_event_find_by_id(event_id))
		return EALREADY;

	rc = event_create(event_id, event_type,
			  clock_id, on_event, user_context, &source);
	if (rc != 0) {
		ERROR("thread %s: failed to create %s %u, %s\n",
		      self->name, type, event_id, strerror(rc));
		return rc;
	}

	memset(&event, 0, sizeof event);
	event.events = EPOLLIN;
	event.data.fd = event_get_fd(source);
	rc = epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
	if (rc != 0) {
		ERROR("thread %s: failed to add %s %u fd %d to epoll, %s\n",
		      self->name, type,
		      event.data.fd, strerror(errno));
		event_destroy(source);
		return errno;
	}

	/* Add the event to the event list for the thread */
	event_set_next(source, self->event_list);
	self->event_list = source;
	return 0;
}


static int timer_start(struct sfptpd_event *timer, bool periodic,
		       bool absolute, const struct sfptpd_timespec *interval)
{
	struct itimerspec timer_spec;
	int flags = 0;

	assert(timer != NULL);
	assert(timer->magic == SFPTPD_EVENT_MAGIC);
	assert(timer->type == THREAD_EVENT_TIMER);
	assert(interval != NULL);

	if (absolute)
		flags |= TIMER_ABSTIME;

	sfptpd_time_to_std_floor(&timer_spec.it_value, interval);
	sfptpd_time_to_std_floor(&timer_spec.it_interval, periodic ? interval : &SFPTPD_NULL_TIME);

	if (timerfd_settime(timer->fd, flags, &timer_spec, NULL) < 0) {
		ERROR("thread %s timer %d: failed to start timer, %s\n",
		      thread_get_name(), timer->id, strerror(errno));
		return errno;
	}

	DBG_L5("thread %s timer %d: started with interval "
	       SFPTPD_FMT_SFTIMESPEC SFPTPD_FORMAT_TIMESPEC "\n",
	       thread_get_name(), timer->id,
	       SFPTPD_ARGS_TIMESPEC(timer_spec.it_value),
	       SFPTPD_ARGS_TIMESPEC(timer_spec.it_interval));
	return 0;
}


static int timer_stop(struct sfptpd_event *timer)
{
	struct itimerspec timer_spec;

	assert(timer != NULL);
	assert(timer->magic == SFPTPD_EVENT_MAGIC);
	assert(timer->type == THREAD_EVENT_TIMER);

	timer_spec.it_value.tv_sec = 0;
	timer_spec.it_value.tv_nsec = 0;
	timer_spec.it_interval.tv_sec = 0;
	timer_spec.it_interval.tv_nsec = 0;

	if (timerfd_settime(timer->fd, 0, &timer_spec, NULL) < 0) {
		ERROR("thread %s timer %d: failed to stop timer, %s\n",
		      thread_get_name(), timer->id, strerror(errno));
		return errno;
	}

	DBG_L3("timer %d: stopped\n", timer->id);
	return 0;
}


static int timer_get_time_left(struct sfptpd_event *timer, struct sfptpd_timespec *interval)
{
	struct itimerspec timer_spec;

	assert(timer != NULL);
	assert(timer->magic == SFPTPD_EVENT_MAGIC);
	assert(timer->type == THREAD_EVENT_TIMER);
	assert(interval != NULL);

	if (timerfd_gettime(timer->fd, &timer_spec) != 0) {
		ERROR("thread %s timer %d: failed to get time, %s\n",
		      thread_get_name(), timer->id, strerror(errno));
		return errno;
	}

	sfptpd_time_from_std_floor(interval, &timer_spec.it_value);
	return 0;
}


static void timer_on_expiry(struct sfptpd_event *timer)
{
	uint64_t expirations;
	ssize_t result;

	assert(timer != NULL);
	assert(timer->magic == SFPTPD_EVENT_MAGIC);
	assert(timer->type == THREAD_EVENT_TIMER);

	result = read(timer->fd, &expirations, sizeof(expirations));
	if (result == -1) {
		if (errno == EAGAIN) {
			WARNING("thread %s timer %d: fd unexpectedly ready when not yet expired\n",
				thread_get_name(), timer->id);
			return;
		} else if (errno == ECANCELED) {
			WARNING("thread %s timer %d: detected discontinuity in clock\n",
				thread_get_name(), timer->id);
		} else {
			WARNING("thread %s timer %d: error reading timer expiry count, %s\n",
				thread_get_name(), timer->id, strerror(errno));
		}
	} else if (result != sizeof(uint64_t)) {
		WARNING("thread %s timer %d: read unexpected length from timer fd, %zd\n",
			thread_get_name(), timer->id, result);
	} else if (expirations > TIMER_EXPIRIES_WARN_THRESH) {
		WARNING("thread %s timer %d: expired %d times since last handled\n",
			thread_get_name(), timer->id, expirations);
	}

	timer->on_event(timer->user_context, timer->id);
}


static void event_on_event(struct sfptpd_event *event)
{
	uint64_t count;
	ssize_t result;

	assert(event != NULL);
	assert(event->magic == SFPTPD_EVENT_MAGIC);
	assert(event->type == THREAD_EVENT_EVENT);

	result = read(event->fd, &count, sizeof(count));
	if (result == -1) {
		if (errno == EAGAIN) {
			WARNING("thread %s event %d: fd unexpectedly ready when not yet fired\n",
				thread_get_name(), event->id);
			return;
		} else {
			WARNING("thread %s event %d: error reading event count, %s\n",
				thread_get_name(), event->id, strerror(errno));
		}
	} else if (result != sizeof(uint64_t)) {
		WARNING("thread %s event %d: read unexpected length from event fd, %zd\n",
			thread_get_name(), event->id, result);
	} else if (count > TIMER_EXPIRIES_WARN_THRESH) {
		WARNING("thread %s event %d: fired %d times since last handled\n",
			thread_get_name(), event->id, count);
	}

	event->on_event(event->user_context, event->id);
}


/****************************************************************************
 * Message Functions
 ****************************************************************************/

void sfptpd_msg_init(sfptpd_msg_hdr_t *msg, unsigned int capacity)
{
	assert(msg != NULL);

	msg->reply = NULL;
	msg->free = NULL;
	msg->pool = NULL;
	msg->id = 0;
	msg->capacity = capacity;

}


sfptpd_msg_hdr_t *sfptpd_msg_alloc(enum sfptpd_msg_pool_id pool_id, bool wait)
{
	sfptpd_msg_hdr_t *msg;
	struct sfptpd_pool *pool;
	int rc;

	assert(pool_id < SFPTPD_MSG_POOL_MAX);
	if (pool_id == SFPTPD_MSG_POOL_GLOBAL)
		pool = &sfptpd_thread_lib.global_msg_pool;
	else if (pool_id == SFPTPD_MSG_POOL_RT_STATS)
		pool = &sfptpd_thread_lib.rt_stats_msg_pool;
	else
		pool = &(sfptpd_thread_self()->msg_pool);

	rc = pool_receive(pool, &msg, wait);
	if (rc != 0)
		msg = NULL;
	else {
		pthread_mutex_lock(&pool->stats_lock);
		pool->free_count--;
		pool->used_count++;
		pthread_mutex_unlock(&pool->stats_lock);
	}
	SFPTPD_MSG_TRACE("pool %s alloc %p rc %d use/free %d/%d\n", pool->name, msg, rc, pool->used_count, pool->free_count);

	return msg;
}


void sfptpd_msg_alloc_failed(const char *pool, const char *file, const char *func, int line)
{
	ERROR("failed to allocate msg from %s pool in %s:%d (%s)\n", pool, func, line, file);
	/* TODO: can we include any useful debug information here? */
}

void sfptpd_msg_free(sfptpd_msg_hdr_t *msg)
{
	assert(msg != NULL);

	if (msg->free == NULL) {
		DBG_L1("thread %s trying to free non-pool message- ignoring\n",
			thread_get_name());
		return;
	}

	msg->reply = NULL;
	msg->id = 0;

	if (msg->pool != NULL) {
		struct sfptpd_pool *pool = msg->pool;
		pthread_mutex_lock(&pool->stats_lock);
		pool->free_count++;
		pool->used_count--;
		pthread_mutex_unlock(&pool->stats_lock);
		SFPTPD_MSG_TRACE("msg %p free to %d use/free %d/%d\n", msg,
				!msg->free ? -1 : msg->free->pipe.fds[PIPE_WRITE_IDX],
				pool->used_count, pool->free_count);
	} else
		SFPTPD_MSG_TRACE("msg %p free to %d\n", msg,
				!msg->free ? -1 : msg->free->pipe.fds[PIPE_WRITE_IDX]);

	(void)queue_send(msg->free, msg);
}


int sfptpd_msg_send(sfptpd_msg_hdr_t *msg, struct sfptpd_thread *recipient,
		    uint32_t id, bool needs_reply)
{
	int reply_fd;
	int rc;

	assert(msg != NULL);
	assert(recipient != NULL);
	assert(recipient->magic == SFPTPD_THREAD_MAGIC);

	/* We do not allow messages allocated on the stack to be sent
	 * asynchronously. Make sure this message is the correct type */
	assert(msg->free != NULL);

	/* Set the message ID */
	msg->id = id;

	/* If the message needs a reply, set the reply queue to our general
	 * queue. Otherwise, set the reply queue to NULL */
	if (needs_reply) {
		struct sfptpd_thread *self = sfptpd_thread_self();
		assert(self);
		msg->reply = &self->queue_general;

		/* save reply fd for use in diagnostic message to avoid race */
		reply_fd = msg->reply->pipe.fds[PIPE_WRITE_IDX];
	} else {
		msg->reply = NULL;
		reply_fd = -1;
	}

	rc = queue_send(&recipient->queue_general, msg);

	SFPTPD_MSG_TRACE("msg %p send %d reply %d rc %d\n", msg,
			recipient->queue_general.pipe.fds[PIPE_WRITE_IDX],
			reply_fd, rc);

	/* If we failed to send the message, clear the reply queue, and send
	 * the message back to its free queue. */
	if (rc != 0) {
		msg->reply = NULL;
		(void)queue_send(msg->free, msg);
	}
	return rc;
}


int sfptpd_msg_send_wait(sfptpd_msg_hdr_t *msg,
			 struct sfptpd_thread *recipient, uint32_t id)
{
	struct sfptpd_thread *self;
	sfptpd_msg_hdr_t *reply;
	int reply_fd;
	int rc;

	assert(msg != NULL);
	assert(recipient != NULL);
	assert(recipient->magic == SFPTPD_THREAD_MAGIC);

	self = sfptpd_thread_self();
	assert(self);

	/* Set the message ID */
	msg->id = id;

	/* Set the reply queue to the priority wait reply queue. This is only
	 * used for send-wait operations so we can guarantee that no other
	 * message will turn up here while we are waiting for the response */
	msg->reply = &self->queue_wait_reply;
	reply_fd = msg->reply->pipe.fds[PIPE_WRITE_IDX];

	rc = queue_send(&recipient->queue_general, msg);

	SFPTPD_MSG_TRACE("msg %p send %d reply %d wait rc %d\n", msg,
			recipient->queue_general.pipe.fds[PIPE_WRITE_IDX],
			reply_fd, rc);

	if (rc == 0) {
		rc = queue_receive(&self->queue_wait_reply, &reply, true);
		SFPTPD_MSG_TRACE("msg %p replied rc %d\n", msg, rc);
		/* If we got back a different message from the one that we sent,
		 * we need to get worried */
		if ((rc == 0) && (msg != reply))
			ERROR("thread %s: message send wait returned different reply message %p, %p\n",
			      thread_get_name(), msg, reply);
	}

	/* We've either failed or succeed and had a reply. Clear the reply
	 * queue for the message */
	msg->reply = NULL;
	return rc;
}


int sfptpd_msg_reply(sfptpd_msg_hdr_t *msg)
{
	struct sfptpd_queue *queue;
	int rc;

	assert(msg != NULL);

	/* If the message doesn't need a reply, just free it */
	if (msg->reply == NULL) {
		if (msg->free != NULL)
			sfptpd_msg_free(msg);
		return 0;
	}

	/* Send the message to the reply queue but first clear the reply queue
	 * to ensure that it is freed later, not sent to the same queue again! */
	queue = msg->reply;
	msg->reply = NULL;
	rc = queue_send(queue, msg);

	SFPTPD_MSG_TRACE("msg %p reply %d rc %d\n", msg, queue->pipe.fds[PIPE_WRITE_IDX], rc);

	/* If the send failed, then re-instate the reply queue value */
	if (rc != 0)
		msg->reply = queue;

	return rc;
}


/****************************************************************************
 * Threading - Internal Functions
 ****************************************************************************/

struct sfptpd_thread *thread_self(void)
{
	struct sfptpd_thread *self
		= (struct sfptpd_thread *)pthread_getspecific(sfptpd_thread_lib.key);

	assert(self == NULL || self->magic == SFPTPD_THREAD_MAGIC);
	return self;
}


static const char *thread_get_name(void)
{
	struct sfptpd_thread *self
		= (struct sfptpd_thread *)pthread_getspecific(sfptpd_thread_lib.key);

	/* This function is used to get the thread name when logging trace
	 * messages. Normally if self is not null then the magic value
	 * should be correct but there is one scenario there this is not the
	 * case. During the library initialisation, the global message pool
	 * is created from outside of an sfptpd thread context and may use
	 * this function to output trace messages. The value of
	 * pthread_getspecific() will be undefined in this case. */
	if ((self != NULL) && (self->magic == SFPTPD_THREAD_MAGIC))
		return self->name;

	return "null";
}


static void thread_exit_notify(struct sfptpd_thread *thread, int rc)
{
	sfptpd_msg_thread_exit_notify_t *msg;

	/* This routine must only be called for the current thread */
	assert(thread == sfptpd_thread_self());

	/* Only send a notify message if we have a parent => this doesn't
	 * happen for the root thread. */
	if (thread->parent == NULL)
		return;

	/* Send a message to parent of the current thread to signal that the
	 * thread has exited. */
	msg = (sfptpd_msg_thread_exit_notify_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
								  false);
	if (msg == NULL) {
		ERROR("thread %s: failed to allocate exit notify message\n",
		      thread->name);
		return;
	}

	msg->thread = thread;
	msg->exit_code = rc;
	SFPTPD_MSG_SEND(msg, thread->parent, SFPTPD_MSG_ID_THREAD_EXIT_NOTIFY, false);
}


static int thread_on_possible_event(struct sfptpd_thread *thread, unsigned int fd)
{
	struct sfptpd_event *event;

	/* This routine must only be called for the current thread */
	assert(thread == sfptpd_thread_self());

	for (event = thread->event_list; event != NULL; event = event_get_next(event)) {
		if (event_get_fd(event) == fd) {
			if (event->type == THREAD_EVENT_TIMER)
				timer_on_expiry(event);
			else
				event_on_event(event);
			return 0;
		}
	}

	return ENOENT;
}


static void thread_on_signal(struct sfptpd_thread *thread)
{
	struct signalfd_siginfo signal;
	ssize_t result;

	/* This routine must only be called for the current thread */
	assert(thread == sfptpd_thread_self());
	assert(thread->signal_fd != -1);

	while (true) {
		result = read(thread->signal_fd, &signal, sizeof(signal));

		if (result == -1) {
			if (errno != EAGAIN) {
				WARNING("thread %s: read from signalfd returned "
					"unexpected error %s\n",
					thread->name, strerror(errno));
			}
			return;
		}

		if (result != sizeof(signal)) {
			WARNING("thread %s: read from signalfd returned "
				"unexpected length %zd\n",
				thread->name, result);
			return;
		}

		if (thread->on_signal != NULL) {
			thread->on_signal(thread->user_context, signal.ssi_signo);
		} else {
			ERROR("thread %s: received unexpected signal %d\n",
			      thread->name, signal.ssi_signo);
		}
	}
}


static void thread_on_message_event(struct sfptpd_thread *thread)
{
	sfptpd_msg_hdr_t *msg;

	/* This routine must only be called for the current thread */
	assert(thread == sfptpd_thread_self());

	/* Try to receive a message without waiting. For each message received,
	 * call the user handler to process it. */
	while (queue_receive(&thread->queue_general, &msg, false) == 0) {
		SFPTPD_MSG_TRACE("msg %p recv on %d\n", msg, thread->queue_general.pipe.fds[PIPE_READ_IDX]);
		thread->ops.on_message(thread->user_context, msg);
	}
}


static void *thread_entry(void *arg)
{
	struct sfptpd_thread *thread = (struct sfptpd_thread *)arg;
	int rc, fd;
	bool exit;

	/* Set the thread specific data to allow message threading to work.
	 * Then call the user entry point for the thread passing in the
	 * context required */
	rc = pthread_setspecific(sfptpd_thread_lib.key, thread);
	if (rc != 0) {
		CRITICAL("thread %s: failed to set thread specific data, %s\n",
			 thread->name, strerror(rc));
	} else {
		/* Call the startup function to perform any thread-specific
		 * initialisation */
		rc = thread->ops.on_startup(thread->user_context);
		if (rc != 0)
			DBG_L1("thread %s: user startup routine failed, %s\n",
			       thread->name, strerror(rc));
	}

	/* Send a 'reply' to the parent indicating the startup status */
	thread->startup_status.status_code = rc;
	SFPTPD_MSG_REPLY(&thread->startup_status);

	/* If we failed, jump to the exit handling code */
	if (rc != 0) {
		thread->exit_errno = rc;
		goto exit;
	}

	/* Enter the main loop */
	exit = false;
	thread->exit_errno = 0;
	while (!exit) {
		/* Note: if we regularly exceed this number of events
		 * (unlikely) it's possible that certain event sources
		 * could be starved. */
		struct epoll_event events[SFPTPD_THREAD_MAX_EPOLL_EVENTS];
		int num_events, i;
		struct sfptpd_thread_readyfd user_evs[SFPTPD_THREAD_MAX_EPOLL_EVENTS];
		unsigned int num_user_fds;

		num_events = epoll_wait(thread->epoll_fd, events,
					sizeof(events)/sizeof(events[0]), -1);
		if (num_events < 0) {
			if (errno != EINTR) {
				ERROR("thread %s: error while waiting for epoll, %s\n",
				      thread->name, strerror(errno));
				thread->exit_errno = errno;
				exit = true;
			}

			/* Don't do anymore processing */
			continue;
		}

		/* Process all events unless but stop immediately if exit
		 * becomes set. */
		num_user_fds = 0;
		for (i = 0; (i < num_events) && !exit; i++) {
			uint32_t evbits = events[i].events;
			fd = events[i].data.fd;

			/* Is this the exit event? */
			if (fd == thread->exit_event_fd) {
				DBG_L1("thread %s: received exit event\n", thread->name);
				exit = true;
				break;
			} else if ((thread->signal_fd != -1) && (fd == thread->signal_fd)) {
				thread_on_signal(thread);
			} else if (fd == queue_get_read_fd(&thread->queue_general)) {
				thread_on_message_event(thread);
			} else if (thread_on_possible_event(thread, fd) != 0) {
				user_evs[num_user_fds++] = (struct sfptpd_thread_readyfd) {
					.fd = fd,
					.flags = {
						.rd = evbits & EPOLLIN,
						.wr = evbits & EPOLLOUT,
						.err = evbits & EPOLLERR,
					},
				};
				DBG_L6("thread %s: fd %d: %08x %c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
				       thread->name,
				       fd,
				       evbits,
				       evbits & EPOLLRDHUP ? 'H' : '-',
				       evbits & 0x1000 ? '?' : '-',
				       evbits & 0x0800 ? '?' : '-',
				       evbits & EPOLLMSG ? 'm' : '-',
				       evbits & EPOLLWRBAND ? 'W' : '-',
				       evbits & EPOLLWRNORM ? 'w' : '-',
				       evbits & EPOLLRDBAND ? 'R' : '-',
				       evbits & EPOLLRDNORM ? 'r' : '-',
				       evbits & 0x0020 ? '?' : '-',
				       evbits & EPOLLHUP ? 'h': '-',
				       evbits & EPOLLERR ? 'e': '-',
				       evbits & EPOLLOUT ? 'o': '-',
				       evbits & EPOLLPRI ? 'p': '-',
				       evbits & EPOLLIN ? 'i': '-');
			}
		}

		/* If any user fds are ready, call the handler to process them */
		if (!exit && (num_user_fds > 0)) {
			DBG_L6("thread %s: %d user_fds ready\n", thread->name, num_user_fds);
			thread->ops.on_user_fds(thread->user_context, num_user_fds,
						user_evs);
		}
	}

	/* Call the shutdown function to perform any thread-specific
	 * tidy up required */
	thread->ops.on_shutdown(thread->user_context);

exit:
	thread_exit_notify(thread, thread->exit_errno);
	return (void *)(long)thread->exit_errno;
}


static int thread_configure_signals(struct sfptpd_thread *thread,
				    const sigset_t *signal_set,
				    sfptpd_thread_on_signal_fn on_signal)
{
	struct epoll_event event;
	int rc, flags;

	assert(thread != NULL);
	assert(thread->magic == SFPTPD_THREAD_MAGIC);
	assert(signal_set != NULL);
	assert(on_signal != NULL);

	/* Set the signal handler in the root thread */
	thread->on_signal = on_signal;

	/* Create a signal file descriptor to wait for the signal */
	thread->signal_fd = signalfd(thread->signal_fd, signal_set, 0);
	if (thread->signal_fd == -1) {
		ERROR("thread %s: failed to create signal fd %s\n",
		      thread->name, strerror(errno));
		return errno;
	}

	flags = fcntl(thread->signal_fd, F_GETFL, 0);
	if (flags == -1) {
		ERROR("thread %s: failed to get signalfd flags, %s\n",
		      thread->name, strerror(errno));
		rc = errno;
		goto fail1;
	}

	/* Clear the blocking flag. */
	flags |= O_NONBLOCK;
	if (fcntl(thread->signal_fd, F_SETFL, flags) == -1) {
		ERROR("thread %s: failed to set signalfd flags, %s\n",
		      thread->name, strerror(errno));
		rc = errno;
		goto fail2;
	}

	memset(&event, 0, sizeof event);
	event.events = EPOLLIN;
	event.data.fd = thread->signal_fd;
	rc = epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
	if (rc != 0) {
		ERROR("thread %s: failed to add signal fd %d to epoll, %s\n",
		      thread->name, event.data.fd, strerror(errno));
		rc = errno;
		goto fail3;
	} else {
		DBG_L2("thread %s: added signals to thread!\n", thread->name);
	}

	return 0;

fail3:
fail2:
fail1:
	(void)close(thread->signal_fd);
	thread->signal_fd = -1;
	return rc;
}


static int thread_destroy(struct sfptpd_thread *thread)
{
	int rc;
	ssize_t wrote;
	struct signalfd_siginfo signal;
	struct sfptpd_thread **trace;
	struct sfptpd_event *event;
	unsigned int waited;
	const uint64_t value = 1;

	assert(thread != NULL);
	assert(thread->magic == SFPTPD_THREAD_MAGIC);

	if (thread != sfptpd_thread_lib.root_thread) {
		/* A thread should not try to kill itself! */
		assert(thread != sfptpd_thread_self());

		/* Send an exit event to the thread and wait for it to exit. */
		wrote = write(thread->exit_event_fd, (const void *)&value, sizeof(value));
		if (wrote != sizeof(value)) {
			WARNING("thread %s: failed to send exit event to thread, %zd\n",
				thread->name, wrote);
		}

		waited = 0;
		while(((rc = pthread_tryjoin_np(thread->pthread, NULL)) != 0) &&
		      (waited < SFPTPD_JOIN_TIMEOUT)) {
			usleep(SFPTPD_JOIN_POLL_INTERVAL);
			waited += SFPTPD_JOIN_POLL_INTERVAL;
		}

		if (rc != 0) {
			ERROR("thread %s: failed to exit within %0.0f second\n",
			      thread->name, SFPTPD_JOIN_TIMEOUT / 1.0e6);
			return rc;
		}
	}

	(void)close(thread->epoll_fd);
	(void)close(thread->exit_event_fd);

	/* Now free up the resources */
	queue_destroy(&thread->queue_general, true);
	queue_destroy(&thread->queue_wait_reply, true);
	pool_destroy(&thread->msg_pool);

	/* Delete all the events */
	while (thread->event_list != NULL) {
		/* Unlink the event */
		event = thread->event_list;
		thread->event_list = event_get_next(event);

		/* Stop and destroy the timer */
		if (event->type == THREAD_EVENT_TIMER)
			timer_stop(event);
		event_destroy(event);
	}

	/* If signal handling has been configured for this thread, free the
	 * file descriptor. Drain any pending signals before closing the handle
	 * to avoid unprocessed signals being sent in the traditional manner. */
	if (thread->signal_fd != -1) {
		while (read(thread->signal_fd, &signal, sizeof(signal)) != -1);
		(void)close(thread->signal_fd);
	}

	/* Remove the thread from the list */
	for (trace = &sfptpd_thread_lib.thread_list; *trace != NULL; trace = &(*trace)->next) {
		if (*trace == thread) {
			*trace = thread->next;
			thread->next = NULL;
			break;
		}
	}

	/* If this is root thread, mark it as deleted */
	if (sfptpd_thread_lib.root_thread == thread)
		sfptpd_thread_lib.root_thread = NULL;

	DBG_L2("thread %s: destroyed\n", thread->name);

	if (sfptpd_thread_lib.zombie_policy == SFPTPD_THREAD_ZOMBIES_REAP_IMMEDIATELY) {
		/* Free the memory */
		thread->magic = SFPTPD_DEAD_MAGIC;
		free(thread);
	} else {
		/* Add the thread to the zombie list */
		thread->magic = SFPTPD_ZOMBIE_MAGIC;
		thread->next_zombie = sfptpd_thread_lib.zombie_list;
		sfptpd_thread_lib.zombie_list = thread;
	}

	return 0;
}


static int thread_create(const char *name, const struct sfptpd_thread_ops *ops,
			 void *user_context, const sigset_t *signal_set,
			 sfptpd_thread_on_signal_fn on_signal, bool root_thread,
			 struct sfptpd_thread **thread)
{
	struct sfptpd_thread *new;
	struct epoll_event event;
	int rc;

	assert(name != NULL);
	assert(ops != NULL);
	assert(thread != NULL);
	assert(!root_thread || (sfptpd_thread_lib.root_thread == NULL));

	new = (struct sfptpd_thread *)calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("thread %s: failed to allocate memory\n", name);
		return ENOMEM;
	}

	new->magic = SFPTPD_THREAD_MAGIC;
	new->name = name;
	new->ops = *ops;
	new->on_signal = NULL;
	new->user_context = user_context;
	new->exit_errno = 0;
	new->event_list = NULL;
	new->signal_fd = -1;
	if (!root_thread)
		new->parent = sfptpd_thread_self();

	new->epoll_fd = epoll_create1(0);
	if (new->epoll_fd < 0) {
		CRITICAL("thread %s: failed to create epoll instance, %s\n",
			 name, strerror(errno));
		rc = errno;
		goto fail1;
	}

	new->exit_event_fd = eventfd(0, 0);
	if (new->exit_event_fd < 0) {
		CRITICAL("thread %s: failed to create exit event, %s\n",
			 name, strerror(errno));
		rc = errno;
		goto fail2;
	}

	/* Add the exit event file descriptor to the epoll */
	memset (&event, 0, sizeof (event));
	event.events = EPOLLIN;
	event.data.fd = new->exit_event_fd;
	rc = epoll_ctl(new->epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
	if (rc != 0) {
		CRITICAL("thread %s: failed to add exit event fd to epoll, %s\n",
			 name, strerror(errno));
		rc = errno;
		goto fail3;
	}

	/* Create an empty message pool for now */
	rc = pool_create(&new->msg_pool, name, 0, 0);
	if (rc != 0) {
		CRITICAL("thread %s: failed to create local message pool, %s\n",
			 name, strerror(rc));
		goto fail3;
	}

	/* Create the general message queue */
	rc = queue_create(&new->queue_general);
	if (rc != 0) {
		CRITICAL("thread %s: failed to create general message queue, %s\n",
			 name, strerror(rc));
		goto fail4;
	}

	/* Add the queue event file descriptor to the epoll */
	event.events = EPOLLIN;
	event.data.fd = queue_get_read_fd(&new->queue_general);
	rc = epoll_ctl(new->epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
	if (rc != 0) {
		CRITICAL("thread %s: failed to add queue event fd to epoll, %s\n",
			 name, strerror(errno));
		rc = errno;
		goto fail5;
	}

	/* Create the send-wait-reply message queue */
	rc = queue_create(&new->queue_wait_reply);
	if (rc != 0) {
		CRITICAL("thread %s: failed to create wait-reply message queue, %s\n",
			 name, strerror(rc));
		goto fail5;
	}

	/* If requested, configure signal handling */
	if (signal_set != NULL) {
		assert(on_signal != NULL);
		rc = thread_configure_signals(new, signal_set, on_signal);
		if (rc != 0)
			goto fail6;
	}

	/* If we are creating a child thread, create a new pthread */
	if (!root_thread) {
		struct sfptpd_thread *self = sfptpd_thread_self();
		sfptpd_msg_thread_startup_status_t *msg = &new->startup_status;
		sfptpd_msg_hdr_t *hdr;
		char thread_name[16];

		/* Initialise the startup complete message ready to wait on.
		 * Set the reply queue to be the priority queue. */
		SFPTPD_MSG_INIT(*msg);
		SFPTPD_MSG_SET_ID(msg, SFPTPD_MSG_ID_THREAD_STARTUP_STATUS);
		msg->hdr.reply = &self->queue_wait_reply;
		msg->thread = new;
		msg->status_code = 0;

		rc = pthread_create(&new->pthread, NULL, thread_entry, new);
		if (rc != 0) {
			CRITICAL("couldn't create thread %s, %s\n", name, strerror(rc));
			goto fail7;
		}

		/* Set the thread name for debugging purposes: ignore failure */
		snprintf(thread_name, sizeof thread_name, "%.7s:%s",
			 program_invocation_short_name, name);
		pthread_setname_np(new->pthread, thread_name);

		/* Wait for the response from the thread to indicate that
		 * startup is complete. */
		rc = queue_receive(&self->queue_wait_reply, &hdr, true);

		/* If the response isn't the message we were expecting, something
		 * is horribly wrong. */
		assert((void *)hdr == (void *)msg);

		/* Assuming the receive operation worked (likely) then get the
		 * thread startup code from the reply. If this is non-zero then
		 * the thread failed during startup - a critical error. */
		if (rc == 0)
			rc = msg->status_code;

		if (rc != 0) {
			DBG_L2("thread %s failed during startup, %s\n",
			       name, strerror(rc));
			(void)thread_destroy(new);
			return rc;
		}
	}

	DBG_L1("thread %s: created successfully msg fds %d/%d %d/%d\n",
			name, new->queue_general.pipe.fds[0], new->queue_general.pipe.fds[1],
			new->queue_wait_reply.pipe.fds[0], new->queue_wait_reply.pipe.fds[1]);

	/* Success! Add the thread to the thread list */
	new->next = sfptpd_thread_lib.thread_list;
	sfptpd_thread_lib.thread_list = new;
	if (root_thread)
		sfptpd_thread_lib.root_thread = new;

	/* Return a handle to the thread */
	*thread = new;
	return 0;

fail7:
	if (new->signal_fd != -1)
		(void)close(new->signal_fd);
fail6:
	queue_destroy(&new->queue_wait_reply, true);
fail5:
	queue_destroy(&new->queue_general, true);
fail4:
	pool_destroy(&new->msg_pool);
fail3:
	(void)close(new->exit_event_fd);
fail2:
	(void)close(new->epoll_fd);
fail1:
	new->magic = 0;
	free(new);
	return rc;
}


/****************************************************************************
 * Threading - Public Functions
 ****************************************************************************/

int sfptpd_threading_initialise(unsigned int num_global_msgs,
				unsigned int msg_size,
				enum sfptpd_thread_zombie_policy zombie_policy)
{
	int rc;
	sigset_t signal_set;

	assert(num_global_msgs > 0);

	sfptpd_thread_lib.root_thread = NULL;
	sfptpd_thread_lib.thread_list = NULL;
	sfptpd_thread_lib.zombie_list = NULL;
	sfptpd_thread_lib.zombie_policy = zombie_policy;

	/* Create a pthread key to allow each thread to store it's message
	 * threading context */
	rc = pthread_key_create(&sfptpd_thread_lib.key, NULL);
	if (rc != 0) {
		CRITICAL("threading: failed to create pthread key, %s\n",
			 strerror(rc));
		return rc;
	}

	/* Create the global message pool */
	rc = sfptpd_thread_alloc_msg_pool(SFPTPD_MSG_POOL_GLOBAL,
					  num_global_msgs, msg_size);
	if (rc != 0) {
		CRITICAL("threading: failed to create global message pool, %s\n",
			 strerror(rc));
		goto fail1;
	}

	/* Block all signals. This signal mask will then be inherited by all
	 * child threads. Signals that are to be processed will be collected
	 * via a signal handler. */
	sigfillset(&signal_set);
	rc = pthread_sigmask(SIG_BLOCK, &signal_set,
			     &sfptpd_thread_lib.original_signal_set);
	if (rc != 0) {
		CRITICAL("threading: couldn't mask signals, %s\n",
			 strerror(rc));
		goto fail2;
	}

	return 0;

fail2:
	pool_destroy(&sfptpd_thread_lib.global_msg_pool);
fail1:
	(void)pthread_key_delete(sfptpd_thread_lib.key);
	return rc;
}


void sfptpd_threading_shutdown(void)
{
	struct sfptpd_thread *thread;

	for (thread = sfptpd_thread_lib.thread_list; thread != NULL; thread = thread->next)
		WARNING("threading shutdown but thread %s still exists\n",
			thread->name);

	for (thread = sfptpd_thread_lib.zombie_list; thread != NULL;) {
		struct sfptpd_thread *next_zombie = thread->next_zombie;

		if (sfptpd_thread_lib.zombie_policy != SFPTPD_THREAD_ZOMBIES_REAP_AT_EXIT)
			WARNING("zombie threads exist at exit contrary to reaping policy\n");
		thread->magic = SFPTPD_DEAD_MAGIC;
		free(thread);
		thread = next_zombie;
	}

	/* Destroy key before pool so that thread_get_name doesn't
	 * reference freed memory */
	(void)pthread_key_delete(sfptpd_thread_lib.key);
	pool_destroy(&sfptpd_thread_lib.global_msg_pool);
	pool_destroy(&sfptpd_thread_lib.rt_stats_msg_pool);
	(void)pthread_sigmask(SIG_SETMASK, &sfptpd_thread_lib.original_signal_set, NULL);
}


int sfptpd_thread_main(const struct sfptpd_thread_ops *ops,
		       const sigset_t *signal_set,
		       sfptpd_thread_on_signal_fn on_signal,
		       void *user_context)
{
	struct sfptpd_thread *thread;
	int rc;
	void *ret_val;

	assert(ops != NULL);
	assert(signal_set != NULL);
	assert(on_signal != NULL);

	/* Create an sfptpd thread structure and resources for the current
	 * thread. This is the 'root' thread. */
	rc = thread_create("main", ops, user_context, signal_set, on_signal,
			   true, &thread);
	if (rc != 0)
		return rc;

	/* Execute as an sfptpd thread */
	ret_val = thread_entry(thread);

	/* Free the resources for the thread */
	(void)thread_destroy(thread);

	/* We return an int from all sfptpd threads but the API requires a
	 * void pointer to be returned, resulting in this ugly bit of code */
	return (int)(long)ret_val;
}


int sfptpd_thread_create(const char *name, const struct sfptpd_thread_ops *ops,
			 void *user_context, struct sfptpd_thread **thread)
{
	/* Create a child thread */
	return thread_create(name, ops, user_context, NULL, NULL, false, thread);
}


int sfptpd_thread_destroy(struct sfptpd_thread *thread)
{
	/* Destroy the child thread */
	return thread_destroy(thread);
}


void sfptpd_thread_exit(int exit_errno)
{
	struct sfptpd_thread *self = sfptpd_thread_self();
	ssize_t wrote;
	const uint64_t value = 1;

	/* Set the error code */
	self->exit_errno = exit_errno;

	/* Send an exit event to the thread to cause it to exit. */

	wrote = write(self->exit_event_fd, (const void *)&value, sizeof(value));
	if (wrote != sizeof(value)) {
		WARNING("thread %s: failed to send exit event to self, %zd\n",
			self->name, wrote);
	}
}


int sfptpd_thread_error(int exit_errno)
{
	struct sfptpd_thread *self = thread_self();

	if (self != NULL) {
		sfptpd_thread_exit(exit_errno);
		return ENOTRECOVERABLE;
	} else {
		return exit_errno;
	}
}


struct sfptpd_thread *sfptpd_thread_self(void)
{
	struct sfptpd_thread *self = thread_self();

	assert(self != NULL);
	return self;
}


struct sfptpd_thread *sfptpd_thread_parent(void)
{
	return sfptpd_thread_self()->parent;
}


struct sfptpd_thread *sfptpd_thread_find(const char *name)
{
	struct sfptpd_thread *thread;

	for (thread = sfptpd_thread_lib.thread_list; thread != NULL; thread = thread->next) {
		if (strcmp(name, thread->name) == 0)
			return thread;
	}

	return NULL;
}


const char *sfptpd_thread_get_name(struct sfptpd_thread *thread)
{
	assert(thread != NULL);
	assert(thread->magic == SFPTPD_THREAD_MAGIC ||
	       thread->magic == SFPTPD_ZOMBIE_MAGIC);

	if (thread->magic == SFPTPD_ZOMBIE_MAGIC) {
		WARNING("zombie thread %p (%s) referenced\n",
			thread, thread->name);
	}

	return thread->name;
}


int sfptpd_thread_alloc_msg_pool(enum sfptpd_msg_pool_id pool_type,
				 unsigned int num_msgs,
				 unsigned int msg_size)
{
	struct sfptpd_thread *self;
	int rc;

	assert(pool_type < SFPTPD_MSG_POOL_MAX);
	assert(num_msgs > 0);
	assert(msg_size > 0);

	switch (pool_type) {
		case SFPTPD_MSG_POOL_LOCAL:
			self = sfptpd_thread_self();
			rc = pool_allocate(&self->msg_pool, num_msgs, msg_size);
			break;
		case SFPTPD_MSG_POOL_GLOBAL:
			rc = pool_create(&sfptpd_thread_lib.global_msg_pool, "global",
				num_msgs, msg_size);
			break;
		case SFPTPD_MSG_POOL_RT_STATS:
			/* Align the message size to 8-byte boundary. */
			msg_size = (msg_size + 3) & ~3;
			/* Create & allocate the realtime stats message pool */
			rc = pool_create(&sfptpd_thread_lib.rt_stats_msg_pool,
								 "rt_stats", num_msgs, msg_size);
			DBG_L3("create rt_stats pool of size %u * %u\n", num_msgs, msg_size);
			break;
		default:
			assert(!"Invalid message pool type in sfptpd_thread_alloc_msg_pool");
	}

	return rc;
}


int sfptpd_thread_timer_create(sfptpd_event_id_t timer_id,
			       clockid_t clock_id,
			       sfptpd_thread_on_event_fn on_expiry,
			       void *user_context)
{
	return thread_event_create(timer_id, THREAD_EVENT_TIMER, clock_id,
				   on_expiry, user_context);
}


int sfptpd_thread_timer_start(sfptpd_event_id_t timer_id, bool periodic,
			      bool absolute, const struct sfptpd_timespec *interval)
{
	struct sfptpd_event *timer;
	int rc;

	assert(interval != NULL);

	timer = thread_event_find_by_id(timer_id);
	rc = thread_event_check_type(timer, THREAD_EVENT_TIMER);
	if (rc == 0)
		rc = timer_start(timer, periodic, absolute, interval);
	return rc;
}


int sfptpd_thread_timer_stop(sfptpd_event_id_t timer_id)
{
	struct sfptpd_event *timer;
	int rc;

	timer = thread_event_find_by_id(timer_id);
	rc = thread_event_check_type(timer, THREAD_EVENT_TIMER);
	if (!rc)
		rc = timer_stop(timer);
	return rc;
}


int sfptpd_thread_timer_get_time_left(sfptpd_event_id_t timer_id,
				      struct sfptpd_timespec *interval)
{
	struct sfptpd_event *timer;
	int rc;

	assert(interval != NULL);

	timer = thread_event_find_by_id(timer_id);
	rc = thread_event_check_type(timer, THREAD_EVENT_TIMER);
	if (!rc)
		return timer_get_time_left(timer, interval);
	return rc;
}

int sfptpd_thread_event_create(sfptpd_event_id_t event_id,
			       sfptpd_thread_on_event_fn on_event,
			       void *user_context)
{
	return thread_event_create(event_id, THREAD_EVENT_EVENT, 0,
				   on_event, user_context);
}


int sfptpd_thread_event_create_writer(struct sfptpd_thread *thread,
				      sfptpd_event_id_t event_id,
				      struct sfptpd_thread_event_writer *writer)
{
	struct sfptpd_event *event;
	int rc;

	assert(writer != NULL);

	event = event_find_by_id(thread, event_id);
	rc = thread_event_check_type(event, THREAD_EVENT_EVENT);
	if (rc == 0) {
		writer->fd = dup(event->fd);
		if (writer->fd == -1)
			rc = errno;
	}
	return rc;
}


void sfptpd_thread_event_destroy_writer(struct sfptpd_thread_event_writer *writer)
{
	assert(writer != NULL);

	close(writer->fd);
}


int sfptpd_thread_event_post(struct sfptpd_thread_event_writer *writer)
{
	uint64_t increment = 1;
	int rc;

	assert(writer != NULL);

	rc = write(writer->fd, &increment, sizeof increment);
	if (rc == -1)
		return errno;

	/* eventfd(2) defines accesses as being "8 bytes". */
	assert(rc == 8);
	return 0;
}


int sfptpd_thread_user_fd_add(int fd, bool read, bool write)
{
	struct sfptpd_thread *self;
	struct epoll_event event;
	int rc, flags;

	assert(fd != -1);
	assert(read || write);

	self = sfptpd_thread_self();

	/* We need to make sure the file descriptor is set to non-blocking */
	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) {
		ERROR("thread %s: failed to get user fd %d flags, %s\n",
		      self->name, fd, strerror(errno));
		return errno;
	}

	/* Clear the blocking flag. */
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1) {
		ERROR("thread %s: failed to set user fd %d flags, %s\n",
		      self->name, fd, strerror(errno));
		return errno;
	}

	/* Add the file descriptor to the epoll */
	memset (&event, 0, sizeof (event));
	event.events = 0;
	if (read)
		event.events |= EPOLLIN;
	if (write)
		event.events |= EPOLLOUT;
	event.data.fd = fd;
	rc = epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
	if (rc != 0) {
		ERROR("thread %s: failed to add user fd %d to epoll, %s\n",
		      self->name, event.data.fd, strerror(errno));
		return errno;
	}

	return 0;
}


int sfptpd_thread_user_fd_remove(int fd)
{
	struct sfptpd_thread *self;
	struct epoll_event event;
	int rc;

	assert(fd != -1);

	self = sfptpd_thread_self();

	/* Remove the file descriptor from the epoll */
	memset (&event, 0, sizeof (event));
	event.events = 0;
	event.data.fd = fd;
	rc = epoll_ctl(self->epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event);
	if (rc != 0) {
		WARNING("thread %s: failed to remove user fd %d from epoll, %s\n",
			self->name, event.data.fd, strerror(errno));
		return errno;
	}

	return 0;
}


/* fin */
