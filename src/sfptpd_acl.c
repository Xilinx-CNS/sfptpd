/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2025 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_acl.c
 * @brief  Provides Access Control List helpers.
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
#include <sys/un.h>
#include <net/if.h>

#include "sfptpd_acl.h"
#include "sfptpd_config_helpers.h"


/****************************************************************************
 * Types
 ****************************************************************************/


/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

const struct sfptpd_acl_prefix sfptpd_acl_v6mapped_prefix = {
	{ { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 } } }, 96
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_acl_table_create(struct sfptpd_acl_table *table,
			    const char *name,
			    int length,
			    const char * const *list)
{
	int row;
	int rc = 0;

	table->prefixes = calloc(length, sizeof *table->prefixes);
	if (table->prefixes == NULL)
		return -errno;

	for (row = 0; rc == 0 && row < length; row++) {
		rc = sfptpd_config_parse_net_prefix(table->prefixes + row,
						    list[row], name);
	}

	if (rc != 0) {
		ERROR("acl: populating table %s, %s\n", name, strerror(rc));
		free(table->prefixes);
		table->prefixes = NULL;
	} else {
		table->length = row;
	}

	return rc;
}

void sfptpd_acl_table_destroy(struct sfptpd_acl_table *table)
{
	free(table->prefixes);
	table->prefixes = NULL;
}

void sfptpd_acl_normalise_prefix(struct sfptpd_acl_prefix *prefix)
{
	int spare_bits = prefix->length & 7;
	int octets = prefix->length >> 3;

	if (spare_bits) {
		prefix->in6.s6_addr[octets] &= ~(0xFF << (8 - spare_bits));
	}
	memset(prefix->in6.s6_addr + octets, '\0', 16 - octets);
}

const bool sfptpd_acl_prefix_match(const struct sfptpd_acl_prefix *prefix,
				   struct in6_addr addr)
{
	int spare_bits = prefix->length & 7;
	int octets = prefix->length >> 3;

	assert(prefix->length <= 128);

	if (spare_bits) {
		addr.s6_addr[octets] &= ~(0xFF << (8 - spare_bits));
		octets++;
	}
	return memcmp(prefix->in6.s6_addr, addr.s6_addr, octets) == 0;
}

const struct sfptpd_acl_prefix *sfptpd_acl_table_match(const struct sfptpd_acl_table *table,
						       const struct in6_addr *addr)
{
	int row;

	if (table == NULL)
		return NULL;

	for (row = 0; row < table->length &&
		      !sfptpd_acl_prefix_match(table->prefixes + row, *addr); row++);

	return row < table->length ? table->prefixes + row : NULL;
}

struct in6_addr sfptpd_acl_map_v4_addr(struct in_addr addr)
{
	struct in6_addr mapped_addr = sfptpd_acl_v6mapped_prefix.in6;

	memcpy(mapped_addr.s6_addr + 12, &addr.s_addr, 4);
	return mapped_addr;
}

bool sfptpd_acl_match(const struct sfptpd_acl *acl,
		      const struct in6_addr *addr)
{
	switch (acl->order) {
	case SFPTPD_ACL_DENY_ALL:
		return false;
	case SFPTPD_ACL_ALLOW_ALL:
		return true;
	case SFPTPD_ACL_ALLOW_DENY:
		return sfptpd_acl_table_match(&acl->allow, addr) &&
		       !sfptpd_acl_table_match(&acl->deny, addr);
	case SFPTPD_ACL_DENY_ALLOW:
		return !sfptpd_acl_table_match(&acl->deny, addr) ||
		       sfptpd_acl_table_match(&acl->allow, addr);
	default:
		return false;
	}
}

void sfptpd_acl_free(struct sfptpd_acl *acl)
{
	acl->order = SFPTPD_ACL_DENY_ALL;
	sfptpd_acl_table_destroy(&acl->allow);
	sfptpd_acl_table_destroy(&acl->deny);
}


/* fin */
