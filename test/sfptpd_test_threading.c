/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_threading.c
 * @brief  Threading support unit test
 */

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <inttypes.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sfptpd_message.h"
#include "sfptpd_thread.h"
#include "sfptpd_logging.h"
#include "sfptpd_misc.h"
#include "sfptpd_test.h"


/****************************************************************************
 * External declarations
 ****************************************************************************/



/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define TEST_NUM_TIMERS (7)
#define TEST_NUM_THREADS (5)

#define TEST_TIMER_TEST_LEN (5)
#define TEST_SIGNAL_TEST_INTERVAL (100000)

#define TEST_MSG_ID_START         (0x1000)
#define TEST_MSG_ID_STOP          (0x1001)
#define TEST_MSG_ID_ASYNC         (0x2000)
#define TEST_MSG_ID_REQ           (0x2001)
#define TEST_MSG_ID_RESP          (0x2002)
#define TEST_MSG_ID_BLOCKING_REQ  (0x2003)
#define TEST_MSG_ID_BLOCKING_RESP (0x2004)

#define TEST_PORT_BASE            (0x1000)

#define TEST_DATAGRAM_SIZE        (0x100)


typedef struct {
	sfptpd_msg_hdr_t hdr;

	unsigned int sender;
	uint8_t payload[64];
} test_msg_t;


/****************************************************************************
 * Local Data
 ****************************************************************************/

struct test_thread
{
	char name[16];
	unsigned int id;
	struct sfptpd_thread *thread;
	bool send_msgs;
	bool send_data;
	bool send_signals;

	uint64_t timer_interval[TEST_NUM_TIMERS];
	uint64_t timer_count[TEST_NUM_TIMERS];

	unsigned int asyncs_txed[TEST_NUM_THREADS];
	unsigned int reqs_txed[TEST_NUM_THREADS];
	unsigned int resps_txed[TEST_NUM_THREADS];
	unsigned int asyncs_rxed[TEST_NUM_THREADS];
	unsigned int reqs_rxed[TEST_NUM_THREADS];
	unsigned int resps_rxed[TEST_NUM_THREADS];
	unsigned int syncs_txed[TEST_NUM_THREADS];
	unsigned int syncs_rxed[TEST_NUM_THREADS];

	unsigned int signals_txed;

	int tx_socket;
	unsigned int data_txed[TEST_NUM_THREADS];
	int rx_sockets[TEST_NUM_THREADS];
	unsigned int data_rxed[TEST_NUM_THREADS];
};

static struct test_thread threads[TEST_NUM_THREADS];

static bool test_timers = false;
static bool test_messaging = false;
static bool test_signals = false;
static bool test_user_fds = false;
static bool expect_signal_coalescing = false;
static int test_rc = 0;
static sigset_t test_signal_set;

static int test_signals_rxed[TEST_NUM_THREADS];


static int test_on_startup(void *context);
static void test_on_shutdown(void *context);
static void test_on_timer(void *context, unsigned int id);
static void test_on_message(void *context, struct sfptpd_msg_hdr *msg);
static void test_on_user_fd(void *context, unsigned int num_fds,
			    struct sfptpd_thread_readyfd events[]);

static const struct sfptpd_thread_ops test_thread_ops =
{
	test_on_startup, test_on_shutdown, test_on_message, test_on_user_fd
};

static int root_on_startup(void *context);
static void root_on_shutdown(void *context);
static void root_on_timer(void *context, unsigned int id);
static void root_on_message(void *context, struct sfptpd_msg_hdr *msg);
static void root_on_user_fd(void *context, unsigned int num_fds,
			    struct sfptpd_thread_readyfd events[]);
static void root_on_signal(void *context, int signal_num);

static const struct sfptpd_thread_ops root_thread_ops =
{
	root_on_startup, root_on_shutdown, root_on_message, root_on_user_fd
};


/****************************************************************************
 * Root Thread Ops
 ****************************************************************************/

static void test_send_msg(struct test_thread *t, unsigned int recipient,
			  unsigned int msg_id, bool needs_reply,
			  unsigned int stats[])
{
	test_msg_t *m;
	int rc;

	assert(t != NULL);
	assert(stats != NULL);

	if (!t->send_msgs)
		return;

	m = (test_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);
	if (m == NULL) {
		printf("thread %d: failed to allocate msg from global pool\n",
		       t->id);
	} else {
		m->sender = t->id;
		rc = SFPTPD_MSG_SEND(m, threads[recipient].thread,
				     msg_id, needs_reply);
		if (rc != 0) {
			printf("thread %d: failed to send msg %x to thread %d\n",
			       t->id, msg_id, threads[recipient].id);
		} else {
			stats[recipient]++;
		}
	}
}


static void test_send_data(struct test_thread *t, unsigned int recipient)
{
	ssize_t bytes;
	size_t length;
	uint8_t buffer[TEST_DATAGRAM_SIZE];
	struct sockaddr_in addr;

	if (!t->send_data)
		return;

	memset(buffer, 0, sizeof(buffer));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT_BASE + (recipient * 0x10) + t->id);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	length = (rand() % (TEST_DATAGRAM_SIZE - 1)) + 1;

	bytes = sendto(t->tx_socket, buffer, length, 0,
		       (struct sockaddr *)&addr, sizeof(struct sockaddr));
	if (bytes != length) {
		printf("thread %d: failed to send %zd bytes to thread %d, %zd, %s\n",
		       t->id, length, recipient, bytes, strerror(errno));
	}

	if (bytes > 0)
		t->data_txed[recipient] += bytes;
}


static void test_send_wait(struct test_thread *t, unsigned int recipient,
			  unsigned int msg_id, unsigned int stats[])
{
	test_msg_t m;
	int rc;

	assert(t != NULL);
	assert(stats != NULL);

	if (!t->send_msgs)
		return;

	SFPTPD_MSG_INIT(m);
	m.sender = t->id;
	rc = SFPTPD_MSG_SEND_WAIT(&m, threads[recipient].thread, msg_id);
	if (rc != 0) {
		printf("thread %d: failed to send-wait msg %x to thread %d\n",
		       t->id, msg_id, threads[recipient].id);
	} else {
		stats[recipient]++;
	}
}



static int test_on_startup(void *context)
{
	struct sfptpd_timespec interval;
	unsigned int i;
	int rc, flags;
	struct sockaddr_in addr;

	struct test_thread *t = (struct test_thread *)context;

	if (test_timers) {
		/* Create some timers */
		for (i = 0; i < TEST_NUM_TIMERS; i++) {
			rc = sfptpd_thread_timer_create(i, CLOCK_MONOTONIC,
							test_on_timer, t);
			if (rc != 0) {
				printf("thread %d: failed to create timer %d, %d\n",
				       t->id, i, rc);
				return rc;
			}

			/* Set interval 10ms + thread * 3ms + timer * 5ms */
			t->timer_interval[i] = 10000000 + ((t->id * 3) + (i * 5)) * 1000000;
			interval.sec = t->timer_interval[i] / 1000000000;
			interval.nsec = t->timer_interval[i] % 1000000000;
			rc = sfptpd_thread_timer_start(i, true, false, &interval);
			if (rc != 0) {
				printf("thread %d: failed to start timer %d, %d\n",
				       t->id, i, rc);
				return rc;
			}
		}

		/* Try to create a timer that already exists */
		rc = sfptpd_thread_timer_create(0, CLOCK_MONOTONIC,
						test_on_timer, t);
		if (rc != EALREADY) {
			printf("thread %d: unexpectedly created a timer that already exists, %d\n",
			       t->id, rc);
			return rc;
		}
	}

	if (test_signals) {
		/* Try a timer to generate data/signals */
		rc = sfptpd_thread_timer_create(100, CLOCK_MONOTONIC,
						test_on_timer, t);
		if (rc != 0) {
			printf("ERROR: failed to create signal timer, %d\n", rc);
			return rc;
		}

		interval.sec = 0;
		interval.nsec = TEST_SIGNAL_TEST_INTERVAL * (t->id + 1);
		rc = sfptpd_thread_timer_start(100, true, false, &interval);
		if (rc != 0) {
			printf("ERROR: failed to start signal timer, %d\n", rc);
			return rc;
		}
	}

	if (test_user_fds) {
		t->tx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (t->tx_socket < 0) {
			printf("thread %d: failed to open socket, %s\n",
			       t->id, strerror(errno));
			return errno;
		}

		/* Make TX non-blocking */
		flags = fcntl(t->tx_socket, F_GETFL, 0);
		if (flags == -1) {
			ERROR("thread %d: failed to get socket flags, %s\n",
			      t->id, strerror(errno));
			return errno;
		}

		/* Clear the blocking flag. */
		flags |= O_NONBLOCK;
		if (fcntl(t->tx_socket, F_SETFL, flags) == -1) {
			ERROR("thread %d: failed to set socket flags, %s\n",
			      t->id, strerror(errno));
			return errno;
		}

		for (i = 0; i < TEST_NUM_THREADS; i++) {
			t->rx_sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (t->rx_sockets[i] < 0) {
				printf("thread %d: failed to open socket, %s\n",
				       t->id, strerror(errno));
				return errno;
			}

			addr.sin_family = AF_INET;
			addr.sin_port = htons(TEST_PORT_BASE + (t->id * 0x10) + i);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			rc = bind(t->rx_sockets[i], (struct sockaddr *)&addr,
				  sizeof(struct sockaddr));
			if (rc != 0) {
				printf("thread %d: failed to bind socket to port %d, %s\n",
				       t->id, addr.sin_port, strerror(rc));
				return errno;
			}

			rc = sfptpd_thread_user_fd_add(t->rx_sockets[i], true, false);
			if (rc != 0) {
				printf("thread %d: failed to add fd %d to epoll, %s\n",
				       t->id, t->rx_sockets[i], strerror(rc));
				return rc;
			}
		}
	}

	return 0;
}

static void test_on_shutdown(void *context)
{
	unsigned int i;
	struct test_thread *t = (struct test_thread *)context;

	//printf("thread %d: on_shutdown()\n", t->id);

	if (test_user_fds) {
		(void)close(t->tx_socket);
		for (i = 0; i < TEST_NUM_THREADS; i++)
			(void)close(t->rx_sockets[i]);
	}

	if (test_signals) {
		sfptpd_thread_timer_stop(100);
	}
}

static void test_on_timer(void *context, unsigned int id)
{
	struct test_thread *t = (struct test_thread *)context;
	int rc;

	/* If this is the signal test timer, send a signal */
	if (id == 100) {
		if (!t->send_signals)
			return;

		//printf("thread %d: sending signal...\n", t->id);
		/* Send a signal to the process */
		rc = kill(getpid(), SIGRTMIN + t->id);
		if (rc != 0) {
			printf("ERROR: root: failed to send signal %d to root thread, %d\n",
			       SIGRTMIN + t->id, errno);
		}
		t->signals_txed += 1;
	} else {
		t->timer_count[id] += 1;
		//if ((t->timer_count[id] % 64) == 0) {
		//	printf("thread %d: timer %d expired %ld times\n",
		//	       t->id, id, t->timer_count[id]);
		//}
	}


}

static void test_on_message(void *context, struct sfptpd_msg_hdr *msg)
{
	struct test_thread *t = (struct test_thread *)context;
	test_msg_t *m;
	unsigned int i, sender, recipient;
	int rc;

	//printf("thread %d: on_message\n", t->id);

	m = (test_msg_t *)msg;
	switch (sfptpd_msg_get_id(msg)) {
	case TEST_MSG_ID_START:
		SFPTPD_MSG_FREE(m);

		if (test_messaging) {
			printf("thread %d: start messaging test\n", t->id);
			t->send_msgs = true;

			/* Pass (no reply) a message to all other threads */
			for (i = 0; i < TEST_NUM_THREADS; i++) {
				if (t != &threads[i]) {
					test_send_msg(t, i, TEST_MSG_ID_ASYNC,
						      false, t->asyncs_txed);
				}
			}

			/* Send (no reply) a message to all other threads */
			for (i = 0; i < TEST_NUM_THREADS; i++) {
				if (t != &threads[i]) {
					test_send_msg(t, i, TEST_MSG_ID_REQ,
						      true, t->reqs_txed);
				}
			}
		}

		if (test_user_fds) {
			printf("thread %d: start user fds test\n", t->id);
			t->send_data = true;

			/* Send data to all threads */
			for (i = 0; i < TEST_NUM_THREADS; i++)
				test_send_data(t, i);
		}

		if (test_signals) {
			printf("thread %d: start signal test\n", t->id);
			t->send_signals = true;
		}
		break;

	case TEST_MSG_ID_STOP:
		SFPTPD_MSG_FREE(m);
		for (i = 0; i < TEST_NUM_TIMERS; i++)
			sfptpd_thread_timer_stop(i);
		if (test_messaging)
			printf("thread %d: stop messaging test\n", t->id);
		if (test_user_fds)
			printf("thread %d: stop user fds test\n", t->id);
		if (test_signals)
			printf("thread %d: stop sending signals\n", t->id);
		t->send_msgs = false;
		t->send_data = false;
		t->send_signals = false;
		break;

	case TEST_MSG_ID_ASYNC:
		sender = m->sender;
		assert(sender < TEST_NUM_THREADS);
		//printf("thread %d: got async from thread %d\n", t->id, sender);
		t->asyncs_rxed[sender]++;
		SFPTPD_MSG_FREE(m);

		/* Send another request to the next thread */
		recipient = (sender + 1) % TEST_NUM_THREADS;
		if (recipient == t->id)
			recipient = (recipient + 1) % TEST_NUM_THREADS;

		test_send_msg(t, recipient, TEST_MSG_ID_ASYNC, false, t->asyncs_txed);

		/* Send another request to the next thread */
		recipient = (sender + 1) % TEST_NUM_THREADS;
		if (recipient == t->id)
			recipient = (recipient + 1) % TEST_NUM_THREADS;

		/* Send waits require a contract between each pair of threads-
		 * for each pair, only one should carry out send-waits with the
		 * other. */
		if ((recipient > t->id) && ((t->asyncs_rxed[sender] % 16) == 0))
			test_send_wait(t, recipient, TEST_MSG_ID_BLOCKING_REQ, t->syncs_txed);
		break;

	case TEST_MSG_ID_REQ:
		sender = m->sender;
		assert(sender < TEST_NUM_THREADS);
		//printf("thread %d: got req from thread %d\n", t->id, sender);
		t->reqs_rxed[sender]++;

		m->sender = t->id;
		SFPTPD_MSG_SET_ID(m, TEST_MSG_ID_RESP);
		rc = SFPTPD_MSG_REPLY(m);
		if (rc != 0) {
			printf("ERROR: thread %d: failed to send resp to thread %d\n",
			       t->id, sender);
		} else {
			t->resps_txed[sender]++;
		}

		/* Send another request to the next thread */
		recipient = (sender + 1) % TEST_NUM_THREADS;
		if (recipient == t->id)
			recipient = (recipient + 1) % TEST_NUM_THREADS;

		test_send_msg(t, recipient, TEST_MSG_ID_REQ, true, t->reqs_txed);
		break;

	case TEST_MSG_ID_RESP:
		sender = m->sender;
		assert(sender < TEST_NUM_THREADS);
		//printf("thread %d: got resp from thread %d\n", t->id, sender);
		t->resps_rxed[sender]++;
		SFPTPD_MSG_FREE(m);
		break;

	case TEST_MSG_ID_BLOCKING_REQ:
		sender = m->sender;
		assert(sender < TEST_NUM_THREADS);
		//printf("thread %d: got send-wait from thread %d\n", t->id, sender);
		t->syncs_rxed[sender]++;
		SFPTPD_MSG_SET_ID(m, TEST_MSG_ID_BLOCKING_RESP);
		rc = SFPTPD_MSG_REPLY(m);
		if (rc != 0) {
			printf("ERROR: thread %d: failed to send sync resp to thread %d\n",
			       t->id, sender);
		}
		break;
		
	
	default:
		printf("unknown msg %d\n", sfptpd_msg_get_id(msg));
	}
}

static void test_on_user_fd(void *context, unsigned int num_fds,
			    struct sfptpd_thread_readyfd events[])
{
	unsigned int idx;

	for (idx = 0; idx < num_fds; idx++) {
		int fd = events[idx].fd;
		ssize_t bytes;
		unsigned int i, recipient;
		struct test_thread *t = (struct test_thread *)context;
		uint8_t buffer[TEST_DATAGRAM_SIZE];

		//printf("thread %d: on_user_fd()\n", t->id);

		for (i = 0; (i < TEST_NUM_THREADS) && (t->rx_sockets[i] != fd); i++);
		
		if (i >= TEST_NUM_THREADS) {
			printf("thread %d: unexpected user fd %d\n", t->id, fd);
			return;
		}

		/* Transmit some data to another thread */
		recipient = (i + 1) % TEST_NUM_THREADS;

		while ((bytes = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
			t->data_rxed[i] += bytes;
			//printf("thread %d: rxed %ld bytes\n", t->id, bytes);

			test_send_data(t, recipient);
		}
		
		if ((bytes < 0) && (errno != EAGAIN)) {
			printf("thread %d: error from socket fd %d, %s\n",
			       t->id, fd, strerror(errno));
		}
	}
}

static int root_on_startup(void *context)
{
	int rc;
	unsigned int i;
	struct sfptpd_timespec interval;
	sigset_t signal_set;
	test_msg_t *msg;

	printf("root_startup: creating %d threads\n", TEST_NUM_THREADS);

	for (i = 0; i < TEST_NUM_THREADS; i++) {
		threads[i].id = i;
		sprintf(threads[i].name, "thread%d", i);
		sigemptyset(&signal_set);
		sigaddset(&signal_set, SIGRTMIN + i);

		rc = sfptpd_thread_create(threads[i].name, &test_thread_ops,
					  &threads[i], &threads[i].thread);
		if (rc != 0) {
			printf("ERROR: failed to create thread %d, %d\n", i, rc);
			return rc;
		}
		//printf("created thread %d = %p\n", i, threads[i].thread);
	}

	/* Create a timer to use to end the test */
	rc = sfptpd_thread_timer_create(0, CLOCK_MONOTONIC,
					root_on_timer, NULL);
	if (rc != 0) {
		printf("ERROR: failed to create exit timer, %d\n", rc);
		return rc;
	}

	if (test_messaging || test_user_fds || test_signals) {
		/* If we are running socket tests, we have to ensure all threads are
		 * initialised and running before starting to send data */
		if (test_user_fds)
			sleep(1);

		for (i = 0; i < TEST_NUM_THREADS; i++) {
			msg = (test_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);

			rc = SFPTPD_MSG_SEND(msg, threads[i].thread,
					     TEST_MSG_ID_START, false);
			if (rc != 0) {
				printf("ERROR: failed to send start msg to thread %d\n", i);
				return rc;
			}
		}
	}

	interval.sec = TEST_TIMER_TEST_LEN;
	interval.nsec = 0;
	rc = sfptpd_thread_timer_start(0, false, false, &interval);
	if (rc != 0) {
		printf("ERROR: failed to start exit timer, %d\n", rc);
		return rc;
	}

	return 0;
}

static void root_on_shutdown(void *context)
{
	unsigned int i, j;
	uint64_t expected;
	int rc;
	bool ok;

	//printf("root: on_shutdown()\n");

	rc = 0;

	/* Check whether the results are good */
	if (test_timers) {
		for (i = 0; i < TEST_NUM_THREADS; i++) {
			for (j= 0; j < TEST_NUM_TIMERS; j++) {
				expected = (TEST_TIMER_TEST_LEN * 1000000000ULL)
					 / threads[i].timer_interval[j];
				/* Allow for 5 tick errors to account for
				 * asynchronous startup/teardown. Don't check
				 * the counts if running a messaging test as
				 * it overloads the threads and interferes 
				 * with timer behaviour. */
				if (!test_messaging &&
				    ((threads[i].timer_count[j] < expected - 5) ||
				     (threads[i].timer_count[j] > expected + 5))) {
					printf("ERROR: thread %d, timer %d expected %"PRIi64", got %"PRIi64"\n",
					       i, j, expected, threads[i].timer_count[j]);
					rc = ERANGE;
				} else {
					//printf("thread %d, timer %d expired %"PRIi64" times\n",
					//       i, j, threads[i].timer_count[j]);
				}
			}
		}
	}

	if (test_messaging) {
		/* We expect the number of messages send and received of each
		 * type to be the same */
		for (i = 0; i < TEST_NUM_THREADS; i++) {
			for (j = 0; j < TEST_NUM_THREADS; j++) {
				ok = (threads[i].asyncs_txed[j] == threads[j].asyncs_rxed[i]);
				if (!ok)
					rc = ERANGE;
				//printf("%sthread %d->%d asyncs txed %d %c= rxed %d\n",
				//       ok? "": "ERROR: ", i, j,
				//       threads[i].asyncs_txed[j],
				//       ok? '=': '!',
				//       threads[j].asyncs_rxed[i]);

				ok = (threads[i].reqs_txed[j] == threads[j].reqs_rxed[i]);
				if (!ok)
					rc = ERANGE;
				//printf("%sthread %d->%d requests txed %d %c= rxed %d\n",
				//       ok? "": "ERROR: ", i, j,
				//       threads[i].reqs_txed[j],
				//       ok? '=': '!',
				//       threads[j].reqs_rxed[i]);

				ok = (threads[i].resps_txed[j] == threads[j].resps_rxed[i]);
				if (!ok)
					rc = ERANGE;
				//printf("%sthread %d->%d responses txed %d %c= rxed %d\n",
				//       ok? "": "ERROR: ", i, j,
				//       threads[i].resps_txed[j],
				//       ok? '=': '!',
				//       threads[j].resps_rxed[i]);

				ok = (threads[i].syncs_txed[j] == threads[j].syncs_rxed[i]);
				if (!ok)
					rc = ERANGE;
				//printf("%sthread %d->%d syncs txed %d %c= rxed %d\n",
				//       ok? "": "ERROR: ", i, j,
				//       threads[i].syncs_txed[j],
				//       ok? '=': '!',
				//       threads[j].syncs_rxed[i]);
				
			}
		}
	}

	if (test_signals) {
		for (i = 0; i < TEST_NUM_THREADS; i++) {
			if (expect_signal_coalescing) {
				if (test_signals_rxed[i] > threads[i].signals_txed) {
					printf("ERROR: thread %d: signals %d rxed > %d txed\n",
					       i, test_signals_rxed[i], threads[i].signals_txed);
					rc = ERANGE;
				} else {
					printf("thread %d: signals %d rxed <= %d txed\n",
					       i, test_signals_rxed[i], threads[i].signals_txed);
				}
			} else {
				if ((test_signals_rxed[i] != threads[i].signals_txed) &&
				    (test_signals_rxed[i] + 1 != threads[i].signals_txed)) {
					printf("ERROR: thread %d: signals %d rxed != %d txed\n",
					       i, test_signals_rxed[i], threads[i].signals_txed);
					rc = ERANGE;
				} else {
					printf("thread %d: signals %d rxed ~= %d txed\n",
					       i, test_signals_rxed[i], threads[i].signals_txed);
				}
			}
		}
	}

	if (test_user_fds) {
		/* We expect the amount of data send and received between each
		 * pair of threads to be the same */
		for (i = 0; i < TEST_NUM_THREADS; i++) {
			for (j = 0; j < TEST_NUM_THREADS; j++) {
				ok = (threads[i].data_txed[j] == threads[j].data_rxed[i]);
				if (!ok)
					rc = ERANGE;
				//printf("%sthread %d->%d data txed %d %c= rxed %d\n",
				//       ok? "": "ERROR: ", i, j,
				//       threads[i].data_txed[j],
				//       ok? '=': '!',
				//       threads[j].data_rxed[i]);
			}
		}
	}

	for (i = 0; i < TEST_NUM_THREADS; i++) {
		//printf("killing thread %d\n", i);
		sfptpd_thread_destroy(threads[i].thread);
	}

	test_rc = rc;
}

static void root_on_message(void *context, struct sfptpd_msg_hdr *msg)
{
	printf("root: on_message()\n");
}

static void root_on_timer(void *context, unsigned int id)
{
	test_msg_t *msg;
	int i;
	if (id == 0) {
		printf("root: exit timer expired\n");
		if (test_messaging || test_user_fds || test_signals) {
			for (i = 0; i < TEST_NUM_THREADS; i++) {
				msg = (test_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);

				SFPTPD_MSG_SEND(msg, threads[i].thread,
						TEST_MSG_ID_STOP, false);
			}

			sleep(1);
		}

		sfptpd_thread_exit(0);
	} else {
		printf("ERROR: root: unexpected timer %d\n", id);
	}
}

static void root_on_user_fd(void *context, unsigned int num_fds,
			    struct sfptpd_thread_readyfd events[])
{
	printf("root: unexpected user event with %u active sockets\n", num_fds);
	assert(false);
}

static void root_on_signal(void *user_context, int signal_num)
{
	//printf("root: rxed signal %d\n", signal_num);

	if ((signal_num >= SIGRTMIN) && (signal_num < SIGRTMIN + TEST_NUM_THREADS)) {
		int t = signal_num - SIGRTMIN;
		test_signals_rxed[t] += 1;
		return;
	}

	if ((signal_num == SIGINT) || (signal_num == SIGTERM))
		printf("root: received signal %d\n", signal_num);
	else
		printf("ERROR: root: received unexpected signal %d\n", signal_num);
	sfptpd_thread_exit(0);
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

static int test_threading(const char *name, bool timers, bool messaging,
			  bool signals, bool user_fds, bool signal_coalescing)
{
	sigset_t signal_set;
	unsigned int i;
	
	printf("threading test %s...\n", name);
	
	test_rc = sfptpd_threading_initialise(256, sizeof(test_msg_t), 0);
	if (test_rc != 0) {
		printf("failed to initialise threading support, %d\n", test_rc);
		return test_rc;
	}

	test_timers = timers;
	test_messaging = messaging;
	test_signals = signals;
	test_user_fds = user_fds;
	expect_signal_coalescing = signal_coalescing;

	memset(test_signals_rxed, 0, sizeof(test_signals_rxed));
	memset(threads, 0, sizeof(threads));

	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGINT);
	sigaddset(&signal_set, SIGTERM);
	for (i = SIGRTMIN; i < SIGRTMIN + TEST_NUM_THREADS; i++) {
		sigaddset(&signal_set, i);
	}
	sfptpd_thread_main(&root_thread_ops, &signal_set, root_on_signal, NULL);

	sfptpd_threading_shutdown();

	printf("threading test %s: rc = %d\n", name, test_rc);

	return test_rc;
}

int sfptpd_test_threading(void)
{
	int rc = 0;
	unsigned int i;

	/* Change this to run a soak */
	bool soak = false;

	//sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_THREADING, 0);

	sigemptyset(&test_signal_set);
	sigaddset(&test_signal_set, SIGINT);
	sigaddset(&test_signal_set, SIGTERM);
	for (i = SIGRTMIN; i < SIGRTMAX; i++)
		sigaddset(&test_signal_set, i);

	do {
		/* Timers */
		rc = test_threading("timers", true, false, false, false, false);
		if (rc != 0)
			return rc;

		/* Messages */
		rc = test_threading("messaging", false, true, false, false, false);
		if (rc != 0)
			return rc;

		/* Signals */
		rc = test_threading("signals", false, false, true, false, false);
		if (rc != 0)
			return rc;

		/* User FDs */
		rc = test_threading("user fds", false, false, false, true, false);
		if (rc != 0)
			return rc;

		/* Everything */
		rc = test_threading("everything", true, true, true, true, true);
		if (rc != 0)
			return rc;
	} while (soak);

	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_SFPTPD, 0);
	return rc;
}


/* fin */
