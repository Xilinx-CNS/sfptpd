/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2012-2024, Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_THREAD_H
#define _SFPTPD_THREAD_H

#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

#include "sfptpd_message.h"
#include "sfptpd_time.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

/* Policy for reaping zombie threads.
 * Immediate reaping prevents mid-execution memory leaks.
 * Delayed reaping allows sfptpd_thread_get_name to be called on threads
 * that have exited. */
enum sfptpd_thread_zombie_policy {
	SFPTPD_THREAD_ZOMBIES_REAP_IMMEDIATELY,
	SFPTPD_THREAD_ZOMBIES_REAP_AT_EXIT,
};


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Type for event ids, including timers. */
typedef unsigned int sfptpd_event_id_t;


/** User I/O event */
struct sfptpd_thread_readyfd {
	int fd;
	struct {
		bool rd:1;
		bool wr:1;
		bool err:1;
	} flags;
};

/** Thread event writer */
struct sfptpd_thread_event_writer {
	int fd;
};


/** Forward declaration of structures */
struct sfptpd_thread;
struct signalfd_siginfo;


/** Typedef for thread startup handler. This handler should be used by a thread
 * to perform any initialisation required before the thread enters the main loop.
 * Returning an error will cause the thread to exit immediately with an error.
 * @param user_context Thread user context supplied when thread was created
 * @return 0 on success or an errno
 */
typedef int (*sfptpd_thread_on_startup_fn)(void *user_context);

/** Typedef for thread shutdown handler. This handler should be used by a
 * thread to perform cleanup and free resources before the thread exits.
 * @param user_context Thread user context supplied when thread was created
 */
typedef void (*sfptpd_thread_on_shutdown_fn)(void *user_context);

/** Typedef for user signal handler
 * @param user_context Thread user context supplied when thread was created
 * @param signal_num Signal that has occurred
 */
typedef void (*sfptpd_thread_on_signal_fn)(void *user_context, int signal_num);

/** Typedef for user messsage handler. This handler will be called whenever
 * a message is received by the thread. It is a responsibility of the thread
 * to free or reply to the message as necessary.
 * @param user_context Thread user context supplied when thread was created
 * @param msg Pointer to message
 */
typedef void (*sfptpd_thread_on_message_fn)(void *user_context,
					    struct sfptpd_msg_hdr *msg);

/** Typedef for user file descriptor handler. This handler will be called when
 * one or more of the user supplied file descriptors registered using
 * sfptpd_thread_user_fd_add() becomes ready. It is the responsibility of the
 * handler to service the file descriptor appropriately.
 * @param user_context Thread user context supplied when thread was created
 * @param num_events Number of ready file descriptors
 * @param events Array of events for ready file descriptors
 */
typedef void (*sfptpd_thread_on_user_fds_fn)(void *user_context,
					     unsigned int num_events,
					     struct sfptpd_thread_readyfd events[]);

/** Thread operations
 * @on_startup: Function to call when thread begins executing
 * @on_shutdown: Function to call when a thread is shutdown
 * @on_message: Function to call when a message is received
 * @on_user_fd: Function to call when a user file descriptor becomes ready
 */
struct sfptpd_thread_ops
{
	sfptpd_thread_on_startup_fn on_startup;
	sfptpd_thread_on_shutdown_fn on_shutdown;
	sfptpd_thread_on_message_fn on_message;
	sfptpd_thread_on_user_fds_fn on_user_fds;
};


/** Typedef for user event handler
 * @user_context Context supplied when signal handling was configured
 * @id ID of the event that has been triggered
 */
typedef void (*sfptpd_thread_on_event_fn)(void *user_context,
					  unsigned int id);

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise the threading library.
 * @param num_global_msgs Number of messages to create in the global message
 * pool.
 * @param msg_size Maximum message size for global messages
 * @param zombie_policy When to reap the state of zombie threads
 * @return 0 on success or an errno otherwise
 */
int sfptpd_threading_initialise(unsigned int num_global_msgs,
			        unsigned int msg_size,
				enum sfptpd_thread_zombie_policy zombie_policy);

/** Shutdown the threading library and free resources. Note that this should
 * only be done after all threads have exited.
 */
void sfptpd_threading_shutdown(void);

/** Main entry point into threading code. This should be called once from
 * main() to enter threading library. The function will only return when the
 * application is shutdown.
 * @param ops Pointer to root thread event handlers
 * @param signal_set Set of signals that the application handles.
 * @param on_signal Pointer to root thread signal handler
 * @param user_context Context to be supplied when a thread event handler is
 * called
 * @return 0 if no error occurred or an errno otherwise
 */
int sfptpd_thread_main(const struct sfptpd_thread_ops *ops,
		       const sigset_t *signal_set,
		       sfptpd_thread_on_signal_fn on_signal,
		       void *user_context);

/** Create a message thread. The thread will begin executing immediately.
 * @param name Textual name of the thread
 * @param ops Pointer to thread event handlers
 * @param user_context Context to be supplied when a thread event handler is
 * called
 * @param thread Returned handle to thread object
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_create(const char *name, const struct sfptpd_thread_ops *ops,
			 void *user_context, struct sfptpd_thread **thread);

/** Destroy another thread. Send an exit message to a thread, wait for it to
 * exit and then destroy the resources associated with it.
 * @param thread Pointer to thread to destroy
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_destroy(struct sfptpd_thread *thread);

/** This function causes the current thread to exit with the specified return
 * code. The thread will shutdown in an orderly manner and a message will be
 * sent to the parent indicating that the thread has exited.
 * @param exit_errno Thread exit code: 0 or an errno
 */
void sfptpd_thread_exit(int exit_errno);

/** This function causes the current thread to exit with the specified return
 * code using sfptpd_thread_exit() if running in a thread. Otherwise return
 * an error code for the caller to use as its return code.
 * @param exit_errno Thread exit code: 0 or an errno
 * @return An error code for the caller to return
 */
int sfptpd_thread_error(int exit_errno);

/** Get a handle to the current thread
 * @return A pointer to the current thread
 */
struct sfptpd_thread *sfptpd_thread_self(void);

/** Get a handle to the parent of the current thread
 * @return A pointer to the parent of the current thread
 */
struct sfptpd_thread *sfptpd_thread_parent(void);

/** Find a thread by name.
 * @param name Name of the thread
 * @return A pointer to the thread or null if not found
 */
struct sfptpd_thread *sfptpd_thread_find(const char *name);

/** Get a thread's name.
 * @param The thread
 * @return Name of the thread
 */
const char *sfptpd_thread_get_name(struct sfptpd_thread *thread);

/** Allocate a message pool.
 * @param pool_type Type of pool. Local pools are freed upon thread termination.
 * @param num_msgs Number of messages to allocate
 * @param msg_size Size for each message
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_alloc_msg_pool(enum sfptpd_msg_pool_id pool_type,
								 unsigned int num_msgs,
								 unsigned int msg_size);

/** Create a timer for the thread.
 * @param clock_id Clock to use for timer e.g. CLOCK_MONOTONIC or CLOCK_REALTIME
 * @param timer_id User supplied ID for the timer which must be unique for
 * this thread.
 * @param on_expiry Function to call when timer expires
 * @param user_context Context to be supplied to timer expiry function
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_timer_create(unsigned int timer_id, clockid_t clock_id,
			       sfptpd_thread_on_event_fn on_expiry,
			       void *user_context);

/** Start a timer
 * @param timer_id ID of the timer
 * @param periodic Configure timer to periodically fire
 * @param absolute Treat interval as an absolute time
 * @param interval Timer interval
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_timer_start(unsigned int timer_id, bool periodic,
			      bool absolute, const struct sfptpd_timespec *interval);

/** Stop a timer
 * @param timer_id ID of the timer
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_timer_stop(unsigned int timer_id);

/** Get remaining time before a timer expires
 * @param timer_id ID of the timer
 * @param interval On success, will contain the time left before the timer expires.
 *                 If both fields are zero, the timer is currently disarmed.
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_timer_get_time_left(unsigned int timer_id, struct sfptpd_timespec *interval);

/** Configure the thread to wait on the supplied file descriptor. When the
 * file descriptor becomes ready the thread user handler
 * sfptpd_thread_on_user_event_fn() will be called. At least one of 'read'
 * and 'write' must be true.
 * @param fd File descriptor
 * @param read Wait for the file descriptor to become readable
 * @param write Wait for the file descriptor to become writable
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_user_fd_add(int fd, bool read, bool write);

/** Configure the thread to stop waiting on the supplied file descriptor.
 * @param fd File descriptor
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_user_fd_remove(int fd);

/** Create an event for the thread.
 * @param event_id User-supplied ID for the event which must be unique for
 * this thread.
 * @param on_event Function to call when event fires
 * @param user_context Context to be supplied to event handler
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_event_create(sfptpd_event_id_t event_id,
			       sfptpd_thread_on_event_fn on_event,
			       void *user_context);

/** Create an event writer
 * @praam thread Thread to receive the events
 * @param event_id ID of the thread's event
 * @param writer Pointer to space that will store writer object
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_event_create_writer(struct sfptpd_thread *thread,
				      sfptpd_event_id_t event_id,
				      struct sfptpd_thread_event_writer *writer);

/** Destroy an event writer
 * @param writer Pointer to writer object
 * @return 0 on success or an errno otherwise
 */
void sfptpd_thread_event_destroy_writer(struct sfptpd_thread_event_writer *writer);

/** Post an event
 * @param writer Pointer to writer object
 * @return 0 on success or an errno otherwise
 */
int sfptpd_thread_event_post(struct sfptpd_thread_event_writer *writer);


#endif /* _SFPTPD_THREAD_H */
