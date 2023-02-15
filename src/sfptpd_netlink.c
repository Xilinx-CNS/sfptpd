/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2016-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_netlink.c
 * @brief  Reconstructs the system link, bond and team information from netlink
 *
 * The Xilinx Onload control plane server implementation was used as a
 * reference for writing this code:
 *
 * https://github.com/Xilinx-CNS/onload/tree/master/src/tools/cplane
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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <linux/genetlink.h>
#include <linux/rtnetlink.h>
#ifdef HAVE_TEAMING
#include <linux/if_team.h>
#endif
#include <linux/netlink.h>
#include <libmnl/libmnl.h>

/* On RHEL7 duplicate definitions get brought in between user and kernel
 * headers if we include everything we need. This can be avoided by not
 * including the bonding definitions and working around them.
 * https://bugzilla.redhat.com/show_bug.cgi?id=1300256 &c.
 * #include <linux/if_bonding.h>
 */
#define BOND_MODE_ACTIVEBACKUP 1
#define BOND_MODE_8023AD 4
/* End of workaround */

#include "sfptpd_constants.h"
#include "sfptpd_logging.h"
#include "sfptpd_interface.h"
#include "sfptpd_netlink.h"
#include "sfptpd_thread.h"

/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

#define INITIAL_LINK_TABLE_SIZE 32

/* Module-specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 1, x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 2, x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 3, x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 4, x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 5, x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NETLINK, 6, x, ##__VA_ARGS__)

struct flag_desc {
	int flag;
	const char *name;
};

const static struct flag_desc if_flag_descs[] = {
	{ IFF_UP		, "UP" },
	{ IFF_BROADCAST		, "BROADCAST" },
	{ IFF_DEBUG		, "DEBUG" },
	{ IFF_LOOPBACK		, "LOOPPBACK" },
	{ IFF_POINTOPOINT	, "POINTOPOINT" },
	{ IFF_NOTRAILERS	, "NOTRAILERS" },
	{ IFF_RUNNING		, "RUNNING" },
	{ IFF_NOARP		, "NOARP" },
	{ IFF_PROMISC		, "PROMISC" },
	{ IFF_ALLMULTI		, "ALLMULTI" },
	{ IFF_MASTER		, "MASTER" },
	{ IFF_SLAVE		, "SLAVE" },
	{ IFF_MULTICAST		, "MULTICAST" },
	{ IFF_PORTSEL		, "PORTSEL" },
	{ IFF_AUTOMEDIA		, "AUTOMEDIA" },
	{ IFF_DYNAMIC		, "DYNAMIC" },
	{ 0x10000		, "LOWER_UP" },
	{ 0, NULL }
};

/* Only act on changes in flags that should cause the application to change
 * behaviour. */
const static int significant_flags = IFF_RUNNING;

#ifdef HAVE_TEAMING
#define NUM_GROUPS 2
#define GRP_CTRL 0
#define GRP_TEAM 1
#endif

struct link_db {
	struct sfptpd_link_table table;
	size_t capacity;
	int refcnt;
};

enum nl_conn {
	NL_CONN_RT,
#ifdef HAVE_TEAMING
	NL_CONN_TEAM_DUMP,
	NL_CONN_TEAM_NOTIFY, /* required to follow _DUMP */
#endif
	NL_CONN_MAX,
};

struct sfptpd_nl_state;

struct nl_conn_state {
	struct mnl_socket *mnl;
	int fd;
	int seq;
	const char *name;
	int (*cb)(const struct nlmsghdr *nh, void *context);
	struct sfptpd_nl_state *state;
};

#define MAX_LINK_DB_VERSIONS 4

struct sfptpd_nl_state {
	struct nl_conn_state conn[NL_CONN_MAX];
	char *buf;
	size_t buf_sz;
	struct link_db db_hist[MAX_LINK_DB_VERSIONS];
	int db_ver_next;   /* number of next db version */
	int db_hist_next;  /* index to next db version */
	int db_hist_count; /* number of db versions populated (>=1 <=MAX) */
	bool need_rescan;  /* true when interfaces need rescanning, e.g.
			      because we didn't have genetlink family id. */
	bool need_service; /* true when fds are overdue servicing, e.g.
			      because there were no free tables. */
};

struct nl_ge_group {
	const char *name;
	int group_id;
	int family;
};

#ifdef HAVE_TEAMING
static struct nl_ge_group nl_ge_groups[NUM_GROUPS] = {
	{ "nlctrl", 0, 0 },
	{ TEAM_GENL_NAME, 0, 0 },
};
#endif


/****************************************************************************
 * Local Variables
 ****************************************************************************/


/****************************************************************************
 * Forward function declarations
 ****************************************************************************/

#ifdef HAVE_TEAMING
static struct nlmsghdr *netlink_create_team_query(struct nl_conn_state *conn,
						  char *buf, size_t space,
						  int team_ifindex);
#endif


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static int snprint_flags_delta(char *buf, size_t space, int flags1, int flags2)
{
	const struct flag_desc *flag;
	int total = 0;
	int len;

	buf[0] = '\0';
	for (flag = if_flag_descs; flag->flag != 0; flag++) {
		if ((flags1 & flag->flag) !=
		    (flags2 & flag->flag)) {
			len = snprintf(buf, space, "%s%c%s%s",
				       total != 0 ? " " : "",
				       flags2 & flag->flag ? '+' : '-',
				       flag->name,
				       flag->flag & significant_flags ? "*" : "");
			if (len >= space) {
				buf = NULL;
			} else {
				buf += len;
			}
			space -= len;
			total += len;
		}
		flags1 &= ~flag->flag;
		flags2 &= ~flag->flag;
	}

	if (flags1 != 0 || flags2 != 0) {
		total += snprintf(buf, space, " -%x +%x", flags1, flags2);
	}

	return total;
}


static void print_link(struct sfptpd_link *link)
{
	DBG_L4("if %d name %s event %s link %d kind %s type %d flags %x family %d master %d type %d bond_mode %d active_slave %d is_slave %d vlan %d\n",
	       link->if_index, link->if_name, sfptpd_link_event_str(link->event),
               link->if_link, link->if_kind, link->if_type, link->if_flags,
               link->if_family, link->bond.if_master, link->type,
               link->bond.bond_mode, link->bond.active_slave, link->is_slave,
               link->vlan_id);
}

static const char *link_bond_mode(enum sfptpd_bond_mode mode) {
	if (mode == SFPTPD_BOND_MODE_ACTIVE_BACKUP)
		return "ab";
	else if (mode == SFPTPD_BOND_MODE_LACP)
		return "lacp";
	else
		return "";
}

void sfptpd_link_log(const struct sfptpd_link *link, const struct sfptpd_link *prev)
{
	const char *header[] = { "if_index", "if_name", "flags", "kind", "mode", "role", "link", "master", "active", "vlan" };
	const char *format_header = "| %-8s | %-*s | %-5s | %-8s | %-4s | %-5s | %-6s | %-6s | %-6s | %-5s |\n";
	const char *format_record = "| %8d%3s%-*s | %05x | %-8s | %-4s | %-5s | %6d | %6d | %6d | %5d |\n";
	const char *format_flags  = "|_%8s___%-*s_| %-66s |\n";
	char flags[256];

	if (link == NULL) {
		DBG_L1(format_header,
		       header[0], IF_NAMESIZE, header[1], header[2], header[3],
		       header[4], header[5], header[6], header[7],
		       header[8], header[9]);
	} else {
		if (prev != NULL) {
			DBG_L1(format_record,
			       prev->if_index, "<--", IF_NAMESIZE, prev->if_name,
			       prev->if_flags, prev->if_kind,
			       link_bond_mode(prev->bond.bond_mode), prev->is_slave ? "slave" : "",
			       prev->if_link, prev->bond.if_master,
			       prev->bond.active_slave, prev->vlan_id);
		}

		DBG_L1(format_record,
		       link->if_index, prev == NULL ? " | " : "-->", IF_NAMESIZE, link->if_name,
		       link->if_flags, link->if_kind,
		       link_bond_mode(link->bond.bond_mode), link->is_slave ? "slave" : "",
		       link->if_link, link->bond.if_master,
		       link->bond.active_slave, link->vlan_id);
		if (prev != NULL && prev->if_flags ^ link->if_flags) {
			snprint_flags_delta(flags, sizeof flags,
					    prev->if_flags,
					    link->if_flags);
			DBG_L1(format_flags,
			       "________", IF_NAMESIZE, "________________", flags);
		}
	}
}

static int link_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, IFLA_MAX) < 0)
		return rc;

	switch(type) {
	case IFLA_IFNAME:
		if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0)
			rc = MNL_CB_ERROR;
		break;
	case IFLA_LINK:
	case IFLA_MASTER:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	case IFLA_LINKINFO:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
			rc = MNL_CB_ERROR;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("link: mnl_attr_validate(<link>), %s\n", strerror(errno));

	return rc;
}

static int link_attr_info_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, IFLA_INFO_MAX) < 0)
		return rc;

	switch(type) {
	case IFLA_INFO_KIND:
		if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0)
			rc = MNL_CB_ERROR;
		break;
	case IFLA_INFO_DATA:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
			rc = MNL_CB_ERROR;
		break;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("link: mnl_attr_validate(<link info>), %s\n", strerror(errno));

	return rc;
}

static int link_attr_info_bond_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, IFLA_BOND_MAX) < 0)
		return rc;

	switch(type) {
	case IFLA_BOND_MODE:
		if (mnl_attr_validate(attr, MNL_TYPE_U8) < 0)
			rc = MNL_CB_ERROR;
		break;
	case IFLA_BOND_ACTIVE_SLAVE:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("link: mnl_attr_validate(<bond data>), %s\n", strerror(errno));

	return rc;
}

static int link_attr_info_vlan_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, IFLA_VLAN_MAX) < 0)
		return rc;

	switch(type) {
	case IFLA_VLAN_ID:
		if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
			rc = MNL_CB_ERROR;
		break;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("link: mnl_attr_validate(<vlan data>), %s\n", strerror(errno));

	return rc;
}

#ifdef HAVE_TEAMING
static bool netlink_send_team_query(struct sfptpd_nl_state *state, struct sfptpd_link *link)
{
	struct nlmsghdr *new_hdr;
	bool query_requested = false;

	assert(state != NULL);
	assert(link != NULL);
	assert(link->type == SFPTPD_LINK_TEAM);

	if (nl_ge_groups[GRP_TEAM].family > 0 &&
	    (new_hdr = netlink_create_team_query(state->conn + NL_CONN_TEAM_NOTIFY,
						 state->buf,
						 state->buf_sz,
						 link->if_index)) != NULL) {
		if (mnl_socket_sendto(state->conn[NL_CONN_TEAM_NOTIFY].mnl, new_hdr, new_hdr->nlmsg_len) < 0)
			ERROR("netlink: sending team dump query, %s\n", strerror(errno));
		else
			query_requested = true;
	}

	if (query_requested) {
		DBG_L5("netlink: sent team query for %d: %d\n", link->if_index, state->conn[NL_CONN_TEAM_NOTIFY].seq);
	} else {
		DBG_L4("netlink: deferring team query for %d\n", link->if_index);
	}

	return query_requested;
}
#endif

static int netlink_handle_link(struct nl_conn_state *conn, const struct nlmsghdr *nh)
{
	struct ifinfomsg *ifm;
	struct sfptpd_link data = { 0 };
	struct sfptpd_link *link = &data;
	struct nlattr *table[IFLA_MAX + 1] = { 0 };
	struct nlattr *nested[IFLA_INFO_MAX + 1] = { 0 };
	struct nlattr *nested2[IFLA_BOND_MAX + 1] = { 0 };
	struct link_db *db = conn->state->db_hist + conn->state->db_hist_next;
	int row;

	assert(conn);
	assert(nh);
	assert(nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK);

	if (nh->nlmsg_type == RTM_DELLINK)
		link->event = SFPTPD_LINK_DOWN;

	ifm = mnl_nlmsg_get_payload(nh);

	link->if_index = ifm->ifi_index;
	link->if_type = ifm->ifi_type;
	link->if_flags = ifm->ifi_flags;
	link->if_family = ifm->ifi_family;

	mnl_attr_parse(nh, sizeof(*ifm), link_attr_cb, table);

	if (table[IFLA_IFNAME])
		strncpy(link->if_name, mnl_attr_get_str(table[IFLA_IFNAME]), sizeof link->if_name - 1);

	if (table[IFLA_LINK])
		link->if_link = mnl_attr_get_u32(table[IFLA_LINK]);

	if (table[IFLA_MASTER])
		link->bond.if_master = mnl_attr_get_u32(table[IFLA_MASTER]);

	if (table[IFLA_LINKINFO]) {
		mnl_attr_parse_nested(table[IFLA_LINKINFO], link_attr_info_cb, nested);
		if (nested[IFLA_INFO_KIND]) {
			const char *kind = mnl_attr_get_str(nested[IFLA_INFO_KIND]);
			if (!strcmp(kind, "vlan"))
				link->type = SFPTPD_LINK_VLAN;
			else if (!strcmp(kind, "macvlan"))
				link->type = SFPTPD_LINK_MACVLAN;
			else if (!strcmp(kind, "ipvlan"))
				link->type = SFPTPD_LINK_IPVLAN;
			else if (!strcmp(kind, "team"))
				link->type = SFPTPD_LINK_TEAM;
			else if (!strcmp(kind, "bond"))
				link->type = SFPTPD_LINK_BOND;
			else if (!strcmp(kind, "veth"))
				link->type = SFPTPD_LINK_VETH;
			else if (!strcmp(kind, "bridge"))
				link->type = SFPTPD_LINK_BRIDGE;
			else if (!strcmp(kind, "tun") || !strcmp(kind, "tap") ||
				 !strcmp(kind, "vxlan") || !strcmp(kind, "gretap") ||
				 !strcmp(kind, "macvtap" ) || !strcmp(kind, "ip6gretap") ||
				 !strcmp(kind, "ipip") || !strcmp(kind, "sit") ||
				 !strcmp(kind, "gre"))
				link->type = SFPTPD_LINK_TUNNEL;
			else if (!strcmp(kind, "dummy"))
				link->type = SFPTPD_LINK_DUMMY;
			else if (!strcmp(kind, "ifb") || !strcmp(kind, "nlmon") ||
				 !strcmp(kind, "vti") || !strcmp(kind, "vrf") ||
				 !strcmp(kind, "gtp") || !strcmp(kind, "ipoib") ||
				 !strcmp(kind, "wireguard"))
				link->type = SFPTPD_LINK_OTHER;
			strncpy(link->if_kind, kind, (sizeof link->if_kind) - 1);
		}

		if (nested[IFLA_INFO_DATA]) {
			switch (link->type) {
			case SFPTPD_LINK_BOND:
				mnl_attr_parse_nested(nested[IFLA_INFO_DATA], link_attr_info_bond_cb, nested2);
				if (nested2[IFLA_BOND_MODE]) {
					uint8_t mode = mnl_attr_get_u8(nested2[IFLA_BOND_MODE]);
					switch (mode) {
					case BOND_MODE_ACTIVEBACKUP:
						link->bond.bond_mode = SFPTPD_BOND_MODE_ACTIVE_BACKUP;
						break;
					case BOND_MODE_8023AD:
						link->bond.bond_mode = SFPTPD_BOND_MODE_LACP;
						break;
					default:
						link->bond.bond_mode = SFPTPD_BOND_MODE_UNSUPPORTED;
					}
				}

				if (nested2[IFLA_BOND_ACTIVE_SLAVE])
					link->bond.active_slave = mnl_attr_get_u32(nested2[IFLA_BOND_ACTIVE_SLAVE]);
				break;

			case SFPTPD_LINK_VLAN:
				mnl_attr_parse_nested(nested[IFLA_INFO_DATA], link_attr_info_vlan_cb, nested2);
				if (nested2[IFLA_VLAN_ID])
					link->vlan_id = mnl_attr_get_u16(nested2[IFLA_VLAN_ID]);
				break;

			default:
				break;
			}
		}

#ifdef HAVE_TEAMING
		if (nested[IFLA_INFO_SLAVE_KIND])
			link->is_slave = true;
#endif
	}

#ifndef HAVE_TEAMING
	/* Using the lack of teaming support as a proxy for the
	 * IFLA_INFO_SLAVE_KIND tag being available, use the interface
	 * flags to determine if this is a slave interface.
	 */
	link->is_slave = (link->if_flags & IFF_SLAVE) ? true : false;
#endif

	if (link->event != SFPTPD_LINK_DOWN &&
	    db->table.count + 1 >= db->capacity) {
		size_t new_capacity;

		if (db->capacity == 0)
			new_capacity = INITIAL_LINK_TABLE_SIZE;
		else
			new_capacity = db->capacity << 1;

		DBG_L5("link: expanding link table %d from %d to %d rows\n",
		       db->table.version, db->capacity, new_capacity);

		db->capacity = new_capacity;
		db->table.rows = realloc(db->table.rows,
					 db->capacity * sizeof *db->table.rows);
		if (db->table.rows == NULL) {
			CRITICAL("link: expanding link table, %s\n",
				 strerror(errno));
			return errno;
		}
	}

	for (row = 0; row < db->table.count; row++) {
		if (db->table.rows[row].if_index == link->if_index) {
			if (link->event == SFPTPD_LINK_DOWN) {
				memmove(&db->table.rows[row],
				        &db->table.rows[row + 1],
					sizeof data * (db->table.count - row - 1));
				db->table.count--;
			} else {
				/* If a team, don't overwrite team info although we will
				   refetch it. */
				if (link->type == SFPTPD_LINK_TEAM)
					link->bond = db->table.rows[row].bond;
				db->table.rows[row] = *link;
			}
			return 0;
		}
	}

	assert(link->event != SFPTPD_LINK_DOWN);
	assert(row < db->capacity);

	db->table.rows[row] = *link;
	db->table.count++;

#ifdef HAVE_TEAMING
	/* If a team interface is in a dump then we need to request team
           details explicitly. */
	if (link->type == SFPTPD_LINK_TEAM)
		if (!netlink_send_team_query(conn->state, link))
			conn->state->need_rescan = true;
#endif

	return 0;
}

#ifdef HAVE_TEAMING
static void netlink_rescan_teams(struct sfptpd_nl_state *state)
{
	int row;
	struct link_db *db = state->db_hist + state->db_hist_next;

	DBG_L3("netlink: issuing deferred scan for teams\n");

	for (row = 0; row < db->table.count; row++) {
		struct sfptpd_link *link = db->table.rows + row;

		if (link->type == SFPTPD_LINK_TEAM) {
			netlink_send_team_query(state, link);
		}
	}

	state->need_rescan = false;
}

static int ctrl_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, CTRL_ATTR_MAX) < 0)
		return rc;

	switch(type) {
	case CTRL_ATTR_FAMILY_NAME:
		if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0)
			rc = MNL_CB_ERROR;
		break;
	case CTRL_ATTR_FAMILY_ID:
		if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
			rc = MNL_CB_ERROR;
		break;
	case CTRL_ATTR_MCAST_GROUPS:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
			rc = MNL_CB_ERROR;
		break;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("ctrl: mnl_attr_validate(<link>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static int ctrl_mcast_grp1_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **item = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (type == 1) {
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0) {
			rc = MNL_CB_ERROR;
		} else {
			*item = attr;
		}
	}

	if (rc != MNL_CB_OK)
		ERROR("ctrl: mnl_attr_validate(<mcast-grp1>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static int ctrl_mcast_grp2_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, CTRL_ATTR_MAX) < 0)
		return rc;

	switch(type) {
	case CTRL_ATTR_MCAST_GRP_ID:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	}

	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("ctrl: mnl_attr_validate(<mcast-grp2>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static int netlink_handle_genl_ctrl(struct nl_conn_state *conn,
				    struct nl_conn_state *team_conn,
				    const struct nlmsghdr *nh)
{
	struct genlmsghdr *genl;
	struct nlattr *attr[CTRL_ATTR_MAX + 1] = { 0 };
	struct nlattr *mcastgrp = NULL;
	struct nlattr *nested[CTRL_ATTR_MCAST_GRP_MAX + 1] = { 0 };
	enum sfptpd_link_event event;
	int i;
	int group = -1;
	int family = -1;
	int group_id = 0;

	assert(conn);
	assert(nh);
	assert(nh->nlmsg_type == GENL_ID_CTRL);

	genl = mnl_nlmsg_get_payload(nh);

	if (genl->cmd == CTRL_CMD_DELFAMILY || genl->cmd == CTRL_CMD_DELMCAST_GRP) {
		event = SFPTPD_LINK_DOWN;
	} else if (genl->cmd == CTRL_CMD_NEWFAMILY || genl->cmd == CTRL_CMD_NEWMCAST_GRP) {
		event = SFPTPD_LINK_UP;
	} else {
		return 0;
	}

	mnl_attr_parse(nh, sizeof(*genl), ctrl_attr_cb, attr);

	if (attr[CTRL_ATTR_FAMILY_NAME]) {
		for (i = 0; i < NUM_GROUPS; i++) {
			if (strcmp(nl_ge_groups[i].name, mnl_attr_get_str(attr[CTRL_ATTR_FAMILY_NAME])) == 0) {
				assert(group == -1 || group == i);
				group = i;
				break;
			}
		}
	}

	if (attr[CTRL_ATTR_FAMILY_ID]) {
		family = mnl_attr_get_u16(attr[CTRL_ATTR_FAMILY_ID]);
		if (family == GENL_ID_CTRL) {
			assert(group != GRP_TEAM);
			group = GRP_CTRL;
		}
	}

	if (attr[CTRL_ATTR_MCAST_GROUPS]) {
		mnl_attr_parse_nested(attr[CTRL_ATTR_MCAST_GROUPS], ctrl_mcast_grp1_cb, &mcastgrp);
		if (mcastgrp) {
			mnl_attr_parse_nested(mcastgrp, ctrl_mcast_grp2_cb, nested);
			if (nested[CTRL_ATTR_MCAST_GRP_ID]) {
				group_id = mnl_attr_get_u32(nested[CTRL_ATTR_MCAST_GRP_ID]);
			}
		}
	}

	if (event != SFPTPD_LINK_DOWN && group != -1) {
		struct nl_ge_group *grp = nl_ge_groups + group;
		if (group_id != 0)
			grp->group_id = group_id;
		if (family != -1)
			grp->family = family;

		if (grp->group_id > 0 && grp->family >= 0) {
			if (setsockopt(team_conn->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
				       &grp->group_id, sizeof grp->group_id) != 0) {
				ERROR("ctrl: subscribing to teamd events, %s\n", strerror(errno));
			}
		}
	}

	return 0;
}

static int team_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, TEAM_ATTR_MAX) < 0)
		return rc;

	switch(type) {
	case TEAM_ATTR_TEAM_IFINDEX:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	case TEAM_ATTR_LIST_PORT:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
			rc = MNL_CB_ERROR;
		break;
	}
	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("ctrl: mnl_attr_validate(<team-attr>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static int team_port_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, TEAM_ATTR_PORT_MAX) < 0)
		return rc;

	switch(type) {
	case TEAM_ATTR_PORT_IFINDEX:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	case TEAM_ATTR_PORT_CHANGED:
	case TEAM_ATTR_PORT_LINKUP:
	case TEAM_ATTR_PORT_REMOVED:
		if (mnl_attr_validate(attr, MNL_TYPE_FLAG) < 0)
			rc = MNL_CB_ERROR;
		break;
	}
	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("team: mnl_attr_validate(<team-attr-port>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static int team_opt_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **table = data;
	int type = mnl_attr_get_type(attr);
	int rc = MNL_CB_OK;

	if (mnl_attr_type_valid(attr, TEAM_ATTR_PORT_MAX) < 0)
		return rc;

	switch(type) {
	case TEAM_ATTR_OPTION_NAME:
		if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0)
			rc = MNL_CB_ERROR;
		break;
	case TEAM_ATTR_OPTION_PORT_IFINDEX:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			rc = MNL_CB_ERROR;
		break;
	case TEAM_ATTR_OPTION_TYPE:
		if (mnl_attr_validate(attr, MNL_TYPE_U8) < 0)
			rc = MNL_CB_ERROR;
		break;
	case TEAM_ATTR_OPTION_DATA:
		/* Option handler will decide the type. */
	case TEAM_ATTR_OPTION_CHANGED:
	case TEAM_ATTR_OPTION_REMOVED:
		/* These options are not validating as FLAG type but
		   we only care about presence so never mind. */
		rc = MNL_CB_OK;
		break;
	}
	if (rc == MNL_CB_OK)
		table[type] = attr;
	else
		ERROR("team: mnl_attr_validate(<team-option>, %d), %s\n",
		      type, strerror(errno));

	return rc;
}

static void team_opt_apply_mode(struct link_db *db, void *value,
				int team_ifindex, int port_ifindex,
				enum sfptpd_link_event event)
{
	int row;

	for (row = 0; row < db->table.count && db->table.rows[row].if_index != team_ifindex; row++) {
		struct sfptpd_link *link = db->table.rows + row;
		if (db->table.rows[row].if_index == team_ifindex) {
			if(strcmp((char *) value, "activebackup") == 0)
				link->bond.bond_mode = SFPTPD_BOND_MODE_ACTIVE_BACKUP;
			else if(strcmp((char *) value, "loadbalance") == 0)
				link->bond.bond_mode = SFPTPD_BOND_MODE_LACP;
			else
				link->bond.bond_mode = SFPTPD_BOND_MODE_UNSUPPORTED;
		}
	}

	if (row == db->table.count)
		ERROR("could not find link %d applying team mode\n", team_ifindex);
}

static void team_opt_apply_activeport(struct link_db *db, void *value,
				      int team_ifindex, int port_ifindex,
				      enum sfptpd_link_event event)
{
	int row;

	for (row = 0; row < db->table.count; row++) {
		struct sfptpd_link *link = db->table.rows + row;

		if (db->table.rows[row].if_index == team_ifindex) {
			if (event != SFPTPD_LINK_DOWN) {
				link->bond.bond_mode = SFPTPD_BOND_MODE_ACTIVE_BACKUP;
				link->bond.active_slave = *((uint32_t *) value);
			}
			break;
		}
	}
}

/* Interesting teaming options */
struct {
	char* name;
	void (*apply)(struct link_db *db, void *value,
		      int team_ifindex, int port_ifindex,
		      enum sfptpd_link_event event);
} team_options[] = {
	{"mode", team_opt_apply_mode},
	{"activeport", team_opt_apply_activeport},
};
#define CP_TEAM_OPTION_MAX (sizeof(team_options) / sizeof(team_options[0]))

static int netlink_handle_genl_team(struct nl_conn_state *conn,
				    const struct nlmsghdr *nh)
{
	struct genlmsghdr *genl;
	struct nlattr *attr[TEAM_ATTR_MAX + 1] = { 0 };
	struct nlattr *port, *option;
	struct nlattr *nested[TEAM_ATTR_PORT_MAX + 1] = { 0 };
	int team_ifindex = -1;
	int port_ifindex = -1;
	int opt = CP_TEAM_OPTION_MAX;
	enum sfptpd_link_event event = SFPTPD_LINK_NONE;
	void *data;

	assert(conn);
	assert(nh);

	genl = mnl_nlmsg_get_payload(nh);

	switch (genl->cmd) {
	case TEAM_CMD_PORT_LIST_GET:
		mnl_attr_parse(nh, sizeof *genl, team_attr_cb, attr);

		if (attr[TEAM_ATTR_TEAM_IFINDEX]) {
			team_ifindex = mnl_attr_get_u32(attr[TEAM_ATTR_TEAM_IFINDEX]);
		}

		if (attr[TEAM_ATTR_LIST_PORT]) {
			mnl_attr_for_each_nested(port, attr[TEAM_ATTR_LIST_PORT]) {
				mnl_attr_parse_nested(port, team_port_cb, nested);

				if (nested[TEAM_ATTR_PORT_IFINDEX])
					port_ifindex = mnl_attr_get_u32(nested[TEAM_ATTR_PORT_IFINDEX]);

				if (nested[TEAM_ATTR_PORT_REMOVED])
					event = SFPTPD_LINK_DOWN;
				else if (nested[TEAM_ATTR_PORT_LINKUP])
					event = SFPTPD_LINK_UP;
				else if (nested[TEAM_ATTR_PORT_CHANGED])
					event = SFPTPD_LINK_CHANGE;
				else
					event = SFPTPD_LINK_NONE;
			}
		}

		break;
	case TEAM_CMD_OPTIONS_GET:
		mnl_attr_parse(nh, sizeof *genl, team_opt_cb, attr);

		if (attr[TEAM_ATTR_TEAM_IFINDEX]) {
			team_ifindex = mnl_attr_get_u32(attr[TEAM_ATTR_TEAM_IFINDEX]);
		}

		if (attr[TEAM_ATTR_OPTION_PORT_IFINDEX]) {
			port_ifindex = mnl_attr_get_u32(attr[TEAM_ATTR_PORT_IFINDEX]);
		}

		if (attr[TEAM_ATTR_LIST_OPTION]) {
			assert(team_ifindex > 0);
			mnl_attr_for_each_nested(option, attr[TEAM_ATTR_LIST_OPTION]) {
				mnl_attr_parse_nested(option, team_opt_cb, nested);

				if (nested[TEAM_ATTR_OPTION_NAME]) {
					for (opt = 0; opt < CP_TEAM_OPTION_MAX &&
					     strcmp(mnl_attr_get_str(nested[TEAM_ATTR_OPTION_NAME]),
						    team_options[opt].name); opt++);
				}

				if (nested[TEAM_ATTR_OPTION_DATA])
					data = mnl_attr_get_payload(nested[TEAM_ATTR_OPTION_DATA]);

				if (nested[TEAM_ATTR_OPTION_PORT_IFINDEX]) {
					port_ifindex = mnl_attr_get_u32(nested[TEAM_ATTR_OPTION_PORT_IFINDEX]);
				}

				if (nested[TEAM_ATTR_OPTION_REMOVED])
					event = SFPTPD_LINK_DOWN;
				else if (nested[TEAM_ATTR_OPTION_CHANGED])
					event = SFPTPD_LINK_CHANGE;

				if (opt != CP_TEAM_OPTION_MAX) {
					team_options[opt].apply(conn->state->db_hist + conn->state->db_hist_next,
								data,
								team_ifindex,
							        port_ifindex,
								event);
				}
			}
		}
		break;
	default:
		WARNING("unexpected team command %d\n", genl->cmd);
	}

	return 0;
}
#endif

static int netlink_rt_cb(const struct nlmsghdr *nh, void *context)
{
	struct nl_conn_state *conn = (struct nl_conn_state *) context;
	int rc = 0;

	assert(conn);

	switch (nh->nlmsg_type) {
	case RTM_NEWLINK:
	case RTM_DELLINK:
		rc = netlink_handle_link(conn, nh);
		break;
	default:
		ERROR("netlink (rt): unexpected message type %d\n",
			 nh->nlmsg_type);
	}

	if (rc != 0) {
		ERROR("netlink (rt): error handling events, %s\n", strerror(rc));
	}

	return MNL_CB_OK;
}

#ifdef HAVE_TEAMING
static int netlink_ge1_cb(const struct nlmsghdr *nh, void *context)
{
	struct nl_conn_state *conn = (struct nl_conn_state *) context;
	int rc = 0;

	assert(conn);

	switch (nh->nlmsg_type) {
	case GENL_ID_CTRL:
		rc = netlink_handle_genl_ctrl(conn,
					      conn + 1,
					      nh);
		break;
	default:
		ERROR("netlink (ge1): unexpected message type %d\n",
			 nh->nlmsg_type);
	}

	if (rc != 0) {
		ERROR("netlink (ge1): error handling events, %s\n", strerror(rc));
	}

	return MNL_CB_OK;
}

static int netlink_ge2_cb(const struct nlmsghdr *nh, void *context)
{
	struct nl_conn_state *conn = (struct nl_conn_state *) context;
	int rc = 0;

	assert(conn);

	if (nh->nlmsg_type == nl_ge_groups[GRP_TEAM].family) {
		rc = netlink_handle_genl_team(conn, nh);
	} else {
		ERROR("netlink (ge2): unexpected message type %d\n",
			 nh->nlmsg_type);
	}

	if (rc != 0) {
		ERROR("netlink (ge2): error handling events, %s\n", strerror(rc));
	}

	return MNL_CB_OK;
}
#endif

static struct nlmsghdr *netlink_create_interface_query(struct nl_conn_state *conn,
						       char *buf, size_t space)
{
	struct nlmsghdr *nh;
	struct ifinfomsg *ifinfomsg;

	assert(buf != NULL);

	nh = mnl_nlmsg_put_header(buf);
	nh->nlmsg_seq = ++conn->seq;
	nh->nlmsg_type = RTM_GETLINK;
	nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

	ifinfomsg = mnl_nlmsg_put_extra_header(nh, sizeof *ifinfomsg);
	ifinfomsg->ifi_family = AF_UNSPEC;

	return nh;
}

#ifdef HAVE_TEAMING
static struct nlmsghdr *netlink_create_team_ctrl_query(struct nl_conn_state *conn,
						  char *buf, size_t space)
{
	struct nlmsghdr *nh;
	struct genlmsghdr *genlmsghdr;

	assert(buf != NULL);

	nh = mnl_nlmsg_put_header(buf);
	nh->nlmsg_seq = ++conn->seq;
	nh->nlmsg_type = GENL_ID_CTRL;
	nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

	genlmsghdr = mnl_nlmsg_put_extra_header(nh, sizeof *genlmsghdr);
	genlmsghdr->cmd = CTRL_CMD_GETFAMILY;

	return nh;
}

static struct nlmsghdr *netlink_create_team_query(struct nl_conn_state *conn,
						  char *buf, size_t space,
						  int team_ifindex)
{
	struct nlmsghdr *nh;
	struct genlmsghdr *genlmsghdr;

	assert(buf != NULL);

	nh = mnl_nlmsg_put_header(buf);
	nh->nlmsg_seq = conn->seq = 0;
	nh->nlmsg_type = nl_ge_groups[GRP_TEAM].family;
	nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;

	genlmsghdr = mnl_nlmsg_put_extra_header(nh, sizeof *genlmsghdr);
	genlmsghdr->cmd = TEAM_CMD_OPTIONS_GET;

	mnl_attr_put_u32_check(nh, space, TEAM_ATTR_TEAM_IFINDEX, team_ifindex);

	return nh;
}
#endif

static int netlink_open_conn(struct nl_conn_state *conn,
			     const char *name,
			     int bus, unsigned int groups)
{
	int rc;
	int flags;

	assert(conn);
	assert(name);

	conn->name = name;
	conn->mnl = mnl_socket_open(bus);
	if (conn->mnl == NULL) {
		ERROR("netlink: %s: could not open mnl socket, %s\n",
		      conn->name, strerror(errno));
		return errno;
	}

	rc = mnl_socket_bind(conn->mnl, groups, MNL_SOCKET_AUTOPID);
	if (rc < 0) {
		ERROR("netlink: %s: could not bind mnl socket, %s\n",
		      conn->name, strerror(errno));
		goto fail;
	}

	conn->fd = mnl_socket_get_fd(conn->mnl);
	assert(conn->fd != -1);

	flags = fcntl(conn->fd, F_GETFL);
	rc = fcntl(conn->fd, F_SETFL, flags | O_NONBLOCK);
	if (rc >= 0)
		return 0;

	ERROR("netlink: %s: setting socket to non-blocking, %s\n",
	      conn->name, strerror(errno));

fail:
	mnl_socket_close(conn->mnl);

	return rc;
}

static int netlink_service_fds(struct sfptpd_nl_state *state)
{
	unsigned int portid;
	int rc = 0;
	int i;
	bool serviced = false;

	for (i = 0; i < NL_CONN_MAX; i++) {
		struct nl_conn_state *conn = state->conn + i;

		portid = mnl_socket_get_portid(conn->mnl);

		do {
			rc = mnl_socket_recvfrom(conn->mnl, state->buf, state->buf_sz);
			if (rc < 0 && (errno == EAGAIN || errno == EINTR)) {
				rc = 0;
				break;
			} else if (rc < 0) {
				ERROR("netlink: %s: error receiving netlink packet\n",
				      conn->name);
				return -errno;
			} else if (rc > 0)
				serviced = true;

			DBG_L5("netlink: %s: handling netlink packet\n",
			       conn->name);

			rc = mnl_cb_run(state->buf, rc, conn->seq, portid,
					conn->cb, conn);

		} while (rc >= MNL_CB_STOP);

		if (rc <= MNL_CB_ERROR) {
			ERROR("netlink: %s: processing netlink packets\n",
			      conn->name);
			return -errno;
		}
	}

	return serviced ? 1 : 0;
}

static struct link_db *netlink_find_version(struct sfptpd_nl_state *state,
					    int version)
{
	struct link_db *db;
	int idx;
	int i;

	/* Search from newest to oldest */
	for (i = 0; i < state->db_hist_count; i++) {
		idx = (state->db_hist_next + MAX_LINK_DB_VERSIONS - 1 - i) % MAX_LINK_DB_VERSIONS;
		db = &state->db_hist[idx];
		if (db->table.version == version)
			return db;
	}
	return NULL;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_nl_state *sfptpd_netlink_init(void)
{
	int rc;

	struct sfptpd_nl_state *state = calloc(1, sizeof *state);
	if (state == NULL) {
		ERROR("netlink: could not allocate handler state, %s\n",
		      strerror(errno));
		goto fail0;
	}

	state->db_ver_next = 1;
	state->db_hist_count = 1;
	state->db_hist[0].table.version = state->db_ver_next++;

	state->buf_sz = MNL_SOCKET_BUFFER_SIZE;
	state->buf = malloc(state->buf_sz);
	if (state->buf == NULL) {
		ERROR("netlink: could not allocate buffer, %s\n",
		      strerror(errno));
		goto fail1;
	};

	rc = netlink_open_conn(state->conn + NL_CONN_RT,
			       "rtnetlink", NETLINK_ROUTE, RTMGRP_LINK);
	if (rc < 0) goto fail2;
	state->conn[NL_CONN_RT].cb = netlink_rt_cb;
	state->conn[NL_CONN_RT].state = state;

#ifdef HAVE_TEAMING
	struct nlmsghdr *hdr;

	rc = netlink_open_conn(state->conn + NL_CONN_TEAM_DUMP,
			       "genetlink1", NETLINK_GENERIC, 0);
	if (rc < 0) goto fail3;
	state->conn[NL_CONN_TEAM_DUMP].cb = netlink_ge1_cb;
	state->conn[NL_CONN_TEAM_DUMP].state = state;

	rc = netlink_open_conn(state->conn + NL_CONN_TEAM_NOTIFY,
			       "genetlink2", NETLINK_GENERIC, 0);
	if (rc < 0) goto fail4;
	state->conn[NL_CONN_TEAM_NOTIFY].cb = netlink_ge2_cb;
	state->conn[NL_CONN_TEAM_NOTIFY].state = state;

	hdr = netlink_create_team_ctrl_query(state->conn + NL_CONN_TEAM_DUMP,
					state->buf,
					state->buf_sz);

	rc = mnl_socket_sendto(state->conn[NL_CONN_TEAM_DUMP].mnl, hdr, hdr->nlmsg_len);
	if (rc < 0) {
		ERROR("netlink: sending team control query, %s\n", strerror(errno));
		goto fail5;
	}
#endif

	return state;

#ifdef HAVE_TEAMING
fail5:
	mnl_socket_close(state->conn[NL_CONN_TEAM_NOTIFY].mnl);
fail4:
	mnl_socket_close(state->conn[NL_CONN_TEAM_DUMP].mnl);
fail3:
#endif
	mnl_socket_close(state->conn[NL_CONN_RT].mnl);
fail2:
	free(state->buf);
fail1:
	free(state);
fail0:
	return NULL;
}

int sfptpd_netlink_get_fd(struct sfptpd_nl_state *state,
			  int *get_fd_state)
{
	int fd;
	int i;

	assert(get_fd_state);
	i = *get_fd_state;

	assert(i <= NL_CONN_MAX);
	if (i == NL_CONN_MAX) {
		return -1;
	} else {
		fd = state->conn[i].fd;
		*get_fd_state = i + 1;
		return fd;
	}
}

int sfptpd_netlink_service_fds(struct sfptpd_nl_state *state,
			       int *fds, int num_fds,
			       int consumers)
{
	bool any_data;
	int serviced;
	int rc;
	struct link_db *next;
	struct link_db *cur;
	struct link_db *prev;
	int row;
	int old_row;
	int i;
	bool change = false;

	DBG_L5("netlink: servicing fds\n");

	next = state->db_hist + (state->db_hist_next + 1) % MAX_LINK_DB_VERSIONS;
	cur = state->db_hist + state->db_hist_next;
	prev = state->db_hist + (state->db_hist_next + MAX_LINK_DB_VERSIONS - 1) % MAX_LINK_DB_VERSIONS;

	/* If the next table in ring buffer has non-zero ref count, refuse to service fds
	   until it is freed by consumers. Also log that this has happened. */
	if (next->refcnt != 0) {
		WARNING("netlink: non-zero refcount for next link table: postponing servicing\n");
		state->need_service = true;
		return -EAGAIN;
	}
	cur->refcnt = consumers;

	do {
#ifdef HAVE_TEAMING
		if (state->need_rescan && nl_ge_groups[GRP_TEAM].family > 0)
			netlink_rescan_teams(state);
#endif

		serviced = netlink_service_fds(state);
		if (serviced > 0)
			any_data = true;
	} while (serviced > 0
#ifdef HAVE_TEAMING
		 || (serviced == 0 && state->need_rescan && nl_ge_groups[GRP_TEAM].family > 0)
#endif
		);


	if (any_data) {
		DBG_L4("new link table (ver %d):\n", cur->table.version);
		for (row = 0; row < cur->table.count; row++) {
			print_link(cur->table.rows + row);
		}
	}
	rc = (serviced >= 0 ? 0 : -serviced);
	if (rc != 0) {
		ERROR("link: servicing fds: %s\n", strerror(rc));
		return -rc;
	}

	/* Rotate history and compare state */
	DBG_L4("comparing ver %d -> %d\n", prev->table.version, cur->table.version);

	/* Handle changes and additions */
	for (row = 0; row < cur->table.count; row++) {
		enum sfptpd_link_event event = SFPTPD_LINK_NONE;

		/* Look for this link in the old table */
		for (old_row = 0; old_row < prev->table.count &&
				  cur->table.rows[row].if_index != prev->table.rows[old_row].if_index;
		     old_row++);

		if (old_row == prev->table.count) {
			event = SFPTPD_LINK_UP;
			DBG_L3("added new if_index %d %s\n", cur->table.rows[row].if_index, cur->table.rows[row].if_name);
		} else {
			struct sfptpd_link *a = prev->table.rows + old_row;
			struct sfptpd_link *b = cur-> table.rows + row;

			/* Stash a link to the old link for internal use. Not valid
			 * out of the scope of this function. */
			b->priv = a;

			if (a->type != b->type) {
				DBG_L2("if_kind changed %d (%s) -> %d (%s)\n", a->type, a->if_kind, b->type, b->if_kind);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->if_type != b->if_type) {
				DBG_L2("if_type changed %d -> %d\n", a->if_type, b->if_type);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->if_family != b->if_family) {
				DBG_L2("if_family changed %d -> %d\n", a->if_family, b->if_family);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->if_flags != b->if_flags) {
				char flags[256];
				int sig_a, sig_b;

				/* ignore truncation of flag text: this is diagnostic */
				snprint_flags_delta(flags, sizeof flags, a->if_flags, b->if_flags);

				DBG_L4("if_flags (any) changed %x -> %x (%s)\n", a->if_flags, b->if_flags, flags);
				sig_a = a->if_flags & significant_flags;
				sig_b = b->if_flags & significant_flags;
				if (sig_a != sig_b) {
					snprint_flags_delta(flags, sizeof flags, sig_a, sig_b);

					DBG_L2("if_flags (significant) changed %x -> %x (%s)\n", sig_a, sig_b, flags);
					       event = SFPTPD_LINK_CHANGE;
				}
			}
			if (a->bond.if_master != b->bond.if_master) {
				DBG_L2("if_master changed %d -> %d\n", a->bond.if_master, b->bond.if_master);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->bond.bond_mode != b->bond.bond_mode) {
				DBG_L2("bond mode changed %d -> %d\n", a->bond.bond_mode, b->bond.bond_mode);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->bond.active_slave != b->bond.active_slave) {
				DBG_L2("active_slave changed %d -> %d\n", a->bond.active_slave, b->bond.active_slave);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->is_slave != b->is_slave) {
				DBG_L2("is_slave changed %d -> %d\n", a->is_slave, b->is_slave);
				event = SFPTPD_LINK_CHANGE;
			}
			if (a->vlan_id != b->vlan_id) {
				DBG_L2("vlan_id changed %d -> %d\n", a->vlan_id, b->vlan_id);
				event = SFPTPD_LINK_CHANGE;
			}
			if (strcmp(a->if_name, b->if_name)) {
				DBG_L2("if_name changed %s -> %s\n", a->if_name, b->if_name);
				event = SFPTPD_LINK_CHANGE;
			}

			if (event == SFPTPD_LINK_CHANGE) {
				DBG_L2("^ significant change to %d %s\n", b->if_index, b->if_name);
			} else if (event == SFPTPD_LINK_NONE && (b->event == SFPTPD_LINK_NONE || b->event == SFPTPD_LINK_UP)) {
				//DBG_L2("minor change to %d %s\n", b->if_index, b->if_name);
			}
		}

		if (event != SFPTPD_LINK_NONE)
			change = true;

		cur->table.rows[row].event = event;
	}

	/* Summarise new links */
	for (row = 0, i = 0; row < cur->table.count; row++) {
		const struct sfptpd_link *link = cur->table.rows + row;

		if (link->event ==  SFPTPD_LINK_UP) {
			if (i++ == 0) {
				DBG_L1("link: new interfaces:\n");
				sfptpd_link_log(NULL, NULL);
			}
			sfptpd_link_log(link, NULL);
		}
	}

	/* Summarise changed links */
	for (row = 0, i = 0; row < cur->table.count; row++) {
		const struct sfptpd_link *link = cur->table.rows + row;

		if (link->event ==  SFPTPD_LINK_CHANGE) {
			if (i++ == 0) {
				DBG_L1("link: changed interfaces:\n");
				sfptpd_link_log(NULL, NULL);
			}
			sfptpd_link_log(link, (const struct sfptpd_link *) link->priv);
		}
	}

	/* Handle deletions */
	for (old_row = 0; old_row < prev->table.count; old_row++) {
		for (row = 0; row < cur->table.count && cur->table.rows[row].if_index != prev->table.rows[old_row].if_index; row++);
		if (row == cur->table.count) {
			DBG_L1("link deleted interface %d: %s\n", prev->table.rows[old_row].if_index, prev->table.rows[old_row].if_name);
			change = true;
		}
	}

	/* Rotate through table history */
	if (state->db_hist_count == MAX_LINK_DB_VERSIONS) {
		if (state->db_hist[(state->db_hist_next + 1) % MAX_LINK_DB_VERSIONS].refcnt > 0) {
			CRITICAL("cannot rotate link db history, ref count > 0 on oldest version\n");
			return -ENOSPC;
		}
	} else {
		state->db_hist_count++;
	}

	if (change) {
		assert(next->refcnt == 0);

		/* Copy current table into next one */
		if (next->capacity < cur->capacity) {
			DBG_L5("link: expanding link table %d from %d to %d rows\n",
			       state->db_ver_next, next->capacity, cur->capacity);
			next->capacity = cur->capacity;
			next->table.rows = realloc(next->table.rows,
						   next->capacity * sizeof *next->table.rows);
			if (next->table.rows == NULL)
				return -errno;
		}
		next->refcnt = cur->refcnt;
		next->table.count = cur->table.count;
		memcpy(next->table.rows, cur->table.rows,
		       next->table.count * sizeof *next->table.rows);

		/* Rotate versions */
		state->db_hist_next = (state->db_hist_next + 1) % MAX_LINK_DB_VERSIONS;
		next->table.version = state->db_ver_next++;
		DBG_L4("netlink: table %d, refcnt = %d\n", cur->table.version, cur->refcnt);
		return cur->table.version;
	} else {
		DBG_L4("abandoning new link table (ver %d) as no significant changes\n", cur->table.version);
		return 0;
	}
}

int sfptpd_netlink_get_table(struct sfptpd_nl_state *state, int version, const struct sfptpd_link_table **table)
{
	struct link_db *db;

	db = netlink_find_version(state, version);

	if (db != NULL) {
		assert(db->table.version == version);
		if (db->refcnt == 0) {
			CRITICAL("netlink: attempt to access link table with refcnt==0");
			sfptpd_thread_exit(EACCES);
			return -EACCES;
		} else {
			*table = &db->table;
			return db->table.count;
		}
	} else {
		ERROR("netlink: cannot find link table version %d\n", version);
		return -ENOENT;
	}
}

int sfptpd_netlink_release_table(struct sfptpd_nl_state *state, int version, int consumers)
{
	struct link_db *db;

	db = netlink_find_version(state, version);

	if (db != NULL) {
		assert(db->table.version == version);
		if (db->refcnt == 0) {
			CRITICAL("netlink: attempt to release link table with refcnt==0\n");
			sfptpd_thread_exit(EACCES);
			return -EACCES;
		} else {
			db->refcnt--;
			DBG_L4("netlink: table %d, --refcnt = %d\n", version, db->refcnt);
		}
	} else {
		CRITICAL("netlink: attempt to release link table that is already freed\n");
		sfptpd_thread_exit(ENOENT);
		return -ENOENT;
	}

	if (state->need_service) {
		state->need_service = false;
		return sfptpd_netlink_service_fds(state, NULL, 0, consumers);
	} else {
		return 0;
	}
}

int sfptpd_netlink_scan(struct sfptpd_nl_state *state)
{
	struct nlmsghdr *hdr;
	int rc;

	hdr = netlink_create_interface_query(state->conn + NL_CONN_RT,
					     state->buf,
					     state->buf_sz);

	rc = mnl_socket_sendto(state->conn[NL_CONN_RT].mnl, hdr, hdr->nlmsg_len);
	if (rc < 0)
		ERROR("netlink: sending interface query, %s\n", strerror(errno));
	else
		DBG_L4("netlink: issued rescan\n");

	return rc >= 0 ? 0 : errno;
}

void sfptpd_netlink_finish(struct sfptpd_nl_state *state)
{
	int i;

	assert(state);
	assert(state->buf);

	for (i = 0; i < MAX_LINK_DB_VERSIONS; i++) {
		struct sfptpd_link_table *table = &state->db_hist[i].table;
		if (table->rows != NULL)
			free(table->rows);
	}

	for (i = 0; i < NL_CONN_MAX; i++) {
		mnl_socket_close(state->conn[i].mnl);
	}

	free(state->buf);
	free(state);
}

const struct sfptpd_link_table *sfptpd_netlink_table_wait(struct sfptpd_nl_state *state,
							  int consumers,
							  int timeout_ms)
{
	int rc;
	int fd;
	int get_fd_state;
	#define max_events 5
	struct epoll_event ev;
	struct epoll_event events[max_events];
	int nfds = 0;
	int epollfd;
	const struct sfptpd_link_table *link_table = NULL;

	epollfd = epoll_create(max_events);
	if (epollfd == -1) {
		CRITICAL("netlink: creating epoll set to wait for link table, %s\n",
			 strerror(errno));
		return NULL;
	}

	get_fd_state = 0;
	do {
		fd = sfptpd_netlink_get_fd(state, &get_fd_state);
		if (fd != -1) {
			ev.events = EPOLLIN;
			ev.data.fd = fd;
			if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
				CRITICAL("netlink: failed to set up epoll set, %s\n",
					 strerror(errno));
				return NULL;
			}
		}
	} while (fd != -1);

	DBG_L3("netlink: waiting for link table\n");
	while (nfds <= 0) {
		nfds = epoll_wait(epollfd, events, max_events, timeout_ms);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			CRITICAL("netlink: failed in epoll_wait, %s\n",
				 strerror(errno));
			goto finish;
		} else if (nfds == 0) {
			errno = EAGAIN;
			goto finish;
		}

		assert(nfds > 0);

		rc = sfptpd_netlink_service_fds(state, NULL, 0, consumers);
		if (rc > 0) {
			int rows;

			DBG_L3("netlink: wait: new link table version %d\n",
				 rc);

			rows = sfptpd_netlink_get_table(state,
							rc, &link_table);
			if (rows < 0)
				errno = -rows;
			else
				assert(rows == link_table->count);
		} else if (rc < 0) {
			DBG_L3("netlink: wait: failed to create link table\n",
				 -rc);
			errno = -rc;
			goto finish;
		}
	}

	if (link_table != NULL)
		DBG_L3("netlink: wait: accepted version %d\n", link_table->version);

finish:
	close(epollfd);
	return link_table;
}
/* fin */
