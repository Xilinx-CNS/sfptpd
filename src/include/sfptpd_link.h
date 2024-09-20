/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2022-2023 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_LINK_H
#define _SFPTPD_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <net/if.h>
#include <linux/if_bonding.h>
#include <linux/ethtool.h>

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

#define SFPTPD_L2ADDR_MAX 10
#define SFPTPD_L2ADDR_STR_MAX SFPTPD_L2ADDR_MAX * 3

#define SFPTPD_LINK_STATS_MAX 16

enum sfptpd_link_type {
	SFPTPD_LINK_PHYSICAL,
	SFPTPD_LINK_VLAN,
	SFPTPD_LINK_BOND,
	SFPTPD_LINK_TEAM,
	SFPTPD_LINK_BRIDGE,
	SFPTPD_LINK_MACVLAN,
	SFPTPD_LINK_IPVLAN,
	SFPTPD_LINK_VETH,
	SFPTPD_LINK_DUMMY,
	SFPTPD_LINK_TUNNEL,
	SFPTPD_LINK_OTHER,
	SFPTPD_LINK_MAX
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

enum sfptpd_link_fulfillment_state {
	QRY_NOT_REQUESTED,
	QRY_REQUESTED,
	QRY_NACKED,
	QRY_POPULATED,
};

struct sfptpd_l2addr {
	size_t len;
	uint8_t addr[SFPTPD_L2ADDR_MAX];
	char string [SFPTPD_L2ADDR_STR_MAX];
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
	struct sfptpd_l2addr perm_addr;
	char bus_addr[ETHTOOL_BUSINFO_LEN];
	struct {
		int if_master;
		enum sfptpd_bond_mode bond_mode;
		int active_slave;
		uint8_t xmit_hash_policy;
	} bond;
	bool is_slave;
	uint16_t vlan_id;

	enum sfptpd_link_fulfillment_state ts_info_state;
	struct ethtool_ts_info ts_info;

	enum sfptpd_link_fulfillment_state drv_stats_ids_state;
	struct {
		int all_count;
		int requested_ids[SFPTPD_LINK_STATS_MAX];
	} drv_stats;

	/* Not for client use */
	void *priv;
};

struct sfptpd_link_table {
	struct sfptpd_link *rows;
	int count;
	int version;
};


/****************************************************************************
 * Functions
 ****************************************************************************/

const char *sfptpd_link_event_str(enum sfptpd_link_event event);

const char *sfptpd_link_type_str(enum sfptpd_link_type type);

const struct sfptpd_link *sfptpd_link_by_name(const struct sfptpd_link_table *link_table,
					      const char *link_name);


const struct sfptpd_link *sfptpd_link_by_if_index(const struct sfptpd_link_table *link_table,
						  int if_index);

int sfptpd_link_table_copy(const struct sfptpd_link_table *src,
			   struct sfptpd_link_table *dest);

void sfptpd_link_table_free_copy(struct sfptpd_link_table *copy);

const char *sfptpd_link_xmit_hash_policy(const struct sfptpd_link *link);

#endif
