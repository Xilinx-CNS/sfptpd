/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_ntpd_client.c
 * @brief  Interface to NTP daemon
 */

#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>

#include "sfptpd_logging.h"
#include "sfptpd_ntpd_client.h"
#include "sfptpd_ntpd_client_impl.h"


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static int select_protocol(struct sfptpd_ntpclient *container)
{
	/* Scenarios:
	 * 	- protocol instances have just been created
	 * 	- ntp daemon was off and has now started
	 */
	if (container->selected == NULL)
	{
		if (container->mode7.fns->test_connection(container->mode7.state) == 0) {
			container->selected = &(container->mode7);
			TRACE_L3("ntpclient: selected NTP Mode 7 Protocol\n");
		} else if (container->mode6.fns->test_connection(container->mode6.state) == 0) {
			container->selected = &(container->mode6);
			TRACE_L3("ntpclient: selected NTP Mode 6 Protocol\n");
		} else {
			/* No NTP protocol available */
			return ENOPROTOOPT;
		}
	}
	return 0;
}


/****************************************************************************
 * Functions
 ****************************************************************************/

void sfptpd_ntpclient_print_peers(struct sfptpd_ntpclient_peer_info *peer_info,
				  const char *subsystem)
{
	assert(peer_info != NULL);

	char remote_host[NI_MAXHOST];
	char local_host[NI_MAXHOST];
	struct sfptpd_ntpclient_peer *peer;
	for (int i = 0; i < peer_info->num_peers; i++)
	{
		peer = &peer_info->peers[i];
		remote_host[0]	= '\0';
		local_host[0]	= '\0';

		int rc = getnameinfo((struct sockaddr *) &peer->remote_address,
				     peer->remote_address_len,
				     remote_host, sizeof remote_host,
				     NULL, 0, NI_NUMERICHOST);
		if (rc != 0) {
			if (peer->self) {
				strcpy(remote_host, "<reference clock>");
			} else {
				TRACE_L5("ntpclient: getnameinfo: error "
					 "retrieving remote_address, %s\n",
					 gai_strerror(rc));
				strcpy(remote_host, "<invalid>");
			}
		}

		rc = getnameinfo((struct sockaddr *) &peer->local_address,
				 peer->local_address_len,
				 local_host, sizeof local_host,
				 NULL, 0, NI_NUMERICHOST);
		if (rc != 0) {
			if (peer->self) {
				strcpy(local_host, "<reference clock>");
			} else {
				TRACE_L5("ntpclient: getnameinfo: error "
					 "retrieving local_address, %s\n",
					 gai_strerror(rc));
				strcpy(local_host, "<invalid>");
			}
		}

		TRACE_L5("%s-peer%d: remote-address %s, "
			 "local-address %s, sent %u, received %u, "
			 "candidate %d, stratum %d, offset %0.3Lf ns, "
			 "root disp %0.3Lf ns\n",
			 subsystem,
			 i, remote_host, local_host,
			 peer->pkts_sent, peer->pkts_received,
			 peer->candidate, peer->stratum,
			 peer->offset, peer->root_dispersion);
	}
}

int sfptpd_ntpclient_create(struct sfptpd_ntpclient **container,
			    int32_t key_id, char *key_value)
{
	struct sfptpd_ntpclient *new;
	int rc;

	assert(container != NULL);
	
	/* Create client container */
	new = calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("ntpclient: failed to allocate memory for client container\n");
		return ENOMEM;
	}

	/* Create instances of all protocols */
	rc = sfptpd_ntpclient_mode7_create(&(new->mode7.state), &(new->mode7.fns),
					   key_id, key_value);
	if (rc != 0) {
		rc = ECANCELED;
		goto fail;
	}
	rc = sfptpd_ntpclient_mode6_create(&(new->mode6.state), &(new->mode6.fns),
					   key_id, key_value);
	if (rc != 0) {
		rc = ECANCELED;
		goto cleanup_mode7;
	}
	
	/* Attempt to select the best available protocol */
	rc = select_protocol(new);
	if (rc != 0) {
		if (rc == ENOPROTOOPT)
			TRACE_L5("ntpclient: could not communicate with NTP "
				 "daemon over any known protocol.\n");
	}

	/* Success */
	*container = new;
	return rc;
	
cleanup_mode7:
	new->mode7.fns->destroy(&(new->mode7.state));
fail:
	free(new);
	return rc;
}

void sfptpd_ntpclient_destroy(struct sfptpd_ntpclient **container)
{
	assert(*container != NULL);
	
	/* Destroy all client protocol instances */
	(*container)->mode7.fns->destroy(&((*container)->mode7.state));
	(*container)->mode6.fns->destroy(&((*container)->mode6.state));

	/* Free memory for client container */
	free(*container);
	*container = NULL;
}

int sfptpd_ntpclient_get_sys_info(struct sfptpd_ntpclient *container,
				  struct sfptpd_ntpclient_sys_info *sys_info)
{
	struct sfptpd_ntpclient_protocol *client;
	int rc;

	/* Select the best available protocol */
	rc = select_protocol(container);
	if (rc != 0)
		return rc;

	client = (container->selected);

	/* Get sys info through whichever protocol we are currently using */
	rc = ENOPROTOOPT;
	if (client->state != NULL)
		rc = client->fns->get_sys_info(client->state, sys_info);
	return rc;
}

int sfptpd_ntpclient_get_peer_info(struct sfptpd_ntpclient *container,
				   struct sfptpd_ntpclient_peer_info *peer_info)
{
	struct sfptpd_ntpclient_protocol *client;
	int rc;

	/* Select the best available protocol */
	rc = select_protocol(container);
	if (rc != 0)
		return rc;

	client = (container->selected);

	/* Get peer info through whichever protocol we are currently using */
	rc = ENOPROTOOPT;
	if (client->state != NULL)
		rc = client->fns->get_peer_info(client->state, peer_info);
	if (rc == 0)
		sfptpd_ntpclient_print_peers(peer_info, "ntp");
	return rc;
}

int sfptpd_ntpclient_clock_control(struct sfptpd_ntpclient *container,
				   bool enable)
{
	struct sfptpd_ntpclient_protocol *client;
	int rc;

	/* Select the best available protocol */
	rc = select_protocol(container);
	if (rc != 0)
		return rc;

	client = (container->selected);

	/* Attempt to set clock control through whichever protocol we are
	 * currently using */
	rc = ENOPROTOOPT;
	if (client->state != NULL)
		rc = client->fns->clock_control(client->state, enable);
	return rc;
}

struct sfptpd_ntpclient_feature_flags *
sfptpd_ntpclient_get_features(struct sfptpd_ntpclient *container)
{
	struct sfptpd_ntpclient_protocol *client = (container->selected);
	
	/* Get features through whichever protocol we are currently using */
	if (client->state != NULL)
		return client->fns->get_features(client->state);
	else {
		WARNING("ntpclient: trying to retrieve features with no "
			"protocol selected.");
		return NULL;
	}
}


/* fin */
