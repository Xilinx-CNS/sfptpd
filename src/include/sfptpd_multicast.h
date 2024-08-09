/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_MULTICAST_H
#define _SFPTPD_MULTICAST_H

#include "sfptpd_message.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise sfptpd message multicast service */
int sfptpd_multicast_init(void);

/** Destroy sfptpd message multicast service */
void sfptpd_multicast_destroy(void);

/** Dump multicast group state */
void sfptpd_multicast_dump_state(void);

/** Join a subscriber to an sfptpd message multicast group.
 * @param msg_id The message id of the multicast group to join
 * @return 0 on success or errno
 */
int sfptpd_multicast_subscribe(uint32_t msg_id);

/** Register a publisher of multicast sfptpd messages.
 * @param msg_id The message id of the multicast group
 * @return 0 on success or errno
 */
int sfptpd_multicast_publish(uint32_t msg_id);

/** Leave an sfptpd message multicast group.
 * @param msg_id The message id of the multicast group to leave
 * @return 0 on success or errno
 */
int sfptpd_multicast_unsubscribe(uint32_t msg_id);

/** Deregister a publisher of multicast sfptpd messages.
 * @param msg_id The message id of the multicast group
 * @return 0 on success or errno
 */
int sfptpd_multicast_unpublish(uint32_t msg_id);

/** Send a multicast sfptpd messages.
 * @param hdr Header of the message to be sent
 * @param msg_id The message id of the multicast group
 * @param pool Pool from which to allocate the replicated messages
 * @param wait Wait for free message slots, else abort if not enough
 * @return 0 on success or errno
 */
int sfptpd_multicast_send(sfptpd_msg_hdr_t *hdr,
			  uint32_t msg_id,
			  enum sfptpd_msg_pool_id pool,
			  bool wait);
#define SFPTPD_MULTICAST_SEND(msg, msg_id, pool, wait) \
		sfptpd_multicast_send(&((msg)->hdr), (msg_id), (pool), (wait))

#endif /* _SFPTPD_MULTICAST_H */
