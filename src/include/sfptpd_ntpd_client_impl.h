/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_NTPD_CLIENT_IMPL_H
#define _SFPTPD_NTPD_CLIENT_IMPL_H

#include <stdint.h>
#include <stdbool.h>
#include <arpa/inet.h>

#include "sfptpd_ntpd_client.h"


/****************************************************************************
 * Defines, Structures and Types
 ****************************************************************************/

/* Maximum NTP key value - longer than strictly necessary */
#define SFPTPD_NTP_KEY_MAX (32)

/* Functions each client protocol implmentation must contain */
struct sfptpd_ntpclient_fns {
	void (*destroy)(struct sfptpd_ntpclient_state **state);
	int (*get_sys_info)(struct sfptpd_ntpclient_state *state,
			    struct sfptpd_ntpclient_sys_info *sys_info);
	int (*get_peer_info)(struct sfptpd_ntpclient_state *state,
			     struct sfptpd_ntpclient_peer_info *peer_info);
	int (*clock_control)(struct sfptpd_ntpclient_state *state, bool enable);
	struct sfptpd_ntpclient_feature_flags *
	(*get_features)(struct sfptpd_ntpclient_state *state);
	int (*test_connection)(struct sfptpd_ntpclient_state *state);
};

/* Protocol implementations interfaces, used by the client wrapper */
extern const struct sfptpd_ntpclient_fns sfptpd_ntpclient_mode6_fns;
extern const struct sfptpd_ntpclient_fns sfptpd_ntpclient_mode7_fns;

/****************************************************************************
 * Functions
 ****************************************************************************/

/* Create mode6 specific state object to use protocol */
int sfptpd_ntpclient_mode6_create(struct sfptpd_ntpclient_state **ntpclient,
				  const struct sfptpd_ntpclient_fns **fns,
				  int32_t key_id, char *key_value);

/* Create mode7 specific state object to use protocol */
int sfptpd_ntpclient_mode7_create(struct sfptpd_ntpclient_state **ntpclient,
				  const struct sfptpd_ntpclient_fns **fns,
				  int32_t key_id, char *key_value);


#endif	/* _SFPTPD_NTPD_CLIENT_IMPL_H */
