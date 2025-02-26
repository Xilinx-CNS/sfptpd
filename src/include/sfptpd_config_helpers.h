/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2025 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CONFIG_HELPERS_H
#define _SFPTPD_CONFIG_HELPERS_H

#include <stdio.h>
#include <net/if.h>
#include <limits.h>
#include <netinet/in.h>

#include <sfptpd_interface.h>
#include <sfptpd_logging.h>
#include <sfptpd_acl.h>


/****************************************************************************
 * Types and Defines
 ****************************************************************************/


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Parse network address.
 * @param ss         Destination
 * @param context    Context to use for log messages
 * @param af         Addressing family or AF_UNSPEC
 * @param socketype  The socket type, e.g. SOCK_DGRAM
 * @param passive    true for listening sockets, else false
 * @param def_serv   string for default port to use
 * @return address length or -errno
 */
int sfptpd_config_parse_net_addr(struct sockaddr_storage *ss,
				 const char *addr,
				 const char *context,
				 int af,
				 int socktype,
				 bool passive,
				 const char *def_serv);

/** Parse ACL prefix.
 * @param buf    Destination
 * @param addr   Prefix text to parse, in the form
 *               2001:db8::/64 or 192.0.2.0/24. If the length is
 *               omitted, a host address (/128 or /32) is assumed.
 * @param context    Context to use for log messages
 * @return 0 or errno on error.
 */
int sfptpd_config_parse_net_prefix(struct sfptpd_acl_prefix *buf,
				   const char *addr,
				   const char *context);

/** Parse ACL order.
 * @param order        Destination for decoded order
 * @param param        The text to parse
 * return 0 or errno on error.
 */
int sfptpd_config_parse_acl_order(enum sfptpd_acl_order *order,
				  const char *param);

#endif /* _SFPTPD_CONFIG_HELPERS_H */
