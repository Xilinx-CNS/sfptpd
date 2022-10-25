/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_NETLINK_H
#define _SFPTPD_NETLINK_H

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>

#include "sfptpd_link.h"

/** Forward structure declarations */

struct sfptpd_nl_state;

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

/* All functions from this module must be called from the same thread
 * except for sfptpd_netlink_get_table() which may be called by
 * consumers who have been ref-counted for that table.
 * In sfptpd the engine thread owns this module.
 */

struct sfptpd_nl_state *sfptpd_netlink_init(void);
int sfptpd_netlink_service_fds(struct sfptpd_nl_state *state,
			       int *fds, int num_fds, int consumers);
int sfptpd_netlink_scan(struct sfptpd_nl_state *state);
void sfptpd_netlink_finish(struct sfptpd_nl_state *state);
void netlink_flush_buffer(struct sfptpd_nl_state *state);
int sfptpd_netlink_get_fd(struct sfptpd_nl_state *state,
			  int *get_fd_state);
int sfptpd_netlink_get_table(struct sfptpd_nl_state *state, int version, const struct sfptpd_link_table **table);
int sfptpd_netlink_release_table(struct sfptpd_nl_state *state, int version, int consumers);

#endif /* SFPTPD_NETLINK_H */
