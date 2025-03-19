/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_link.c
 * @brief  Utility functions for sfptpd link table
 */

#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sfptpd_link.h"
#include "sfptpd_logging.h"
#include "sfptpd_thread.h"

/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

static const char *sfptpd_link_type_names[] = {
	[SFPTPD_LINK_PHYSICAL] = "phys",
	[SFPTPD_LINK_VLAN]     = "vlan",
	[SFPTPD_LINK_BOND]     = "bond",
	[SFPTPD_LINK_TEAM]     = "team",
	[SFPTPD_LINK_BRIDGE]   = "bridge",
	[SFPTPD_LINK_MACVLAN]  = "macvlan",
	[SFPTPD_LINK_IPVLAN]   = "ipvlan",
	[SFPTPD_LINK_VETH]     = "veth",
	[SFPTPD_LINK_DUMMY]    = "dummy",
	[SFPTPD_LINK_TUNNEL]   = "tunnel",
	[SFPTPD_LINK_OTHER]    = "other",
};


/****************************************************************************
 * Local Variables
 ****************************************************************************/


/****************************************************************************
 * Forward function declarations
 ****************************************************************************/


/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Public Functions
 ****************************************************************************/

const char *sfptpd_link_xmit_hash_policy(const struct sfptpd_link *link)
{
	switch (link->bond.bond_mode) {
	case SFPTPD_BOND_MODE_LACP:
		switch (link->bond.xmit_hash_policy) {
		case BOND_XMIT_POLICY_LAYER2:
			return "l2";
		case BOND_XMIT_POLICY_LAYER23:
			return "l2+3";
		case BOND_XMIT_POLICY_LAYER34:
			return "l3+4";
		case BOND_XMIT_POLICY_SFPTPD_UNKNOWN_HASH:
			return "hash";
		case BOND_XMIT_POLICY_SFPTPD_UNKNOWN:
		default:
			return "other";
		}
		break;
	default:
		return "";
	}
}

const char *sfptpd_link_event_str(enum sfptpd_link_event event)
{
	switch (event) {
	case SFPTPD_LINK_NONE:
		return "no-event";
	case SFPTPD_LINK_DOWN:
		return "down";
	case SFPTPD_LINK_UP:
		return "up";
	case SFPTPD_LINK_CHANGE:
		return "change";
	default:
		return "bad-link-event";
	}
}


const char *sfptpd_link_type_str(enum sfptpd_link_type type)
{
	if (type < 0 || type >= SFPTPD_LINK_MAX) {
		return "invalid";
	} else {
		return sfptpd_link_type_names[type];
	}
}


const struct sfptpd_link *sfptpd_link_by_name(const struct sfptpd_link_table *link_table,
					      const char *link_name)
{
	const struct sfptpd_link *link = NULL;
	int row;

	for (row = 0; row < link_table->count; row++) {
		if (strncmp(link_table->rows[row].if_name, link_name, IF_NAMESIZE) == 0) {
			TRACE_L4("link: table %d: found link table entry for %s\n",
				 link_table->version, link_name);
			link = link_table->rows + row;
		}
	}
	if (link == NULL) {
		TRACE_L3("link: no entry in link table version %d for %s\n",
		      link_table->version, link_name);
		errno = ENOENT;
	}
	return link;
}


const struct sfptpd_link *sfptpd_link_by_if_index(const struct sfptpd_link_table *link_table,
					          int if_index)
{
	const struct sfptpd_link *link = NULL;
	int row;

	for (row = 0; row < link_table->count; row++) {
		if (link_table->rows[row].if_index == if_index) {
			TRACE_L4("link: table %d: found link table entry for if_index %d\n",
				 link_table->version, if_index);
			link = link_table->rows + row;
		}
	}
	if (link == NULL) {
		TRACE_L3("link: no entry in link table version %d for if_index %d\n",
		      link_table->version, if_index);
		errno = ENOENT;
	}
	return link;
}


int sfptpd_link_table_copy(const struct sfptpd_link_table *src,
			   struct sfptpd_link_table *dest)
{
	assert(src != NULL);
	assert(dest != NULL);

	*dest = *src;

	dest->rows = malloc(dest->count * sizeof *dest->rows);
	if (dest->rows == NULL)
		return errno;

	memcpy(dest->rows, src->rows, dest->count * sizeof *dest->rows);
	return 0;
}


void sfptpd_link_table_free_copy(struct sfptpd_link_table *copy)
{
	assert(copy != NULL);

	free(copy->rows);

	copy->rows = NULL;
	copy->count = 0;
	copy->version = -1;
}
