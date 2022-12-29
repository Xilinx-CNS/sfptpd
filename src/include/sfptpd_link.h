/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_LINK_H
#define _SFPTPD_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <net/if.h>

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

#define SFPTPD_LINK_TABLE_SIZE 128

enum sfptpd_link_type {
	SFPTPD_LINK_PHYSICAL,
	SFPTPD_LINK_BOND,
	SFPTPD_LINK_TEAM,
	SFPTPD_LINK_BRIDGE,
	SFPTPD_LINK_VLAN,
	SFPTPD_LINK_MACVLAN,
	SFPTPD_LINK_IPVLAN,
	SFPTPD_LINK_VETH,
	SFPTPD_LINK_DUMMY,
	SFPTPD_LINK_TUNNEL,
	SFPTPD_LINK_OTHER,
};

enum sfptpd_link_event {
	SFPTPD_LINK_NONE,
	SFPTPD_LINK_DOWN,
	SFPTPD_LINK_UP,
	SFPTPD_LINK_CHANGE,
};

enum sfptpd_bond_mode {
	SFPTPD_BOND_MODE_NONE,
	SFPTPD_BOND_MODE_ACTIVE_BACKUP,
	SFPTPD_BOND_MODE_LACP,
	SFPTPD_BOND_MODE_UNSUPPORTED,
	SFPTPD_NUM_BOND_TYPES = SFPTPD_BOND_MODE_UNSUPPORTED
};

struct sfptpd_link {
	enum sfptpd_link_type type;
	enum sfptpd_link_event event;

	int if_index;
	int if_type;
	int if_family;
	int if_flags;
	char if_name[IF_NAMESIZE];
	char if_kind[16];
	int if_link;

	struct {
		int if_master;
		enum sfptpd_bond_mode bond_mode;
		int active_slave;
	} bond;
	bool is_slave;
	uint16_t vlan_id;
};

struct sfptpd_link_table {
	struct sfptpd_link rows[SFPTPD_LINK_TABLE_SIZE];
	int count;
	int version;
};


/****************************************************************************
 * Functions
 ****************************************************************************/

const struct sfptpd_link *sfptpd_link_by_name(const struct sfptpd_link_table *link_table,
					      const char *link_name);


const struct sfptpd_link *sfptpd_link_by_if_index(const struct sfptpd_link_table *link_table,
						  int if_index);

#endif
