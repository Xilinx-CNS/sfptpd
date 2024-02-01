/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2020 Xilinx, Inc. */

/**
 * @file   ptpd_lib.c
 *
 * Public API for library form of PTPD.
 */

#include <stdbool.h>
#include <assert.h>

#include <sfptpd_misc.h>
#include <sfptpd_constants.h>
#include "ptpd_lib.h"
#include "ptpd.h"
#include "sfptpd_statistics.h"


/* Default configuration */
void ptpd_config_port_initialise(struct ptpd_port_config *config,
				 const char *name)
{
	config->name = name;

	/* initialize run-time options to default values */
	config->announceInterval = DEFAULT_ANNOUNCE_INTERVAL;
	config->syncInterval = DEFAULT_SYNC_INTERVAL;
	config->minDelayReqInterval = DEFAULT_DELAYREQ_INTERVAL;
	config->minPdelayReqInterval = DEFAULT_PDELAYREQ_INTERVAL;
	config->ignore_delayreq_interval_master = FALSE;

	config->announceReceiptTimeout = DEFAULT_ANNOUNCE_RECEIPT_TIMEOUT;
	config->syncReceiptTimeout = DEFAULT_SYNC_RECEIPT_TIMEOUT;
	config->delayRespReceiptTimeout = DEFAULT_DELAY_RESP_RECEIPT_TIMEOUT;

	config->delayRespAlarmThreshold = DEFAULT_DELAY_RESP_ALARM_THRESHOLD;
	config->delayRespHybridThreshold = DEFAULT_DELAY_RESP_HYBRID_THRESHOLD;

	config->path_delay_filter_size = DEFAULT_MPD_FILTER_SIZE;
	config->path_delay_filter_ageing = DEFAULT_MPD_FILTER_AGEING;
	config->outlier_filter_size = DEFAULT_OUTLIER_FILTER_SIZE;
	config->outlier_filter_adaption = DEFAULT_OUTLIER_FILTER_ADAPTION;
	config->fir_filter_size = DEFAULT_FIR_FILTER_SIZE;

	config->clockQuality.clockAccuracy = DEFAULT_CLOCK_ACCURACY;
	config->clockQuality.clockClass = DEFAULT_CLOCK_CLASS;
	config->clockQuality.offsetScaledLogVariance = DEFAULT_CLOCK_VARIANCE;
	config->priority1 = DEFAULT_PRIORITY1;
	config->priority2 = DEFAULT_PRIORITY2;

	config->comm_caps.syncCapabilities = PTPD_COMM_MULTICAST_CAPABLE;
	config->comm_caps.delayRespCapabilities = PTPD_COMM_MULTICAST_CAPABLE | PTPD_COMM_UNICAST_CAPABLE;
	config->comm_caps_tlv_enabled = TRUE;
	config->node_type = PTPD_NODE_CLOCK;

	config->domainNumber = 0;
	config->stepsRemoved = 0;

	config->timeProperties.currentUtcOffset = 0;
	config->timeProperties.currentUtcOffsetValid = FALSE;
	config->timeProperties.timeTraceable = FALSE;
	config->timeProperties.frequencyTraceable = FALSE;
	config->timeProperties.ptpTimescale = FALSE;
	config->timeProperties.timeSource = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;

	config->unicastAddress[0] = '\0';

	config->clock_ctrl = SFPTPD_CLOCK_CTRL_SLEW_AND_STEP;

	config->step_threshold = SFPTPD_DEFAULT_STEP_THRESHOLD_NS;

	config->maxReset = 0;

	config->servoKP = PTPD_DEFAULT_KP;
	config->servoKI = PTPD_DEFAULT_KI;
	config->servoKD = PTPD_DEFAULT_KD;
	config->inboundLatency.sec = 0;
	config->inboundLatency.nsec = 0;
	config->inboundLatency.nsec_frac = 0;
	config->outboundLatency.sec = 0;
	config->outboundLatency.nsec = 0;
	config->outboundLatency.nsec_frac = 0;
	config->max_foreign_records = DEFAULT_MAX_FOREIGN_RECORDS;
	config->delayMechanism = PTPD_DELAY_MECHANISM_E2E;

	config->alwaysRespectUtcOffset = FALSE;
	config->preferUtcValid = FALSE;
	config->requireUtcValid = FALSE;
	config->overrideUtcOffset = FALSE;
	config->overrideUtcOffsetSeconds = 0;

	config->missingInterfaceTolerance = FALSE;

	config->managementEnabled = FALSE;
	config->managementSetEnable = FALSE;

	config->monMeinbergNetSync = FALSE;

	config->delay_resp_ignore_port_id = FALSE;

	config->slaveOnly = TRUE;
	config->masterOnly = FALSE;

	/* Set some reasonable defaults for the slave event monitoring
	   mechanism. */
	config->rx_sync_timing_data_config.logging_enable = FALSE;
	config->rx_sync_timing_data_config.tlv_enable = FALSE;

	/* Number of events to skip between samples for instantaneous reporting */
	config->rx_sync_timing_data_config.logging_skip = 0;

	/* Number of events to include per TLV */
	config->rx_sync_timing_data_config.events_per_tlv = 8;

	/* Default to multicast transmission */
	config->rx_sync_computed_data_config = config->rx_sync_timing_data_config;
	config->tx_event_timestamps_config = config->rx_sync_timing_data_config;

	/* Slave status monitoring. Empty first address implies use PTP multicast */
	config->slave_status_monitoring_enable = false;
	config->num_monitor_dests = 0;
	config->monitor_address_len[0] = 0;

	/* Clear remote stats logger */
	memset(&config->remoteStatsLogger, '\0', sizeof config->remoteStatsLogger);

	/* BMC discriminator */
	config->discriminator_name[0] = '\0';
	config->discriminator_threshold = 0;

	config->test.bad_timestamp.type = BAD_TIMESTAMP_TYPE_OFF;
	config->test.xparent_clock.enable = FALSE;
	config->test.no_sync_pkts = FALSE;
	config->test.no_follow_ups = FALSE;
	config->test.no_delay_resps = FALSE;

	/* PTP version */
	config->ptp_version_minor = PTPD_PROTOCOL_VERSION_MINOR_DEFAULT;

	/* Timestamping preference */
	config->timestamp_pref = PTPD_TIMESTAMP_TYPE_AUTO;
}


/* Default configuration */
void ptpd_config_intf_initialise(struct ptpd_intf_config *config)
{
	config->ifaceName[0] = '\0';
	config->snmp_enabled = FALSE;
	config->timestampType = PTPD_TIMESTAMP_TYPE_HW;
	config->dscpValue = 0;
	config->ttl = PTPD_DEFAULT_TTL;
	config->masterRefreshIgmp = FALSE;
	config->masterIgmpRefreshInterval = 0;

	config->timingAclEnabled = FALSE;
	config->managementAclEnabled = FALSE;
	config->monitoringAclEnabled = FALSE;
	config->timingAclAllowText[0] = '\0';
	config->timingAclDenyText[0] = '\0';
	config->managementAclAllowText[0] = '\0';
	config->managementAclDenyText[0] = '\0';
	config->monitoringAclAllowText[0] = '\0';
	config->monitoringAclDenyText[0] = '\0';
	config->timingAclOrder = PTPD_ACL_ALLOW_DENY;
	config->managementAclOrder = PTPD_ACL_ALLOW_DENY;
	config->monitoringAclOrder = PTPD_ACL_ALLOW_DENY;

	config->displayPackets = FALSE;
	config->transportAF = AF_INET;
	config->linkLocalScope = TRUE;
	config->use_onload_ext = FALSE;

	sfptpd_strncpy(config->user_description, SFPTPD_USER_DESCRIPTION,
		       sizeof(config->user_description));
}

int ptpd_init(struct ptpd_global_context **ptpd_global) {
	struct ptpd_global_context *new;

	if (ptpd_global == NULL) {
		CRITICAL("null context pointer (%p) supplied\n",
			 ptpd_global);
		return EINVAL;
	}

	*ptpd_global = NULL;

	new = (struct ptpd_global_context *)calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("failed to allocate PTP module memory\n");
		return ENOMEM;
	}

	doInitGlobal();

	*ptpd_global = new;
	return 0;
}


int ptpd_create_interface(struct ptpd_intf_config *config, struct ptpd_global_context *global, struct ptpd_intf_context **ptpd_if)
{
	struct ptpd_intf_context *new;
	struct ptpd_intf_config *ifOpts;

	if ((config == NULL) || (ptpd_if == NULL)) {
		CRITICAL("null ptpd if config (%p) or context pointer (%p) supplied\n",
			 config, ptpd_if);
		return EINVAL;
	}

	*ptpd_if = NULL;

	new = (struct ptpd_intf_context *)calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("failed to allocate PTP module memory\n");
		return ENOMEM;
	}

	new->global = global;
	ifOpts = &new->ifOpts;

	/* Initialise non-zero reset values */
	new->transport.eventSock = -1;
	new->transport.generalSock = -1;
	new->transport.monitoringSock = -1;

	/* Insert into linked list */
	new->next = global->interfaces;
	global->interfaces = new;

	/* Copy the configuration into the static runtime options and set the global
	 * pointer to the PTP data */
	memcpy(ifOpts, config, sizeof *ifOpts);

	/* Create set for ptp-nodes logging */
	new->nodeSet = sfptpd_stats_create_set();

	/* Create the error queue message buffer */
	new->msgEbuf.msg_iovlen = 1;
	new->msgEbuf.msg_iov = calloc(new->msgEbuf.msg_iovlen, sizeof(struct iovec));
	if (new->msgEbuf.msg_iov == NULL)
		goto fail_msg_iov;
	new->msgEbuf.msg_iov[0].iov_base = calloc(1, PACKET_SIZE);
	new->msgEbuf.msg_iov[0].iov_len = PACKET_SIZE;
	if (new->msgEbuf.msg_iov[0].iov_base == NULL)
		goto fail_msg_pkt;
	new->msgEbuf.msg_control = calloc(1, CONTROL_MSG_SIZE);
	if (new->msgEbuf.msg_control == NULL)
		goto fail_msg_control;

	/* Put PTPD into the initializing state and carry out the initialisation.
	 * If this fails, then return with an error. */
	if (!doInitInterface(ifOpts, new)) {
		CRITICAL("failed to initialize PTP module\n");
		free(new);
		return EIO;
	}

	*ptpd_if = new;
	return 0;

fail_msg_control:
	free(new->msgEbuf.msg_iov->iov_base);
fail_msg_pkt:
	free(new->msgEbuf.msg_iov);
fail_msg_iov:
	free(new);
	CRITICAL("failed to allocate error queue message buffer\n");
	return ENOMEM;
}


int ptpd_create_port(struct ptpd_port_config *config, struct ptpd_intf_context *ifcontext, struct ptpd_port_context **ptpd)
{
	struct ptpd_port_context *new;
	struct ptpd_port_config *rtOpts;
	int rc;

	if ((config == NULL) || (ptpd == NULL)) {
		CRITICAL("null ptpd config (%p) or context pointer (%p) supplied\n",
			 config, ptpd);
		return EINVAL;
	}

	*ptpd = NULL;

	new = (struct ptpd_port_context *)calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("failed to allocate PTP module memory\n");
		return ENOMEM;
	}

	new->physIface = config->physIface;
	new->interface = ifcontext;
	rtOpts = &new->rtOpts;

	/* Insert into linked list */
	new->next = ifcontext->ports;
	ifcontext->ports = new;

	/* Number the port */
	new->portIdentity.portNumber = ++ifcontext->global->ports_created;

	/* Allocate the foreign master database according to the configuration */
	rc = initForeignMasterDS(&new->foreign, config->max_foreign_records);
	if (rc != 0) {
		CRITICAL("failed to initialise foreign master data set\n");
		free(new);
		return rc;
	}

	/* Copy the configuration into the static runtime options and set the global
	 * pointer to the PTP data */
	memcpy(rtOpts, config, sizeof *rtOpts);
	rtOpts->ifOpts = &ifcontext->ifOpts;

	/* Put PTPD into the initializing state and carry out the initialisation.
	 * If this fails, then return with an error. */
	new->portState = PTPD_INITIALIZING;
	if (!doInitPort(rtOpts, new)) {
		CRITICAL("failed to initialize PTP module\n");
		servo_shutdown(&new->servo);
		freeForeignMasterDS(&new->foreign);
		free(new);
		return EIO;
	}

	*ptpd = new;
	return 0;
}

void ptpd_port_destroy(struct ptpd_port_context *ptpd_port)
{
	struct ptpd_port_context **ptr;

	assert(ptpd_port);

	/* Remove from list */
	for (ptr = &ptpd_port->interface->ports;
	     ptr && *ptr != ptpd_port;
	     ptr = &(*ptr)->next);

	assert(ptr);
	*ptr = ptpd_port->next;

	/* Shutdown port-specific components */
	managementShutdown(ptpd_port);
	if (ptpd_port->rtOpts.node_type == PTPD_NODE_CLOCK) {
		servo_shutdown(&ptpd_port->servo);
	}

	/* Destroy contents */
	freeForeignMasterDS(&ptpd_port->foreign);
	free(ptpd_port);
}

void ptpd_interface_destroy(struct ptpd_intf_context *ptpd_if)
{
	struct ptpd_port_context *port, *next;
	struct ptpd_intf_context **ptr;

	assert(ptpd_if);

	/* Destroy error queue message buffer */
	if (ptpd_if->msgEbuf.msg_iov)
		free(ptpd_if->msgEbuf.msg_iov[0].iov_base);
	free(ptpd_if->msgEbuf.msg_iov);
	free(ptpd_if->msgEbuf.msg_control);

	/* Destroy ports */
	for (port = ptpd_if->ports; port; port = next) {
		next = port->next;
		ptpd_port_destroy(port);
	}

	/* Remove from list */
	for (ptr = &ptpd_if->global->interfaces;
	     ptr && *ptr != ptpd_if;
	     ptr = &(*ptr)->next);

	assert(ptr);
	*ptr = ptpd_if->next;

	/* Stop and destroy PTP module */
	netShutdown(&ptpd_if->transport);

	sfptpd_ht_free(ptpd_if->nodeSet);

	/* free management messages, they can have dynamic memory allocated */
	if (ptpd_if->msgTmpHeader.messageType == PTPD_MSG_MANAGEMENT)
		freeManagementTLV(&ptpd_if->msgTmp.manage);
	freeManagementTLV(&ptpd_if->outgoingManageTmp);

	free(ptpd_if);
}

void ptpd_destroy(struct ptpd_global_context *ptpd)
{
	struct ptpd_intf_context *interface;

	/* Destroy interfaces */
	for (interface = ptpd->interfaces; interface; interface = interface->next) {
		ptpd_interface_destroy(interface);
	}

	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return;
	}

	free(ptpd);
}


void ptpd_timer_tick(struct ptpd_port_context *ptpd,
		     sfptpd_sync_module_ctrl_flags_t ctrl_flags)
{
	struct ptpd_port_config *rtOpts = &ptpd->rtOpts;

	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return;
	}

	if (ptpd->portState == PTPD_INITIALIZING) {
		/* Restart interface if there is only one port on this
		   interface otherwise log a warning */

		if (ptpd == ptpd->interface->ports &&
		    ptpd->next == NULL) {
			doInitInterface(&ptpd->interface->ifOpts, ptpd->interface);
		} else {
			WARNING("need to restart interface but cannot because multiple ports use it\n");
		}

		doInitPort(rtOpts, ptpd);
		ptpd_control(ptpd, ctrl_flags);
	} else {
		doTimerTick(rtOpts, ptpd);
	}
}


void ptpd_sockets_ready(struct ptpd_intf_context *ptpd_if, bool event,
			bool general, bool error)
{
	struct ptpd_intf_config *ifOpts = &ptpd_if->ifOpts;

	if (ptpd_if == NULL) {
		ERROR("null ptpd if context supplied\n");
		return;
	}

	doHandleSockets(ifOpts, ptpd_if, event, general, error);
}


void ptpd_control(struct ptpd_port_context *ptpd,
		  sfptpd_sync_module_ctrl_flags_t ctrl_flags)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return;
	}

	if (ctrl_flags & SYNC_MODULE_LEAP_SECOND_GUARD)
		ptpd->leapSecondInProgress = TRUE;

	if (((ctrl_flags & SYNC_MODULE_LEAP_SECOND_GUARD) == 0) &&
	    ptpd->leapSecondInProgress)
		ptpd->leapSecondWaitingForAnnounce = TRUE;

	servo_control(&ptpd->servo, ctrl_flags);
}


void ptpd_update_gm_info(struct ptpd_port_context *ptpd,
			 bool remote_grandmaster,
			 uint8_t clock_id[8],
			 uint8_t clock_class,
			 ptpd_time_source_e time_source,
			 ptpd_clock_accuracy_e clock_accuracy,
			 unsigned int offset_scaled_log_variance,
			 unsigned int steps_removed,
			 bool time_traceable,
			 bool freq_traceable)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return;
	}

	/* The clock quality variables are only used in master mode. In addition
	 * they are only copied across from rtOpts during initialisation, not
	 * during the transition to master state. To be safe, update both the
	 * rtOpts and live values.
	 * The grandmasterClockQuality is only updated during the transition to
	 * master state, so if we are in master state, we need to update these. */
	ptpd->rtOpts.clockQuality.clockClass = clock_class;
	ptpd->rtOpts.clockQuality.clockAccuracy = clock_accuracy;
	ptpd->rtOpts.clockQuality.offsetScaledLogVariance = offset_scaled_log_variance;
	ptpd->clockQuality = ptpd->rtOpts.clockQuality;
	if (ptpd->portState == PTPD_MASTER) {
		ptpd->grandmasterClockQuality = ptpd->clockQuality;
	}

	/* The time source and steps removed variables are used to store the
	 * remote master values in slave and passive states. When switching to
	 * master mode, they are copied from rtOpts into the live values. We
	 * update the rtOpts values in all states and also update the live
	 * values if in the master state. */
	ptpd->rtOpts.timeProperties.timeSource = time_source;
	ptpd->rtOpts.timeProperties.timeTraceable = time_traceable;
	ptpd->rtOpts.timeProperties.frequencyTraceable = freq_traceable;
	ptpd->rtOpts.stepsRemoved = steps_removed;
	if (ptpd->portState == PTPD_MASTER) {
		ptpd->timePropertiesDS.timeSource = time_source;
		ptpd->timePropertiesDS.timeTraceable = time_traceable;
		ptpd->timePropertiesDS.frequencyTraceable = freq_traceable;
		ptpd->stepsRemoved = steps_removed;
	}

	/* If a grandmaster clock ID has been specified and this is a remote
	 * grandmaster then update the grandmaster ID to reflect this. Otherwise
	 * we will use our clock ID as the grandmaster ID. */
	if (remote_grandmaster &&
	    (memcmp(clock_id, &SFPTPD_CLOCK_ID_UNINITIALISED, SFPTPD_CLOCK_HW_ID_SIZE) != 0)) {
		memcpy(ptpd->boundaryGrandmasterIdentity, clock_id,
		       sizeof ptpd->boundaryGrandmasterIdentity);
		ptpd->boundaryGrandmasterDefined = TRUE;
	} else {
		memset(ptpd->boundaryGrandmasterIdentity, 0,
		       sizeof ptpd->boundaryGrandmasterIdentity);
		ptpd->boundaryGrandmasterDefined = FALSE;
	}

	if (ptpd->portState == PTPD_MASTER) {
		copyClockIdentity(ptpd->grandmasterIdentity,
				  (ptpd->boundaryGrandmasterDefined ?
				   ptpd->boundaryGrandmasterIdentity :
				   ptpd->clockIdentity));
	}
}


void ptpd_update_leap_second(struct ptpd_port_context *ptpd,
			     bool leap59, bool leap61)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return;
	}

	/* We only update the leap second state when in master state. In
	 * other states, e.g. slave, the leap second information comes from
	 * the remote grandmaster. */
	if (ptpd->portState == PTPD_MASTER) {
		ptpd->timePropertiesDS.leap59 = leap59;
		ptpd->timePropertiesDS.leap61 = leap61;
	}
}


void ptpd_step_clock(struct ptpd_port_context *ptpd, struct sfptpd_timespec *offset)
{
	if ((ptpd == NULL) || (offset == NULL)) {
		ERROR("null ptpd context (%p) or time structure (%p) supplied\n",
		      ptpd, offset);
		return;
	}

	servo_step_clock(&ptpd->servo, offset);
}


void ptpd_pid_adjust(struct ptpd_port_context *ptpd, double kp, double ki, double kd, bool reset)
{
	ptpd->rtOpts.servoKP = kp;
	ptpd->rtOpts.servoKI = ki;
	ptpd->rtOpts.servoKD = kd;
	servo_pid_adjust(&ptpd->rtOpts, &ptpd->servo, reset);
}


ptpd_timestamp_type_e ptpd_get_timestamping(struct ptpd_intf_context *ptpd_if)
{
	assert(ptpd_if != NULL);

	return ptpd_if->ifOpts.timestampType;
}


int ptpd_change_interface(struct ptpd_port_context *ptpd_port, Octet *logical_iface_name,
			  struct sfptpd_interface *physical_iface,
			  ptpd_timestamp_type_e timestamp_type)
{
	bool new_time_mode = false;
	bool new_lrc = false;
	struct ptpd_intf_context *ptpd_if;
	struct ptpd_intf_config *ifOpts;
	int rc = 0;

	if ((ptpd_port == NULL) || (logical_iface_name == NULL)) {
		ERROR("null ptpd port context (%p) or logical (%p) interface supplied\n",
		      ptpd_port, logical_iface_name, physical_iface);
		return EINVAL;
	}
	
	ptpd_if = ptpd_port->interface;
	ifOpts = &ptpd_if->ifOpts;

	netShutdown(&ptpd_if->transport);

	/* If the timestamp type is changing time mode between software and
	 * hardware timestamping clear the one-way-delay and offset-from-master
	 * filters as the values will be wrong by an order of magnitude */
	if (ifOpts->timestampType != timestamp_type)
		new_time_mode = true;

	new_lrc = (ptpd_port->physIface != physical_iface);

	sfptpd_strncpy(ifOpts->ifaceName, logical_iface_name, sizeof(ifOpts->ifaceName));
	ptpd_port->physIface = physical_iface;
	ptpd_port->clock = sfptpd_interface_get_clock(ptpd_port->physIface);
	ifOpts->physIface = physical_iface;
	ifOpts->timestampType = timestamp_type;

	/* Initialize networking */
	if(ptpd_port->physIface == NULL) {
		NOTICE("no physical interface for logical interface %s\n",
		       logical_iface_name);
		rc = ENOENT;
	} else if (!netInit(&ptpd_if->transport, ifOpts, ptpd_if)) {
		rc = EIO;
	}

	/* TODO: Do the servos for ports not 'active' need to be touched? */

	/* In all cases, if the ptp clock changes we need to update the servo. */
	if (new_lrc) {
		servo_set_slave_clock(&ptpd_port->servo, ptpd_port->clock);
	}

	/* If the time mode is changing, reset the servo */
	if (new_time_mode) {
		servo_reset(&ptpd_port->servo);
	}

	if (rc != 0) {
		ERROR("failed to initialize network\n");
		toStateAllPorts(PTPD_FAULTY, ptpd_if);
	}

	return rc;
}


int ptpd_get_snapshot(struct ptpd_port_context *ptpd, struct ptpd_port_snapshot *snapshot)
{
	if ((ptpd == NULL) || (snapshot == NULL)) {
		ERROR("null ptpd context (%p) or snapshot structure (%p) supplied\n",
		      ptpd, snapshot);
		return EINVAL;
	}

	memset(snapshot, 0, sizeof (*snapshot));
	snapshot->port.state = ptpd->portState;
	snapshot->port.alarms = ptpd->portAlarms | servo_get_alarms(&ptpd->servo);
	snapshot->port.delay_mechanism = ptpd->delayMechanism;
	snapshot->port.announce_interval = powl(2, ptpd->logAnnounceInterval);
	snapshot->port.domain_number = ptpd->domainNumber;
	snapshot->port.slave_only = ptpd->slaveOnly;
	snapshot->port.master_only = ptpd->masterOnly;
	snapshot->port.last_sync_ifindex = ptpd->lastSyncIfindex;
	snapshot->current.servo_outlier_threshold = servo_get_outlier_threshold(&ptpd->servo);
	snapshot->port.effective_comm_caps = ptpd->effective_comm_caps;

	if (ptpd->portState == PTPD_SLAVE) {
		snapshot->current.offset_from_master = servo_get_offset_from_master(&ptpd->servo);
		snapshot->current.one_way_delay = servo_get_mean_path_delay(&ptpd->servo);
		snapshot->current.last_offset_time = servo_get_time_of_last_offset(&ptpd->servo);
		snapshot->current.servo_p_term = servo_get_p_term(&ptpd->servo);
		snapshot->current.servo_i_term = servo_get_i_term(&ptpd->servo);
		if (ptpd->timePropertiesDS.currentUtcOffsetValid)
			snapshot->current.last_offset_time.sec -=
				ptpd->timePropertiesDS.currentUtcOffset;
		snapshot->current.transparent_clock
			= ((ptpd->syncXparent || ptpd->followXparent) ||
			   (ptpd->delayRespXparent || ptpd->pDelayRespFollowXparent));
	} else {
		snapshot->current.offset_from_master = 0.0;
		snapshot->current.one_way_delay = 0.0;
		sfptpd_time_zero(&snapshot->current.last_offset_time);
		snapshot->current.transparent_clock = false;
	}

	snapshot->current.frequency_adjustment
		= servo_get_frequency_adjustment(&ptpd->servo);

	/* The grandmaster characteristics are only valid in the master, slave
	 * and passive states. In all other states, return default values. */
	if ((ptpd->portState == PTPD_MASTER) || (ptpd->portState == PTPD_SLAVE) ||
	    (ptpd->portState == PTPD_PASSIVE)) {
		memcpy(snapshot->parent.clock_id,
		       ptpd->parentPortIdentity.clockIdentity,
		       sizeof(snapshot->parent.clock_id));
		snapshot->parent.port_num = ptpd->parentPortIdentity.portNumber;
		memcpy(snapshot->parent.grandmaster_id,
		       ptpd->grandmasterIdentity,
		       sizeof(snapshot->parent.grandmaster_id));
		snapshot->parent.grandmaster_clock_class
			= ptpd->grandmasterClockQuality.clockClass;
		snapshot->parent.grandmaster_clock_accuracy
			= ptpd->grandmasterClockQuality.clockAccuracy;
		snapshot->parent.grandmaster_offset_scaled_log_variance
			= ptpd->grandmasterClockQuality.offsetScaledLogVariance;
		snapshot->parent.grandmaster_priority1
			= ptpd->grandmasterPriority1;
		snapshot->parent.grandmaster_priority2
			= ptpd->grandmasterPriority2;
		snapshot->parent.grandmaster_time_source
			= ptpd->timePropertiesDS.timeSource;
		snapshot->current.steps_removed = ptpd->stepsRemoved;
		snapshot->current.two_step = ptpd->twoStepFlag;
		snapshot->parent.protocol_address_len = ptpd->parentAddressLen;
		memcpy(&snapshot->parent.protocol_address,
		       &ptpd->parentAddress,
		       sizeof snapshot->parent.protocol_address);
	} else {
		memset(snapshot->parent.clock_id, 0,
		       sizeof(snapshot->parent.clock_id));
		snapshot->parent.port_num = 0;
		memset(snapshot->parent.grandmaster_id, 0,
		       sizeof(snapshot->parent.grandmaster_id));
		snapshot->parent.grandmaster_clock_class = DEFAULT_CLOCK_CLASS;
		snapshot->parent.grandmaster_clock_accuracy = PTPD_ACCURACY_UNKNOWN;
		snapshot->parent.grandmaster_offset_scaled_log_variance = 0;
		snapshot->parent.grandmaster_priority1 = 0;
		snapshot->parent.grandmaster_priority2 = 0;
		snapshot->parent.grandmaster_time_source
			= PTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
		snapshot->current.steps_removed = 0;
		snapshot->current.two_step = FALSE;
		snapshot->parent.protocol_address_len = 0;
		memset(&snapshot->parent.protocol_address, 0,
		       sizeof snapshot->parent.protocol_address);
	}

	snapshot->time.current_utc_offset = ptpd->timePropertiesDS.currentUtcOffset;
	snapshot->time.current_utc_offset_valid = ptpd->timePropertiesDS.currentUtcOffsetValid;
	snapshot->time.ptp_timescale = ptpd->timePropertiesDS.ptpTimescale;
	snapshot->time.leap59 = ptpd->timePropertiesDS.leap59;
	snapshot->time.leap61 = ptpd->timePropertiesDS.leap61;
	snapshot->time.time_traceable = ptpd->timePropertiesDS.timeTraceable;
	snapshot->time.freq_traceable = ptpd->timePropertiesDS.frequencyTraceable;

	return 0;
}

int ptpd_get_intf_fds(struct ptpd_intf_context *ptpd, struct ptpd_intf_fds *fds)
{
	if ((ptpd == NULL) || (fds == NULL)) {
		ERROR("null ptpd if context (%p) or fds structure (%p) supplied\n",
		      ptpd, fds);
		return EINVAL;
	}

	memset(fds, 0, sizeof *fds);
	fds->event_sock = ptpd->transport.eventSock;
	fds->general_sock = ptpd->transport.generalSock;

	return 0;
}

int ptpd_get_counters(struct ptpd_port_context *ptpd, struct ptpd_counters *counters)
{
	struct ptp_servo_counters servo_counters;
	
	if ((ptpd == NULL) || (counters == NULL)) {
		ERROR("null ptpd context (%p) or counters structure (%p) supplied\n",
		      ptpd, counters);
		return EINVAL;
	}

	*counters = ptpd->counters;

	/* Add the clock steps and outliers count from the ptp servo to the overall stats */
	servo_get_counters(&ptpd->servo, &servo_counters);
	counters->clockSteps += servo_counters.clock_steps;
	counters->outliers += servo_counters.outliers;
	counters->outliersNumSamples += servo_counters.outliers_num_samples;

	/* Add the interface counters to the overall stats */
	counters->discardedMessages += ptpd->interface->counters.discardedMessages;
	counters->aclTimingDiscardedMessages += ptpd->interface->counters.aclTimingDiscardedMessages;
	counters->aclManagementDiscardedMessages += ptpd->interface->counters.aclManagementDiscardedMessages;
	counters->messageRecvErrors += ptpd->interface->counters.messageRecvErrors;
	counters->messageFormatErrors += ptpd->interface->counters.messageFormatErrors;
	counters->versionMismatchErrors += ptpd->interface->counters.versionMismatchErrors;
	counters->domainMismatchErrors += ptpd->interface->counters.domainMismatchErrors;

	return 0;
}


int ptpd_clear_counters(struct ptpd_port_context *ptpd)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	memset(&ptpd->counters, 0, sizeof(ptpd->counters));
	servo_reset_counters(&ptpd->servo);
	return 0;
}


void ptpd_publish_mtie_window(struct ptpd_port_context *ptpd,
			      bool mtie_valid,
			      uint32_t window_number,
			      int window_seconds,
			      long double min,
			      long double max,
			      const struct sfptpd_timespec *min_time,
			      const struct sfptpd_timespec *max_time)
{
	/* Sink the lost sub-ns timestamps that don't fit in MTIE TLV */
	TimeInterval correction;

	ptpd->mtie_window.mtie_valid = mtie_valid ? TRUE : FALSE;
	ptpd->mtie_window.mtie_window_number = (UInteger16) window_number;
	ptpd->mtie_window.mtie_window_duration = (UInteger16) window_seconds;
	ptpd->mtie_window.min_offs_from_master =
		sfptpd_time_float_ns_to_scaled_ns(min);
	ptpd->mtie_window.max_offs_from_master =
		sfptpd_time_float_ns_to_scaled_ns(max);
	fromInternalTime(min_time, &ptpd->mtie_window.min_offs_from_master_at,
			 &correction);
	fromInternalTime(max_time, &ptpd->mtie_window.max_offs_from_master_at,
			 &correction);
}


/** Turn an alarms bitfield into a bitfield of message types
 * corresponding to missing message alarms.
 * @param alarms Pointer to the alarms bitfield. On return this updated
 * with all the converted alarm bits cleared.
 * @return The bitfield of message types. */
static int ptpd_translate_alarms_to_msg_type_bitfield(int *alarms)
{
	int msg_alarms = 0;

	#define TRANSLATE_ALARM_TO_MSG_TYPE(alarm, msg_type)		\
		if (*alarms & SYNC_MODULE_ALARM_ ## alarm) {		\
			msg_alarms |= (1 << PTPD_MSG_ ## msg_type);	\
			*alarms &= ~SYNC_MODULE_ALARM_ ## alarm;	\
		}
	TRANSLATE_ALARM_TO_MSG_TYPE(NO_SYNC_PKTS, SYNC);
	TRANSLATE_ALARM_TO_MSG_TYPE(NO_FOLLOW_UPS, FOLLOW_UP);
	TRANSLATE_ALARM_TO_MSG_TYPE(NO_DELAY_RESPS, DELAY_RESP);
	TRANSLATE_ALARM_TO_MSG_TYPE(NO_PDELAY_RESPS, PDELAY_RESP);
	TRANSLATE_ALARM_TO_MSG_TYPE(NO_PDELAY_RESP_FOLLOW_UPS, PDELAY_RESP_FOLLOW_UP);

	return msg_alarms;
}


int ptpd_translate_alarms_from_msg_type_bitfield(int *msg_alarms)
{
	int alarms = 0;

	#define TRANSLATE_ALARM_FROM_MSG_TYPE(alarm, msg_type) \
		if (*msg_alarms & (1 << PTPD_MSG_ ## msg_type)) {	\
			alarms |= SYNC_MODULE_ALARM_ ## alarm;		\
			*msg_alarms &= ~(1 << PTPD_MSG_ ## msg_type);	\
		}

	TRANSLATE_ALARM_FROM_MSG_TYPE(NO_SYNC_PKTS, SYNC);
	TRANSLATE_ALARM_FROM_MSG_TYPE(NO_FOLLOW_UPS, FOLLOW_UP);
	TRANSLATE_ALARM_FROM_MSG_TYPE(NO_DELAY_RESPS, DELAY_RESP);
	TRANSLATE_ALARM_FROM_MSG_TYPE(NO_PDELAY_RESPS, PDELAY_RESP);
	TRANSLATE_ALARM_FROM_MSG_TYPE(NO_PDELAY_RESP_FOLLOW_UPS, PDELAY_RESP_FOLLOW_UP);

	return alarms;
}


/** Turn alarms not relating to missing messages into the format used
 * in slave status reporting for the Solarflare extension TLV.
 * @param alarms Pointer to the alarms bitfield. On return this updated
 * with all the converted alarm bits cleared.
 * @return The alarm bits in protocol format */
static int ptpd_translate_alarms_to_protocol(int *alarms)
{
	int other_alarms = 0;

	#define TRANSLATE_ALARM_TO_PROTO(alarm, flag)			\
		if (*alarms & SYNC_MODULE_ALARM_ ## alarm) {		\
			other_alarms |= (1 << PTPD_SFC_ALARM_ ## flag);	\
			*alarms &= ~SYNC_MODULE_ALARM_ ## alarm;	\
		}

	TRANSLATE_ALARM_TO_PROTO(NO_TX_TIMESTAMPS, NO_TX_TIMESTAMPS);
	TRANSLATE_ALARM_TO_PROTO(NO_RX_TIMESTAMPS, NO_RX_TIMESTAMPS);
	TRANSLATE_ALARM_TO_PROTO(NO_INTERFACE, NO_INTERFACE);
	TRANSLATE_ALARM_TO_PROTO(CLOCK_CTRL_FAILURE, SERVO_FAIL);

	return other_alarms;
}


int ptpd_translate_alarms_from_protocol(int *other_alarms)
{
	int alarms = 0;

	#define TRANSLATE_ALARM_FROM_PROTO(alarm, flag)			\
		if (*other_alarms & (1 << PTPD_SFC_ALARM_ ## flag)) {	\
			alarms |= SYNC_MODULE_ALARM_ ## alarm;	\
			*other_alarms &= ~(1 << PTPD_SFC_ALARM_ ## flag); \
		}

	TRANSLATE_ALARM_FROM_PROTO(NO_TX_TIMESTAMPS, NO_TX_TIMESTAMPS);
	TRANSLATE_ALARM_FROM_PROTO(NO_RX_TIMESTAMPS, NO_RX_TIMESTAMPS);
	TRANSLATE_ALARM_FROM_PROTO(NO_INTERFACE, NO_INTERFACE);
	TRANSLATE_ALARM_FROM_PROTO(CLOCK_CTRL_FAILURE, SERVO_FAIL);

	return alarms;
}


void ptpd_publish_status(struct ptpd_port_context *ptpd,
			 int alarms,
			 bool selected,
			 bool in_sync,
			 bool bond_changed)
{
	int missingMessageAlarms;
	int otherAlarms;
	int events = 0;
	int flags = 0;

	missingMessageAlarms = ptpd_translate_alarms_to_msg_type_bitfield(&alarms);
	otherAlarms = ptpd_translate_alarms_to_protocol(&alarms);

	if (alarms != 0)
		otherAlarms |= 1 << PTPD_SFC_ALARM_UNKNOWN;

	if (bond_changed)
		events |= 1 << PTPD_SFC_EVENT_BOND_CHANGED;

	if (in_sync)
		flags |= 1 << PTPD_SFC_FLAG_IN_SYNC;

	if (selected)
		flags |= 1 << PTPD_SFC_FLAG_SELECTED;

	slaveStatusMonitor(ptpd, &ptpd->rtOpts,
			   missingMessageAlarms, otherAlarms, events, flags);
}


int ptpd_test_set_utc_offset(struct ptpd_port_context *ptpd, int offset,
			     int compensation)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	if (ptpd->portState != PTPD_MASTER) {
		WARNING("UTC offset test mode can only be used in master state\n");
		return EPERM;
	}

	ptpd->timePropertiesDS.currentUtcOffset = offset;
	ptpd->timePropertiesDS.currentUtcOffsetValid = TRUE;
	ptpd->fakeUtcAdjustment += compensation;
	DBG("test: set UTC offset = %d\n", offset);
	return 0;
}


int ptpd_test_get_bad_timestamp_type(struct ptpd_port_context *ptpd)
{
	struct ptpd_port_config *rtOpts;

	assert(ptpd != NULL);

	rtOpts = &ptpd->rtOpts;

	return rtOpts->test.bad_timestamp.type;
}


int ptpd_test_set_bad_timestamp(struct ptpd_port_context *ptpd,
				   int type,
				   int interval_pkts,
				   int max_jitter)
{
	struct ptpd_port_config *rtOpts = &ptpd->rtOpts;

	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	rtOpts->test.bad_timestamp.type = type;
	rtOpts->test.bad_timestamp.interval_pkts = interval_pkts;
	rtOpts->test.bad_timestamp.max_jitter = max_jitter;

	return 0;
}


int ptpd_test_set_transparent_clock_emulation(struct ptpd_port_context *ptpd,
					      int max_correction)
{
	struct ptpd_port_config *rtOpts = &ptpd->rtOpts;

	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	rtOpts->test.xparent_clock.enable = (max_correction != 0);
	rtOpts->test.xparent_clock.max_correction = max_correction;

	return 0;
}


int ptpd_test_set_boundary_clock_emulation(struct ptpd_port_context *ptpd,
					   UInteger8 grandmaster_id[],
					   UInteger32 steps_removed)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	memcpy(ptpd->grandmasterIdentity, grandmaster_id,
	       sizeof(ptpd->grandmasterIdentity));
	ptpd->stepsRemoved = steps_removed;

	return 0;
}


int ptpd_test_change_grandmaster_clock(struct ptpd_port_context *ptpd,
				       UInteger8 clock_class,
				       Enumeration8 clock_accuracy,
				       UInteger16 offset_scaled_log_variance,
				       UInteger8 priority1,
				       UInteger8 priority2)
{
	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	ptpd->clockQuality.clockClass = clock_class;
	ptpd->clockQuality.clockAccuracy = clock_accuracy;
	ptpd->clockQuality.offsetScaledLogVariance = offset_scaled_log_variance;
	ptpd->grandmasterPriority1 = priority1;
	ptpd->grandmasterPriority2 = priority2;

	return 0;
}


int ptpd_test_pkt_suppression(struct ptpd_port_context *ptpd,
			      bool no_announce_pkts,
			      bool no_sync_pkts,
			      bool no_follow_ups,
			      bool no_delay_resps)
{
	struct ptpd_port_config *rtOpts = &ptpd->rtOpts;

	if (ptpd == NULL) {
		ERROR("null ptpd context supplied\n");
		return EINVAL;
	}

	rtOpts->test.no_announce_pkts = no_announce_pkts;
	rtOpts->test.no_sync_pkts     = no_sync_pkts;
	rtOpts->test.no_follow_ups    = no_follow_ups;
	rtOpts->test.no_delay_resps   = no_delay_resps;

	return 0;
}

void ptpd_process_intf_stats(struct ptpd_intf_context *intf)
{
	netCheckTimestampStats(&intf->ts_cache);
}

/* fin */
