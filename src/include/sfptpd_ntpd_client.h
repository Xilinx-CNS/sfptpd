/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_NTPD_CLIENT_H
#define _SFPTPD_NTPD_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/socket.h>

/****************************************************************************
 * Defines, Structures and Types
 ****************************************************************************/

/** Maximum number of peers supported */
#define SFPTPD_NTP_PEERS_MAX (32)

/** Timeout for mode 7 queries */
#define SFPTPD_NTP_MODE7_TIMEOUT_NS 300000000LL

/* Forward declare NTP client structure */
struct sfptpd_ntpclient;

/* Forward declare NTP client state */
struct sfptpd_ntpclient_state;

/* Forward declare NTP client functions */
struct sfptpd_ntpclient_fns;

/* NTP client structure */
struct sfptpd_ntpclient_protocol {
	const struct sfptpd_ntpclient_fns *fns;
	struct sfptpd_ntpclient_state *state;
};

/* NTP client container
 * @mode6: NTP Mode 6 protocol client
 * @mode7: NTP Mode 7 protocol client
 * @selected: selected protocol client = mode6/7, or NULL for unselected
 */
struct sfptpd_ntpclient {
	struct sfptpd_ntpclient_protocol mode6;
	struct sfptpd_ntpclient_protocol mode7;
	struct sfptpd_ntpclient_protocol *selected;
};

/** Structure to return information about the state of the NTP daemon.
 * Note: this is only a small subset of the data available. More information
 * will be exposed through this interface as required.
 * @peer_address: Currently selected peer IP address
 * @clock_control_enabled: NTP is controlling the system clock
 */
struct sfptpd_ntpclient_sys_info {
	struct sockaddr_storage peer_address;
	socklen_t peer_address_len;
	bool clock_control_enabled;
};

/** Structure to return information about a peer of the NTP daemon.
 * Note: this is only a small subset of the data available. More information
 * will be exposed through this interface as required.
 * @remote_address: Address of the peer
 * @local_address: Address of the local interface
 * @pkts_sent: Number of packets sent
 * @pkts_received: Number of packets received
 * @stratum: Clock stratum of peer
 * @selected: Currently selected peer
 * @shortlist: Shortlisted peer
 * @candidate: Candidate peer
 * @self: This peer is the localhost
 * @offset: Estimated offset between this peer and the local clock
 * @root_dispersion: Estimated error between peer and primary time source
 */
struct sfptpd_ntpclient_peer {
	struct sockaddr_storage remote_address;
	socklen_t remote_address_len;
	struct sockaddr_storage local_address;
	socklen_t local_address_len;
	uint32_t pkts_sent;
	uint32_t pkts_received;
	uint32_t ref_id;
	unsigned int stratum;
	bool selected;
	bool shortlist;
	bool candidate;
	bool self;
	long double offset;
	long double root_dispersion;
	long double smoothed_offset;
	long double smoothed_root_dispersion;
	long double tracking_offset;
};

/** Structure to return information about the peers of the NTP daemon.
 * @num_peers: Number of peers
 * @peers: Array of structures containing peer information
 */
struct sfptpd_ntpclient_peer_info {
	unsigned int num_peers;
	struct sfptpd_ntpclient_peer peers[SFPTPD_NTP_PEERS_MAX];
};

/* Structure to hold feature flags for each NTP protocol
 * 
 * |   |                   | mode 7 | mode 6 |
 * |---+-------------------+--------+--------|
 * | 1 | Detect presence   | *      | *      |
 * | 2 | Get peers         | *      | *      |
 * | 3 | Get state         | *      | *      |
 * | 4 | Get clock control | *      |        |
 * | 5 | Set clock control | *      | *      |
 * | 6 | Act as source     |        |        |
 */
struct sfptpd_ntpclient_feature_flags {
	bool detect_presense;
	bool get_peers;
	bool get_state;
	bool get_clock_control;
	bool set_clock_control;
};


/****************************************************************************
 * Functions
 ****************************************************************************/

/* Wrapper functions to hide implementation and protocol selection */

/** Create all client instances, and test that communication with the selected
 * protocol (hidden from the ntp module) works.
 * @param container: NTP client container 
 * @param key_id: Key ID
 * @param key_value: Key passphrase
 * @return 0 - success
 * 	ENOMEM - Failed to allocate memory for client container
 * 	ECANCELED - At least one protocol instance failed to be created, and any
 * 	protocol instances that were successfully created have been destroyed.
 * 	ENOPROTOPT - No protocols available (likely ntpd is not active)
 */
int sfptpd_ntpclient_create(struct sfptpd_ntpclient **container,
			    int32_t key_id, char *key_value);

/* Destroy all client protocol instances */
void sfptpd_ntpclient_destroy(struct sfptpd_ntpclient **container);

/** Get system info from NTP daemon. This function sends a request and to
 * the NTP daemon and waits for a response.
 * @param container: NTP client container 
 * @param sys_info Returned system info
 * @return 0 - success
 *         ETIMEDOUT - timed out waiting for response
 *         EIO - communications error
 *         EPROTO - malformed/incomplete/corrupt response
 */
int sfptpd_ntpclient_get_sys_info(struct sfptpd_ntpclient *container,
				  struct sfptpd_ntpclient_sys_info *sys_info);

/** Get peer info from NTP daemon. This function sends a request and to
 * the NTP daemon and waits for a response.
 * @param container: NTP client container 
 * @param peer_info Returned peer info
 * @return 0 - success
 *         ETIMEDOUT - timed out waiting for response
 *         EPROTO - malformed/incomplete/corrupt response
 *         EIO - communications error
 */
int sfptpd_ntpclient_get_peer_info(struct sfptpd_ntpclient *container,
				   struct sfptpd_ntpclient_peer_info *peer_info);

/** Enable or disable clock control. This function sends a request to the NTP
 * daemon to modify its system control flags and either enable or disable
 * disciplining of the system clock.
 * @param container: NTP client container 
 * @param enable Boolean indicating whether to enable or disable
 * @return 0 - success
 *         ETIMEDOUT - timed out waiting for response
 *         EACCES - permission denied
 *         EPROTO - malformed/incomplete/corrupt response
 *         EIO - communications error
 */
int sfptpd_ntpclient_clock_control(struct sfptpd_ntpclient *container,
				   bool enable);

/** Get feature flags. This function returns an array of bool flags which
 * describe the features available for the current protocol.
 * @param container: NTP client container 
 * @return: bool features[NTPD_NUM_FEATURE_FLAGS]
 */
struct sfptpd_ntpclient_feature_flags *
sfptpd_ntpclient_get_features(struct sfptpd_ntpclient *container);

/** Output trace to document NTP peers.
 * @param peer_info The NTP peer information
 */
void sfptpd_ntpclient_print_peers(struct sfptpd_ntpclient_peer_info *peer_info,
				  const char *subsystem);

/* Get best estimate of offset to NTP peer
 * @param peer the peer info object
 * @return the offset in ns
 */
static inline sfptpd_time_t sfptpd_ntpclient_offset(struct sfptpd_ntpclient_peer *peer)
{
	return isnormal(peer->tracking_offset) ? peer->tracking_offset :
			(isnormal(peer->smoothed_offset) ? peer->smoothed_offset :
			 peer->offset);
}


/* Get best estimate of error for NTP peer
 * @param peer the peer info object
 * @return the offset in ns
 */
static inline sfptpd_time_t sfptpd_ntpclient_error(struct sfptpd_ntpclient_peer *peer)
{
	return isnormal(peer->smoothed_root_dispersion) ? peer->smoothed_root_dispersion : peer->root_dispersion;
}

static inline struct sfptpd_ntpclient_peer sfptpd_ntpclient_peer_null(void) {
	return (struct sfptpd_ntpclient_peer) {
		.offset = NAN,
		.root_dispersion = NAN,
		.smoothed_offset = NAN,
		.smoothed_root_dispersion = NAN,
	};
}

#endif /* _SFPTPD_NTPD_CLIENT_H */
