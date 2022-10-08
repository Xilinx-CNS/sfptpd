/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_NETLINK_H
#define _SFPTPD_NETLINK_H

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>

/** Forward structure declarations */
struct sfptpd_engine;

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

struct sfptpd_netlink_event {
	int32_t if_index;
	char if_name[IFNAMSIZ];
	int8_t insert;
} __attribute__ ((packed));

#define SFPTPD_EVENT_BUFFER_SIZE 8


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int sfptpd_netlink_init(int *fd, unsigned int timer_id);
int sfptpd_netlink_service_fd(struct sfptpd_engine *engine, int fd);
void sfptpd_netlink_finish(int fd);
void netlink_flush_buffer(struct sfptpd_engine *engine);


#endif /* SFPTPD_NETLINK_H */
