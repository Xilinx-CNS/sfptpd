/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2016 Xilinx, Inc. */

/**
 * @file   sfptpd_netlink.c
 * @brief  Receives link change events from netlink.
 */

#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

#include "sfptpd_constants.h"
#include "sfptpd_logging.h"
#include "sfptpd_control.h"
#include "sfptpd_interface.h"
#include "sfptpd_netlink.h"
#include "sfptpd_engine.h"
#include "sfptpd_thread.h"



/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

const struct timespec timeout_interval = {
	.tv_sec = 0,
	.tv_nsec = 500000000
};

/****************************************************************************
 * Local Variables
 ****************************************************************************/

/* All state to be accessed through engine thread only */

static struct sfptpd_netlink_event event_buffer[SFPTPD_EVENT_BUFFER_SIZE];
static int event_buffer_idx = 0;
static int netlink_timer_id;


/****************************************************************************
 * Local Functions
 ****************************************************************************/

void netlink_flush_buffer(struct sfptpd_engine *engine) {
	sfptpd_engine_interface_events(engine, event_buffer, event_buffer_idx);
	event_buffer_idx = 0;
}

int netlink_buffer_event(struct sfptpd_engine *engine,
			 bool insert,
			 int if_index,
			 const char *if_name) {

	/* Pointer to an event in the event buffer that
	   is under consideration for overwriting with
	   a later event that supercedes it. */
	struct sfptpd_netlink_event *other_event;

	/* Pointer to where we will write the new event.
	   Starts off at the buffer's 'write pointer' but
	   might get changed to point to an old entry to
	   be overwritten. */
	struct sfptpd_netlink_event *copy_to;

	if (event_buffer_idx == SFPTPD_EVENT_BUFFER_SIZE) {

		/* Buffer full: send events to engine */
		sfptpd_thread_timer_stop(netlink_timer_id);
		TRACE_L3("netlink: flushing full interface change event buffer full\n");
		netlink_flush_buffer(engine);
	}

	/* Try to coalesce event.
	   If there is an older event for the same device then we
	   overwrite it UNLESS the old event was a deletion and the
	   new event was an insertion, so that a deletion is
	   guaranteed to take effect first.
	   Note that changes appear as insertions.
	*/
	other_event = copy_to = event_buffer + event_buffer_idx;

	/* Search backwards through the buffer so we process the latest
	   events first. */
	while (--other_event >= event_buffer) {

		/* Look for an event for the same interface */
		if (other_event->if_index == if_index) {

			/* Overwrite matching event without
			   eliminating deleted periods */
			if (!insert || other_event->insert) {

				/* Cause event to be overwritten
				   by moving the copy pointer to it. */
				copy_to = other_event;
			}

			/* No need to go back any further as any
			   earlier events are already coalesced. */
			break;
		}
	}

	/* Append or overwrite with new event by copying at the copy pointer */
	copy_to->insert = insert;
	copy_to->if_index = if_index;
	strncpy(copy_to->if_name,
		if_name,
		sizeof copy_to->if_name);

	/* If the copy pointer was at the event buffer write pointer position
	   then the we have filled another slot in the event buffer so advance
	   its write pointer. */
	if (copy_to == event_buffer + event_buffer_idx) {
		event_buffer_idx++;
	}

	sfptpd_thread_timer_start(netlink_timer_id, false, false, &timeout_interval);

	return 0;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_netlink_init(int *fd, unsigned int timer_id)
{
	int rc;
	int nl_fd;
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK
	};

	/* Save the timer id */
	netlink_timer_id = timer_id;

	/* Create a Unix domain socket for receiving control packets */
	nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nl_fd == -1) {
		ERROR("couldn't create netlink socket");
	        return errno;
	}

	/* Bind to the path in the filesystem. */
	rc = bind(nl_fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		ERROR("couldn't bind netlink socket");
	        return errno;
	}

	event_buffer_idx = 0;

	*fd = nl_fd;
	return 0;
}


static size_t netlink_create_interface_query(char *buf, size_t space) {
	struct nlmsghdr nh = { 0 };
	struct ifinfomsg ifinfomsg = { 0 };
	static int seq;
	const size_t length = sizeof nh + sizeof ifinfomsg;

	assert(buf != NULL);
	assert(length <= space);

	nh.nlmsg_pid = 0;
	nh.nlmsg_seq = ++seq;
	nh.nlmsg_len = NLMSG_LENGTH(sizeof ifinfomsg);
	nh.nlmsg_type = RTM_GETLINK;
	nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;

	ifinfomsg.ifi_family = AF_UNSPEC;

	memcpy(buf, &nh, sizeof nh);
	memcpy(buf + sizeof nh, &ifinfomsg, sizeof ifinfomsg);

	return length;
}


int sfptpd_netlink_service_fd(struct sfptpd_engine *engine, int fd)
{
	int len;
	char buf[4096];
	struct iovec iov = { buf, sizeof buf };
	struct sockaddr_nl peer;
	struct nlmsghdr *nh;
	struct msghdr msg;
	int rc = 0;

	TRACE_L4("netlink: handling netlink packet\n");

	do {
		msg.msg_name = &peer;
		msg.msg_namelen = sizeof peer;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;

		len = recvmsg(fd, &msg, MSG_DONTWAIT);
		assert(len >= -1);

		if (len == -1) {
			if (errno == ENOBUFS) {
				/* Events were lost in the kernel so request all
				   current entries to be dumped. */
				WARNING("netlink: interface events overran kernel buffer: refetching state\n");
				memset(&peer, 0, sizeof peer);
				peer.nl_family = AF_NETLINK;
				iov.iov_len = netlink_create_interface_query(buf, sizeof buf);
				len = sendmsg(fd, &msg, 0);
				assert(len != -1);
				iov.iov_len = sizeof buf;
			} else if(errno == EAGAIN || errno == EINTR) {
				/* No problem. Wait til next event on fd */
				break;
			} else {
				ERROR("netlink: error receiving netlink events, %s\n", strerror(errno));
				rc = errno;
				break;
			}
		} else if (len == 0) {
			break;
		} else {
			for (nh = (struct nlmsghdr *) buf; NLMSG_OK (nh, len);
			     nh = NLMSG_NEXT (nh, len)) {

				/* The end of multipart message */
				if (nh->nlmsg_type == NLMSG_DONE) {
					goto finish;
				}
				if (nh->nlmsg_type == NLMSG_ERROR) {
					struct nlmsgerr *err;

					/* Do some error handling */
					err = (struct nlmsgerr *) NLMSG_DATA(nh);
					if (err->error != 0) {
						ERROR("netlink: error from netlink\n");
						rc = EIO;
						break;
					}
				} else {
					struct ifinfomsg *info = (struct ifinfomsg *) NLMSG_DATA(nh);
					struct rtattr *attr;
					int len2 = nh->nlmsg_len - NLMSG_LENGTH(sizeof *info);
					const char *if_name = NULL;

					switch (nh->nlmsg_type) {
					case RTM_NEWLINK:
					case RTM_DELLINK:
						for (attr = IFLA_RTA(info); RTA_OK(attr, len2); attr = RTA_NEXT(attr, len2)) {
							if (attr->rta_type == IFLA_IFNAME) {
								if_name = (char *) RTA_DATA(attr);
								TRACE_L4("netlink: received RTM_%sLINK event, if_index %d, %s\n",
									 (nh->nlmsg_type == RTM_NEWLINK ? "NEW" : "DEL"),
									 info->ifi_index,
									 (char *) RTA_DATA(attr));
								break;
							}
						}

						assert(if_name != NULL);

						rc = netlink_buffer_event(engine,
									  nh->nlmsg_type == RTM_NEWLINK,
									  info->ifi_index,
									  if_name);
						if (rc != 0) break;
					}
				}
			}
		}
	} while (true);

 finish:

	if (rc != 0) {
		ERROR("error handling netlink events, %s\n", strerror(rc));
	} else {
		rc = 0;
	}

	return rc;
}

void sfptpd_netlink_finish(int fd)
{
	close(fd);
}


/* fin */
