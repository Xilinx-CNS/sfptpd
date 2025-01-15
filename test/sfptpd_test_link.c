/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019,2022 Xilinx, Inc. */

/**
 * @file   sfptpd_test_link.c
 * @brief  Link unit tests
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
#include <sys/epoll.h>
#include <sys/stat.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <float.h>

#include "sfptpd_config.h"
#include "sfptpd_misc.h"
#include "sfptpd_test.h"
#include "sfptpd_netlink.h"


/****************************************************************************
 * External declarations
 ****************************************************************************/


/****************************************************************************
 * Types and Defines
 ****************************************************************************/


/****************************************************************************
 * Local Data
 ****************************************************************************/

static const bool continuous = false;


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static int test_link(void)
{
	#define MAX_EVENTS 10
	struct epoll_event ev = {};
	struct epoll_event events[MAX_EVENTS];
	int fds[SFPTPD_NETLINK_MAX_FDS];
	int num_fds;
	struct sfptpd_nl_state *nl_state;
	int epollfd;
	int nfds;
	int rc = 0;
	int i;
	int consumers = 1;
	const struct sfptpd_link_table *table;
	int rows;

	nl_state = sfptpd_netlink_init();
	assert(nl_state);

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		ERROR("link: epoll_create1(), %s\n", strerror(errno));
		return 1;
	}

	num_fds = sfptpd_netlink_get_fds(nl_state, fds);
	for (i = 0; i < num_fds; i++) {
		ev.events = EPOLLIN;
		ev.data.fd = fds[i];
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fds[i], &ev) == -1) {
			ERROR("link: epoll_ctl: netlink fd, %s\n", strerror(errno));
			return 1;
		}
	}

	sfptpd_netlink_scan(nl_state);

	do {
		nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			ERROR("link: epoll_wait(), %s\n", strerror(errno));
			return 1;
		}

		rc = sfptpd_netlink_service_fds(nl_state, consumers, false);
		while (rc != 0) {
			int ver;

			if (rc < 0) {
				ERROR("link: servicing netlink fd: %s\n", strerror(rc));
				return rc;
			}

			ver = rc;
			INFO("link: change detected: table version %d\n", ver);

			rows = sfptpd_netlink_get_table(nl_state, ver, &table);
			INFO("link: table has %d rows\n", rows);

			assert(rows == table->count);

			rc = sfptpd_netlink_release_table(nl_state, ver, consumers);
		}
	} while (continuous);

	close(epollfd);
	sfptpd_netlink_finish(nl_state);

	return rc;
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_link(void)
{
	int rc;

	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_NETLINK, 6);

	rc = test_link();

	return rc;
}


/* fin */
