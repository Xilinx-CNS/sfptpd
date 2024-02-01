/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_ptp_config.c
 * @brief  PTP Synchronization Module Configuration
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <regex.h>

#include "sfptpd_ptp_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "ptpd_lib.h"


/****************************************************************************
 * Defaults
 ****************************************************************************/

const static struct sfptpd_ptp_profile_def ptp_profiles[] = {
	{ .name = "default-e2e",
	  .uri = "https://standards.ieee.org/standard/1588-2019.html",
	  .version = "1.0",
	  .id = {0x00, 0x1B, 0x19, 0x00, 0x01, 0x00},
	  .announce_interval = {-4, 4, 1},
	  .sync_interval = {-4, 4, 0},
	  .delayreq_interval = {-4, 5, 0},
	  .announce_timeout = {2, INT8_MAX, 6},
	  .delay_mechanisms = 1 << PTPD_DELAY_MECHANISM_E2E,
	},
	{ .name = "default-p2p",
	  .uri = "https://standards.ieee.org/standard/1588-2019.html",
	  .version = "1.0",
	  .id = {0x00, 0x1B, 0x19, 0x00, 0x02, 0x00},
	  .announce_interval = {-4, 4, 1},
	  .sync_interval = {-4, 4, 0},
	  .delayreq_interval = {-4, 5, 0},
	  .announce_timeout = {2, INT8_MAX, 6},
	  .delay_mechanisms = 1 << PTPD_DELAY_MECHANISM_P2P,
	},
	{ .name = "enterprise",
	  .uri = "https://datatracker.ietf.org/doc/html/draft-ietf-tictoc-ptp-enterprise-profile-19",
	  .version = "1.0 draft 19",
	  .id = {0x00, 0x00, 0x5E, 0x00, 0x01, 0x00},
	  .announce_interval = {0, 0, 0},
	  .sync_interval = {-4, 4, 0}, /*!< [-128,128] in spec */
	  .delayreq_interval = {-4, 5, 0}, /*!< [-128,128] in spec */
	  .announce_timeout = {3, 3, 3},
	  .delay_mechanisms = 1 << PTPD_DELAY_MECHANISM_E2E,
	}
	/* Telecoms profile
	  .announce_interval = {-3, 4, ?}
	  .sync_interval = {-1, 1, ?}
	  .delayreq_interval = {0, 5, ?}
	 */
	/* 802.1as profile
	  .announce_interval = {?, ?, 0}
	  .sync_interval = {?, ?, -7}
	  .delayreq_interval = {?, ?, -4}
	 */
};


/****************************************************************************
 * Helper Functions
 ****************************************************************************/

const struct sfptpd_ptp_profile_def *sfptpd_ptp_get_profile_def(enum sfptpd_ptp_profile profile_index) {
	if (profile_index == SFPTPD_PTP_PROFILE_UNDEF)
		profile_index = SFPTPD_PTP_PROFILE_DEFAULT_E2E;

	assert(profile_index >=0);
	assert(profile_index < sizeof ptp_profiles / sizeof ptp_profiles[0]);

	return &ptp_profiles[profile_index];
}


/****************************************************************************
 * Config File Options
 ****************************************************************************/

static int parse_ptp_profile(struct sfptpd_config_section *section, const char *option,
	                     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "default-e2e") == 0) {
		ptp->profile = SFPTPD_PTP_PROFILE_DEFAULT_E2E;
	} else if (strcmp(params[0], "default-p2p") == 0) {
		ptp->profile = SFPTPD_PTP_PROFILE_DEFAULT_P2P;
	} else if (strcmp(params[0], "enterprise") == 0) {
		ptp->profile = SFPTPD_PTP_PROFILE_ENTERPRISE;
	} else {
		rc = EINVAL;
	}

	/* Apply profile defaults. Profile should be specified first within
	   the configuration section and preferably the global [ptp] one. */
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	ptp->ptpd_port.announceInterval = profile->announce_interval.def;
	ptp->ptpd_port.syncInterval = profile->sync_interval.def;
	ptp->ptpd_port.minDelayReqInterval = profile->delayreq_interval.def;
	ptp->ptpd_port.minPdelayReqInterval = profile->delayreq_interval.def;
	ptp->ptpd_port.announceReceiptTimeout = profile->announce_timeout.def;

	return rc;
}

static int parse_ptp_version(struct sfptpd_config_section *section, const char *option,
	                     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "2.0") == 0) {
		ptp->ptpd_port.ptp_version_minor = 0;
	} else if (strcmp(params[0], "2.1") == 0) {
		ptp->ptpd_port.ptp_version_minor = 1;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_ptp_mode(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "slave") == 0) {
		ptp->ptpd_port.slaveOnly = TRUE;
		ptp->ptpd_port.masterOnly = FALSE;
		ptp->ptpd_port.node_type = PTPD_NODE_CLOCK;
	} else if (strcmp(params[0], "master") == 0) {
		ptp->ptpd_port.slaveOnly = FALSE;
		ptp->ptpd_port.masterOnly = FALSE;
		ptp->ptpd_port.node_type = PTPD_NODE_CLOCK;
	} else if (strcmp(params[0], "master-only") == 0) {
		ptp->ptpd_port.slaveOnly = FALSE;
		ptp->ptpd_port.masterOnly = TRUE;
		ptp->ptpd_port.node_type = PTPD_NODE_CLOCK;
	} else if (strcmp(params[0], "monitor") == 0) {
		ptp->ptpd_port.slaveOnly = FALSE;
		ptp->ptpd_port.masterOnly = FALSE;
		ptp->ptpd_port.node_type = PTPD_NODE_MONITOR;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_interface(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(ptp->interface_name, params[0], sizeof(ptp->interface_name));
	return 0;
}

static int parse_transport(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "ipv4") == 0) {
		ptp->ptpd_intf.transportAF = AF_INET;
	} else if (strcmp(params[0], "ipv6") == 0) {
		ptp->ptpd_intf.transportAF = AF_INET6;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_scope(struct sfptpd_config_section *section, const char *option,
		       unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "link-local") == 0) {
		ptp->ptpd_intf.linkLocalScope = TRUE;
	} else if (strcmp(params[0], "global") == 0) {
		ptp->ptpd_intf.linkLocalScope = FALSE;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens, priority;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &priority);
	if (tokens != 1)
		return EINVAL;

	ptp->priority = (unsigned int)priority;
	return 0;
}

static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	ptp->convergence_threshold = threshold;
	return 0;
}

static int parse_timestamping(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "sw") == 0) {
		ptp->ptpd_port.timestamp_pref = PTPD_TIMESTAMP_TYPE_SW;
	} else if (strcmp(params[0], "hw") == 0) {
		ptp->ptpd_port.timestamp_pref = PTPD_TIMESTAMP_TYPE_HW;
	} else if (strcmp(params[0], "auto") == 0) {
		ptp->ptpd_port.timestamp_pref = PTPD_TIMESTAMP_TYPE_AUTO;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_pkt_dump(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	ptp->ptpd_intf.displayPackets = TRUE;
	return 0;
}

static int parse_pps_log(struct sfptpd_config_section *section, const char *option,
			 unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	ptp->pps_logging = true;
	return 0;
}

static int parse_tx_latency(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, outboundLatency;

	ptp->ptpd_port.outboundLatency.sec = 0;
	ptp->ptpd_port.outboundLatency.nsec_frac = 0;
	tokens = sscanf(params[0], "%i", &outboundLatency);
	if (tokens != 1)
		return EINVAL;

	ptp->ptpd_port.outboundLatency.nsec = (Integer32)outboundLatency;
	return 0;
}

static int parse_rx_latency(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, inboundLatency;

	ptp->ptpd_port.inboundLatency.sec = 0;
	tokens = sscanf(params[0], "%i", &inboundLatency);
	if (tokens != 1)
		return EINVAL;

	ptp->ptpd_port.inboundLatency.nsec = (Integer32)inboundLatency;
	return 0;
}

static int parse_delay_mechanism(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	assert(num_params == 1);

	if (strcmp(params[0], "end-to-end") == 0) {
		ptp->ptpd_port.delayMechanism = PTPD_DELAY_MECHANISM_E2E;
	} else if (strcmp(params[0], "peer-to-peer") == 0) {
		ptp->ptpd_port.delayMechanism = PTPD_DELAY_MECHANISM_P2P;
	} else {
		rc = EINVAL;
	}

	if (((1 << ptp->ptpd_port.delayMechanism) & profile->delay_mechanisms) == 0 &&
	    ptp->profile != SFPTPD_PTP_PROFILE_UNDEF) {
		ERROR("PTP profile %s does not support %s delay mechanism\n",
		      profile->name, params[0]);
		return EINVAL;
	}

	return rc;
}

static int parse_network_mode(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	PortCommunicationCapabilities *caps = &ptp->ptpd_port.comm_caps;

	assert(num_params == 1);

	if (strcmp(params[0], "multicast") == 0) {
		caps->delayRespCapabilities = PTPD_COMM_MULTICAST_CAPABLE;
	} else if (strcmp(params[0], "hybrid") == 0) {
		caps->delayRespCapabilities = PTPD_COMM_MULTICAST_CAPABLE | PTPD_COMM_UNICAST_CAPABLE;
	} else if (strcmp(params[0], "hybrid-no-fallback") == 0) {
		caps->delayRespCapabilities = PTPD_COMM_UNICAST_CAPABLE;
	} else {
		rc = EINVAL;
	}
	
	return rc;
}

static int parse_multicast_ttl(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens;

	tokens = sscanf(params[0], "%i", &(ptp->ptpd_intf.ttl));
	if (tokens != 1)
		return EINVAL;

	return 0;
}

static int parse_utc_offset(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, currentUtcOffset;

	tokens = sscanf(params[0], "%i", &currentUtcOffset);
	if (tokens != 1)
		return EINVAL;

	if ((currentUtcOffset < INT16_MIN) || (currentUtcOffset > INT16_MAX)) {
		CFG_ERROR(section, "PTP UTC Offset outside allowed range of [%d,%d]\n",
			  INT16_MIN, INT16_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.timeProperties.currentUtcOffsetValid = TRUE;
	ptp->ptpd_port.timeProperties.currentUtcOffset = (Integer16)currentUtcOffset;
	return 0;
}

static int parse_utc_valid_handling(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);

	/* The 'override' option takes an additional parameter. */
	if (strcmp(params[0], "override") == 0) {
		int override_utc_offset_seconds;
		int tokens;

		if (num_params != 2)
			return EINVAL;
		tokens = sscanf(params[1], "%i", &override_utc_offset_seconds);
		if (tokens != 1)
			return EINVAL;

		ptp->ptpd_port.overrideUtcOffset = TRUE;
		ptp->ptpd_port.overrideUtcOffsetSeconds = override_utc_offset_seconds;
		ptp->ptpd_port.alwaysRespectUtcOffset = FALSE;
		ptp->ptpd_port.preferUtcValid = FALSE;
		ptp->ptpd_port.requireUtcValid = FALSE;
		return 0;
	}

	/* The other options take no additional parameters. */
	if (num_params != 1)
		return EINVAL;
	ptp->ptpd_port.overrideUtcOffset = FALSE;
	ptp->ptpd_port.overrideUtcOffsetSeconds = FALSE;
	if (strcmp(params[0], "default") == 0) {
		ptp->ptpd_port.alwaysRespectUtcOffset = FALSE;
		ptp->ptpd_port.preferUtcValid = FALSE;
		ptp->ptpd_port.requireUtcValid = FALSE;
	} else if (strcmp(params[0], "ignore") == 0) {
		ptp->ptpd_port.alwaysRespectUtcOffset = TRUE;
		ptp->ptpd_port.preferUtcValid = FALSE;
		ptp->ptpd_port.requireUtcValid = FALSE;
	} else if (strcmp(params[0], "prefer") == 0) {
		ptp->ptpd_port.alwaysRespectUtcOffset = FALSE;
		ptp->ptpd_port.preferUtcValid = TRUE;
		ptp->ptpd_port.requireUtcValid = FALSE;
	} else if (strcmp(params[0], "require") == 0) {
		ptp->ptpd_port.alwaysRespectUtcOffset = FALSE;
		ptp->ptpd_port.preferUtcValid = FALSE;
		ptp->ptpd_port.requireUtcValid = TRUE;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_ptp_timescale(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "tai") == 0) {
		ptp->ptpd_port.timeProperties.ptpTimescale = TRUE;
	} else if (strcmp(params[0], "utc") == 0) {
		ptp->ptpd_port.timeProperties.ptpTimescale = FALSE;
	} else {
		rc = EINVAL;
	}

	return rc;
}


static int parse_ptp_domain(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, domainNumber;

	tokens = sscanf(params[0], "%i", &domainNumber);
	if (tokens != 1)
		return EINVAL;

	if ((domainNumber < 0) || (domainNumber > UINT8_MAX)) {
		CFG_ERROR(section, "PTP Domain outside allowed range [%d,%d]\n",
		          0, UINT8_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.domainNumber = (UInteger8)domainNumber;
	return 0;
}

static int parse_ptp_mgmt_msgs(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "disabled") == 0) {
		ptp->ptpd_port.managementEnabled = FALSE;
		ptp->ptpd_port.managementSetEnable = FALSE;
	} else if (strcmp(params[0], "read-only") == 0) {
		ptp->ptpd_port.managementEnabled = TRUE;
		ptp->ptpd_port.managementSetEnable = FALSE;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_ptp_acl_list(char *acl, unsigned int acl_size, const char *option_text,
			      unsigned int num_params, const char * const params[])
{
	int len;
	assert(acl != NULL);
	assert(option_text != NULL);
	assert(params != NULL);

	len = 0;
	acl[0] = '\0';

	for ( ; num_params > 0; num_params--, params++) {
		len += snprintf(acl + len, acl_size - len, "%s ", params[0]);
		if (len > acl_size) {
			ERROR("ACL %s list too long. Have \"%s\" but still %d tokens to add\n",
			      option_text, acl, num_params);
			return E2BIG;
		}
	}

	return 0;
}

static int parse_ptp_timing_acl_allow(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.timingAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.timingAclAllowText,
				  sizeof(ptp->ptpd_intf.timingAclAllowText),
				  "timing allow", num_params, params);
}

static int parse_ptp_timing_acl_deny(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.timingAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.timingAclDenyText,
				  sizeof(ptp->ptpd_intf.timingAclDenyText),
				  "timing deny", num_params, params);
}

static int parse_ptp_mgmt_acl_allow(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.managementAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.managementAclAllowText,
				  sizeof(ptp->ptpd_intf.managementAclAllowText),
				  "management allow", num_params, params);
}

static int parse_ptp_mgmt_acl_deny(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.managementAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.managementAclDenyText,
				  sizeof(ptp->ptpd_intf.managementAclDenyText),
				  "management deny", num_params, params);
}

static int parse_ptp_mon_acl_allow(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.monitoringAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.monitoringAclAllowText,
				  sizeof(ptp->ptpd_intf.monitoringAclAllowText),
				  "monitoring allow", num_params, params);
}

static int parse_ptp_mon_acl_deny(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params >= 1);
	ptp->ptpd_intf.monitoringAclEnabled = TRUE;
	return parse_ptp_acl_list(ptp->ptpd_intf.monitoringAclDenyText,
				  sizeof(ptp->ptpd_intf.monitoringAclDenyText),
				  "monitoring deny", num_params, params);
}

static int parse_ptp_acl_order(PtpdAclOrder *order, const char *option_text,
			       const char *param)
{
	int rc = 0;
	const char *deprecated_fmt = "ptp %s acl: deprecated alias %s treated as %s\n";

	assert(order != NULL);
	assert(option_text != NULL);
	assert(param != NULL);

	if (strcmp(param, "allow-deny") == 0) {
		*order = PTPD_ACL_ALLOW_DENY;
	} else if (strcmp(param, "deny-allow") == 0) {
		*order = PTPD_ACL_DENY_ALLOW;
	} else if (strcmp(param, "permit-deny") == 0) {
		WARNING(deprecated_fmt, option_text, param, "deny-allow");
		*order = PTPD_ACL_DENY_ALLOW;
	} else if (strcmp(param, "deny-permit") == 0) {
		WARNING(deprecated_fmt, option_text, param, "allow-deny");
		*order = PTPD_ACL_ALLOW_DENY;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_ptp_timing_acl_order(struct sfptpd_config_section *section, const char *option,
				      unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	return parse_ptp_acl_order(&ptp->ptpd_intf.timingAclOrder,
				   "timing", params[0]);
}

static int parse_ptp_mgmt_acl_order(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	return parse_ptp_acl_order(&ptp->ptpd_intf.managementAclOrder,
				   "management", params[0]);
}

static int parse_ptp_mon_acl_order(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	ptp->ptpd_intf.monitoringAclEnabled = TRUE;
	return parse_ptp_acl_order(&ptp->ptpd_intf.monitoringAclOrder,
				   "monitoring", params[0]);
}

static int parse_mon_meinberg_netsync(struct sfptpd_config_section *section, const char *option,
				      unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	ptp->ptpd_port.monMeinbergNetSync = TRUE;
	return 0;
}

static int parse_announce_pkt_interval(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	assert(num_params == 1);
	int tokens, announceInterval;

	tokens = sscanf(params[0], "%i", &announceInterval);
	if (tokens != 1)
		return EINVAL;

	if ((announceInterval < profile->announce_interval.min) ||
	    (announceInterval > profile->announce_interval.max)) {
		CFG_ERROR(section, "PTP Announce interval outside allowed range [%d,%d]\n",
			  profile->announce_interval.min, profile->announce_interval.max);
		return ERANGE;
	}

	ptp->ptpd_port.announceInterval = (Integer8)announceInterval;
	return 0;
}

static int parse_announce_pkt_timeout(struct sfptpd_config_section *section, const char *option,
				      unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	assert(num_params == 1);
	int tokens, announceReceiptTimeout;

	tokens = sscanf(params[0], "%i", &announceReceiptTimeout);
	if (tokens != 1)
		return EINVAL;

	if ((announceReceiptTimeout < profile->announce_timeout.min) ||
	    (announceReceiptTimeout > profile->announce_timeout.max)) {
		CFG_ERROR(section, "PTP Announce packet receipt timeout outside allowed range [%d,%d]\n",
			  profile->announce_timeout.min, profile->announce_timeout.max);
		return ERANGE;
	}

	ptp->ptpd_port.announceReceiptTimeout = (Integer8)announceReceiptTimeout;
	return 0;
}

static int parse_sync_pkt_interval(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	assert(num_params == 1);
	int tokens, syncInterval;

	tokens = sscanf(params[0], "%i", &syncInterval);
	if (tokens != 1)
		return EINVAL;

	if ((syncInterval < profile->sync_interval.min) ||
	    (syncInterval > profile->sync_interval.max)) {
		CFG_ERROR(section,
			  "PTP Sync packet interval outside allowed range [%d,%d]\n",
			  profile->sync_interval.min, profile->sync_interval.max);
		return ERANGE;
	}

	ptp->ptpd_port.syncInterval = (Integer8)syncInterval;
	TRACE_L3("PTP Sync packet interval set to 2^%d seconds\n",
		 ptp->ptpd_port.syncInterval);
	return 0;
}

static int parse_sync_pkt_timeout(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, syncReceiptTimeout;

	tokens = sscanf(params[0], "%i", &syncReceiptTimeout);
	if (tokens != 1)
		return EINVAL;

	if ((syncReceiptTimeout < INT8_MIN) || (syncReceiptTimeout > INT8_MAX)) {
		CFG_ERROR(section,
			  "PTP Sync packet receipt timeout outside allowed range [%d,%d]\n",
			  INT8_MIN, INT8_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.syncReceiptTimeout = (Integer8)syncReceiptTimeout;
	return 0;
}

static int parse_delayreq_pkt_interval(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	const sfptpd_ptp_profile_def_t *profile = sfptpd_ptp_get_profile_def(ptp->profile);
	assert(num_params == 1);
	int tokens, minDelayReqInterval;

	tokens = sscanf(params[0], "%i", &minDelayReqInterval);
	if (tokens != 1)
		return EINVAL;

	if ((minDelayReqInterval < profile->delayreq_interval.min) ||
	    (minDelayReqInterval > profile->delayreq_interval.max)) {
		CFG_ERROR(section,
			  "PTP Delay Request interval outside allowed range [%d,%d]\n",
			  profile->delayreq_interval.min, profile->delayreq_interval.max);
		return ERANGE;
	}

	/* Note that we set both the delay and peer-delay intervals but only
	 * one of these will be used depending on the configured delay
	 * mechanism. */
	ptp->ptpd_port.minDelayReqInterval = (Integer8)minDelayReqInterval;
	ptp->ptpd_port.ignore_delayreq_interval_master = TRUE;
	ptp->ptpd_port.minPdelayReqInterval = (Integer8)minDelayReqInterval;
	return 0;
}

static int parse_delayresp_pkt_timeout(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, delayRespReceiptTimeout;

	tokens = sscanf(params[0], "%i", &delayRespReceiptTimeout);
	if (tokens != 1)
		return EINVAL;

	if ((delayRespReceiptTimeout < INT8_MIN) || (delayRespReceiptTimeout > INT8_MAX)) {
		CFG_ERROR(section,
			  "PTP Delay Response receipt timeout outside allowed range [%d,%d]\n",
			  INT8_MIN, INT8_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.delayRespReceiptTimeout = (Integer8)delayRespReceiptTimeout;
	return 0;
}

static int parse_max_missing_delayresps(struct sfptpd_config_section *section, const char *option,
				        unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens;
	int value;

	assert(num_params == 2);

	tokens = sscanf(params[0], "%i", &value);
	if (tokens != 1)
		return EINVAL;

	if (value < 0 || value > INT8_MAX)
		return ERANGE;

	ptp->ptpd_port.delayRespAlarmThreshold = value;

	tokens = sscanf(params[0], "%i", &value);
	if (tokens != 1)
		return EINVAL;

	if (value < 0 || value > INT8_MAX)
		return ERANGE;

	ptp->ptpd_port.delayRespHybridThreshold = value;
	return 0;
}

static int parse_max_foreign_records(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, maxForeignRecords;

	tokens = sscanf(params[0], "%i", &maxForeignRecords);
	if (tokens != 1)
		return EINVAL;

	if ((maxForeignRecords < 1) || (maxForeignRecords > INT16_MAX)) {
		CFG_ERROR(section,
			  "PTP Max Foreign Records outside allowed range [%d,%d]\n",
			  1, INT16_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.max_foreign_records = (Integer16)maxForeignRecords;
	return 0;
}

static int parse_bmc_priority1(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, priority1;

	tokens = sscanf(params[0], "%i", &priority1);
	if (tokens != 1)
		return EINVAL;

	if ((priority1 < 0) || (priority1 > UINT8_MAX)) {
		CFG_ERROR(section,
			  "PTP BMC Priority 1 outside allowed range [%d,%d]\n",
			  0, UINT8_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.priority1 = (UInteger8)priority1;
	return 0;
}

static int parse_bmc_priority2(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, priority2;

	tokens = sscanf(params[0], "%i", &priority2);
	if (tokens != 1)
		return EINVAL;

	if ((priority2 < 0) || (priority2 > UINT8_MAX)) {
		CFG_ERROR(section,
			  "PTP BMC Priority 2 outside allowed range [%d,%d]\n",
			  0, UINT8_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.priority2 = (UInteger8)priority2;
	return 0;
}

static int parse_trace_level(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);
	int tokens, trace_level;

	tokens = sscanf(params[0], "%u", &trace_level);
	if (tokens != 1)
		return EINVAL;

	ptp->trace_level = (unsigned int)trace_level;
	return 0;
}

static int parse_ptp_delay_resp_ignore_port_id(struct sfptpd_config_section *section, const char *option,
                                               unsigned int num_params,
                                               const char * const params[])
{
	int rc = 0;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "on") == 0)
		ptp->ptpd_port.delay_resp_ignore_port_id = TRUE;
	else if (strcmp(params[0], "off") == 0)
		ptp->ptpd_port.delay_resp_ignore_port_id = FALSE;
	else
		rc = EINVAL;

	return rc;
}

static int parse_pid_filter_kp(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens;
	long double kp;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%Lf", &kp);
	if (tokens != 1)
		return EINVAL;

	if ((kp < 0.0) || (kp > 1.0)) {
		ERROR("pid_filter_p %s outside valid range [0,1]\n", params[0]);
		return ERANGE;
	}

	ptp->ptpd_port.servoKP = kp;
	return 0;
}

static int parse_pid_filter_ki(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens;
	long double ki;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%Lf", &ki);
	if (tokens != 1)
		return EINVAL;

	if ((ki < 0.0) || (ki > 1.0)) {
		ERROR("pid_filter_i %s outside valid range [0,1]\n", params[0]);
		return ERANGE;
	}

	ptp->ptpd_port.servoKI = ki;
	return 0;
}

static int parse_outlier_filter_size(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens, outlier_filter_size;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%i", &outlier_filter_size);
	if (tokens != 1)
		return EINVAL;

	if ((outlier_filter_size < SFPTPD_PEIRCE_FILTER_SAMPLES_MIN) ||
		(outlier_filter_size > SFPTPD_PEIRCE_FILTER_SAMPLES_MAX)) {
		ERROR("PTP outlier filter size outside allowed range [%d,%d]\n",
			SFPTPD_PEIRCE_FILTER_SAMPLES_MIN, SFPTPD_PEIRCE_FILTER_SAMPLES_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.outlier_filter_size = (UInteger8)outlier_filter_size;
	return 0;
}

static int parse_outlier_filter_adaption(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens;
	long double outlier_filter_adaption;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%Lf", &outlier_filter_adaption);
	if (tokens != 1)
		return EINVAL;

	if ((outlier_filter_adaption < 0.0) ||
		(outlier_filter_adaption > 1.0)) {
		ERROR("PTP outlier filter adaption outside allowed range of [0,1]\n");
		return ERANGE;
	}

	ptp->ptpd_port.outlier_filter_adaption = (long double)outlier_filter_adaption;
	return 0;
}

static int parse_mpd_filter_size(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens, path_delay_filter_size;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%i", &path_delay_filter_size);
	if (tokens != 1)
		return EINVAL;

	if ((path_delay_filter_size < SFPTPD_SMALLEST_FILTER_SAMPLES_MIN) ||
		(path_delay_filter_size > SFPTPD_SMALLEST_FILTER_SAMPLES_MAX)) {
		ERROR("PTP mean path delay filter size outside allowed range [%d,%d]\n",
			SFPTPD_SMALLEST_FILTER_SAMPLES_MIN, SFPTPD_SMALLEST_FILTER_SAMPLES_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.path_delay_filter_size = path_delay_filter_size;
	return 0;
}

static int parse_mpd_filter_ageing(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens;
	long double path_delay_filter_ageing;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%Lf", &path_delay_filter_ageing);
	if (tokens != 1)
		return EINVAL;

	if (path_delay_filter_ageing < 0.0) {
		ERROR("PTP mean path delay ageing must be non-negative\n");
		return ERANGE;
	}

	ptp->ptpd_port.path_delay_filter_ageing = path_delay_filter_ageing;
	return 0;
}

static int parse_fir_filter_size(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	assert(section != NULL);
	int tokens, filter_size;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;

	tokens = sscanf(params[0], "%i", &filter_size);
	if (tokens != 1)
		return EINVAL;

	if ((filter_size < SFPTPD_FIR_FILTER_STIFFNESS_MIN) ||
	    (filter_size > SFPTPD_FIR_FILTER_STIFFNESS_MAX)) {
		ERROR("PTP FIR filter size outside allowed range [%d,%d]\n",
		      SFPTPD_FIR_FILTER_STIFFNESS_MIN, SFPTPD_FIR_FILTER_STIFFNESS_MAX);
		return ERANGE;
	}

	ptp->ptpd_port.fir_filter_size = filter_size;
	return 0;
}


static int parse_remote_monitor(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 0);

	ptp->remote_monitor = true;
	return 0;
}


static int parse_mon_monitor_address(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	int i, j;
	int gai_rc;
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	struct addrinfo hints = {
		.ai_family = ptp->ptpd_intf.transportAF,
		.ai_socktype = SOCK_DGRAM,
	};
	struct addrinfo *result;
	regex_t rquot;
	regex_t runquot;
	regmatch_t matches[4];
	char *spec = NULL;
	regoff_t node;
	regoff_t serv;

	assert(regcomp(&rquot, "^\\[(.*)](:([^:]*))?$", REG_EXTENDED) == 0);
	assert(regcomp(&runquot, "^([^:]*)(:([^:]*))?$", REG_EXTENDED) == 0);

	j = ptp->ptpd_port.num_monitor_dests;
	for (i = 0; rc == 0 && i < num_params; i++, j++) {
		if (j >= MAX_SLAVE_EVENT_DESTS) {
			ERROR("too many monitoring destinations specified (%d > %d)\n",
			      j + num_params - i, MAX_SLAVE_EVENT_DESTS);
			rc = E2BIG;
			continue;
		}

		spec = strdup(params[i]);
		rc = regexec(&rquot, spec, sizeof matches / sizeof *matches, matches, 0);
		if (rc != 0)
			rc = regexec(&runquot, params[i], sizeof matches / sizeof *matches, matches, 0);
		node = matches[1].rm_so;
		serv = matches[3].rm_so;
		if (rc != 0 || node == -1) {
			ERROR("invalid monitor address: %s\n", params[i]);
			rc = EINVAL;
			goto finish;
		}
		spec[matches[1].rm_eo] = '\0';
		if (serv != -1)
			spec[matches[3].rm_eo] = '\0';

		gai_rc = getaddrinfo(spec + node,
				     serv == -1 ? NULL : spec + serv,
				     &hints, &result);
		if (gai_rc != 0 || result == NULL) {
			ERROR("monitor address lookup for %s failed, %s\n",
			      params[i], gai_strerror(gai_rc));
			rc = EINVAL;
		} else {
			assert(result->ai_addrlen <= sizeof ptp->ptpd_port.monitor_address);
			memcpy(ptp->ptpd_port.monitor_address + j,
			       result->ai_addr, result->ai_addrlen);
			ptp->ptpd_port.monitor_address_len[j] = result->ai_addrlen;
		}
		if (gai_rc == 0)
			freeaddrinfo(result);
		free(spec);
		spec = NULL;
	}
	ptp->ptpd_port.num_monitor_dests = j;

finish:
	if (spec != NULL)
		free(spec);
	regfree(&rquot);
	regfree(&runquot);
	return rc;
}


static int parse_mon_rx_sync_timing_data(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens, logging_skip;

	if (num_params > 1)
		return EINVAL;

	ptp->ptpd_port.rx_sync_timing_data_config.logging_enable = TRUE;
	ptp->ptpd_port.rx_sync_timing_data_config.tlv_enable = TRUE;

	if (num_params == 1) {
		tokens = sscanf(params[0], "%i", &logging_skip);
		if (tokens != 1)
			return EINVAL;
		ptp->ptpd_port.rx_sync_timing_data_config.logging_skip = logging_skip;
	}

	return 0;
}


static int parse_mon_rx_sync_computed_data(struct sfptpd_config_section *section, const char *option,
					   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens, logging_skip;

	if (num_params > 1)
		return EINVAL;

	ptp->ptpd_port.rx_sync_computed_data_config.logging_enable = TRUE;
	ptp->ptpd_port.rx_sync_computed_data_config.tlv_enable = TRUE;

	if (num_params == 1) {
		tokens = sscanf(params[0], "%i", &logging_skip);
		if (tokens != 1)
			return EINVAL;
		ptp->ptpd_port.rx_sync_computed_data_config.logging_skip = logging_skip;
	}

	return 0;
}


static int parse_mon_tx_event_timestamps(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	int tokens, logging_skip;

	if (num_params > 1)
		return EINVAL;

	ptp->ptpd_port.tx_event_timestamps_config.logging_enable = TRUE;
	ptp->ptpd_port.tx_event_timestamps_config.tlv_enable = TRUE;

	if (num_params == 1) {
		tokens = sscanf(params[0], "%i", &logging_skip);
		if (tokens != 1)
			return EINVAL;
	       ptp->ptpd_port.tx_event_timestamps_config.logging_skip = logging_skip;
	}

	return 0;
}


static int parse_mon_slave_status(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 0);

	ptp->ptpd_port.slave_status_monitoring_enable = true;
	return 0;
}

static int parse_announce_comm_caps(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		ptp->ptpd_port.comm_caps_tlv_enabled = false;
	} else if (strcmp(params[0], "on") == 0) {
		ptp->ptpd_port.comm_caps_tlv_enabled = true;
	} else {
		return EINVAL;
	}

	if (ptp->ptpd_port.ptp_version_minor == 0 &&
	    ptp->ptpd_port.comm_caps_tlv_enabled) {
		ERROR("PTP version %d.%d does not support sending Communication "
		      "Capabilities TLV\n",
		      PTPD_PROTOCOL_VERSION,
		      ptp->ptpd_port.ptp_version_minor);
		return EINVAL;
	}

	return 0;
}

static int parse_onload_ext(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		ptp->ptpd_intf.use_onload_ext = false;
	} else if (strcmp(params[0], "on") == 0) {
		ptp->ptpd_intf.use_onload_ext = true;
#ifndef HAVE_ONLOAD_EXT
		WARNING("config: onload extensions requested but not compiled in\n");
#endif
	} else {
		return EINVAL;
	}

	return 0;
}

static int parse_bmc_discriminator(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_ptp_module_config_t *ptp = (sfptpd_ptp_module_config_t *)section;
	long double threshold;
	int tokens;

	assert(num_params == 2);

	if (strlen(params[0]) >= SFPTPD_CONFIG_SECTION_NAME_MAX) {
		CFG_ERROR(section, "instance name %s too long\n",
			  params[0]);
		return ERANGE;
	}
	strcpy(ptp->ptpd_port.discriminator_name, params[0]);

	tokens = sscanf(params[1], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	/* Convert from user-specified ms to ns for internal use */
	ptp->ptpd_port.discriminator_threshold = threshold * 1000000.0L;

	return 0;
}


static const sfptpd_config_option_t ptp_config_options[] =
{
	{"ptp_profile", "<default-e2e | default-p2p | enterprise>",
		"Specifes the PTP Profile. The default profile is the default-e2e or "
		"default-p2p depending on the delay measurement mode.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_ptp_profile},
	{"ptp_version", "<2.0 | 2.1>",
		"Specifies the PTP version, where 2.0 => IEEE1588-2008 and "
		"2.1 => IEEE1588-2019. The default version is 2.0.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_ptp_version},
	{"ptp_mode", "<slave | master | master-only | monitor>",
		"Specifies the PTP mode of operation. The default mode is slave",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mode},
	{"interface", "interface-name",
		"Specifies the name of the interface that PTP should use",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_interface},
	{"transport", "<ipv4 | ipv6>",
		"Specifies the transport for this instance. The default transport is ipv4",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_transport},
	{"scope", "<link-local | global>",
		"Specifies the scope for ipv6 the transport. The default scope is link-local",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_scope},
	{"priority", "<NUMBER>",
		"Relative priority of sync module instance. Smaller values have higher "
		"priority. The default is 128. N.B. This is the user priority for this "
		"sync instance within this daemon and is unrelated to the PTP 'priority1' "
		"and 'priority2' values. ",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_priority},
	{"sync_threshold", "<NUMBER>",
		"Threshold in nanoseconds of the offset from the clock source over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync (converged). The default is "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT)
		" with hardware timestamping and "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_SW_TS)
		" with software timestamping",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_threshold},
	{"timestamping", "<hw | sw | auto>",
		"Specify required timestamping type. The default is to use hardware "
		"timestamping if possible.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_timestamping},
	{"ptp_pkt_dump", "",
		"Dump each received PTP packet in detail",
		0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pkt_dump},
	{"ptp_pps_log", "",
		"Enable logging of PPS measurements",
		0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pps_log},
	{"ptp_tx_latency", "NUMBER",
		"Specifies the outbound latency in nanoseconds",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_tx_latency},
	{"ptp_rx_latency", "NUMBER",
		"Specifies the inbound latency in nanoseconds",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_rx_latency},
	{"ptp_delay_mechanism", "<end-to-end | peer-to-peer>",
		"Peer delay mode. The default mode is end-to-end",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_delay_mechanism},
	{"ptp_network_mode", "<multicast | hybrid | hybrid-no-fallback>",
		"Network mode. Multicast is always used for Sync messages. "
		"Hybrid mode allows delay requests/responses to be unicast but falls "
		"back to multicast mode. hybrid-no-fallback does not fall back. "
		"The default mode is hybrid.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_network_mode},
	{"ptp_ttl", "NUMBER",
		"The TTL value to use in transmitted multicast PTP packets. Default value 64.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_multicast_ttl},
	{"ptp_utc_offset", "NUMBER",
		"The current UTC offset in seconds. Only applicable to PTP master mode.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_utc_offset,
		.hidden = true},
	{"ptp_utc_valid_handling", "<default | ignore | prefer | require | override N>",
		"Controls how the UTC offset valid flag is used.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_utc_valid_handling},
	{"ptp_timescale", "<tai | utc>",
		"Control whether PTP advertises a TAI or UTC (Arbitrary) timescale. Only "
		"applicable to PTP master mode. Default is UTC.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timescale,
		.hidden = true},
	{"ptp_domain", "NUMBER",
		"Specifies the PTP domain. Default value 0.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_domain},
	{"ptp_mgmt_msgs", "<disabled | read-only>",
		"Configures PTP Management Message support. Disabled by default.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_ptp_mgmt_msgs},
	{"ptp_timing_acl_allow", "<ip-address-list>",
		"Access control allow list for timing packets. The format is a series of "
		"network prefixes in a.b.c.d/x notation where a.b.c.d is the subnet and "
		"x is the mask. For single IP addresses, 32 should be specified for the mask.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timing_acl_allow},
	{"ptp_timing_acl_permit", "<ip-address-list>",
		"Deprecated alias for ptp_timing_acl_allow.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timing_acl_allow,
		.hidden = true},
	{"ptp_timing_acl_deny", "<ip-address-list>",
		"Access control deny list for timing packets.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timing_acl_deny},
	{"ptp_timing_acl_order", "<allow-deny | deny-allow>",
		"Access control list evaluation order for timing packets. Default allow-deny.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timing_acl_order},
	{"ptp_mgmt_acl_allow", "<ip-address-list>",
		"Access control allow list for management packets.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mgmt_acl_allow},
	{"ptp_mgmt_acl_permit", "<ip-address-list>",
		"Deprecated alias for ptp_mgmt_acl_allow.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_timing_acl_allow,
		.hidden = true},
	{"ptp_mgmt_acl_deny", "<ip-address-list>",
		"Access control deny list for management packets.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mgmt_acl_deny},
	{"ptp_mgmt_acl_order", "<allow-deny | deny-allow>",
		"Access control list evaluation order for management packets. Default allow-deny.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mgmt_acl_order},
	{"ptp_mon_acl_allow", "<ip-address-list>",
		"Access control allow list for monitoring protocols.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mon_acl_allow},
	{"ptp_mon_acl_deny", "<ip-address-list>",
		"Access control deny list for monitoring protocols.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mon_acl_deny},
	{"ptp_mon_acl_order", "<allow-deny | deny-allow>",
		"Access control list evaluation order for monitoring protocols. Default allow-deny. "
		"This ACL controls the availability of a non-standard monitoring extension. ",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_mon_acl_order},
	{"ptp_announce_interval", "NUMBER",
		"The PTP Announce packet interval in 2^NUMBER seconds where NUMBER "
		"is in the range [" STRINGIFY(PTPD_ANNOUNCE_INTERVAL_MIN)
		"," STRINGIFY(PTPD_ANNOUNCE_INTERVAL_MAX) "]. Default value 1.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_announce_pkt_interval},
	{"ptp_announce_timeout", "NUMBER",
		"The PTP Announce packet receipt timeout as a number of Announce "
		"packet intervals. Default value 6.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_announce_pkt_timeout},
	{"ptp_sync_pkt_interval", "NUMBER",
		"The PTP Sync packet interval in 2^NUMBER seconds where NUMBER "
		"is in the range [" STRINGIFY(PTPD_SYNC_INTERVAL_MIN)
		"," STRINGIFY(PTPD_SYNC_INTERVAL_MAX) "]. Default value 0.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_pkt_interval},
	{"ptp_sync_pkt_timeout", "NUMBER",
		"The PTP Sync packet receipt timeout as a number of Sync packet intervals. "
		"Default value 6.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_pkt_timeout},
	{"ptp_delayreq_interval", "NUMBER",
		"The PTP Delay Request / Peer Delay Request packet interval in "
		"2^NUMBER seconds where number is in the range ["
		STRINGIFY(PTPD_DELAY_REQ_INTERVAL_MIN) ","
		STRINGIFY(PTPD_DELAY_REQ_INTERVAL_MAX) "]. If specified, "
		"overrides the value communicated to the slave from the master.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_delayreq_pkt_interval},
	{"ptp_delayresp_timeout", "NUMBER",
		"The PTP Delay Response receipt timeout in 2^NUMBER seconds. Default value -2.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_delayresp_pkt_timeout},
	{"max_missing_delayresps", "A B",
		"The maximimum number of missing delay responses to alarm (A) "
		"or fall back from hybrid mode (B). Default "
		STRINGIFY(DEFAULT_DELAY_RESP_ALARM_THRESHOLD) " "
		STRINGIFY(DEFAULT_DELAY_RESP_HYBRID_THRESHOLD) ".",
		2, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_max_missing_delayresps},
	{"ptp_max_foreign_records", "NUMBER",
		"The maximum number of PTP foreign master records.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_max_foreign_records},
	{"ptp_bmc_priority1", "NUMBER",
		"PTP master mode- BMC priority 1.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_bmc_priority1},
	{"ptp_bmc_priority2", "NUMBER",
		"PTP master mode- BMC priority 2.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_bmc_priority2},
	{"ptp_trace", "NUMBER",
		"PTP trace level. 0 corresponds to off, 3 corresponds to maximum verbosity.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_trace_level},
	{"ptp_delay_resp_ignore_port_id", "<off | on>",
		"Off by default.  When set to 'on' the clock ID and port "
		"number in delay responses are not validated.  This can be "
		"used as a work-around to interoperate with certain boundary "
		"clocks that do not support link aggregation properly.  If you "
		"are not using link aggregation together with boundary clock "
		"then you are unlikely to need to enable this option.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_ptp_delay_resp_ignore_port_id},
	{"pid_filter_p", "NUMBER",
		"PID filter proportional term coefficient. Default value is "
		STRINGIFY(PTPD_DEFAULT_KP) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_kp},
	{"pid_filter_i", "NUMBER",
		"PID filter integral term coefficient. Default value is "
		STRINGIFY(PTPD_DEFAULT_KI) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_ki},
	{"outlier_filter_size", "NUMBER",
		"Number of data samples stored in the offset from master filter. "
		"The valid range is [" STRINGIFY(SFPTPD_PEIRCE_FILTER_SAMPLES_MIN) ","
		STRINGIFY(SFPTPD_PEIRCE_FILTER_SAMPLES_MAX) "] and the default is "
		STRINGIFY(DEFAULT_OUTLIER_FILTER_SIZE) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_outlier_filter_size},
	{"outlier_filter_adaption", "NUMBER",
		"Controls how outliers are fed into the offset from master filter. "
		"A value of 0 means that outliers are not fed into filter (not "
		"recommended) whereas a value of 1 means that each outlier is fed "
		"into the filter unchanged. Values between result in a portion of "
		"the value being fed in. Default is "
		STRINGIFY(DEFAULT_OUTLIER_FILTER_ADAPTION) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_outlier_filter_adaption},
	{"mpd_filter_size", "NUMBER",
		"Number of data samples stored in the mean path delay filter. The "
		"valid range is [" STRINGIFY(SFPTPD_SMALLEST_FILTER_SAMPLES_MIN) ","
		STRINGIFY(SFPTPD_SMALLEST_FILTER_SAMPLES_MAX) "]. A value of 1 "
		"means that the filter is off while higher values will reduce the "
		"adaptability of PTP but increase its stability. Default is "
		STRINGIFY(DEFAULT_MPD_FILTER_SIZE) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mpd_filter_size},
	{"mpd_filter_ageing", "NUMBER",
		"Controls ageing of samples in the mean path delay filter. The "
		"ageing is expressed in units of nanoseconds per second. The "
		"default is " STRINGIFY(DEFAULT_MPD_FILTER_AGEING) " ns/s.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mpd_filter_ageing},
	{"fir_filter_size", "NUMBER",
		"Number of data samples stored in the FIR filter. The "
		"valid range is [" STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MIN)
		"," STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MAX) "]. A value of "
		"1 means that the filter is off while higher values will "
		"reduce the adaptability of PTP but increase its stability. "
		"Default is " STRINGIFY(DEFAULT_FIR_FILTER_SIZE) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_fir_filter_size},
	{"remote_monitor", "",
		"Enable the remote monitor. Collects Slave Event Monitoring "
		"messages. DEPRECATED since v3.7.0.",
		0, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_remote_monitor},
	{"mon_monitor_address", "ADDRESS[:PORT]*",
		"Address of up to " STRINGIFY(MAX_SLAVE_EVENT_DESTS) " "
		"monitoring stations to which to send unicast signaling "
		"messages with event monitoring data. "
		"Default is multicast to the standard PTP address.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_monitor_address},
	{"mon_rx_sync_timing_data", "[NUMBER]",
		"Enable slave event monitoring for rx sync timing data. "
		"Skips sampling every given number of events. If not specified is 0.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_rx_sync_timing_data},
	{"mon_rx_sync_computed_data", "[NUMBER]",
		"Enable slave event monitoring for rx sync computed data. "
		"Skips sampling every given number of events. If not specified is 0.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_rx_sync_computed_data},
	{"mon_tx_event_timestamps", "[NUMBER]",
		"Enable slave event monitoring for tx event timestamps. "
		"Skips sampling every given number of events. If not specified is 0.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_tx_event_timestamps},
	{"mon_meinberg_netsync", "",
		"Enable the Meinberg NetSync Monitor protocol. Packets must also pass both the "
		"monitoring and timing ACLs.",
		0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_meinberg_netsync},
	{"mon_slave_status", "",
		"Enable slave status monitoring.",
		0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_mon_slave_status},
	{"bmc_discriminator", "<CLOCK> <THRESHOLD>",
		"Disqualify foreign masters that differ from discriminator CLOCK "
		"in excess of THRESHOLD ms and the assumed PTP accuracy. CLOCK "
		"can be a sync instance name or clock name",
		2, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_bmc_discriminator},
	{"announce_comm_caps", "<off | on>",
		"Specify whether to append port communications capabilities to Announce messages. Disabled by default",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_announce_comm_caps},
	{"onload_ext", "<off | on>",
		"Specify whether to use Onload extensions API if avaialable. "
		"Disabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_onload_ext,
		.hidden = true},
};


static const sfptpd_config_option_set_t ptp_config_option_set =
{
	.description = "PTP Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_PTP,
	.num_options = sizeof(ptp_config_options)/sizeof(ptp_config_options[0]),
	.options = ptp_config_options
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void ptp_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_PTP);
	free(section);
}


static struct sfptpd_config_section *ptp_config_create(const char *name,
						       enum sfptpd_config_scope scope,
						       bool allows_instances,
						       const struct sfptpd_config_section *src)
{
	struct sfptpd_ptp_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_PTP));

	new = (struct sfptpd_ptp_module_config *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("ptp %s: failed to allocate memory for PTP configuration\n", name);
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
		new->ptpd_port.name = new->hdr.name;
	} else {
		/* Set the default PTPD options. */
		ptpd_config_port_initialise(&new->ptpd_port, new->hdr.name);
		ptpd_config_intf_initialise(&new->ptpd_intf);

		/* Set default values configuration */
		new->interface_name[0] = '\0';
		new->priority = SFPTPD_DEFAULT_PRIORITY;
		new->convergence_threshold = 0.0;
		new->uuid_filtering = true;
		new->domain_filtering = true;
		new->pps_logging = false;
		new->trace_level = 0;
		new->profile = SFPTPD_PTP_PROFILE_UNDEF;
	}

	sfptpd_config_section_init(&new->hdr, ptp_config_create,
				   ptp_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_PTP,
				   scope, allows_instances, name);

	return &new->hdr;
}


int sfptpd_ptp_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_ptp_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_ptp_module_config *)
		ptp_config_create(SFPTPD_PTP_MODULE_NAME,
				  SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	sfptpd_config_section_add(config, &new->hdr);

	/* Register the configuration options */
	sfptpd_config_register_options(&ptp_config_option_set);

	return 0;
}


struct sfptpd_ptp_module_config *sfptpd_ptp_module_get_config(struct sfptpd_config *config)
{
	assert(config != NULL);
	return (struct sfptpd_ptp_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_PTP);
}


void sfptpd_ptp_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name)
{
	struct sfptpd_ptp_module_config *ptp;
	assert(interface_name != NULL);

	ptp = sfptpd_ptp_module_get_config(config);
	assert(ptp != NULL);

	sfptpd_strncpy(ptp->interface_name, interface_name, sizeof(ptp->interface_name));
}


void sfptpd_ptp_module_set_default_domain(struct sfptpd_config *config,
					  int domain)
{
	struct sfptpd_ptp_module_config *ptp;

	ptp = sfptpd_ptp_module_get_config(config);
	assert(ptp != NULL);

	if (domain < 0 || domain > UINT8_MAX) {
		ERROR("ptp: ignoring default domain outside allowed range [%d,%d]\n",
		        0, UINT8_MAX);
		return;
	}

	ptp->ptpd_port.domainNumber = (UInteger8) domain;
}

/* fin */
