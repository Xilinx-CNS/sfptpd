/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_MESSAGE_H
#define _SFPTPD_MESSAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Forward declaration of structures */
struct sfptpd_queue;
struct sfptpd_thread;

/** Enum identifying different message pools */
enum sfptpd_msg_pool_id
{
	/** Local threads message pool */
	SFPTPD_MSG_POOL_LOCAL,
	/** Global message pool */
	SFPTPD_MSG_POOL_GLOBAL,
	/** Realtime stats message pool */
	SFPTPD_MSG_POOL_RT_STATS,
	SFPTPD_MSG_POOL_MAX
};


/** Message bases for various components */
#define SFPTPD_MSG_BASE_THREADING   (0x00000000)
#define SFPTPD_MSG_BASE_ENGINE      (0x00010000)
#define SFPTPD_MSG_BASE_SYNC_MODULE (0x00020000)
#define SFPTPD_MSG_BASE_APP         (0x00030000)
#define SFPTPD_MSG_BASE_SERVO       (0x00040000)
#define SFPTPD_MSG_BASE_CLOCK_FEED  (0x00050000)


/** struct sfptpd_msg_hdr
 * Common message header for all messages sent and received sfptpd message
 * threads. This structure should be placed at the start of all messages
 * defined.
 * @reply: Message queue to send replies to
 * @free: Message queue to send freed messages to
 * @id: Message ID - unique message ID
 * @capacity: Maximum size of message payload
 * @payload: Start of message payload
 */
typedef struct sfptpd_msg_hdr
{
	struct sfptpd_queue *reply;
	struct sfptpd_queue *free;
	void* pool;
	uint32_t id;
	unsigned int capacity;
	uint8_t payload[0];
} sfptpd_msg_hdr_t;


/** Notify Exit Message
 * Used to notify a thread's parent that it has exited
 * @hdr Message header
 * @thread Handle of thread that has exited
 * @exit_code Exit code
 */
#define SFPTPD_MSG_ID_THREAD_EXIT_NOTIFY (SFPTPD_MSG_BASE_THREADING + 0)
typedef struct sfptpd_msg_thread_exit_notify
{
	sfptpd_msg_hdr_t hdr;
	struct sfptpd_thread *thread;
	int exit_code;
} sfptpd_msg_thread_exit_notify_t;


/** Startup complete message.
 * Used to notify a thread's parent of the status after startup has completed
 * @hdr Message header
 * @thread Handle of thread
 * @exit_code Status code
 */
#define SFPTPD_MSG_ID_THREAD_STARTUP_STATUS (SFPTPD_MSG_BASE_THREADING + 1)
typedef struct sfptpd_msg_thread_startup_status
{
	sfptpd_msg_hdr_t hdr;
	struct sfptpd_thread *thread;
	int status_code;
} sfptpd_msg_thread_startup_status_t;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise the message header of a message allocated statically or locally
 * NOTE The SFPTPD_MSG_INIT macro should be used in preference to calling this
 * function directly.
 * @param hdr Pointer to the message header
 * @param capacity Maximum size of message payload
 */
void sfptpd_msg_init(sfptpd_msg_hdr_t *hdr, unsigned int capacity);

/** Macro to initialise the message header of a message allocated statically
 * or locally
 * @param msg Message to initialise
 * @param id Message ID
 */
#define SFPTPD_MSG_INIT(msg) sfptpd_msg_init(&((msg).hdr), \
					     (sizeof(msg)-sizeof(sfptpd_msg_hdr_t)))


/** Allocate a message from a message pool
 * @param pool The message pool to allocate from
 * @param wait Boolean indicating that caller should block if no message
 * is available.
 * @return A pointer to the allocated message or NULL if none available
 */
sfptpd_msg_hdr_t *sfptpd_msg_alloc(enum sfptpd_msg_pool_id pool_id, bool wait);


void sfptpd_msg_alloc_failed(const char *pool, const char *file, const char *func, int line);

#define SFPTPD_MSG_LOG_ALLOC_FAILED(_pool) sfptpd_msg_alloc_failed(_pool, __FILE__, __func__, __LINE__);

/** Free message and return to pool from which it was allocated. In the case
 * of messages allocated statically or on the static, has no effect.
 * @param hdr Pointer to the message header
 */
void sfptpd_msg_free(sfptpd_msg_hdr_t *hdr);
#define SFPTPD_MSG_FREE(msg) sfptpd_msg_free(&((msg)->hdr))


/** Set the message ID
 * @param hdr Pointer to the message header of the message
 * @param id Message ID
 */
static inline void sfptpd_msg_set_id(sfptpd_msg_hdr_t *hdr, uint32_t id)
{
	assert(hdr != NULL);
	hdr->id = id;
}

#define SFPTPD_MSG_SET_ID(msg, id) sfptpd_msg_set_id(&((msg)->hdr), (id))

/** Get the message ID
 * @param hdr Pointer to the message header of the message
 * @return The message ID
 */
static inline uint32_t sfptpd_msg_get_id(sfptpd_msg_hdr_t *hdr)
{
	assert(hdr != NULL);
	return hdr->id;
}

#define SFPTPD_MSG_GET_ID(msg) sfptpd_msg_get_id(&((msg)->hdr))

/** Get the message capacity
 * @param hdr Pointer to the message header of the message
 * @return The message capacity
 */
static inline unsigned int sfptpd_msg_get_capacity(sfptpd_msg_hdr_t *hdr)
{
	assert(hdr != NULL);
	return hdr->capacity;
}

#define SFPTPD_MSG_GET_CAPACITY(msg) sfptpd_msg_get_capacity(&((msg)->hdr))

/** Get a pointer to the message payload
 * @param hdr Pointer to the message header of the message
 * @return A pointer to the payload of the message
 */
static inline uint8_t *sfptpd_msg_get_payload(sfptpd_msg_hdr_t *hdr)
{
	assert(hdr != NULL);
	return hdr->payload;
}

#define SFPTPD_MSG_GET_PAYLOAD(msg) sfptpd_msg_get_payload(&((msg)->hdr))


/** Send a message to another thread. If the message needs a reply, the reply
 * queue will be set appropriately.
 * @param hdr Pointer to the message header of the message
 * @param recipient Pointer to recipient thread
 * @param id Message ID
 * @param needs_reply Boolean indicating if a reply is required
 * @return 0 on success or an errno otherwise
 */
int sfptpd_msg_send(sfptpd_msg_hdr_t *hdr, struct sfptpd_thread *recipient,
		    uint32_t id, bool needs_reply);

#define SFPTPD_MSG_SEND(msg, recipient, id, needs_reply) \
		sfptpd_msg_send(&((msg)->hdr), (recipient), (id), (needs_reply))

/** Send a message to another thread and block waiting for a reply.
 * This operation is very useful and can help to dramatically simplify
 * communications between threads. However it comes with a strong health
 * warning.
 * WARNING: Send-wait operations should only be used in very specific cases.
 * The fact that the call blocks until the recipient thread responds means
 * that there is scope for deadlock and also scope to starve the calling thread.
 * Send wait should only be used if the following conditions are true:
 *   * For any given pair of threads, send-wait operations only occur in one
 *     direction. Otherwise deadlock can occur.
 *   * The thread to which the send-wait is generally idle and able to process
 *     the message quickly and respond i.e. does not carry out CPU intensive
 *     calculations for long periods of time.
 * If the above conditions are not met, use an asynchronous send and receive
 * instead.
 * @param hdr Pointer to the message header of the message. Note that the
 * response will be located in the same buffer so the caller should ensure that
 * 
 * @param recipient Pointer to recipient thread
 * @param id Message ID
 * @return 0 on success or an errno otherwise
 */
int sfptpd_msg_send_wait(sfptpd_msg_hdr_t *hdr,
			 struct sfptpd_thread *recipient, uint32_t id);

#define SFPTPD_MSG_SEND_WAIT(msg, recipient, id) \
		sfptpd_msg_send_wait(&((msg)->hdr), (recipient), (id))

/** Send a reply to a message. If the message does not require a reply
 * it will be freed
 * @param hdr Pointer to the message header of the message to reply to
 * @return 0 on success or an errno otherwise
 */
int sfptpd_msg_reply(sfptpd_msg_hdr_t *hdr);

#define SFPTPD_MSG_REPLY(msg) sfptpd_msg_reply(&((msg)->hdr))


#endif /* _SFPTPD_MESSAGE_H */
