/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2022-2023 Xilinx, Inc. */

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

/* Maximum number of file descriptors that the netlink module may need to
 * service. */
#define SFPTPD_NETLINK_MAX_FDS 3


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/* All functions from this module must be called from the same thread
 * except for sfptpd_netlink_get_table() which may be called by
 * consumers who have been ref-counted for that table.
 * In sfptpd the engine thread owns this module.
 */

/** Initialise the netlink client and link table database.
 * @return context
 */
struct sfptpd_nl_state *sfptpd_netlink_init(void);

/** Service the sockets for the netlink client.
 * @param state the netlink client context
 * @param consumers the number of consumers for whom to lock new link tables.
 * @param coalescing true if user is coalescing events and wants to hold back
 *        new tables.
 * @return -errno on error, 0 if no new link tables or the version of the new
 *         link table.
 */
int sfptpd_netlink_service_fds(struct sfptpd_nl_state *state,
			       int consumers,
			       bool coalescing);

/** Initiate a fresh dump of all interfaces over netlink.
 * @param state the netlink client context
 * @return 0 on success else errno.
 */
int sfptpd_netlink_scan(struct sfptpd_nl_state *state);

/** Close the netlink client and databases.
 * @param state the netlink client context
 */
void sfptpd_netlink_finish(struct sfptpd_nl_state *state);

/** Enumerate file descriptors from which the netlink client reads.
 * @param state the netlink client context
 * @param fds array to populate with file descriptors, consecutively.
 * @return number of file descriptors returned.
 */
int sfptpd_netlink_get_fds(struct sfptpd_nl_state *state,
			   int fds[SFPTPD_NETLINK_MAX_FDS]);

/** Get a pointer to a version of a link table. This must only called by
 * legitimate link table consumers. The owner of the netlink client state
 * already set the number of consumers at the time the table was created so
 * the reference count already starts at its maximum and does not get
 * incremented dynamically and consumers should therefore only be alerted
 * by the owner if they have already been counted at that point.
 * @param state the netlink client state
 * @param version the version of the link table to be fetched
 * @param table where the link table pointer should be returned
 * @result number of rows in table or -errno on error.
 */
int sfptpd_netlink_get_table(struct sfptpd_nl_state *state, int version, const struct sfptpd_link_table **table);

/** Release a version link table version and if a new one has been blocked on
 * the release of this table, behave as sfptpd_netlink_service_fds(), returning
 * a new one if necessary. The caller must therefore be prepared for this to
 * happen so this should be called by the owner of netlink client state having
 * been notified by the consumer.
 * @param state the netlink client state
 * @param version the version of the link table being released
 * @param consumers the reference count to set in any new table that is returned
 * @return -errno on error, 0 if no new link tables or the version of the new
 *         link table.
 */
int sfptpd_netlink_release_table(struct sfptpd_nl_state *state, int version, int consumers);

/** Wait for a new link table to become available and return it. This assumes
 * that the file descriptors are not already being polled. A reasonable
 * workflow would be like this:
 *   1. sfptpd_netlink_init()
 *   2. sfptpd_netlink_scan()
 *   3. sfptpd_netlink_table_wait()
 *   4. <use initial version of link table for application initialisation>
 *   5. sfptpd_netlink_get_fd() until all fds retrieved.
 *   6. <start polling on fds>
 *   7. sfptpd_netlink_service_fds() until application is finished.
 *   8.   sfptpd_netlink_get_table()
 *   9.   sfptpd_netlink_release_table()
 *  10. sfptpd_netlink_finish()
 * @param state the netlink client state
 * @param consumers the initial ref count for the first table. Should be >0.
 * @param timeout_ms the timeout (or -1) according to epoll_wait(2) semantics.
 * @return the link table or NULL with errno set on error or to EAGAIN on timeout.
 */
const struct sfptpd_link_table *sfptpd_netlink_table_wait(struct sfptpd_nl_state *state,
							  int consumers,
							  int timeout_ms);

/** Set desired set of driver statistics for which string indexes are
 *  required. The indexes will be returned in link tables.
 * @param state the netlink client context
 * @param keys list of key names
 * @param num_keys number of keys
 * @return 0 on success, else errno
 */
int sfptpd_netlink_set_driver_stats(struct sfptpd_nl_state *state,
				    const char **keys, size_t num_keys);

#endif /* SFPTPD_NETLINK_H */
