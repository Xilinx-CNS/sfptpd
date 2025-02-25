/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2025, Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_ACL_H
#define _SFPTPD_ACL_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/* Apache httpd ACL semantics apply */
enum sfptpd_acl_order {
	SFPTPD_ACL_DENY_ALL,
	SFPTPD_ACL_ALLOW_ALL,

	/* Default is deny; then apply ALLOW list; then apply DENY list. */
	SFPTPD_ACL_ALLOW_DENY,

	/* Default is allow; then apply DENY list; then apply ALLOW list. */
	SFPTPD_ACL_DENY_ALLOW,
};

/* Prefixes represented in network byte order. IPv4 addresses are
   RFC2373 v6mapped. */
struct sfptpd_acl_prefix {
	struct in6_addr in6;
	uint8_t length;
};

struct sfptpd_acl_table {
	int length;
	struct sfptpd_acl_prefix *prefixes;
};

struct sfptpd_acl {
	const char *name;
	enum sfptpd_acl_order order;
	struct sfptpd_acl_table allow;
	struct sfptpd_acl_table deny;
};


/****************************************************************************
 * Constants and macros
 ****************************************************************************/

extern const struct sfptpd_acl_prefix sfptpd_acl_v6mapped_prefix;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

extern int sfptpd_acl_table_create(struct sfptpd_acl_table *table,
				   const char *name,
				   int length,
				   const char * const *list);

extern void sfptpd_acl_table_destroy(struct sfptpd_acl_table *table);

extern void sfptpd_acl_normalise_prefix(struct sfptpd_acl_prefix *prefix);

extern const bool sfptpd_acl_prefix_match(const struct sfptpd_acl_prefix *prefix,
					  struct in6_addr addr);

extern const struct sfptpd_acl_prefix *sfptpd_acl_table_match(const struct sfptpd_acl_table *table,
							      const struct in6_addr *addr);

extern struct in6_addr sfptpd_acl_map_v4_addr(struct in_addr addr);

static inline bool sfptpd_acl_is_v6mapped(struct in6_addr addr) {
	return sfptpd_acl_prefix_match(&sfptpd_acl_v6mapped_prefix, addr);
}

extern bool sfptpd_acl_match(const struct sfptpd_acl *acl,
			     const struct in6_addr *addr);

extern void sfptpd_acl_free(struct sfptpd_acl *acl);

#endif /* _SFPTPD_ACL_H */
