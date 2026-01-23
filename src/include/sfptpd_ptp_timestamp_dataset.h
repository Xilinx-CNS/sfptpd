/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_PTP_TIMESTAMP_DATASET_H
#define _SFPTPD_PTP_TIMESTAMP_DATASET_H

#include <assert.h>

#include "sfptpd_time.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** PTP timestamp delay data
 * @valid Indicates if the timestamp set is valid
 * @tx Transmit timestamp
 * @rx Recevie timestamp
 * @correction Value from PTP correction field
 */
struct sfptpd_ptp_delay_data {
	bool valid;
	struct sfptpd_timespec tx;
	struct sfptpd_timespec rx;
	struct sfptpd_timespec correction;
};

/** PTP timestamp dataset
 * @complete Indicates that the dataset is complete
 * @time Time at which this dataset was assembled
 * @path_delay Calculated path delay between the slave and peer or
 * slave and master, according to the current mode
 * @offset_from_master Calculated offset from the slave to the master
 * @ts.s2m Slave to master timestamps
 * @ts.m2s Master to slave timestamps
 * @ts.s2p Slave to peer timestamps
 * @ts.p2s Peer to slave timestamps
 */
typedef struct sfptpd_ptp_tsd {
	bool complete;
	struct sfptpd_timespec time_monotonic;
	struct sfptpd_timespec time_protocol;
	sfptpd_time_t path_delay;
	sfptpd_time_t offset_from_master;

	struct {
		struct sfptpd_ptp_delay_data s2m;
		struct sfptpd_ptp_delay_data m2s;
		struct sfptpd_ptp_delay_data s2p;
		struct sfptpd_ptp_delay_data p2s;
	} ts;
} sfptpd_ptp_tsd_t;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise a PTP dataset
 * @param tsd Pointer to dataset object
 */
void sfptpd_ptp_tsd_init(sfptpd_ptp_tsd_t *tsd);

/** Clear the master to slave timestamp set
 * @param tsd Pointer to dataset object
 */
void sfptpd_ptp_tsd_clear_m2s(sfptpd_ptp_tsd_t *tsd);

/** Clear the slave to master timestamp set
 * @param tsd Pointer to dataset object
 */
void sfptpd_ptp_tsd_clear_s2m(sfptpd_ptp_tsd_t *tsd);

/** Clear the peer to peer timestamp sets
 * @param tsd Pointer to dataset object
 */
void sfptpd_ptp_tsd_clear_p2p(sfptpd_ptp_tsd_t *tsd);

/** Set the master to slave timestamps. This is used when a PTP Sync/FollowUp
 * packet is received to record the delay measurement from the master to
 * slave.
 * @param tsd Pointer to dataset object
 * @param tx_timestamp Timestamp taken when the Master transmitted the packet
 * @param rx_timestamp Timestamp taken when the Slave received the packet
 * @param correction Correction required due to sub-ns accuracy and/or
 * packet residency time in a transparent clock.
 * @return A boolean indicating whether the dataset is complete
 */
bool sfptpd_ptp_tsd_set_m2s(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *tx_timestamp,
			    struct sfptpd_timespec *rx_timestamp,
			    struct sfptpd_timespec *correction);

/** Set the slave to master timestamps. This is used when a PTP node is
 * operating in end-to-end mode when a PTP DelayReq/Resp packet exchange
 * has occurred in order to record the delay measurement from the slave to
 * the master.
 * @param tsd Pointer to dataset object
 * @param tx_timestamp Timestamp taken when the Slave transmitted the packet
 * @param rx_timestamp Timestamp taken when the Master received the packet
 * @param correction Correction required due to sub-ns accuracy and/or
 * packet residency time in a transparent clock.
 * @return A boolean indicating whether the dataset is complete
 */
bool sfptpd_ptp_tsd_set_s2m(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *tx_timestamp,
			    struct sfptpd_timespec *rx_timestamp,
			    struct sfptpd_timespec *correction);

/** Set the peer to peer timestamps. This is used when a PTP node is
 * operating in peer-to-peer mode when a PTP PeerDelayReq/Resp/Followup
 * packet exchange has occurred in order to record the delay measurement
 * between the slave and the peer.
 * @param tsd Pointer to dataset object
 * @param s2p_tx_timestamp Timestamp taken when the Slave transmitted the Peer
 * Delay Request packet
 * @param s2p_rx_timestamp Timestamp taken when the Master received the Peer
 * Delay Request packet
 * @param p2s_tx_timestamp Timestamp taken when the Master transmitted the Peer
 * Delay Response packet
 * @param p2s_rx_timestamp Timestamp taken when the Slave received the Peer
 * Delay Response packet
 * @param correction Correction required due to sub-ns accuracy and/or
 * packet residency time in the peer.
 * @return A boolean indicating whether the dataset is complete
 */
bool sfptpd_ptp_tsd_set_p2p(sfptpd_ptp_tsd_t *tsd,
			    struct sfptpd_timespec *s2p_tx_timestamp,
			    struct sfptpd_timespec *s2p_rx_timestamp,
			    struct sfptpd_timespec *p2s_tx_timestamp,
			    struct sfptpd_timespec *p2s_rx_timestamp,
			    struct sfptpd_timespec *correction);

/** Return the offset from master based on the current set of timestamps.
 * @param tsd Pointer to dataset object
 * @return The offset from master in nanoseconds
 */
static inline sfptpd_time_t sfptpd_ptp_tsd_get_offset_from_master(const sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd);
	assert(tsd->complete);
	return tsd->offset_from_master;
}

/** Return the path delay based on the current set of timestamps.
 * @param tsd Pointer to dataset object
 * @return The current path delay in nanoseconds
 */
static inline sfptpd_time_t sfptpd_ptp_tsd_get_path_delay(const sfptpd_ptp_tsd_t *tsd)
{
	assert(tsd);
	assert(tsd->complete);
	return tsd->path_delay;
}

/** Return the monotonic time of the current set of timestamps.
 * @param tsd Pointer to dataset object
 * @return The time in the dataset
*/
static inline struct sfptpd_timespec sfptpd_ptp_tsd_get_monotonic_time(const sfptpd_ptp_tsd_t *tsd)
{
       assert(tsd);
       assert(tsd->complete);
       return tsd->time_monotonic;
}

/** Return the protocol time of the current set of timestamps.
 * @param tsd Pointer to dataset object
 * @return The time in the dataset
*/
static inline struct sfptpd_timespec sfptpd_ptp_tsd_get_protocol_time(const sfptpd_ptp_tsd_t *tsd)
{
       assert(tsd);
       return tsd->time_protocol;
}


#endif /* _SFPTPD_PTP_TIMESTAMP_DATASET_H */
