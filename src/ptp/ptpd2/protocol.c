/*-
 * Copyright (c) 2019-2020 Xilinx, Inc.
 * Copyright (c) 2014-2018 Solarflare Communications Inc.
 * Copyright (c) 2013      Harlan Stenn,
 *                         George N. Neville-Neil,
 *                         Wojciech Owczarek
 *                         Solarflare Communications Inc.
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Wojciech Owczarek,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments.
 *                         Solarflare Communications Inc.
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   protocol.c
 * @date   Wed Jun 23 09:40:39 2010
 *
 * @brief  The code that handles the IEEE-1588 protocol and state machine
 *
 *
 */

#include "ptpd.h"
#include "ptpd_lib.h"
#include "sfptpd_time.h"

static void handleAnnounce(MsgHeader*, ssize_t, RunTimeOpts*, PtpClock*);
static void handleSync(const MsgHeader*, ssize_t, struct sfptpd_timespec*, Boolean, UInteger32, RunTimeOpts*, PtpClock*);
static void handleFollowUp(const MsgHeader*, ssize_t, const MsgFollowUp*, Boolean, RunTimeOpts*, PtpClock*);
static void handlePDelayReq(MsgHeader*, ssize_t, struct sfptpd_timespec*, Boolean, RunTimeOpts*, PtpClock*);
static void handleDelayReq(const MsgHeader*, ssize_t, struct sfptpd_timespec*, Boolean, RunTimeOpts*, PtpClock*);
static void handlePDelayResp(const MsgHeader*, ssize_t, struct sfptpd_timespec*, Boolean, RunTimeOpts*, PtpClock*);
static void handleDelayResp(const MsgHeader*, ssize_t, RunTimeOpts*, PtpClock*);
static void handlePDelayRespFollowUp(const MsgHeader*, ssize_t, RunTimeOpts*, PtpClock*);
static void handleManagement(MsgHeader*, ssize_t, RunTimeOpts*, PtpClock*);
static void handleSignaling(PtpClock*);

static void issueAnnounce(RunTimeOpts*, PtpClock*);
static void issueSync(RunTimeOpts*, PtpClock*);
static void issueFollowup(const struct sfptpd_timespec*, RunTimeOpts*, PtpClock*, const UInteger16);
static void issuePDelayReq(RunTimeOpts*, PtpClock*);
static void issueDelayReq(RunTimeOpts*, PtpClock*);
static void issuePDelayResp(struct sfptpd_timespec*, MsgHeader*, RunTimeOpts*, PtpClock*);
static void issueDelayResp(struct sfptpd_timespec*, MsgHeader*, RunTimeOpts*, PtpClock*);
static void issuePDelayRespFollowUp(struct sfptpd_timespec*, MsgHeader*, RunTimeOpts*, PtpClock*, const UInteger16);
static void issueManagementRespOrAck(MsgManagement*, RunTimeOpts*, PtpClock*, const struct sockaddr_storage *address, socklen_t addressLength);
static void issueManagementErrorStatus(MsgManagement*, RunTimeOpts*, PtpClock*, const struct sockaddr_storage *address, socklen_t addressLength);
static void processMessage(InterfaceOpts *ifOpts, PtpInterface *ptpInterface, struct sfptpd_timespec *timestamp, Boolean timestampValid, UInteger32 rxPhysIfindex, ssize_t length);
static void processPortMessage(RunTimeOpts *rtOpts, PtpClock *ptpClock, struct sfptpd_timespec *timestamp, Boolean timestampValid, UInteger32 rxPhysIfindex, ssize_t length, int offset, acl_bitmap_t acls_checked, acl_bitmap_t acls_passed);
static ssize_t unpackPortMessage(PtpClock *ptpClock, ssize_t safe_length);
static bool processTLVs(RunTimeOpts *rtOpts, PtpClock *ptpClock, int payload_offset,
			ssize_t  unpack_result, ssize_t safe_length,
			struct sfptpd_timespec *timestamp, Boolean timestampValid,
			acl_bitmap_t acls_checked, acl_bitmap_t acls_passed);
static void handleMessage(RunTimeOpts *rtOpts, PtpClock *ptpClock,
			  ssize_t safe_length,
			  struct sfptpd_timespec *timestamp, Boolean timestampValid,
			  UInteger32 rxPhysIfindex);
static void processSyncFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts, PtpClock *ptpClock, const UInteger16 sequenceId);
static void processDelayReqFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts, PtpClock *ptpClock);
static void processPDelayReqFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts, PtpClock *ptpClock);
static void processPDelayRespFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts, PtpClock *ptpClock, const UInteger16 sequenceId);
static void issueDelayRespWithMonitoring(struct sfptpd_timespec *time, MsgHeader *header, RunTimeOpts *rtOpts, PtpClock *ptpClock);
static void issueSyncForMonitoring(RunTimeOpts*, PtpClock*, UInteger16 sequenceId);
static void issueFollowupForMonitoring(const struct sfptpd_timespec*, RunTimeOpts*, PtpClock*, const UInteger16);
static void processMonitoringSyncFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts, PtpClock *ptpClock, const UInteger16 sequenceId);
static void
processTxTimestamp(PtpInterface *interface,
		   struct sfptpd_ts_user ts_user,
		   struct sfptpd_ts_ticket ts_ticket,
		   struct sfptpd_timespec timestamp);

struct tlv_dispatch_info {
	TLV tlv;
	off_t tlv_offset;
	const struct tlv_handling *handler;
};

#define MAX_TLVS 32

static enum ptpd_tlv_result
ptpmon_req_tlv_handler(const MsgHeader *header, ssize_t length,
		       struct sfptpd_timespec *time, Boolean timestampValid,
		       RunTimeOpts *rtOpts, PtpClock *ptpClock,
		       TLV *tlv, size_t tlv_offset);

static enum ptpd_tlv_result
mtie_req_tlv_handler(const MsgHeader *header, ssize_t length,
		     struct sfptpd_timespec *time, Boolean timestampValid,
		     RunTimeOpts *rtOpts, PtpClock *ptpClock,
		     TLV *tlv, size_t tlv_offset);

static enum ptpd_tlv_result
port_communication_capabilities_handler(const MsgHeader *header, ssize_t length,
					struct sfptpd_timespec *time, Boolean timestampValid,
					RunTimeOpts *rtOpts, PtpClock *ptpClock,
					TLV *tlv, size_t tlv_offset);

static void statsAddNode(Octet *buf, MsgHeader *header, PtpInterface *ptpInterface);

static void applyUtcOffset(struct sfptpd_timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock);

static const struct tlv_handling tlv_handlers[] = {
	{
		.tlv_type = PTPD_TLV_PTPMON_REQ_OLD,
		.name = "PTPMON_REQ_TLV",
		.permitted_message_types_mask = 1 << PTPD_MSG_DELAY_REQ,
		.required_acl_types_mask = PTPD_ACL_TIMING | PTPD_ACL_MONITORING,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = ptpmon_req_tlv_handler
	},
	{
		.tlv_type = PTPD_TLV_MTIE_REQ_OLD,
		.name = "MTIE_REQ_TLV",
		.permitted_message_types_mask = 1 << PTPD_MSG_DELAY_REQ,
		.required_acl_types_mask = PTPD_ACL_TIMING | PTPD_ACL_MONITORING,
		.pass1_handler_fn = mtie_req_tlv_handler,
		.pass2_handler_fn = NULL
	},
	{
		.tlv_type = PTPD_TLV_PAD,
		.name = "PAD",
		.permitted_message_types_mask = ~0,
		.required_acl_types_mask = 0,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = NULL
	},
	{
		.tlv_type = PTPD_TLV_SLAVE_RX_SYNC_TIMING_DATA,
		.name = "SLAVE_RX_SYNC_TIMING_DATA",
		.permitted_message_types_mask = 1 << PTPD_MSG_SIGNALING,
		.required_acl_types_mask = PTPD_ACL_MONITORING,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = slave_rx_sync_timing_data_handler
	},
	{
		.tlv_type = PTPD_TLV_SLAVE_RX_SYNC_COMPUTED_DATA,
		.name = "SLAVE_RX_SYNC_COMPUTED_DATA",
		.permitted_message_types_mask = 1 << PTPD_MSG_SIGNALING,
		.required_acl_types_mask = PTPD_ACL_MONITORING,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = slave_rx_sync_computed_data_handler
	},
	{
		.tlv_type = PTPD_TLV_SLAVE_TX_EVENT_TIMESTAMPS,
		.name = "SLAVE_TX_EVENT_TIMESTAMPS",
		.permitted_message_types_mask = 1 << PTPD_MSG_SIGNALING,
		.required_acl_types_mask = PTPD_ACL_MONITORING,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = slave_tx_event_timestamps_handler
	},
	{
		.tlv_type = PTPD_TLV_ORGANIZATION_EXTENSION_NON_FORWARDING,
		.organization_id = PTPD_SFC_TLV_ORGANISATION_ID,
		.organization_sub_type = PTPD_TLV_SFC_SLAVE_STATUS,
		.permitted_message_types_mask = 1 << PTPD_MSG_SIGNALING,
		.required_acl_types_mask = PTPD_ACL_MONITORING,
		.pass1_handler_fn = NULL,
		.pass2_handler_fn = slave_status_handler
	},
	{
		.tlv_type = PTPD_TLV_PORT_COMMUNICATION_CAPABILITIES,
		.name = "PORT_COMMUNICATION_CAPABILITIES",
		.permitted_message_types_mask = 1 << PTPD_MSG_ANNOUNCE,
		.required_acl_types_mask = 0,
		.pass1_handler_fn = port_communication_capabilities_handler,
		.pass2_handler_fn = NULL,
	},
};

/* perform actions required when leaving 'port_state' and entering 'state' */
void
toState(ptpd_state_e state, RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	bool valid = true;

	/* Stop all protocol timers */
	timerStop(ANNOUNCE_INTERVAL_TIMER, ptpClock->itimer);
	timerStop(ANNOUNCE_RECEIPT_TIMER, ptpClock->itimer);
	timerStop(SYNC_INTERVAL_TIMER, ptpClock->itimer);
	timerStop(SYNC_RECEIPT_TIMER, ptpClock->itimer);
	timerStop(DELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
	timerStop(DELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
	timerStop(FAULT_RESTART_TIMER, ptpClock->itimer);
	timerStop(FOREIGN_MASTER_TIMER, ptpClock->itimer);

	/* Reset the port alarms - these are generally only valid in the slave
	 * state. */
	ptpClock->portAlarms = 0;

	/* Note that we don't reset the servo when entering or leaving the slave
	 * state. Instead we assume let the servo continue to work. If the time
	 * on a the next master is significantly different, this will cause a
	 * servo reset and a time correction. Otherwise we will converge as
	 * normal.
	 */

	ptpClock->counters.stateTransitions++;

	/* Default to our configured communication capabilities */
	ptpClock->effective_comm_caps = rtOpts->comm_caps;

	DBG("ptp %s: state %s\n", rtOpts->name, portState_getName(state));

	switch (state) {
	case PTPD_INITIALIZING:
		timerStop(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
		timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
		timerStop(TIMESTAMP_CHECK_TIMER, ptpClock->itimer);
		break;

	case PTPD_FAULTY:
		timerStop(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
		timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
		timerStop(TIMESTAMP_CHECK_TIMER, ptpClock->itimer);
		timerStart(FAULT_RESTART_TIMER, PTPD_FAULT_RESTART_INTERVAL,
			   ptpClock->itimer);
		break;

	case PTPD_DISABLED:
		timerStop(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
		timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
		timerStop(TIMESTAMP_CHECK_TIMER, ptpClock->itimer);
		break;

	case PTPD_LISTENING:
		/* in Listening mode, we don't send anything. Instead we just
		 * expect/wait for announces (started below) */

		/* Count how many _unique_ timeouts happen to us. If we were
		 * already in Listen mode, then do not count this as a separate
		 * reset, but still do a new IGMP refresh */
		if (ptpClock->portState != PTPD_LISTENING) {
			ptpClock->resetCount++;
		}

		/* Revert to the original DelayReq, Announce and Sync intervals */
		ptpClock->logMinDelayReqInterval = rtOpts->minDelayReqInterval;
		ptpClock->logSyncInterval = rtOpts->syncInterval;
		ptpd_update_announce_interval(ptpClock, rtOpts);

		/* Update the expected interval in the servo */
		servo_set_interval(&ptpClock->servo,
				   powl(2, ptpClock->logSyncInterval));

		timerStart(ANNOUNCE_RECEIPT_TIMER,
			   (ptpClock->announceReceiptTimeout *
			    powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(FOREIGN_MASTER_TIMER,
			   (FOREIGN_MASTER_TIME_CHECK * powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		/* Avoid restarting the peer-delay timer if it's already
		 * running. Unlike delay requests (end-to-end) there is no
		 * randomization in when peer-delay messages are sent so it is
		 * quite easy to end up with all slaves sending peer delay
		 * messages at the same time: note that this wouldn't be a 
		 * problem in a network where the peer is a genuine transparent
		 * clock (switch), however we don't want code that clearly
		 * would ddos the GM in a misconfigured network. */
		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
		    !timerRunning(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}
		timerStop(TIMESTAMP_CHECK_TIMER, ptpClock->itimer);
		break;

	case PTPD_MASTER:
		/* Revert to the original DelayReq, Announce and Sync intervals */
		ptpClock->logMinDelayReqInterval = rtOpts->minDelayReqInterval;
		ptpClock->logAnnounceInterval = rtOpts->announceInterval;
		ptpClock->logSyncInterval = rtOpts->syncInterval;

		/* Update the expected interval in the servo */
		servo_set_interval(&ptpClock->servo,
				   powl(2, ptpClock->logSyncInterval));

		timerStart(SYNC_INTERVAL_TIMER,
			   powl(2,ptpClock->logSyncInterval), ptpClock->itimer);
		DBG("SYNC INTERVAL TIMER : %Lf \n",
		    powl(2,ptpClock->logSyncInterval));

		timerStart(ANNOUNCE_INTERVAL_TIMER,
			   powl(2,ptpClock->logAnnounceInterval),
			   ptpClock->itimer);

		timerStart(FOREIGN_MASTER_TIMER,
			   (FOREIGN_MASTER_TIME_CHECK * powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(TIMESTAMP_CHECK_TIMER,
			   TIMESTAMP_HEALTH_CHECK_INTERVAL,
			   ptpClock->itimer);

		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
		    !timerRunning(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}
		break;

	case PTPD_PASSIVE:
		timerStart(ANNOUNCE_RECEIPT_TIMER,
			   (ptpClock->announceReceiptTimeout *
			    powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(FOREIGN_MASTER_TIMER,
			   (FOREIGN_MASTER_TIME_CHECK * powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(TIMESTAMP_CHECK_TIMER,
			   TIMESTAMP_HEALTH_CHECK_INTERVAL,
			   ptpClock->itimer);

		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
		    !timerRunning(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}
		break;

	case PTPD_UNCALIBRATED:
		break;

	case PTPD_SLAVE:
		/* Don't reset the servo when entering or leaving the slave
		 * state. Instead we assume let the servo continue to work.
		 * If the time on a the next master is significantly different,
		 * this will cause a servo reset and a time correction.
		 * Otherwise we will converge as normal.
		 */
		ptpClock->waitingForFollow = FALSE;
		ptpClock->waitingForDelayResp = FALSE;

		/* Copy announced communication capabilities from foreign master record */
		ptpClock->partner_comm_caps =
			ptpClock->foreign.records[ptpClock->foreign.best_index].comm_caps;
		/* Mask local and remote communication capability sets */
		ptpClock->effective_comm_caps.syncCapabilities =
			ptpClock->partner_comm_caps.syncCapabilities &
			rtOpts->comm_caps.syncCapabilities;

		ptpClock->effective_comm_caps.delayRespCapabilities =
			ptpClock->partner_comm_caps.delayRespCapabilities &
			rtOpts->comm_caps.delayRespCapabilities;

		if (ptpClock->effective_comm_caps.syncCapabilities == 0) {
			WARNING("ptp %s: no common sync message capabilities\n", rtOpts->name);
		}

		if (ptpClock->effective_comm_caps.delayRespCapabilities == 0) {
			WARNING("ptp %s: no common delay resp capabilities\n", rtOpts->name);
		}

		ptpClock->unicast_delay_resp_failures = 0;

		timerStart(OPERATOR_MESSAGES_TIMER,
			   OPERATOR_MESSAGES_INTERVAL,
			   ptpClock->itimer);

		timerStart(ANNOUNCE_RECEIPT_TIMER,
			   (ptpClock->announceReceiptTimeout * powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(FOREIGN_MASTER_TIMER,
			   (FOREIGN_MASTER_TIME_CHECK * powl(2,ptpClock->logAnnounceInterval)),
			   ptpClock->itimer);

		timerStart(TIMESTAMP_CHECK_TIMER,
			   TIMESTAMP_HEALTH_CHECK_INTERVAL,
			   ptpClock->itimer);

		ptpClock->sync_missing_interval = 0.0;
		ptpClock->sync_missing_next_warning =
			ptpClock->syncReceiptTimeout * powl(2,ptpClock->logSyncInterval);
		timerStart(SYNC_RECEIPT_TIMER,
			   ptpClock->sync_missing_next_warning,
			   ptpClock->itimer);

		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
		    !timerRunning(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}

		/*
		 * Previously, this state transition would start the
		 * delayreq timer immediately.  However, if this was
		 * faster than the first received sync, then the servo
		 * would drop the delayResp Now, we only start the
		 * timer after we receive the first sync (in
		 * handle_sync())
		 */
		ptpClock->waiting_for_first_sync = TRUE;
		ptpClock->waiting_for_first_delayresp = TRUE;
		break;

	default:
		DBG("to unrecognized state\n");
		valid = false;
		break;
	}

	if (valid && ptpClock->portState != state) {
		ptpClock->portState = state;
		displayStatus(ptpClock, "now in state: ");
	}
}

void
toStateAllPorts(ptpd_state_e state, PtpInterface *ptpInterface)
{
	PtpClock *port;

	for (port = ptpInterface->ports; port; port = port->next) {
		toState(state, &port->rtOpts, port);
	}
}

void
handleSendFailure(RunTimeOpts *rtOpts, PtpClock *ptpClock, const char *message) {
	ptpClock->counters.messageSendErrors++;
	if (rtOpts->missingInterfaceTolerance) {
		toState(PTPD_LISTENING, rtOpts, ptpClock);
		DBGV("%s message can't be sent. In missing interface tolerance mode -> LISTENING state\n",
		     message);
	} else {
		toState(PTPD_FAULTY, rtOpts, ptpClock);
		DBGV("%s message can't be sent -> FAULTY state\n",
		     message);
	}
}

Boolean
doInitGlobal(void)
{
	initTimer();
	return TRUE;
}

Boolean
doInitPort(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	int rc;
	struct sfptpd_clock *system_clock;

	/* In case we are re-initializing, first shutdown components that
	 * require it before initializing.
	 */
	managementShutdown(ptpClock);

	/* initialize networking */
	if (!netInitPort(ptpClock, rtOpts)) {
		ERROR("ptp %s: failed to initialize network\n", rtOpts->name);
		return FALSE;
	}

	/* Determine which clock to use based on the interface */
	assert(ptpClock->physIface != NULL);
	ptpClock->clock = sfptpd_interface_get_clock(ptpClock->physIface);
	assert(ptpClock->clock != NULL);

	/* Get clock id */
	sfptpd_clock_get_hw_id(ptpClock->clock, &rtOpts->ifOpts->clock_id);

	/* Initialize the PTP data sets */
	initData(rtOpts, ptpClock);

	system_clock = sfptpd_clock_get_system_clock();

	INFO("ptp: clock is %s\n", sfptpd_clock_get_long_name(ptpClock->clock));

	/* If using a NIC clock and we are in a PTP master mode then step the
	 * NIC clock to the current system time. */
	if ((ptpClock->clock != system_clock) && !ptpClock->rtOpts.slaveOnly) {
		rc = sfptpd_clock_set_time(ptpClock->clock, system_clock, NULL);
		if (rc != 0) {
			TRACE_L4("ptp: failed to compare and set clock %s to system clock, %s\n",
				 sfptpd_clock_get_short_name(ptpClock->clock),
				 strerror(rc));
			if (rc != EAGAIN && rc != EBUSY)
				return FALSE;
		}
	}

	/* initialize other stuff */
	if (!servo_init(rtOpts, &ptpClock->servo, ptpClock->clock)) {
		ERROR("ptp %s: failed to initialize servo\n", rtOpts->name);
		toState(PTPD_FAULTY, rtOpts, ptpClock);
		return FALSE;
	}

	managementInit(rtOpts, ptpClock);

	m1(rtOpts, ptpClock);
	msgPackHeader(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
		      ptpClock, PTPD_MSG_SYNC);

	if (rtOpts->node_type == PTPD_NODE_CLOCK) {
		toState(PTPD_LISTENING, rtOpts, ptpClock);
	} else {
		toState(PTPD_DISABLED, rtOpts, ptpClock);
	}

	return TRUE;
}

Boolean
doInitInterface(InterfaceOpts *ifOpts, PtpInterface *ptpInterface)
{
	/* In case we are re-initializing, shutdown and then
	 * initialize networking.
	 */
	netShutdown(&ptpInterface->transport);

	/* Initialize networking */
	 if (!netInit(&ptpInterface->transport, ifOpts, ptpInterface)) {
		ERROR("failed to initialize network\n");
		return FALSE;
	}

	return TRUE;
}

/* handle a timer tick */
void
doTimerTick(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	UInteger8 state;

	/* Update the timers */
	timerTick(ptpClock->itimer);

	/* Process record_update (BMC algorithm) before everything else */
	switch (ptpClock->portState) {
	case PTPD_LISTENING:
	case PTPD_PASSIVE:
	case PTPD_SLAVE:
	case PTPD_MASTER:
		/*State decision Event*/

		/* If we received a valid Announce message
 		 * and can use it (record_update),
		 * or we received a SET management message that
		 * changed an attribute in ptpClock,
		 * then run the BMC algorithm
		 */
		if (ptpClock->record_update) {
			DBG2("event STATE_DECISION_EVENT\n");
			ptpClock->record_update = FALSE;
			state = bmc(&ptpClock->foreign, rtOpts, ptpClock);
			if (state != ptpClock->portState)
				toState(state, rtOpts, ptpClock);
		}
		break;

	default:
		break;
	}

	/* Timers valid in multiple states */
	if (timerExpired(TIMESTAMP_CHECK_TIMER, ptpClock->itimer)) {
		bool alarm;

		DBGV("event TIMESTAMP_CHECK_TIMER expires\n");
		timerStart(TIMESTAMP_CHECK_TIMER,
			   TIMESTAMP_HEALTH_CHECK_INTERVAL,
			   ptpClock->itimer);

		alarm = netCheckTimestampAlarms(ptpClock);

		if (alarm)
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
		else
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
	}

	switch(ptpClock->portState) {
	case PTPD_FAULTY:
		/* If the restart timer has expired, clear fault and attempt
		 * to re-initialise. Otherwise sleep until the next SIGALRM */
		if (timerExpired(FAULT_RESTART_TIMER, ptpClock->itimer)) {
			DBG("event FAULT_CLEARED\n");
			timerStop(FAULT_RESTART_TIMER, ptpClock->itimer);
			toState(PTPD_INITIALIZING, rtOpts, ptpClock);
		}
		break;

	case PTPD_LISTENING:
	case PTPD_UNCALIBRATED:
	case PTPD_SLAVE:
	/* passive mode behaves like the SLAVE state, in order to wait for the announce
	 * timeout of the current active master */
	case PTPD_PASSIVE:
		/*
		 * handle SLAVE timers:
		 *   - No Announce message was received
		 *   - No Sync message was received
		 *   - No DelayResponse message was received
		 *   - Time to send new delayReq  (miss of delayResp is not monitored explicitelly)
		 */
		if (timerExpired(ANNOUNCE_RECEIPT_TIMER, ptpClock->itimer)) {
			WARNING("ptp %s: failed to receive Announce within %0.3Lf seconds\n",
				rtOpts->name,
				(ptpClock->announceReceiptTimeout * powl(2,ptpClock->logAnnounceInterval)));
			ptpClock->counters.announceTimeouts++;

			if (!ptpClock->slaveOnly &&
			    ptpClock->clockQuality.clockClass != SLAVE_ONLY_CLOCK_CLASS) {
				m1(rtOpts, ptpClock);
				toState(PTPD_MASTER, rtOpts, ptpClock);
			} else {
				/*
				 *  Force a reset when getting a timeout in state listening, that will lead to an IGMP reset
				 *  previously this was not the case when we were already in LISTENING mode
				 */
				toState(PTPD_LISTENING, rtOpts, ptpClock);
			}
		}

		if (timerExpired(FOREIGN_MASTER_TIMER, ptpClock->itimer)) {
			DBGV("event FOREIGN_MASTER_TIME_CHECK expires\n");
			timerStart(FOREIGN_MASTER_TIMER,
				   (FOREIGN_MASTER_TIME_CHECK * powl(2,ptpClock->logAnnounceInterval)),
				   ptpClock->itimer);

			struct sfptpd_timespec threshold;

			/* Expire old foreign master records */
			getForeignMasterExpiryTime(ptpClock, &threshold);
			expireForeignMasterRecords(&ptpClock->foreign, &threshold);
		}

		if (timerExpired(SYNC_RECEIPT_TIMER, ptpClock->itimer)) {
			LongDouble interval =
				ptpClock->syncReceiptTimeout * powl(2,ptpClock->logSyncInterval);

			ptpClock->sync_missing_interval += interval;
			if (ptpClock->sync_missing_interval >= ptpClock->sync_missing_next_warning) {
				WARNING("ptp %s: failed to receive Sync for sequence number %d for %0.1Lf seconds\n",
					rtOpts->name,
					(ptpClock->recvSyncSequenceId + 1) & 0xffff,
					ptpClock->sync_missing_interval);
				
				ptpClock->sync_missing_next_warning *= 2.0;
			}

			/* Increment the timeout stat and set the alarm */
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_SYNC_PKTS);
			ptpClock->counters.syncTimeouts++;

			/* Reset the last sync index */
			ptpClock->lastSyncIfindex = 0;

			/* Record the fact that the data is missing */
			servo_missing_m2s_ts(&ptpClock->servo);

			/* Restart the missing sync timer */
			timerStart(SYNC_RECEIPT_TIMER, interval, ptpClock->itimer);
		}

		if (timerExpired(DELAYRESP_RECEIPT_TIMER, ptpClock->itimer)) {
			WARNING("ptp %s: failed to receive DelayResp for DelayReq sequence number %d\n",
				rtOpts->name,
				(ptpClock->sentDelayReqSequenceId - 1) & 0xffff);
			/* Record the fact that we didn't get a timely response
			 * and set the alarm if it's happened too many times. */
			ptpClock->sequentialMissingDelayResps++;
			if (ptpClock->sequentialMissingDelayResps >= rtOpts->delayRespAlarmThreshold) {
				SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_DELAY_RESPS);
				ptpClock->sequentialMissingDelayResps = rtOpts->delayRespAlarmThreshold;
			}
			ptpClock->counters.delayRespTimeouts++;

			/* Record the data as missing */
			servo_missing_s2m_ts(&ptpClock->servo);

			/* Stop the response receipt timer and start the timer
			 * to issue the next Delay Request. */
			timerStop(DELAYRESP_RECEIPT_TIMER, ptpClock->itimer);

			/* If in hybrid mode and it has never succeeded, increment the
			 * failure count unless not multicast capable */
			if ((ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_UNICAST_CAPABLE) &&
			    (ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_MULTICAST_CAPABLE) &&
			    (ptpClock->unicast_delay_resp_failures >= 0)) {
				ptpClock->unicast_delay_resp_failures++;
				if (ptpClock->unicast_delay_resp_failures >= rtOpts->delayRespHybridThreshold) {
					ptpClock->effective_comm_caps.delayRespCapabilities &= ~PTPD_COMM_UNICAST_CAPABLE;
					WARNING("ptp %s: failed to receive DelayResp %d times in "
						"hybrid mode. Reverting to multicast mode.\n",
						rtOpts->name,
						rtOpts->delayRespHybridThreshold);
				}
			}

			timerStart_random(DELAYREQ_INTERVAL_TIMER,
					  powl(2,ptpClock->logMinDelayReqInterval),
					  ptpClock->itimer);
		}

		if (timerExpired(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer)) {
			/* We only make a fuss about failure to receive a
			 * response in the slave state. */
			if (ptpClock->portState == PTPD_SLAVE) {
				WARNING("ptp %s: failed to receive PDelayResp for "
					"PDelayReq sequence number %d\n",
					rtOpts->name,
					(ptpClock->sentPDelayReqSequenceId - 1) & 0xffff);

				/* Record the fact that we didn't get a timely response,
				 * and set the alarm if it's happened too many times. */
				ptpClock->sequentialMissingDelayResps++;
				if (ptpClock->sequentialMissingDelayResps >= rtOpts->delayRespAlarmThreshold) {
					SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_DELAY_RESPS);
					ptpClock->sequentialMissingDelayResps = rtOpts->delayRespAlarmThreshold;
				}
				ptpClock->counters.delayRespTimeouts++;
			}

			/* Record the data as missing */
			servo_missing_p2p_ts(&ptpClock->servo);

			/* Stop the response receipt timer and start the timer
			 * to issue the next peer delay request. */
			timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}

		if (timerExpired(OPERATOR_MESSAGES_TIMER, ptpClock->itimer)) {
			servo_reset_operator_messages(&ptpClock->servo);
		}

		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_E2E) &&
		    timerExpired(DELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			DBG2("event DELAYREQ_INTERVAL_TIMEOUT_EXPIRES\n");
			issueDelayReq(rtOpts,ptpClock);
		} else if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
			   timerExpired(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			DBGV("event PDELAYREQ_INTERVAL_TIMEOUT_EXPIRES\n");
			issuePDelayReq(rtOpts,ptpClock);
		}

		break;

	case PTPD_MASTER:
		/*
		 * handle MASTER timers:
		 *   - Time to send new Sync
		 *   - Time to send new Announce
		 *   - Time to send new PathDelay
		 *      (DelayResp has no timer - as these are sent and retransmitted by the slaves)
		 */
		if (timerExpired(SYNC_INTERVAL_TIMER, ptpClock->itimer)) {
			DBGV("event SYNC_INTERVAL_TIMEOUT_EXPIRES\n");
			issueSync(rtOpts, ptpClock);
		}

		if (timerExpired(ANNOUNCE_INTERVAL_TIMER, ptpClock->itimer)) {
			DBGV("event ANNOUNCE_INTERVAL_TIMEOUT_EXPIRES\n");
			issueAnnounce(rtOpts, ptpClock);
		}

		if (timerExpired(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer)) {
			/* Record the data as missing */
			servo_missing_p2p_ts(&ptpClock->servo);

			/* Stop the response receipt timer and start the timer
			 * to issue the next peer delay request. */
			timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);
		}

		if ((ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) &&
		    timerExpired(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer)) {
			DBGV("event PDELAYREQ_INTERVAL_TIMEOUT_EXPIRES\n");
			issuePDelayReq(rtOpts,ptpClock);
		}

		if (ptpClock->slaveOnly ||
		    (ptpClock->clockQuality.clockClass == SLAVE_ONLY_CLOCK_CLASS))
			toState(PTPD_LISTENING, rtOpts, ptpClock);

		break;

	case PTPD_DISABLED:
		break;

	default:
		DBG("doTimerTick() unrecognized state\n");
		break;
	}
}

static Boolean
isFromCurrentParent(const PtpClock *ptpClock, const MsgHeader* header)
{
	return(!memcmp(
		ptpClock->parentPortIdentity.clockIdentity,
		header->sourcePortIdentity.clockIdentity,
		CLOCK_IDENTITY_LENGTH)	&& 
		(ptpClock->parentPortIdentity.portNumber ==
		 header->sourcePortIdentity.portNumber));
}

static bool checkACL(enum ptpd_acl_type acl_type,
		     struct in_addr address,
		     const char *name,
		     PtpInterface *ptpInterface,
		     InterfaceOpts *ifOpts,
		     acl_bitmap_t *checked,
		     acl_bitmap_t *passed) {

	Ipv4AccessList *acl = NULL;
	bool pass = false;

	if ((*checked & acl_type) != 0) {
		return ((*passed & acl_type) ? true : false);
	}

	switch (acl_type) {
	case PTPD_ACL_MANAGEMENT:
		acl = ptpInterface->transport.managementAcl;
		if (!ifOpts->managementAclEnabled) {
			pass = true;
			goto result;
		}
		break;
	case PTPD_ACL_TIMING:
		acl = ptpInterface->transport.timingAcl;
		if (!ifOpts->timingAclEnabled) {
			pass = true;
			goto result;
		}
		break;
	case PTPD_ACL_MONITORING:
		acl = ptpInterface->transport.monitoringAcl;
		if (!ifOpts->monitoringAclEnabled) {
			pass = true;
			goto result;
		}
		break;
	}

	if (acl == NULL) {
		ERROR("unknown ACL type %d\n", acl_type);
		/* Do not save the result because the input was nonsense */
		return false;
	}

	if (!matchIpv4AccessList(acl, ntohl(address.s_addr))) {
		if (name == NULL) {
			DBG("ACL type %d denied message from %s\n", acl_type, inet_ntoa(address));
		} else {
			DBG("ACL dropped %s from %s\n", inet_ntoa(address));
		}
	} else {
		if (name == NULL) {
			DBG("ACL type %d accepted message from %s\n", acl_type, inet_ntoa(address));
		} else {
			DBG2("ACL accepted %s from %s\n", inet_ntoa(address));
		}
		pass = true;
	}

 result:
	*checked |= acl_type;
	if (pass) {
		*passed |= acl_type;
	}

	return pass;
}


static bool checkACLmask(acl_bitmap_t mask,
			 struct in_addr address,
			 PtpInterface *ptpInterface,
			 InterfaceOpts *ifOpts,
			 acl_bitmap_t *checked,
			 acl_bitmap_t *passed) {

	int i;
	acl_bitmap_t bit;

	for (i = 0; mask != 0; i++) {
		bit = mask & (1 << i);
		if (bit != 0) {
			if (!checkACL(bit,
				      address,
				      NULL,
				      ptpInterface,
				      ifOpts,
				      checked,
				      passed)) {
				return false;
			}
			mask &= ~bit;
		}
	}

	return true;
}


static void
processMessage(InterfaceOpts *ifOpts, PtpInterface *ptpInterface, struct sfptpd_timespec *timestamp,
	       Boolean timestampValid, UInteger32 rxPhysIfindex, ssize_t length)
{
	PtpClock *port;
	ssize_t unpack_result;
	acl_bitmap_t acls_checked = 0;
	acl_bitmap_t acls_passed = 0;

	if (length < PTPD_HEADER_LENGTH) {
		DBG("message shorter than header length (%zd, %d)\n",
		    length, PTPD_HEADER_LENGTH);
		ptpInterface->counters.messageFormatErrors++;
		return;
	}

	unpack_result = msgUnpackHeader(ptpInterface->msgIbuf, length, &ptpInterface->msgTmpHeader);
	if (!UNPACK_OK(unpack_result)) {
		ERROR("unpacking header\n");
		ptpInterface->counters.messageFormatErrors++;
		return;
	}

	/* If the packet is not from us and is from a non-zero source address
	 * check ACLs */
	if (ptpInterface->transport.lastRecvAddrLen != 0 &&
	    !hostAddressesEqual(&ptpInterface->transport.lastRecvAddr,
				ptpInterface->transport.lastRecvAddrLen,
				&ptpInterface->transport.interfaceAddr,
				ptpInterface->transport.interfaceAddrLen)) {

		struct sockaddr_in *in = ((struct sockaddr_in *) &ptpInterface->transport.lastRecvAddr);
		ptpInterface->transport.lastRecvHost[0] = '\0';
		getnameinfo((struct sockaddr *) &ptpInterface->transport.lastRecvAddr,
			    ptpInterface->transport.lastRecvAddrLen,
			    ptpInterface->transport.lastRecvHost,
			    sizeof ptpInterface->transport.lastRecvHost,
			    NULL, 0, NI_NUMERICHOST);

		if (ptpInterface->msgTmpHeader.messageType == PTPD_MSG_MANAGEMENT) {
			if (!checkACL(PTPD_ACL_MANAGEMENT, in->sin_addr,
				      "management message",
				      ptpInterface, ifOpts, &acls_checked, &acls_passed)) {
				ptpInterface->counters.aclManagementDiscardedMessages++;
				return;
			}
	        } else if (ifOpts->timingAclEnabled) {
			if (!checkACL(PTPD_ACL_TIMING, in->sin_addr,
				      "timing message",
				      ptpInterface, ifOpts, &acls_checked, &acls_passed)) {
				ptpInterface->counters.aclTimingDiscardedMessages++;
				return;
			}
		}
	}

	if (ptpInterface->msgTmpHeader.versionPTP != PTPD_PROTOCOL_VERSION) {
		DBG2("ignore version %d message\n",
		     ptpInterface->msgTmpHeader.versionPTP);
		ptpInterface->counters.discardedMessages++;
		ptpInterface->counters.versionMismatchErrors++;
		return;
	}


	PtpClock *monitoringPort = NULL;
	for (port = ptpInterface->ports; port; port = port->next) {
		if (port->rtOpts.node_type == PTPD_NODE_MONITOR)
			monitoringPort = port;
		if (port->domainNumber == ptpInterface->msgTmpHeader.domainNumber)
			break;
	}

	/* Divert any traffic for unhandled domains to the monitoring
	   port if one is defined. */
	if (port == NULL && monitoringPort != NULL)
		port = monitoringPort;

	if (port != NULL) {
		DBG2("delivering message from %s for domain %d to port %d (instance %s)\n",
		     ptpInterface->ifOpts.ifaceName,
		     ptpInterface->msgTmpHeader.domainNumber,
		     port->portIdentity.portNumber,
		     port->rtOpts.name);
		processPortMessage(&port->rtOpts, port, timestamp,
				   timestampValid, rxPhysIfindex, length,
				   UNPACK_GET_SIZE(unpack_result), acls_checked, acls_passed);
	} else {
		DBG2("ignoring message from %s for unhandled domainNumber %d\n",
		     ptpInterface->ifOpts.ifaceName,
		     ptpInterface->msgTmpHeader.domainNumber);
		if (ptpInterface->msgTmpHeader.messageType == PTPD_MSG_ANNOUNCE ||
		    ptpInterface->msgTmpHeader.messageType == PTPD_MSG_DELAY_REQ)
			statsAddNode(ptpInterface->msgIbuf, &ptpInterface->msgTmpHeader, ptpInterface);

		ptpInterface->counters.discardedMessages++;
		ptpInterface->counters.domainMismatchErrors++;
		return;
	}

	if (ifOpts->displayPackets)
		msgDump(ptpInterface);
}

static void
processPortMessage(RunTimeOpts *rtOpts, PtpClock *ptpClock,
		   struct sfptpd_timespec *timestamp, Boolean timestampValid,
		   UInteger32 rxPhysIfindex, ssize_t length, int offset,
		   acl_bitmap_t acls_checked, acl_bitmap_t acls_passed)
{
	PtpInterface *ptpInterface;
	size_t safe_length = 0;
	Boolean isFromSelf;
	ssize_t unpack_result;

	assert(ptpClock);
	assert(ptpClock->interface);
	assert(ptpClock->interface->msgTmpHeader.domainNumber == ptpClock->domainNumber ||
	       rtOpts->node_type == PTPD_NODE_MONITOR);

	ptpInterface = ptpClock->interface;

	/* Clear transient state */
	memset(&ptpClock->transient_packet_state, '\0', sizeof ptpClock->transient_packet_state);

	/* Define a length that is the shorter of that received and that claimed
	   so that bogus fields are not decoded */
	safe_length = length;

	if (ptpInterface->msgTmpHeader.messageLength < length) {
		safe_length = ptpInterface->msgTmpHeader.messageLength;

		if (length - safe_length != 2 ||
		    rtOpts->ifOpts->transportAF != AF_INET6) {
			/* For IPv6 transport (Annex E) there should be
			   a spare pair of bytes at the end of the message
			   but some devices don't include this so we couldn't
			   strip them in the transport layer, so we ignore
			   them at this point instead. */

			TRACE_L4("message received with surplus bytes (%d < %d)\n",
				 ptpInterface->msgTmpHeader.messageLength, length);
		}
	}

	if(length < ptpInterface->msgTmpHeader.messageLength) {
		ERROR("message shorter than claimed in header (%d < %d)\n",
		      length, ptpInterface->msgTmpHeader.messageLength);

		ptpClock->counters.messageFormatErrors++;

		/* This is known to happen in the wild. Don't bow out yet
		   because there are now guards on the unpacking functions. */
	}

	/*
	 * Make sure we use the TAI to UTC offset specified if the master is
	 * sending the UTC_VALID bit.
	 *
	 * On the slave, all timestamps that we handle here have been collected
	 * by our local clock (loopback+kernel-level timestamp). This includes
	 * delayReq just send, and delayResp, when it arrives.
	 *
	 * These are then adjusted to the same timebase of the Master (+35 leap
	 * seconds, as of July 2012)
	 *
	 * NOTE We only apply the UTC offset if we are a slave, otherwise the
	 * master can't correctly signal the TAI plus offset to a slave.
	 */
	DBGV("__UTC_offset: %d %d \n",
	     ptpClock->timePropertiesDS.currentUtcOffsetValid,
	     ptpClock->timePropertiesDS.currentUtcOffset);

	/* Apply UTC offset if appropriate */
	if (timestampValid)
		applyUtcOffset(timestamp, rtOpts, ptpClock);

	/*Spec 9.5.2.2*/
	isFromSelf = (ptpClock->portIdentity.portNumber == ptpInterface->msgTmpHeader.sourcePortIdentity.portNumber
		      && !memcmp(ptpInterface->msgTmpHeader.sourcePortIdentity.clockIdentity, ptpClock->portIdentity.clockIdentity, CLOCK_IDENTITY_LENGTH));

	if (isFromSelf) {
		struct sfptpd_ts_ticket ticket;
		struct sfptpd_ts_user user;

		ticket = netMatchPacketToTsCache(&ptpInterface->ts_cache,
						 &user,
						 ptpInterface->msgIbuf, length);
		processTxTimestamp(ptpInterface, user, ticket, *timestamp);

		/* Looped-back packets need no further processing */
		return;
	}

	/* subtract the inbound latency adjustment */
	if (timestampValid && timestamp->sec > 0)
		sfptpd_time_subtract(timestamp, timestamp, &rtOpts->inboundLatency);

	unpack_result = unpackPortMessage(ptpClock,
					  safe_length);
	if (UNPACK_OK(unpack_result)) {

		if (processTLVs(rtOpts, ptpClock,
				offset, unpack_result,
				safe_length,
				timestamp, timestampValid,
				acls_checked, acls_passed)) {

			handleMessage(rtOpts,
				      ptpClock,
				      safe_length,
				      timestamp, timestampValid,
				      rxPhysIfindex);
		}
	}
}

static ssize_t
unpackPortMessage(PtpClock *ptpClock, ssize_t safe_length)
{
	PtpInterface *ptpInterface;
	ssize_t unpack_result = UNPACK_INIT;

	assert(ptpClock);
	assert(ptpClock->interface);

	ptpInterface = ptpClock->interface;

	/* Message unpacking and diagnostics */
	switch (ptpInterface->msgTmpHeader.messageType) {
	case PTPD_MSG_ANNOUNCE:
		DBG("      ==> Announce received\n");
		unpack_result = msgUnpackAnnounce(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.announce);
		break;
	case PTPD_MSG_SYNC:
		DBG("      ==> Sync received\n");
		unpack_result = msgUnpackSync(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.sync);
		break;
	case PTPD_MSG_FOLLOW_UP:
		DBG("      ==> FollowUp received\n");
		unpack_result = msgUnpackFollowUp(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.follow);
		break;
	case PTPD_MSG_DELAY_REQ:
		DBG("      ==> DelayReq received\n");
		unpack_result = msgUnpackDelayReq(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.req);
		break;
	case PTPD_MSG_PDELAY_REQ:
		DBG("      ==> PDelayReq received\n");
		unpack_result = msgUnpackPDelayReq(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.preq);
		break;
	case PTPD_MSG_DELAY_RESP:
		DBG("      ==> DelayResp received\n");
		unpack_result = msgUnpackDelayResp(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.resp);
		break;
	case PTPD_MSG_PDELAY_RESP:
		DBG("      ==> PDelayResp received\n");
		unpack_result = msgUnpackPDelayResp(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.presp);
		break;
	case PTPD_MSG_PDELAY_RESP_FOLLOW_UP:
		DBG("      ==> PDelayRespFollowUp received\n");
		unpack_result = msgUnpackPDelayRespFollowUp(ptpInterface->msgIbuf, safe_length,
							    &ptpInterface->msgTmp.prespfollow);
		break;
	case PTPD_MSG_MANAGEMENT:
		DBG("      ==> Management received\n");
		unpack_result = msgUnpackManagement(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.manage,
						    &ptpInterface->msgTmpHeader, ptpClock);
		break;
	case PTPD_MSG_SIGNALING:
		DBG("      ==> Signaling received\n");
		unpack_result = msgUnpackSignaling(ptpInterface->msgIbuf, safe_length, &ptpInterface->msgTmp.signaling,
						   &ptpInterface->msgTmpHeader, ptpClock);
		break;
	default:
		DBG("handle: unrecognized message\n");
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.unknownMessages++;
		return UNPACK_ERROR;
	}

	if (!UNPACK_OK(unpack_result)) {
		ERROR("underrun unpacking message\n");
		ptpClock->counters.messageFormatErrors++;
	}

	return unpack_result;
}

static bool
processTLVs(RunTimeOpts *rtOpts, PtpClock *ptpClock, int payload_offset,
	    ssize_t  unpack_result, ssize_t safe_length,
	    struct sfptpd_timespec *timestamp, Boolean timestampValid,
	    acl_bitmap_t acls_checked, acl_bitmap_t acls_passed)
{
	PtpInterface *ptpInterface;
	enum ptpd_tlv_result all_tlvs_result = PTPD_TLV_RESULT_CONTINUE;
	enum ptpd_tlv_result tlv_result;
	struct tlv_dispatch_info tlvs[MAX_TLVS];
	int num_tlvs = 0;
	off_t tlv_offset;
	int offset;
	int i;

	assert(ptpClock);
	assert(ptpClock->interface);

	ptpInterface = ptpClock->interface;

	/* Handle TLVs before processing the functional message.
	   Do this for all but management messages.
	 */
	offset = payload_offset + UNPACK_GET_SIZE(unpack_result);
	while (ptpInterface->msgTmpHeader.messageType != PTPD_MSG_MANAGEMENT &&
	       offset < safe_length) {
		TLV tlv;
		const struct tlv_handling *handler = NULL;
		bool org_ext;
		UInteger24 oui;
		UInteger24 org_subtype;

		/* Unpack the current TLV header, but not the content
		   since we may well be ignoring it. */
		unpack_result = msgUnpackTLVHeader(ptpInterface->msgIbuf + offset,
						   safe_length - offset,
						   &tlv,
						   ptpClock);

		if (!UNPACK_OK(unpack_result)) {
			if (tlv.tlvType == 0) {
				/* If we started unpacking the reserved TLV type 0
				 * then in practice this is just padding, so move on. */
				break;
			}
			ERROR("ptp %s, underrun unpacking tlv header\n", rtOpts->name);
			ptpClock->counters.messageFormatErrors++;
			return false;
		}

		/* Move past TLV header */
		tlv_offset = offset;
		offset += UNPACK_GET_SIZE(unpack_result);
		if (tlv.lengthField > safe_length - offset) {
			ERROR("ptp %s: underrun unpacking tlv contents (type 0x%04X, length %d, space %d\n",
			      rtOpts->name, tlv.tlvType, tlv.lengthField, safe_length - offset);
			ptpClock->counters.messageFormatErrors++;
			return false;
		}

		org_ext = (tlv.tlvType == PTPD_TLV_ORGANIZATION_EXTENSION ||
			   tlv.tlvType == PTPD_TLV_ORGANIZATION_EXTENSION_FORWARDING ||
			   tlv.tlvType == PTPD_TLV_ORGANIZATION_EXTENSION_NON_FORWARDING);

		if (org_ext) {
			unpack_result = msgUnpackOrgTLVSubHeader(ptpInterface->msgIbuf + offset,
								 safe_length - offset,
								 &oui, &org_subtype, ptpClock);
			if (!UNPACK_OK(unpack_result)) {
				ERROR("ptp %s: underrun unpacking org tlv subheader\n", rtOpts->name);
				ptpClock->counters.messageFormatErrors++;
				return false;
			}
			offset += UNPACK_GET_SIZE(unpack_result);
		}

		/* Look for a handler for this TLV type */
		for (i = 0; i < sizeof tlv_handlers / sizeof *tlv_handlers; i++) {
			if (tlv_handlers[i].tlv_type == tlv.tlvType) {
				if (!org_ext ||
				    (oui == tlv_handlers[i].organization_id &&
				     org_subtype == tlv_handlers[i].organization_sub_type)) {
					handler = &tlv_handlers[i];
					break;
				}
			}
		}

		if (handler != NULL) {
			struct sockaddr_in *in;
			bool pass;

			in = (struct sockaddr_in *) &ptpInterface->transport.lastRecvAddr;

			pass = checkACLmask(handler->required_acl_types_mask, in->sin_addr,
					    ptpInterface, &ptpInterface->ifOpts,
					    &acls_checked, &acls_passed);
			if (pass) {
				if (((1 << ptpInterface->msgTmpHeader.messageType) &
				     handler->permitted_message_types_mask) != 0) {
					DBG("ptp %s: handling %s TLV\n",
					    rtOpts->name, handler->name);

					if (num_tlvs == MAX_TLVS) {
						ERROR("ptp %s: too many TLVs in message (>%d), dropping message\n",
						      rtOpts->name, MAX_TLVS);
						ptpClock->counters.discardedMessages++;
						return false;
					}

					/* Point the payload to after the header or
					 * organization extension subheader if present */
					tlv.valueField = ptpInterface->msgIbuf + offset;

					tlvs[num_tlvs].tlv = tlv;
					tlvs[num_tlvs].tlv_offset = tlv_offset;
					tlvs[num_tlvs].handler = handler;

					/* Adjust payload length in saved TLV object to
					 * remove organization extension subheader */
					if (org_ext) {
						tlvs[num_tlvs].tlv.lengthField -= 6;
					}

					num_tlvs++;

					if (handler->pass1_handler_fn != NULL) {
						tlv_result = handler->pass1_handler_fn(&ptpInterface->msgTmpHeader,
										       safe_length,
										       timestamp,
										       timestampValid,
										       rtOpts,
										       ptpClock,
										       &tlv,
										       tlv_offset);
					} else {
						tlv_result = PTPD_TLV_RESULT_CONTINUE;
					}

					if (tlv_result == PTPD_TLV_RESULT_ERROR) {
						ERROR("ptp %s: stopped processing packet after error result from TLV handler %s\n",
						      rtOpts->name, handler->name);
						return false;
					} else if (tlv_result == PTPD_TLV_RESULT_DROP) {
						DBGV("ptp %s: %s TLV overrides normal processing of this message\n",
						     rtOpts->name, handler->name);
						all_tlvs_result = PTPD_TLV_RESULT_DROP;
					}
				} else {
					WARNING("ptp %s: %s TLV irrelevant for message type 0x%x\n",
						rtOpts->name, handler->name,
						ptpInterface->msgTmpHeader.messageType);
				}
			} else {
				WARNING("ptp %s: ignoring %s TLV from source excluded "
					"by the access control list \n",
					rtOpts->name, handler->name);
			}
		} else {
			/* Common usage includes padding with the zero value
			 * even though this strictly refers to a reserved TLV.
			 * Just ignore it. */
			if (tlv.tlvType != 0) {
				DBG("ptp %s: ignoring unhandled TLV type 0x%04X\n",
				    rtOpts->name, tlv.tlvType);
			}
		}

		/* Go to next TLV */
		offset += tlv.lengthField;
	}

	/* Bail out if any TLV handers required processing to stop. */
	if (all_tlvs_result != PTPD_TLV_RESULT_CONTINUE) {
		return false;
	}

	/* Do a second pass of the TLV handlers, so that they can act on the
	   presence of each other. */
	for (i = 0; i < num_tlvs; i++) {
		struct tlv_dispatch_info *entry = &tlvs[i];
		if (entry->handler->pass2_handler_fn != NULL) {
			tlv_result = entry->handler->pass2_handler_fn(&ptpInterface->msgTmpHeader,
								     safe_length,
								     timestamp,
								     timestampValid,
								     rtOpts,
								     ptpClock,
								     &entry->tlv,
								     entry->tlv_offset);
		} else {
			tlv_result = PTPD_TLV_RESULT_CONTINUE;
		}

		if (tlv_result == PTPD_TLV_RESULT_ERROR) {
			ERROR("ptp %s: stopped processing packet after error result from TLV handler %s\n",
			      rtOpts->name, entry->handler->name);
			return false;
		} else if (tlv_result == PTPD_TLV_RESULT_DROP) {
			DBGV("ptp %s: %s TLV overrides normal processing of this message\n",
			     rtOpts->name, entry->handler->name);
			all_tlvs_result = PTPD_TLV_RESULT_DROP;
		}
	}

	/* Bail out if any TLV handers required processing to stop. */
	if (all_tlvs_result != PTPD_TLV_RESULT_CONTINUE) {
		return false;
	}

	return true;
}

static void
handleMessage(RunTimeOpts *rtOpts, PtpClock *ptpClock,
	      ssize_t safe_length,
	      struct sfptpd_timespec *timestamp, Boolean timestampValid,
	      UInteger32 rxPhysIfindex)
{
	PtpInterface *ptpInterface;

	assert(ptpClock);
	assert(ptpClock->interface);

	ptpInterface = ptpClock->interface;

	/* Handle the message
	 *
	 *  on the table below, note that only the event messsages are passed the local time,
	 *  (collected by us by loopback+kernel TS, and adjusted with UTC seconds
	 *
	 *  (SYNC / DELAY_REQ / PDELAY_REQ / PDELAY_RESP)
	 */
	switch (ptpInterface->msgTmpHeader.messageType) {
	case PTPD_MSG_ANNOUNCE:
		handleAnnounce(&ptpInterface->msgTmpHeader, safe_length,
			       rtOpts, ptpClock);
		break;
	case PTPD_MSG_SYNC:
		handleSync(&ptpInterface->msgTmpHeader, safe_length,
			   timestamp, timestampValid, rxPhysIfindex,
			   rtOpts, ptpClock);
		break;
	case PTPD_MSG_FOLLOW_UP:
		handleFollowUp(&ptpInterface->msgTmpHeader, safe_length,
                               &ptpInterface->msgTmp.follow,
			       FALSE, rtOpts, ptpClock);
		break;
	case PTPD_MSG_DELAY_REQ:
		handleDelayReq(&ptpInterface->msgTmpHeader, safe_length,
			       timestamp, timestampValid,
			       rtOpts, ptpClock);
		break;
	case PTPD_MSG_PDELAY_REQ:
		handlePDelayReq(&ptpInterface->msgTmpHeader, safe_length,
				timestamp, timestampValid,
				rtOpts, ptpClock);
		break;
	case PTPD_MSG_DELAY_RESP:
		handleDelayResp(&ptpInterface->msgTmpHeader, safe_length,
				rtOpts, ptpClock);
		break;
	case PTPD_MSG_PDELAY_RESP:
		handlePDelayResp(&ptpInterface->msgTmpHeader, safe_length,
				 timestamp, timestampValid,
				 rtOpts, ptpClock);
		break;
	case PTPD_MSG_PDELAY_RESP_FOLLOW_UP:
		handlePDelayRespFollowUp(&ptpInterface->msgTmpHeader, safe_length,
					 rtOpts, ptpClock);
		break;
	case PTPD_MSG_MANAGEMENT:
		handleManagement(&ptpInterface->msgTmpHeader, safe_length,
				 rtOpts, ptpClock);
		break;
	case PTPD_MSG_SIGNALING:
		handleSignaling(ptpClock);
		break;
	}
}

static void
processTxTimestamp(PtpInterface *interface,
		   struct sfptpd_ts_user ts_user,
		   struct sfptpd_ts_ticket ts_ticket,
		   struct sfptpd_timespec timestamp)
{
	struct sfptpd_ts_ticket check_ticket;
	PtpClock *ptpClock = ts_user.port;
	uint16_t check_seq;
	bool match = true;
	char desc[48];

	assert(ptpClock);

	if (ts_ticket.slot == TS_NULL_TICKET.slot) {
		WARNING("ptpd: tx timestamp received without matching packet\n");
		return;
	}

	formatTsPkt(&ts_user, desc);

	switch (ts_user.type) {
	case TS_SYNC:
		check_ticket = ptpClock->sync_ticket;
		/* "sent id" field is actually the next one... */
		check_seq = ptpClock->sentSyncSequenceId - 1;
		break;
	case TS_DELAY_REQ:
		check_ticket = ptpClock->delayreq_ticket;
		/* "sent id" field is actually the next one... */
		check_seq = ptpClock->sentDelayReqSequenceId - 1;
		break;
	case TS_PDELAY_REQ:
		check_ticket = ptpClock->pdelayreq_ticket;
		/* "sent id" field is actually the next one... */
		check_seq = ptpClock->sentPDelayReqSequenceId - 1;
		break;
	case TS_PDELAY_RESP:
		check_ticket = ptpClock->pdelayresp_ticket;
		/* Non-stateful; alway succeed; answer sender */
		check_seq = ts_user.seq_id;
		break;
	case TS_MONITORING_SYNC:
		check_ticket = ptpClock->monsync_ticket;
		/* Non-stateful; alway succeed; answer sender */
		check_seq = ts_user.seq_id;
		break;
	default:
		match = false;
		check_ticket.slot = TS_CACHE_SIZE;
		check_ticket.seq = 0;
		check_seq = 0;
	}

	if (match)
		match = (ts_ticket.slot == check_ticket.slot &&
			 ts_ticket.seq == check_ticket.seq &&
			 ts_user.seq_id == check_seq);

	if (!match) {
		WARNING("ptp: discarding non-matching %s timestamp"
			"(ts %" PRIu32 ", slot %d, seq %" PRIu16 ") != "
			"(%" PRIu32 ",%d,%" PRIu16 ")\n",
			desc,
			ts_ticket.seq, ts_ticket.slot, ts_user.seq_id,
			check_ticket.seq, check_ticket.slot, check_seq);
		return;
	}

	SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_TX_TIMESTAMPS);

	/* Apply UTC offset to convert timestamp to TAI if appropriate. */
	applyUtcOffset(&timestamp, &ptpClock->rtOpts, ptpClock);

	switch (ts_user.type) {
	case TS_SYNC:
		processSyncFromSelf(&timestamp, &ptpClock->rtOpts, ptpClock,
				    ts_user.seq_id);
		ptpClock->sync_ticket = TS_NULL_TICKET;
		break;
	case TS_DELAY_REQ:
		processDelayReqFromSelf(&timestamp, &ptpClock->rtOpts,
					ptpClock);
		ptpClock->delayreq_ticket = TS_NULL_TICKET;
		break;
	case TS_PDELAY_REQ:
		processPDelayReqFromSelf(&timestamp, &ptpClock->rtOpts,
					 ptpClock);
		ptpClock->pdelayreq_ticket = TS_NULL_TICKET;
		break;
	case TS_PDELAY_RESP:
		processPDelayRespFromSelf(&timestamp, &ptpClock->rtOpts,
					  ptpClock, ts_user.seq_id);
		ptpClock->pdelayresp_ticket = TS_NULL_TICKET;
		break;
	case TS_MONITORING_SYNC:
		processMonitoringSyncFromSelf(&timestamp, &ptpClock->rtOpts, ptpClock,
					      ts_user.seq_id);
		ptpClock->monsync_ticket = TS_NULL_TICKET;
		break;
	}
}

/* Check and handle received messages */
void
doHandleSockets(InterfaceOpts *ifOpts, PtpInterface *ptpInterface,
		Boolean event, Boolean general, Boolean error)
{
	struct sfptpd_ts_ticket ts_ticket = TS_NULL_TICKET;
	struct sfptpd_ts_info ts_info;
	struct sfptpd_ts_user ts_user;
	ssize_t length;

	while (error) {
		length = netRecvError(ptpInterface);
		if (length == -EAGAIN || length == -EINTR) {
			/* No more messges to read on error queue */
			error = false;
		} else if (length < 0) {
			/* TODO: add stat */
			ERROR("ptp: error reading socket error queue, %s\n",
			      strerror(-length));
			error = false;
		} else {
			netProcessError(ptpInterface, length, &ts_user, &ts_ticket, &ts_info);
			if (is_suitable_timestamp(ptpInterface, &ts_info))
				processTxTimestamp(ptpInterface, ts_user, ts_ticket,
					           *get_suitable_timestamp(ptpInterface, &ts_info));
			else
				WARNING("ptp: ignoring unsuitable timestamp type\n");
		}
	}

	if (event) {
		length = netRecvEvent(ptpInterface->msgIbuf, ptpInterface, &ts_info);
		if (length < 0) {
			PERROR("failed to receive on the event socket\n");
			toStateAllPorts(PTPD_FAULTY, ptpInterface);
			ptpInterface->counters.messageRecvErrors++;
			return;
		}

		if (length > 0) {
			processMessage(ifOpts, ptpInterface,
				       get_suitable_timestamp(ptpInterface, &ts_info),
				       is_suitable_timestamp(ptpInterface, &ts_info),
				       ts_info.if_index,
				       length);
		}
	}

	if (general) {
		length = netRecvGeneral(ptpInterface->msgIbuf, &ptpInterface->transport);
		if (length < 0) {
			PERROR("failed to receive on the general socket\n");
			toStateAllPorts(PTPD_FAULTY, ptpInterface);
			ptpInterface->counters.messageRecvErrors++;
			return;
		}

		if (length > 0)
			processMessage(ifOpts, ptpInterface, NULL, FALSE, 0, length);
	}
}


/*spec 9.5.3*/
static void
handleAnnounce(MsgHeader *header, ssize_t length,
	       RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("HandleAnnounce : Announce message received : \n");

	if (length < PTPD_ANNOUNCE_LENGTH) {
		DBG("Error: Announce message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	statsAddNode(ptpClock->interface->msgIbuf,header,ptpClock->interface);

	if (rtOpts->requireUtcValid && !IS_SET(header->flagField1, PTPD_UTCV)) {
		ptpClock->counters.ignoredAnnounce++;
		return;
	}

	switch (ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
		DBG("HandleAnnounce : disregard \n");
		ptpClock->counters.discardedMessages++;
		break;

	case PTPD_UNCALIBRATED:
	case PTPD_SLAVE:
		/*
		 * Valid announce message is received : BMC algorithm
		 * will be executed
		 */
		ptpClock->record_update = TRUE;

		if (isFromCurrentParent(ptpClock, header)) {
			/* update current master in the fmr as well */
			addForeign(ptpClock->interface->msgIbuf, length, header, ptpClock);

			/* If this is the first announce after a leap second,
			 * clear the leap second flags.
			 * Note that we must be this before running the BMC
			 * algorithm as this can signal a leap second again.
			 */
			if(ptpClock->leapSecondWaitingForAnnounce) {
				ptpClock->leapSecondInProgress = FALSE;
				ptpClock->leapSecondWaitingForAnnounce = FALSE;
				ptpClock->timePropertiesDS.leap59 = FALSE;
				ptpClock->timePropertiesDS.leap61 = FALSE;
			}

			DBG2("___ Announce: received Announce from current Master, so reset the Announce timer\n");
			/*Reset Timer handling Announce receipt timeout*/
			timerStart(ANNOUNCE_RECEIPT_TIMER,
				   (ptpClock->announceReceiptTimeout * powl(2,ptpClock->logAnnounceInterval)),
				   ptpClock->itimer);
		} else {
	   		/*addForeign takes care of AnnounceUnpacking*/
			/* the actual decision to change masters is only done in  doState() / record_update == TRUE / bmc() */

			/* the original code always called: addforeign(new master) + timerstart(announce) */

			addForeign(ptpClock->interface->msgIbuf,length, header, ptpClock);
		}
		break;

	/*
	 * Passive case: previously, this was handled in the default, just
	 * like the master case. This the announce would call addForeign(),
	 * but NOT reset the timer, so after 12s it would expire and we
	 * would come alive periodically.
	 */
	case PTPD_PASSIVE:
		/*
		 * Valid announce message is received : BMC algorithm
		 * will be executed
		 */
		ptpClock->record_update = TRUE;

		if (isFromCurrentParent(ptpClock, header)) {
			/* Update the foreign master records */
			addForeign(ptpClock->interface->msgIbuf, length, header, ptpClock);

			DBG("___ Announce: received Announce from current Master, so reset the Announce timer\n\n");
			/*Reset Timer handling Announce receipt timeout*/
			timerStart(ANNOUNCE_RECEIPT_TIMER,
				   (ptpClock->announceReceiptTimeout * powl(2,ptpClock->logAnnounceInterval)),
				   ptpClock->itimer);
		} else {
			/* the actual decision to change masters is only done in  doState() / record_update == TRUE / bmc() */
			/* the original code always called: addforeign(new master) + timerstart(announce) */
			DBG("___ Announce: received Announce from another master, will add to the list, as it might be better\n\n");
			DBGV("this is to be decided immediatly by bmc())\n\n");
			addForeign(ptpClock->interface->msgIbuf, length, header, ptpClock);
		}
		break;

	case PTPD_MASTER:
	case PTPD_LISTENING:			/* listening mode still causes timeouts in order to send IGMP refreshes */
		DBGV("Announce message from another foreign master\n");
		addForeign(ptpClock->interface->msgIbuf, length, header, ptpClock);
		ptpd_update_announce_interval(ptpClock, rtOpts);
		ptpClock->record_update = TRUE;		/* run BMC() as soon as possible */
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	} /* switch on (port_state) */

	ptpClock->counters.announceMessagesReceived++;
}


static void
handleSync(const MsgHeader *header, ssize_t length,
	   struct sfptpd_timespec *time, Boolean timestampValid,
	   UInteger32 rxPhysIfindex,
	   RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	Integer8 msgInterval;

	DBGV("Sync message received : \n");

	if (length < PTPD_SYNC_LENGTH) {
		DBG("Error: Sync message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	/* Record all foreign Sync messages when BMC discriminator in use */
	if (timestampValid) {
		recordForeignSync(header, ptpClock, time);
	}

	switch(ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_LISTENING:
		DBGV("HandleSync : disregard \n");
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.syncMessagesReceived++;
		break;

	case PTPD_UNCALIBRATED:
	case PTPD_SLAVE:
		if (isFromCurrentParent(ptpClock, header)) {
			if (!timestampValid) {
				/* We didn't get a timestamp for this message.
				 * Set the receive timestamp alarm and terminate. */
				SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
				ptpClock->counters.rxPktNoTimestamp++;
				WARNING("ptp %s: received Sync with no timestamp\n", rtOpts->name);
				break;
			}

			/* If the ifindex is valid, then store it to be used later */
			if (rxPhysIfindex != 0 &&
			    rxPhysIfindex != ptpClock->lastSyncIfindex) {
				ptpClock->lastSyncIfindex = rxPhysIfindex;
			}

			/* Clear the RX timestamp alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_RX_TIMESTAMPS);

			/* We only start our own delayReq timer after receiving the first sync */
			if (ptpClock->waiting_for_first_sync) {
				ptpClock->waiting_for_first_sync = FALSE;
				INFO("ptp %s: received first Sync from Master\n", rtOpts->name);

				if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_E2E) {
					timerStart(DELAYREQ_INTERVAL_TIMER, 
						   powl(2,ptpClock->logMinDelayReqInterval),
						   ptpClock->itimer);
				}
			}

			/* Test Function: Packet timestamp - bad timestamp */
			if ((rtOpts->test.bad_timestamp.type != BAD_TIMESTAMP_TYPE_OFF)&& 
			    ((header->sequenceId % rtOpts->test.bad_timestamp.interval_pkts) == 0)) {
				Integer32 jitter = (Integer32)((getRand() - 0.5) * 2.0 *
						               rtOpts->test.bad_timestamp.max_jitter);
				time->nsec += jitter;
				sfptpd_time_normalise(time);
				INFO("ptp %s: added jitter %d to sync RX timestamp\n",
				     rtOpts->name, jitter);
			}

			ptpClock->sync_receive_time = *time;

			/* We have received a sync so clear the Sync packet alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_SYNC_PKTS);

			/* If we're waiting for a follow up and we get another 
			 * sync message we consider this a follow up timeout */
			if (ptpClock->waitingForFollow) {
				SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_FOLLOW_UPS);
				ptpClock->counters.followUpTimeouts++;
				
				/* Record the fact that the data is missing. */
				servo_missing_m2s_ts(&ptpClock->servo);
				
				WARNING("ptp %s: failed to receive FollowUp for Sync sequence number %d\n",
					rtOpts->name,
					ptpClock->recvSyncSequenceId);
			}

			/* Test mode: emulate transparent clock */
			if (rtOpts->test.xparent_clock.enable) {
				sfptpd_time_t adj_fl;
				struct sfptpd_timespec adj_ts;
				sfptpd_time_fp16_t adj_sns;
				
				adj_fl = getRand() * rtOpts->test.xparent_clock.max_correction;
				sfptpd_time_float_ns_to_timespec(adj_fl, &adj_ts);
				adj_sns = sfptpd_time_float_ns_to_scaled_ns(adj_fl);

				ptpClock->interface->msgTmpHeader.correctionField += adj_sns;
				sfptpd_time_add(&ptpClock->sync_receive_time,
						&ptpClock->sync_receive_time,
						&adj_ts);

				INFO("ptp %s: added %0.3Lf ns to correction field of sync\n",
				     rtOpts->name, adj_fl);
			}

			/* Save the correctionField of Sync message */
			sfptpd_time_from_ns16(&ptpClock->sync_correction_field, header->correctionField);

			/* If the correction field is more than 1ns then infer
			 * that there is a transparent clock in the network */
			ptpClock->syncXparent
				= (header->correctionField >= 65536)? TRUE: FALSE;

			/* Store the sync message sequence ID */
			ptpClock->recvSyncSequenceId = header->sequenceId;

			if ((header->flagField0 & PTPD_FLAG_TWO_STEP) != 0) {
				DBG2("HandleSync: waiting for follow-up \n");
				ptpClock->twoStepFlag = TRUE;
				ptpClock->waitingForFollow = TRUE;
			} else {
				ptpClock->twoStepFlag = FALSE;
				ptpClock->waitingForFollow = FALSE;

				toInternalTime(&ptpClock->sync_send_time,
					       &ptpClock->interface->msgTmp.sync.originTimestamp);

				/* Provide the new measurements to any ingress event monitors. */
				ingressEventMonitor(ptpClock, rtOpts);

				/* Provide the new measurements to the servo. */
				if (servo_provide_m2s_ts(&ptpClock->servo,
							 &ptpClock->sync_send_time,
							 &ptpClock->sync_receive_time,
							 &ptpClock->sync_correction_field)) {
					servo_update_clock(&ptpClock->servo);
				}
			}

			/* If the sync message interval is defined then update
			 * our copy */
			msgInterval = header->logMessageInterval;
			if (msgInterval != PTPD_MESSAGE_INTERVAL_UNDEFINED) {
				/* Saturate the interval such that it is within
				 * the range of values we can support */
				if (msgInterval < PTPD_SYNC_INTERVAL_MIN)
					msgInterval = PTPD_SYNC_INTERVAL_MIN;
				else if (msgInterval > PTPD_SYNC_INTERVAL_MAX)
					msgInterval = PTPD_SYNC_INTERVAL_MAX;

				/* Log a message if the interval has changed */
				if (ptpClock->logSyncInterval != msgInterval) {
					if (msgInterval != header->logMessageInterval) {
						WARNING("ptp %s: received out-of-range Sync interval "
							"%d from master (was %d, using %d)\n",
							rtOpts->name,
							header->logMessageInterval,
							ptpClock->logSyncInterval, msgInterval);
					} else {
						INFO("ptp %s: received new Sync interval %d from "
						     "master (was %d)\n", rtOpts->name,
						     msgInterval, ptpClock->logSyncInterval);
					}

					ptpClock->logSyncInterval = msgInterval;

					/* Update the expected interval in the servo */
					servo_set_interval(&ptpClock->servo,
							   powl(2, msgInterval));
				}
			}

			/* Reset Timer handling Sync receipt timeout */
			ptpClock->sync_missing_next_warning =
				ptpClock->syncReceiptTimeout * powl(2,ptpClock->logSyncInterval);
			ptpClock->sync_missing_interval = 0.0;
			timerStart(SYNC_RECEIPT_TIMER,
				   ptpClock->sync_missing_next_warning,
				   ptpClock->itimer);

			/* If we previously received an out-of-order follow-up, try to process it now */
			if (ptpClock->outOfOrderFollowUpHeader.sequenceId != 0) {
				/* Only do this if the sequence number matches */
				if (ptpClock->outOfOrderFollowUpHeader.sequenceId == ptpClock->recvSyncSequenceId) {
					DBG("Handling out-of-order FollowUp %d\n", ptpClock->outOfOrderFollowUpHeader.sequenceId);
					handleFollowUp(&(ptpClock->outOfOrderFollowUpHeader), PTPD_FOLLOW_UP_LENGTH,
                                                       &(ptpClock->outOfOrderFollowUpPayload),
						       TRUE /*isDeferred*/,
						       rtOpts, ptpClock);
					ptpClock->counters.outOfOrderFollowUps++;
				} else {
					INFO("ptp %s: Discarding cached FollowUp with unexpected SequenceID %d\n",
					     rtOpts->name, ptpClock->outOfOrderFollowUpHeader.sequenceId);
					ptpClock->counters.discardedMessages++;
				}
				ptpClock->outOfOrderFollowUpHeader.sequenceId = 0;
			}
		} else {
			DBG("HandleSync: Sync message received from "
			     "another Master not our own \n");
			ptpClock->counters.discardedMessages++;
		}
		ptpClock->counters.syncMessagesReceived++;
		break;

	case PTPD_MASTER:
		DBGV("HandleSync: Sync message received from another Master\n");
		/* we are the master, but another is sending */
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.syncMessagesReceived++;
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}
}


static void
processSyncFromSelf(const struct sfptpd_timespec *time, RunTimeOpts *rtOpts,
		    PtpClock *ptpClock, const UInteger16 sequenceId)
{
	struct sfptpd_timespec timestamp;

	/* Add latency */
	sfptpd_time_add(&timestamp, time, &rtOpts->outboundLatency);

	/* Issue follow-up CORRESPONDING TO THIS SYNC */
	issueFollowup(&timestamp, rtOpts, ptpClock, sequenceId);
}


static void
processMonitoringSyncFromSelf(const struct sfptpd_timespec *time, RunTimeOpts *rtOpts,
			      PtpClock *ptpClock, const UInteger16 sequenceId)
{
	struct sfptpd_timespec timestamp;

	/* Add latency */
	sfptpd_time_add(&timestamp, time, &rtOpts->outboundLatency);

	/* Issue follow-up CORRESPONDING TO THIS SYNC */
	issueFollowupForMonitoring(&timestamp, rtOpts, ptpClock, sequenceId);
}


static void
handleFollowUp(const MsgHeader *header, ssize_t length,
               const MsgFollowUp *payload,
	       Boolean isDeferred,
               RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("HandleFollowUp : Follow up message received\n");

	if (length < PTPD_FOLLOW_UP_LENGTH) {
		DBG("Error: Follow Up message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	/* Record all foreign FollowUp messages when BMC discriminator in use.
	   In the case of out-of-order followups we have to pass in the cached
	   followup message otherwise it will incorrectly record the sync
	   message timestamps as followup timestamps instead.
	*/
	recordForeignFollowUp(header, ptpClock, payload);

	switch (ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_LISTENING:
		DBGV("Handfollowup : disregard \n");
		ptpClock->counters.discardedMessages++;
		break;

	case PTPD_UNCALIBRATED:
	case PTPD_SLAVE:

		if (isFromCurrentParent(ptpClock, header)) {
			/* If there is an old message in the cache, evict it */
			if (ptpClock->outOfOrderFollowUpHeader.sequenceId != 0 &&
			    !isDeferred) {
				DBG("Discarding cached followup %d, Slave was not waiting a follow up "
				    "message \n", ptpClock->outOfOrderFollowUpHeader.sequenceId);
				ptpClock->outOfOrderFollowUpHeader.sequenceId = 0;
				ptpClock->counters.discardedMessages++;
			}
		}

		if (!isFromCurrentParent(ptpClock, header)) {
			DBG2("Ignored, Follow up message is not from current parent \n");
			ptpClock->counters.discardedMessages++;
		} else if (!ptpClock->waitingForFollow) {
			/* Cache 1 follow-up in case we receive the sync out-of-order */
			DBGV("Caching out-of-order FollowUp %d\n", header->sequenceId);
			memcpy(&(ptpClock->outOfOrderFollowUpHeader), header, sizeof(MsgHeader));
			memcpy(&(ptpClock->outOfOrderFollowUpPayload), &ptpClock->interface->msgTmp.follow,
                                                                                        sizeof(MsgFollowUp));
		} else if (ptpClock->recvSyncSequenceId != header->sequenceId) {
			INFO("ptp %s: Ignored followup, SequenceID doesn't match with "
			     "last Sync message, expected %d, got %d\n", rtOpts->name,
			     ptpClock->recvSyncSequenceId, header->sequenceId);
			ptpClock->counters.sequenceMismatchErrors++;
		} else {
			/* We have received a Follow Up so clear the alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_FOLLOW_UPS);
			ptpClock->waitingForFollow = FALSE;

			toInternalTime(&ptpClock->sync_send_time,
				       &payload->preciseOriginTimestamp);

			/* Test mode: emulate transparent clock */
			if (rtOpts->test.xparent_clock.enable) {
				sfptpd_time_t adj_fl;
				struct sfptpd_timespec adj_ts;
				sfptpd_time_fp16_t adj_sns;
				
				adj_fl = getRand() * rtOpts->test.xparent_clock.max_correction;
				sfptpd_time_float_ns_to_timespec(adj_fl, &adj_ts);
				adj_sns = sfptpd_time_float_ns_to_scaled_ns(adj_fl);

				ptpClock->interface->msgTmpHeader.correctionField += adj_sns;
				sfptpd_time_add(&ptpClock->sync_receive_time,
						&ptpClock->sync_receive_time,
						&adj_ts);

				INFO("ptp %s: added %0.3Lf ns to correction field of follow up\n",
				     rtOpts->name, adj_fl);
			}

			ptpClock->followXparent
				= (header->correctionField >= 65536)? TRUE: FALSE;

			sfptpd_time_from_ns16(&ptpClock->sync_correction_field, header->correctionField);

			/* Provide the new measurements to any ingress event monitors. */
			ingressEventMonitor(ptpClock, rtOpts);

			/* Provide the new measurements to the servo. */
			if (servo_provide_m2s_ts(&ptpClock->servo,
						 &ptpClock->sync_send_time,
						 &ptpClock->sync_receive_time,
						 &ptpClock->sync_correction_field)) {
				servo_update_clock(&ptpClock->servo);
			}
		}
		break;

	case PTPD_MASTER:
	case PTPD_PASSIVE:
		DBGV("Ignored, Follow up message received from another master \n");
		ptpClock->counters.discardedMessages++;
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	} /* Switch on (port_state) */

	ptpClock->counters.followUpMessagesReceived++;
}


static void
handleDelayReq(const MsgHeader *header, ssize_t length,
	       struct sfptpd_timespec *time, Boolean timestampValid,
	       RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("delayReq message received : \n");

	if (length < PTPD_DELAY_REQ_LENGTH) {
		DBG("Error: DelayReq message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_DISABLED) {
		ptpClock->counters.discardedMessages++;
		return;
	} else if (ptpClock->delayMechanism != PTPD_DELAY_MECHANISM_E2E) {
		WARNING("ptp %s: unexpected DelayReq message in peer-to-peer mode \n", rtOpts->name);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayModeMismatchErrors++;
		return;
	}

	/* Record details of sender of message for logging */
	statsAddNode(ptpClock->interface->msgIbuf, &ptpClock->interface->msgTmpHeader, ptpClock->interface);

	switch(ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_UNCALIBRATED:
	case PTPD_LISTENING:
	case PTPD_PASSIVE:
		DBGV("HandledelayReq : disregard \n");
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayReqMessagesReceived++;
		break;

	case PTPD_SLAVE:
		DBG2("HandledelayReq : disregard delayreq from other client\n");
		ptpClock->counters.discardedMessages++;
		break;

	case PTPD_MASTER:
		if (!timestampValid) {
			/* We didn't get a receive timestamp for this message.
			 * Set the receive timestamp alarm */
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
			ptpClock->counters.rxPktNoTimestamp++;
			WARNING("ptp %s: received DelayReq with no timestamp\n", rtOpts->name);
		} else {
			/* Clear the RX timestamp alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_RX_TIMESTAMPS);

			if (!UNPACK_OK(msgUnpackHeader(ptpClock->interface->msgIbuf, length, &ptpClock->delayReqHeader))) {
				ERROR("unpacking delay request message\n");
				ptpClock->counters.messageFormatErrors++;
				return;
			}
			issueDelayResp(time, &ptpClock->delayReqHeader, rtOpts, ptpClock);
		}
		ptpClock->counters.delayReqMessagesReceived++;
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}
}


static void
processDelayReqFromSelf(const struct sfptpd_timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	ptpClock->waitingForDelayResp = TRUE;

	/* Provide the new measurements to any egress event monitors. */
	egressEventMonitor(ptpClock, rtOpts, PTPD_MSG_DELAY_REQ, time);

	/* Add latency */
	sfptpd_time_add(&ptpClock->delay_req_send_time, time, &rtOpts->outboundLatency);

	DBGV("processDelayReqFromSelf: seq# %d ts " SFPTPD_FMT_SFTIMESPEC "\n",
	     ptpClock->sentDelayReqSequenceId,
	     SFPTPD_ARGS_SFTIMESPEC(ptpClock->delay_req_send_time));
}


static void
handleDelayResp(const MsgHeader *header, ssize_t length,
		RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	Integer8 msgInterval;

	DBGV("delayResp message received : \n");
	
	if(length < PTPD_DELAY_RESP_LENGTH) {
		DBG("Error: DelayResp message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_DISABLED) {
		ptpClock->counters.discardedMessages++;
		return;
	} else if (ptpClock->delayMechanism != PTPD_DELAY_MECHANISM_E2E) {
		WARNING("ptp %s: unexpected DelayResp message in peer-to-peer mode\n", rtOpts->name);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayModeMismatchErrors++;
		return;
	}

	switch(ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_UNCALIBRATED:
	case PTPD_LISTENING:
		DBGV("HandledelayResp : disregard \n");
		ptpClock->counters.discardedMessages++;
		break;

	case PTPD_SLAVE:
		if ((memcmp(ptpClock->portIdentity.clockIdentity,
			    ptpClock->interface->msgTmp.resp.requestingPortIdentity.clockIdentity,
			    CLOCK_IDENTITY_LENGTH) == 0) &&
		    (ptpClock->portIdentity.portNumber ==
		     ptpClock->interface->msgTmp.resp.requestingPortIdentity.portNumber)
		    && (isFromCurrentParent(ptpClock, header) || rtOpts->delay_resp_ignore_port_id)) {

			DBG("==> Handle DelayResp (%d)\n", header->sequenceId);

			if (!ptpClock->waitingForDelayResp) {
				DBGV("Ignored DelayResp - not waiting for one\n");
				ptpClock->counters.discardedMessages++;
				break;
			}

			if (ptpClock->sentDelayReqSequenceId !=
			    ((UInteger16)(header->sequenceId + 1))) {
				DBG("HandleDelayResp : sequence mismatch - "
				    "last DelayReq sent: %d, delayResp received: %d\n",
				    ptpClock->sentDelayReqSequenceId,
				    header->sequenceId);
				ptpClock->counters.discardedMessages++;
				ptpClock->counters.sequenceMismatchErrors++;
				break;
			}

			/* We have received a Delay Response so clear the alarm. */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_DELAY_RESPS);
			ptpClock->sequentialMissingDelayResps = 0;
			ptpClock->waitingForDelayResp = FALSE;

			/* Hybrid mode has succeeded - mark the failure count as
			 * negative to indicate this */
			if (ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_UNICAST_CAPABLE)
				ptpClock->unicast_delay_resp_failures = -1;

			/* Stop the receipt timeout timer and start the timer to
			 * transmit the next delay request */
			timerStop(DELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
			timerStart_random(DELAYREQ_INTERVAL_TIMER,
					  powl(2,ptpClock->logMinDelayReqInterval),
					  ptpClock->itimer);

			toInternalTime(&ptpClock->delay_req_receive_time,
				       &ptpClock->interface->msgTmp.resp.receiveTimestamp);

			sfptpd_time_from_ns16(&ptpClock->delay_correction_field, header->correctionField);

			/*
			 * send_time = delay_req_send_time (received as CMSG in handleEvent)
			 * recv_time = requestReceiptTimestamp (received inside delayResp)
			 */
			/* Provide the new measurements to the servo. */
			servo_provide_s2m_ts(&ptpClock->servo,
					     &ptpClock->delay_req_send_time,
					     &ptpClock->delay_req_receive_time,
					     &ptpClock->delay_correction_field);

			ptpClock->delayRespXparent = (header->correctionField >= 65536)
						      ? TRUE : FALSE;

			if (ptpClock->waiting_for_first_delayresp) {
				ptpClock->waiting_for_first_delayresp = FALSE;
				INFO("ptp %s: received first DelayResp from Master\n", rtOpts->name);
			}

			/* If we are configured to use the delay request
			 * interval from the master or it is not defined then
			 * update our copy */
			msgInterval = header->logMessageInterval;
			if ((rtOpts->ignore_delayreq_interval_master == 0) &&
			    (msgInterval != PTPD_MESSAGE_INTERVAL_UNDEFINED)) {
				/* Saturate the interval such that it is within
				 * the range of values we can support */
				if (msgInterval < PTPD_DELAY_REQ_INTERVAL_MIN)
					msgInterval = PTPD_DELAY_REQ_INTERVAL_MIN;
				else if (msgInterval > PTPD_DELAY_REQ_INTERVAL_MAX)
					msgInterval = PTPD_DELAY_REQ_INTERVAL_MAX;

				/* Log a message if the interval has changed */
				if (ptpClock->logMinDelayReqInterval != msgInterval) {
					if (msgInterval != header->logMessageInterval) {
						WARNING("ptp %s: received out-of-range DelayReq interval "
							"%d from master (was %d, using %d)\n",
							rtOpts->name,
							header->logMessageInterval,
							ptpClock->logMinDelayReqInterval,
							msgInterval);
					} else {
						INFO("ptp %s: received new DelayReq interval %d from "
						     "master (was %d)\n",
						     rtOpts->name,
						     msgInterval,
						     ptpClock->logMinDelayReqInterval);
					}

					ptpClock->logMinDelayReqInterval = msgInterval;
				}
			}
		} else {
			DBG("HandledelayResp : delayResp doesn't match with the delayReq. \n");
			ptpClock->counters.discardedMessages++;
		}
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}

	ptpClock->counters.delayRespMessagesReceived++;
}


static void
handlePDelayReq(MsgHeader *header, ssize_t length,
		struct sfptpd_timespec *time, Boolean timestampValid,
		RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("PdelayReq message received : \n");

	if(length < PTPD_PDELAY_REQ_LENGTH) {
		DBG("Error: PDelayReq message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_DISABLED) {
		ptpClock->counters.discardedMessages++;
		return;
	} else if (ptpClock->delayMechanism != PTPD_DELAY_MECHANISM_P2P) {
		WARNING("ptp %s: unexpected PDelayReq message in end-to-end mode\n", rtOpts->name);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayModeMismatchErrors++;
		return;
	}

	/* Record details of sender of message for logging */
	statsAddNode(ptpClock->interface->msgIbuf, &ptpClock->interface->msgTmpHeader, ptpClock->interface);

	switch(ptpClock->portState ) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_UNCALIBRATED:
		DBGV("HandlePdelayReq : disregard \n");
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.pdelayReqMessagesReceived++;
		break;

	case PTPD_LISTENING:
	case PTPD_SLAVE:
	case PTPD_MASTER:
	case PTPD_PASSIVE:
		if (!timestampValid) {
			/* We didn't get a receive timestamp for this
			 * message. Set the receive timestamp alarm */
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
			ptpClock->counters.rxPktNoTimestamp++;
			WARNING("ptp %s: received PDelayReq with no timestamp\n", rtOpts->name);
		} else {
			/* Clear the RX timestamp alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_RX_TIMESTAMPS);

			if (!UNPACK_OK(msgUnpackHeader(ptpClock->interface->msgIbuf, length, &ptpClock->PdelayReqHeader))) {
				ERROR("unpacking peer delay request message\n");
				ptpClock->counters.messageFormatErrors++;
				return;
			}
			issuePDelayResp(time, header, rtOpts, ptpClock);
		}
		ptpClock->counters.pdelayReqMessagesReceived++;
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}
}


static void
processPDelayReqFromSelf(const struct sfptpd_timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	ptpClock->waitingForPDelayResp = true;
	ptpClock->waitingForPDelayRespFollow = false;

	/* Provide the new measurements to any egress event monitors. */
	egressEventMonitor(ptpClock, rtOpts, PTPD_MSG_PDELAY_REQ, time);

	/* Add latency */
	sfptpd_time_add(&ptpClock->pdelay_req_send_time, time, &rtOpts->outboundLatency);

	DBGV("processPDelayReqFromSelf: seq# %d ts " SFPTPD_FMT_SFTIMESPEC "\n",
	     ptpClock->sentPDelayReqSequenceId,
	     SFPTPD_ARGS_SFTIMESPEC(ptpClock->pdelay_req_send_time));
}


static void
handlePDelayResp(const MsgHeader *header, ssize_t length,
		 struct sfptpd_timespec *time, Boolean timestampValid,
		 RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("PdelayResp message received : \n");

	if(length < PTPD_PDELAY_RESP_LENGTH)	{
		DBG("Error: PDelayResp message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_DISABLED) {
		ptpClock->counters.discardedMessages++;
		return;
	} else if (ptpClock->delayMechanism != PTPD_DELAY_MECHANISM_P2P) {
		WARNING("ptp %s: unexpected PDelayResp message in end-to-end mode\n", rtOpts->name);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayModeMismatchErrors++;
		return;
	}

	switch(ptpClock->portState ) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_UNCALIBRATED:
		DBGV("HandlePdelayResp : disregard \n");
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.pdelayRespMessagesReceived++;
		break;

	case PTPD_LISTENING:
	case PTPD_SLAVE:
	case PTPD_MASTER:
		/* If the response isn't for us ignore it. */
		if ((memcmp(ptpClock->portIdentity.clockIdentity,
			    ptpClock->interface->msgTmp.presp.requestingPortIdentity.clockIdentity,
			    CLOCK_IDENTITY_LENGTH) == 0) &&
		    (ptpClock->portIdentity.portNumber ==
		     ptpClock->interface->msgTmp.presp.requestingPortIdentity.portNumber)) {

			DBG("==> Handle PDelayResp (%d)\n", header->sequenceId);

			if (!timestampValid) {
				/* We didn't get a receive timestamp for this
				 * message. Set the receive timestamp alarm and
				 * don't do any further processing. */
				SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
				ptpClock->counters.rxPktNoTimestamp++;
				ptpClock->counters.pdelayRespMessagesReceived++;
				WARNING("ptp %s: received PDelayResp with no timestamp\n", rtOpts->name);
				break;
			}

			if (!ptpClock->waitingForPDelayResp) {
				DBGV("Ignored PDelayResp - not waiting for one\n");
				ptpClock->counters.discardedMessages++;
				break;
			}

			if (ptpClock->sentPDelayReqSequenceId !=
			    ((UInteger16)(header->sequenceId + 1))) {
				DBGV("HandlePDelayResp: sequence mismatch - "
				     "request: %d, response: %d\n",
				     ptpClock->sentPDelayReqSequenceId,
				     header->sequenceId);
				ptpClock->counters.discardedMessages++;
				ptpClock->counters.sequenceMismatchErrors++;
				break;
			}

			/* Clear the RX timestamp alarm */
			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
			ptpClock->waitingForPDelayResp = false;

			/*store t2 (Fig 35)*/
			toInternalTime(&ptpClock->pdelay_req_receive_time,
				       &ptpClock->interface->msgTmp.presp.requestReceiptTimestamp);

			/* Store t4 (Fig 35)*/
			ptpClock->pdelay_resp_receive_time = *time;

			/* Store the correction field */
			sfptpd_time_from_ns16(&ptpClock->pdelay_correction_field, header->correctionField);

			ptpClock->delayRespXparent
				= (header->correctionField >= 65536)? TRUE: FALSE;

			ptpClock->recvPDelayRespSequenceId = header->sequenceId;

			/* If the peer is a two-step clock we have to wait for
			 * a peer delay response follow up message. Otherwise,
			 * we have all the timestamps to calculate the peer
			 * delay. */
			if ((header->flagField0 & PTPD_FLAG_TWO_STEP) != 0) {
				ptpClock->waitingForPDelayRespFollow = true;
			} else {
				SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_DELAY_RESPS);
				ptpClock->sequentialMissingDelayResps = 0;

				/* Stop the receipt timer and restart the
				 * interval timer for the next request. */
				timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
				timerStart(PDELAYREQ_INTERVAL_TIMER,
					   powl(2,ptpClock->logMinPdelayReqInterval),
					   ptpClock->itimer);

				/* In the case of a one-step clock the
				 * turnaround time between delay request and
				 * response is included in the correction field
				 * therefore there is a no explicit peer delay
				 * response transmit time - it's effectively
				 * the same as the request receive time. */
				ptpClock->pdelay_resp_send_time
					= ptpClock->pdelay_req_receive_time;

				/* Provide the new measurements to the servo. */
				servo_provide_p2p_ts(&ptpClock->servo,
						     &ptpClock->pdelay_req_send_time,
						     &ptpClock->pdelay_req_receive_time,
						     &ptpClock->pdelay_resp_send_time,
						     &ptpClock->pdelay_resp_receive_time,
						     &ptpClock->pdelay_correction_field);
			}
		} else {
			DBGV("HandlePdelayResp : Pdelayresp doesn't "
			     "match with the PdelayReq. \n");
			ptpClock->counters.discardedMessages++;
		}
		ptpClock->counters.pdelayRespMessagesReceived++;
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}
}


static void
processPDelayRespFromSelf(const struct sfptpd_timespec *tint, RunTimeOpts *rtOpts,
			  PtpClock *ptpClock, UInteger16 sequenceId)
{
	struct sfptpd_timespec timestamp;

	/* Provide the new measurements to any egress event monitors. */
	egressEventMonitor(ptpClock, rtOpts, PTPD_MSG_PDELAY_RESP, tint);

	sfptpd_time_add(&timestamp, tint, &rtOpts->outboundLatency);

	issuePDelayRespFollowUp(&timestamp, &ptpClock->PdelayReqHeader,
				rtOpts, ptpClock, sequenceId);
}


static void
handlePDelayRespFollowUp(const MsgHeader *header, ssize_t length,
			 RunTimeOpts *rtOpts,
			 PtpClock *ptpClock)
{
	DBGV("PdelayRespfollowup message received : \n");

	if (length < PTPD_PDELAY_RESP_FOLLOW_UP_LENGTH) {
		DBG("Error: PDelayRespFollowUp message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_DISABLED) {
		ptpClock->counters.discardedMessages++;
		return;
	} else if (ptpClock->delayMechanism != PTPD_DELAY_MECHANISM_P2P) {
		WARNING("ptp %s: unexpected PDelayRespFollowUp message in end-to-end mode\n", rtOpts->name);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.delayModeMismatchErrors++;
		return;
	}

	switch(ptpClock->portState) {
	case PTPD_INITIALIZING:
	case PTPD_FAULTY:
	case PTPD_DISABLED:
	case PTPD_UNCALIBRATED:
		DBGV("HandlePdelayRespFollowUp : disregard \n");
		ptpClock->counters.discardedMessages++;
		break;

	case PTPD_LISTENING:
	case PTPD_SLAVE:
	case PTPD_MASTER:
		/* If the response isn't for us ignore it. */
		if ((memcmp(ptpClock->portIdentity.clockIdentity,
			    ptpClock->interface->msgTmp.prespfollow.requestingPortIdentity.clockIdentity,
			    CLOCK_IDENTITY_LENGTH) == 0) &&
		    (ptpClock->portIdentity.portNumber ==
		     ptpClock->interface->msgTmp.prespfollow.requestingPortIdentity.portNumber)) {

			DBG("==> Handle PDelayRespFollowUp (%d)\n", header->sequenceId);

			if (!ptpClock->waitingForPDelayRespFollow) {
				DBGV("Ignored PDelayRespFollowUp - not waiting for one\n");
				ptpClock->counters.discardedMessages++;
				break;
			}

			if (((UInteger16)(header->sequenceId + 1) !=
		             ptpClock->sentPDelayReqSequenceId) ||
		            (header->sequenceId != ptpClock->recvPDelayRespSequenceId)) {
				DBG("HandleDelayRespFollowUp : sequence mismatch - "
				    "request: %d, response: %d, followup: %d\n",
				    ptpClock->sentDelayReqSequenceId,
				    ptpClock->recvPDelayRespSequenceId,
				    header->sequenceId);
				ptpClock->counters.discardedMessages++;
				ptpClock->counters.sequenceMismatchErrors++;
				break;
			}

			SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_DELAY_RESPS);
			ptpClock->sequentialMissingDelayResps = 0;
			ptpClock->waitingForPDelayRespFollow = false;

			/* Stop the receipt timer and restart the interval
			 * timer for the next request. */
			timerStop(PDELAYRESP_RECEIPT_TIMER, ptpClock->itimer);
			timerStart(PDELAYREQ_INTERVAL_TIMER,
				   powl(2,ptpClock->logMinPdelayReqInterval),
				   ptpClock->itimer);

			toInternalTime(&ptpClock->pdelay_resp_send_time,
				       &ptpClock->interface->msgTmp.prespfollow.responseOriginTimestamp);

			sfptpd_time_from_ns16(&ptpClock->pdelay_correction_field, header->correctionField);

			/* Provide the new measurements to the servo. */
			servo_provide_p2p_ts(&ptpClock->servo,
					     &ptpClock->pdelay_req_send_time,
					     &ptpClock->pdelay_req_receive_time,
					     &ptpClock->pdelay_resp_send_time,
					     &ptpClock->pdelay_resp_receive_time,
					     &ptpClock->pdelay_correction_field);

			ptpClock->pDelayRespFollowXparent
				= (header->correctionField >= 65536)? TRUE: FALSE;
		} else {
			DBGV("PdelayRespFollowup: sequence mismatch - Received: %d "
			     "PdelayReq sent: %d, PdelayResp received: %d\n",
			     header->sequenceId, ptpClock->sentPDelayReqSequenceId,
			     ptpClock->recvPDelayRespSequenceId);
			ptpClock->counters.discardedMessages++;
			ptpClock->counters.sequenceMismatchErrors++;
		}
		break;

	default:
		DBG("unrecognized state %d\n", ptpClock->portState);
		break;
	}

	ptpClock->counters.pdelayRespFollowUpMessagesReceived++;
}

/* Only accept the management message if it satisfies 15.3.1 Table 36 */
static int
acceptManagementMessage(PortIdentity thisPort, PortIdentity targetPort)
{
        ClockIdentity allOnesClkIdentity;
        UInteger16    allOnesPortNumber = 0xFFFF;
        memset(allOnesClkIdentity, 0xFF, sizeof(allOnesClkIdentity));

        return ((memcmp(targetPort.clockIdentity, thisPort.clockIdentity, CLOCK_IDENTITY_LENGTH) == 0) ||
                (memcmp(targetPort.clockIdentity, allOnesClkIdentity, CLOCK_IDENTITY_LENGTH) == 0))
                &&
                ((targetPort.portNumber == thisPort.portNumber) ||
                (targetPort.portNumber == allOnesPortNumber));
}


static void
handleManagement(MsgHeader *header, ssize_t length,
		 RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	PtpInterface *ptpInterface;
	struct sockaddr_storage destAddress;
	socklen_t destAddressLen = 0;
	ptpd_mgmt_action_e action;
	ptpd_mgmt_error_e rc;
	ssize_t unpack_result = UNPACK_INIT;

	assert(ptpClock);
	assert(ptpClock->interface);

	ptpInterface = ptpClock->interface;

	DBGV("Management message received : \n");

	if (!rtOpts->managementEnabled) {
		DBGV("Dropping management message - management message support disabled\n");
		ptpClock->counters.discardedMessages++;
		freeManagementTLV(&ptpInterface->msgTmp.manage);
		return;
	}

	if (ptpInterface->msgTmp.manage.tlv == NULL) {
		DBGV("handleManagement: TLV is empty\n");
		ptpClock->counters.messageFormatErrors++;
		return;
	}

	if (!acceptManagementMessage(ptpClock->portIdentity, ptpInterface->msgTmp.manage.targetPortIdentity))
	{
		DBGV("handleManagement: The management message was not accepted\n");
		ptpClock->counters.discardedMessages++;
		freeManagementTLV(&ptpInterface->msgTmp.manage);
		return;
	}

	/* is this an error status management TLV? */
	if (ptpInterface->msgTmp.manage.tlv->tlvType == PTPD_TLV_MANAGEMENT_ERROR_STATUS) {
		DBGV("handleManagement: Error Status TLV\n");
		unpack_result = unpackMMErrorStatus(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
		if (!UNPACK_OK(unpack_result)) {
			ERROR("unpacking management error status\n");
			ptpClock->counters.messageFormatErrors++;
		} else {
			handleMMErrorStatus(&ptpInterface->msgTmp.manage);
			ptpClock->counters.managementMessagesReceived++;
			freeManagementTLV(&ptpInterface->msgTmp.manage);
		}
		return;
	} else if (ptpInterface->msgTmp.manage.tlv->tlvType != PTPD_TLV_MANAGEMENT) {
		/* do nothing, implemention specific handling */
		DBGV("handleManagement: Currently unsupported management TLV type\n");
		ptpClock->counters.discardedMessages++;
		freeManagementTLV(&ptpInterface->msgTmp.manage);
		return;
	}

	if ((ptpInterface->msgTmp.manage.actionField == PTPD_MGMT_ACTION_RESPONSE) ||
	    (ptpInterface->msgTmp.manage.actionField == PTPD_MGMT_ACTION_ACKNOWLEDGE)) {
		DBGV("Ignoring RESPONSE/ACKNOWLEDGE management message\n");
		ptpClock->counters.discardedMessages++;
		freeManagementTLV(&ptpInterface->msgTmp.manage);
		return;
	}

	/* We've validated the message. Increment the received message counter */
	ptpClock->counters.managementMessagesReceived++;

	/* Before calling the individual handlers, initialise an outgoing
	 * management message */
	managementInitOutgoingMsg(&ptpInterface->msgTmp.manage,
				  &ptpInterface->outgoingManageTmp, ptpClock);

	action = ptpInterface->msgTmp.manage.actionField;

	/* If "set" and "command" actions are disabled, just send an error
	 * status message. Otherwise, process the management command. */
	if (!rtOpts->managementSetEnable &&
	    (action == PTPD_MGMT_ACTION_SET ||
	     action == PTPD_MGMT_ACTION_COMMAND)) {
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
	} else {
		switch(ptpInterface->msgTmp.manage.tlv->managementId) {
		case MM_NULL_MANAGEMENT:
			DBGV("handleManagement: Null Management\n");
			rc = handleMMNullManagement(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			break;
		case MM_CLOCK_DESCRIPTION:
			DBGV("handleManagement: Clock Description\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMClockDescription(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMClockDescription(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_USER_DESCRIPTION:
			DBGV("handleManagement: User Description\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMUserDescription(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMUserDescription(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_INITIALIZE:
			DBGV("handleManagement: Initialize\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMInitialize(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMInitialize(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_DEFAULT_DATA_SET:
			DBGV("handleManagement: Default Data Set\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMDefaultDataSet(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMDefaultDataSet(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_CURRENT_DATA_SET:
			DBGV("handleManagement: Current Data Set\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMCurrentDataSet(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMCurrentDataSet(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_PARENT_DATA_SET:
			DBGV("handleManagement: Parent Data Set\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMParentDataSet(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMParentDataSet(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_TIME_PROPERTIES_DATA_SET:
			DBGV("handleManagement: TimeProperties Data Set\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMTimePropertiesDataSet(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMTimePropertiesDataSet(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_PORT_DATA_SET:
			DBGV("handleManagement: Port Data Set\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMPortDataSet(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMPortDataSet(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_PRIORITY1:
			DBGV("handleManagement: Priority1\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMPriority1(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMPriority1(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_PRIORITY2:
			DBGV("handleManagement: Priority2\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMPriority2(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMPriority2(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_DOMAIN:
			DBGV("handleManagement: Domain\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMDomain(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMDomain(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_SLAVE_ONLY:
			DBGV("handleManagement: Slave Only\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMSlaveOnly(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMSlaveOnly(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_LOG_ANNOUNCE_INTERVAL:
			DBGV("handleManagement: Log Announce Interval\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMLogAnnounceInterval(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMLogAnnounceInterval(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_ANNOUNCE_RECEIPT_TIMEOUT:
			DBGV("handleManagement: Announce Receipt Timeout\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMAnnounceReceiptTimeout(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMAnnounceReceiptTimeout(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_LOG_SYNC_INTERVAL:
			DBGV("handleManagement: Log Sync Interval\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMLogSyncInterval(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMLogSyncInterval(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_VERSION_NUMBER:
			DBGV("handleManagement: Version Number\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMVersionNumber(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMVersionNumber(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_ENABLE_PORT:
			DBGV("handleManagement: Enable Port\n");
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMEnablePort(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_DISABLE_PORT:
			DBGV("handleManagement: Disable Port\n");
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMDisablePort(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_TIME:
			DBGV("handleManagement: Time\n");
 			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMTime(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMTime(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock, rtOpts);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_CLOCK_ACCURACY:
			DBGV("handleManagement: Clock Accuracy\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMClockAccuracy(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMClockAccuracy(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_UTC_PROPERTIES:
			DBGV("handleManagement: Utc Properties\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMUtcProperties(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMUtcProperties(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_TRACEABILITY_PROPERTIES:
			DBGV("handleManagement: Traceability Properties\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMTraceabilityProperties(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMTraceabilityProperties(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_DELAY_MECHANISM:
			DBGV("handleManagement: Delay Mechanism\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMDelayMechanism(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMDelayMechanism(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_LOG_MIN_PDELAY_REQ_INTERVAL:
			DBGV("handleManagement: Log Min Pdelay Req Interval\n");
			if (action != PTPD_MGMT_ACTION_GET)
				unpack_result = unpackMMLogMinPdelayReqInterval(ptpInterface->msgIbuf, length, &ptpInterface->msgTmp.manage, ptpClock);
			if (UNPACK_OK(unpack_result)) {
				rc = handleMMLogMinPdelayReqInterval(&ptpInterface->msgTmp.manage, &ptpInterface->outgoingManageTmp, ptpClock);
			} else {
				rc = PTPD_MGMT_ERROR_WRONG_LENGTH;
			}
			break;
		case MM_SAVE_IN_NON_VOLATILE_STORAGE:
		case MM_RESET_NON_VOLATILE_STORAGE:
		case MM_FAULT_LOG:
		case MM_FAULT_LOG_RESET:
		case MM_TIMESCALE_PROPERTIES:
		case MM_UNICAST_NEGOTIATION_ENABLE:
		case MM_PATH_TRACE_LIST:
		case MM_PATH_TRACE_ENABLE:
		case MM_GRANDMASTER_CLUSTER_TABLE:
		case MM_UNICAST_MASTER_TABLE:
		case MM_UNICAST_MASTER_MAX_TABLE_SIZE:
		case MM_ACCEPTABLE_MASTER_TABLE:
		case MM_ACCEPTABLE_MASTER_TABLE_ENABLED:
		case MM_ACCEPTABLE_MASTER_MAX_TABLE_SIZE:
		case MM_ALTERNATE_MASTER:
		case MM_ALTERNATE_TIME_OFFSET_ENABLE:
		case MM_ALTERNATE_TIME_OFFSET_NAME:
		case MM_ALTERNATE_TIME_OFFSET_MAX_KEY:
		case MM_ALTERNATE_TIME_OFFSET_PROPERTIES:
		case MM_TRANSPARENT_CLOCK_DEFAULT_DATA_SET:
		case MM_TRANSPARENT_CLOCK_PORT_DATA_SET:
		case MM_PRIMARY_DOMAIN:
			DBGV("handleManagement: Unsupported managementTLV %d\n",
			ptpInterface->msgTmp.manage.tlv->managementId);
			rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
			break;
		default:
			DBGV("handleManagement: Unknown managementTLV %d\n",
			ptpInterface->msgTmp.manage.tlv->managementId);
			rc = PTPD_MGMT_ERROR_NO_SUCH_ID;
			break;
		}
	}

	/* If the management message we received was unicast, we also reply with unicast */
	if ((header->flagField0 & PTPD_FLAG_UNICAST) == PTPD_FLAG_UNICAST)
		copyAddress(&destAddress, &destAddressLen,
			    &ptpInterface->transport.lastRecvAddr,
			    ptpInterface->transport.lastRecvAddrLen);
	else
		destAddressLen = 0;

	/* If the message has been successfully handled, send the response.
	 * Otherwise, construct a Management Error Status message and send
	 * it. */
	if (rc == 0) {
		/* If we get here we expect a management type TLV and either
		 * a response or an acknowledgement */
		assert(ptpInterface->outgoingManageTmp.tlv->tlvType == PTPD_TLV_MANAGEMENT);
		assert((ptpInterface->outgoingManageTmp.actionField == PTPD_MGMT_ACTION_RESPONSE) ||
		       (ptpInterface->outgoingManageTmp.actionField == PTPD_MGMT_ACTION_ACKNOWLEDGE));
		issueManagementRespOrAck(&ptpInterface->outgoingManageTmp,
					 rtOpts, ptpClock, &destAddress, destAddressLen);
	} else {
		handleErrorManagementMessage(&ptpInterface->msgTmp.manage,
					     &ptpInterface->outgoingManageTmp,
					     ptpClock, rc);
		issueManagementErrorStatus(&ptpInterface->outgoingManageTmp,
					   rtOpts, ptpClock, &destAddress, destAddressLen);
	}

	/* cleanup msgTmp managementTLV */
	freeManagementTLV(&ptpInterface->msgTmp.manage);
	/* cleanup outgoing managementTLV */
	freeManagementTLV(&ptpInterface->outgoingManageTmp);
}

static void
handleSignaling(PtpClock *ptpClock)
{
	ptpClock->counters.signalingMessagesReceived++;
}

static enum ptpd_tlv_result
ptpmon_req_tlv_handler(const MsgHeader *header, ssize_t length,
		       struct sfptpd_timespec *time, Boolean timestampValid,
		       RunTimeOpts *rtOpts, PtpClock *ptpClock,
		       TLV *tlv, size_t tlv_offset)
{
	DBGV("DelayReq+PTPMON_REQ_TLV received : \n");

	if (!rtOpts->monMeinbergNetSync) {
		DBG("ignoring MeinbergNetSync TLVs (not enabled)\n");
		return PTPD_TLV_RESULT_CONTINUE;
	}

	if (length < PTPD_DELAY_REQ_LENGTH) {
		DBG("Error: DelayReq message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return PTPD_TLV_RESULT_ERROR;
	}

	/* Always end-to-end */

	if (!timestampValid) {
		/* We didn't get a receive timestamp for this message.
		 * Set the receive timestamp alarm */
		SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_RX_TIMESTAMPS);
		ptpClock->counters.rxPktNoTimestamp++;
		WARNING("ptp %s: received DelayReq+PTPMON_REQ_TLV with no timestamp\n", rtOpts->name);
	} else {
		/* Clear the RX timestamp alarm */
		SYNC_MODULE_ALARM_CLEAR(ptpClock->portAlarms, NO_RX_TIMESTAMPS);

		if (!UNPACK_OK(msgUnpackHeader(ptpClock->interface->msgIbuf, length, &ptpClock->delayReqHeader))) {
			ERROR("error unpacking delay request+PTPMON_REQ_TLV message\n");
			ptpClock->counters.messageFormatErrors++;
			return PTPD_TLV_RESULT_ERROR;
		}

		/* Save the peer address in case we don't get to send the FollowUp immediately */
		copyAddress(&ptpClock->nsmMonitorAddr, &ptpClock->nsmMonitorAddrLen,
			    &ptpClock->interface->transport.lastRecvAddr,
			    ptpClock->interface->transport.lastRecvAddrLen);

		/* Issue the replies */
		issueDelayRespWithMonitoring(time, &ptpClock->delayReqHeader, rtOpts, ptpClock);
		issueSyncForMonitoring(rtOpts, ptpClock, ptpClock->delayReqHeader.sequenceId);
	}
	ptpClock->counters.monitoringTLVsReceived++;

	return PTPD_TLV_RESULT_DROP;
}

static enum ptpd_tlv_result
mtie_req_tlv_handler(const MsgHeader *header, ssize_t length,
		     struct sfptpd_timespec *time, Boolean timestampValid,
		     RunTimeOpts *rtOpts, PtpClock *ptpClock,
		     TLV *tlv, size_t tlv_offset)
{
	DBGV("DelayReq+MTIE_REQ_TLV received : \n");

	ptpClock->transient_packet_state.mtie_tlv_requested = true;

	return PTPD_TLV_RESULT_CONTINUE;
}

static enum ptpd_tlv_result
port_communication_capabilities_handler(const MsgHeader *header, ssize_t length,
					struct sfptpd_timespec *time, Boolean timestampValid,
					RunTimeOpts *rtOpts, PtpClock *ptpClock,
					TLV *tlv, size_t tlv_offset)
{
	ssize_t result;

	DBGV("PORT_COMMUNICATION_CAPABILITIES received : \n");

	if (rtOpts->ptp_version_minor < 1) {
		DBG2("ignore COMMUNICATION_CAPABILITIES TLV in version %d.%d mode\n",
		     PTPD_PROTOCOL_VERSION,
		     rtOpts->ptp_version_minor);
		ptpClock->counters.discardedMessages++;
		ptpClock->counters.versionMismatchErrors++;
		return PTPD_TLV_RESULT_CONTINUE;
	}

	/* Save a pointer for the announce handler to use */
	result = unpackPortCommunicationCapabilities(tlv->valueField,
						     tlv->lengthField,
						     &ptpClock->transient_packet_state.port_comm_caps,
						     ptpClock);
	if (!UNPACK_OK(result))
		return PTPD_TLV_RESULT_ERROR;

	ptpClock->transient_packet_state.port_comm_caps_provided = true;

	return PTPD_TLV_RESULT_CONTINUE;
}


/*Pack and send on general multicast ip address an Announce message*/
static void
issueAnnounce(RunTimeOpts *rtOpts,PtpClock *ptpClock)
{
	ssize_t pack_result;

	/* Test Function: Suppress Announce messsages */
	if (rtOpts->test.no_announce_pkts)
		return;

	pack_result = msgPackAnnounce(ptpClock->msgObuf, sizeof ptpClock->msgObuf, ptpClock);
	assert(PACK_OK(pack_result));

	/* @task71885: append multicast/unicast capability information */
	if (rtOpts->comm_caps_tlv_enabled) {
		pack_result = appendPortCommunicationCapabilitiesTLV(&rtOpts->comm_caps,
								     ptpClock->msgObuf,
								     sizeof ptpClock->msgObuf);
		assert(PACK_OK(pack_result));
	}

	/* Send the message */
	if (netSendGeneral(ptpClock->msgObuf, getHeaderLength(ptpClock->msgObuf),
			   ptpClock, rtOpts, NULL, 0) != 0) {
		handleSendFailure(rtOpts, ptpClock, "Announce");
	} else {
		DBGV("Announce MSG sent!\n");
		ptpClock->sentAnnounceSequenceId++;
		ptpClock->counters.announceMessagesSent++;
	}
}


/*Pack and send on event multicast ip address a Sync message*/
static void
issueSync(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	struct sfptpd_ts_user ts_user = {
		.port = ptpClock,
		.type = TS_SYNC,
		.seq_id = ptpClock->sentSyncSequenceId,
	};
	struct sfptpd_ts_ticket ticket;
	int rc;

	/* Test Function: Suppress Sync messsages */
	if (rtOpts->test.no_sync_pkts)
		return;

	msgPackSync(ptpClock->msgObuf, sizeof ptpClock->msgObuf, ptpClock);

	rc = netSendEvent(ptpClock->msgObuf, PTPD_SYNC_LENGTH, ptpClock,
			  rtOpts, NULL, 0, 0);
	if (rc == 0) {
		/* We successfully transmitted the packet */
		ptpClock->counters.syncMessagesSent++;
		DBGV("Sync MSG sent!\n");

		ticket = netExpectTimestamp(&ptpClock->interface->ts_cache,
					    &ts_user,
					    ptpClock->msgObuf,
					    PTPD_SYNC_LENGTH,
					    getTrailerLength(ptpClock));
		if (sfptpd_ts_is_ticket_valid(ticket)) {
			ptpClock->sync_ticket = ticket;
		} else {
			WARNING("ptp %s: did not get tx timestamp ticket for Sync msg\n", rtOpts->name);
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
			ptpClock->counters.txPktNoTimestamp++;
		}

		ptpClock->sentSyncSequenceId++;

		/* Check error queue immediately before falling back to epoll.
		 * This optimisation does not seem to succeed in the way
		 * you might expect: the timestamp is probably _not_ ready
		 * but warming the code path seems to shave off 10us! */
		doHandleSockets(&ptpClock->interface->ifOpts,
				ptpClock->interface,
				TRUE, FALSE, FALSE);
	} else if (rc != 0) {
		/* If we failed for any reason then something is seriously
		 * wrong with the socket. Go to the faulty state and
		 * re-initialise. */
		handleSendFailure(rtOpts, ptpClock, "Sync");
	}
}


/* task 70154: Pack and send on event unicast ip address a Sync message*/
static void
issueSyncForMonitoring(RunTimeOpts *rtOpts, PtpClock *ptpClock, UInteger16 sequenceId)
{
	struct sfptpd_ts_user ts_user= {
		.port = ptpClock,
		.type = TS_MONITORING_SYNC,
		.seq_id = sequenceId,
	};
	struct sfptpd_ts_ticket ticket;
	int rc;

	msgPackSync(ptpClock->msgObuf, sizeof ptpClock->msgObuf, ptpClock);

	/* Update header fields */
	msgUpdateHeaderSequenceId(ptpClock->msgObuf, sequenceId);
	msgUpdateHeaderFlags(ptpClock->msgObuf, ~0, PTPD_FLAG_TWO_STEP);

	rc = netSendEvent(ptpClock->msgObuf, PTPD_SYNC_LENGTH, ptpClock, rtOpts,
			  &ptpClock->nsmMonitorAddr,
			  ptpClock->nsmMonitorAddrLen, 0);
	if (rc == 0) {
		/* We successfully transmitted the packet */
		ptpClock->counters.monitoringTLVsSyncsSent++;
		DBGV("Monitoring sync MSG sent!\n");

		ticket = netExpectTimestamp(&ptpClock->interface->ts_cache,
					    &ts_user,
					    ptpClock->msgObuf,
					    PTPD_SYNC_LENGTH,
					    getTrailerLength(ptpClock));
		if (sfptpd_ts_is_ticket_valid(ticket)) {
			ptpClock->monsync_ticket = ticket;
		} else {
			WARNING("ptp %s: did not get tx timestamp ticket for monitoring Sync msg\n", rtOpts->name);
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
			ptpClock->counters.txPktNoTimestamp++;
		}
	} else if (rc != 0) {
		/* If we failed for any reason then something is seriously
		 * wrong with the socket but we are not going to take us
		 * to the faulty state for the monitoring extension. */
		ptpClock->counters.messageSendErrors++;
		DBGV("Monitoring sync message can't be sent.\n");
	}
}


/*Pack and send on general multicast ip address a FollowUp message*/
static void
issueFollowup(const struct sfptpd_timespec *preciseOriginTimestamp,
	      RunTimeOpts *rtOpts, PtpClock *ptpClock,
	      const UInteger16 sequenceId)
{
	/* Test Function: Suppress Follow Up messsages */
	if (rtOpts->test.no_follow_ups)
		return;

	msgPackFollowUp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			preciseOriginTimestamp,
			ptpClock, sequenceId);

	if (netSendGeneral(ptpClock->msgObuf, PTPD_FOLLOW_UP_LENGTH,
			   ptpClock, rtOpts, NULL, 0) != 0) {
		handleSendFailure(rtOpts, ptpClock, "FollowUp");
	} else {
		DBGV("FollowUp MSG sent!\n");
		ptpClock->counters.followUpMessagesSent++;
	}
}


/* task 70154: Pack and send on general unicast ip address a FollowUp message*/
static void
issueFollowupForMonitoring(const struct sfptpd_timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock,
			   const UInteger16 sequenceId)
{
	msgPackFollowUp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			time, ptpClock, sequenceId);

	/* Update header fields */
	msgUpdateHeaderFlags(ptpClock->msgObuf, ~0, PTPD_FLAG_TWO_STEP);

	if (netSendGeneral(ptpClock->msgObuf, PTPD_FOLLOW_UP_LENGTH,
			   ptpClock, rtOpts,
			   &ptpClock->nsmMonitorAddr,
			   ptpClock->nsmMonitorAddrLen) != 0) {
		ptpClock->counters.messageSendErrors++;
	} else {
		DBGV("Monitoring FollowUp MSG sent!\n");
		ptpClock->counters.monitoringTLVsFollowUpsSent++;
	}
}


/*Pack and send on event multicast ip address a DelayReq message*/
static void
issueDelayReq(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	struct sfptpd_ts_user ts_user = {
		.port = ptpClock,
		.type = TS_DELAY_REQ,
		.seq_id = ptpClock->sentDelayReqSequenceId,
	};
	struct sfptpd_ts_ticket ticket;
	struct sockaddr_storage dst;
	socklen_t dstLen = 0;
	int rc;

	ptpClock->waitingForDelayResp = FALSE;

	DBG("==> Issue DelayReq (%d)\n", ptpClock->sentDelayReqSequenceId);

	/* This uses sentDelayReqSequenceId as the sequence number */
	msgPackDelayReq(ptpClock->msgObuf, sizeof ptpClock->msgObuf, ptpClock);

	if (ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_UNICAST_CAPABLE) {
		copyAddress(&dst, &dstLen, &ptpClock->parentAddress, ptpClock->parentAddressLen);
	} else if (!(ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_MULTICAST_CAPABLE)) {
		SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, CAPS_MISMATCH);
		return;
	}

	rc = netSendEvent(ptpClock->msgObuf, PTPD_DELAY_REQ_LENGTH, ptpClock,
			  rtOpts, &dst, dstLen, ptpClock->lastSyncIfindex);
	if (rc != 0) {
		/* If we failed for any reason other than failure to retrieve
		 * the transmit then something is seriously wrong with the
		 * socket. Go to the faulty state and re-initialise. */
		handleSendFailure(rtOpts, ptpClock, "delayReq");
	} else {
		ticket = netExpectTimestamp(&ptpClock->interface->ts_cache,
					    &ts_user,
					    ptpClock->msgObuf,
					    PTPD_DELAY_REQ_LENGTH,
					    getTrailerLength(ptpClock));
		if (sfptpd_ts_is_ticket_valid(ticket)) {
			ptpClock->delayreq_ticket = ticket;
		} else {
			WARNING("ptp %s: did not get tx timestamp ticket for Delay_Request msg\n", rtOpts->name);
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
			ptpClock->counters.txPktNoTimestamp++;
		}

		ptpClock->counters.delayReqMessagesSent++;
		DBGV("DelayReq MSG sent!\n");

		/* From now on, we will only accept delayreq and delayresp of 
		 * (sentDelayReqSequenceId - 1) */
		ptpClock->sentDelayReqSequenceId++;

		/* Stop the delay request timer and start the timer for delay
		 * response timeout */
		timerStop(DELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
		timerStart(DELAYRESP_RECEIPT_TIMER,
			   powl(2, ptpClock->logDelayRespReceiptTimeout),
			   ptpClock->itimer);

		/* Check error queue immediately before falling back to epoll. */
		doHandleSockets(&ptpClock->interface->ifOpts,
				ptpClock->interface,
				TRUE, FALSE, FALSE);
	}
}

/*Pack and send on event multicast ip address a PDelayReq message*/
static void
issuePDelayReq(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	struct sfptpd_ts_user ts_user = {
		.port = ptpClock,
		.type = TS_PDELAY_REQ,
		.seq_id = ptpClock->sentPDelayReqSequenceId,
	};
	struct sfptpd_ts_ticket ticket;
	int rc;

	ptpClock->waitingForPDelayResp = false;
	ptpClock->waitingForPDelayRespFollow = false;

	DBG("==> Issue PDelayReq (%d)\n", ptpClock->sentPDelayReqSequenceId);

	msgPackPDelayReq(ptpClock->msgObuf, sizeof ptpClock->msgObuf, ptpClock);

	rc = netSendPeerEvent(ptpClock->msgObuf, PTPD_PDELAY_REQ_LENGTH,
			      ptpClock, rtOpts);
	if (rc != 0) {
		/* If we failed for any reason other than failure to retrieve
		 * the timestamp then something is seriously wrong with the
		 * socket. Go to the faulty state and re-initialise. */
		handleSendFailure(rtOpts, ptpClock, "PdelayReq");
	} else {
		ticket = netExpectTimestamp(&ptpClock->interface->ts_cache,
					    &ts_user,
					    ptpClock->msgObuf,
					    PTPD_PDELAY_REQ_LENGTH,
					    getTrailerLength(ptpClock));
		if (sfptpd_ts_is_ticket_valid(ticket)) {
			ptpClock->pdelayreq_ticket = ticket;
		} else {
			WARNING("ptp %s: did not get tx timestamp ticket for Peer_Delay_Request msg\n", rtOpts->name);
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
			ptpClock->counters.txPktNoTimestamp++;
		}

		ptpClock->counters.pdelayReqMessagesSent++;
		DBGV("PDelayReq MSG sent!\n");

		ptpClock->sentPDelayReqSequenceId++;

		/* Stop the delay request timer and start the timer for delay
		 * response timeout */
		timerStop(PDELAYREQ_INTERVAL_TIMER, ptpClock->itimer);
		timerStart(PDELAYRESP_RECEIPT_TIMER,
			   powl(2, ptpClock->logDelayRespReceiptTimeout),
			   ptpClock->itimer);

		/* Check error queue immediately before falling back to epoll. */
		doHandleSockets(&ptpClock->interface->ifOpts,
				ptpClock->interface,
				TRUE, FALSE, FALSE);
	}
}

/*Pack and send on event multicast ip address a PDelayResp message*/
void
issuePDelayResp(struct sfptpd_timespec *time, MsgHeader *header,
		RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	struct sfptpd_ts_user ts_user = {
		.port = ptpClock,
		.type = TS_PDELAY_RESP,
		.seq_id = header->sequenceId,
	};
	struct sfptpd_ts_ticket ticket;
	int rc;

	/* Test Function: Suppress Delay Response messsages */
	if (rtOpts->test.no_delay_resps)
		return;

	/* Test Function: Packet timestamp - bad timestamp */
	if ((rtOpts->test.bad_timestamp.type != BAD_TIMESTAMP_TYPE_OFF) && 
	    ((header->sequenceId % rtOpts->test.bad_timestamp.interval_pkts) == 0)) {
		Integer32 jitter = (Integer32)((getRand() - 0.5) * 2.0 *
					       rtOpts->test.bad_timestamp.max_jitter);
		time->nsec += jitter;
		sfptpd_time_normalise(time);
		INFO("ptp %s: added jitter %d to pdelay req RX timestamp\n",
		     rtOpts->name, jitter);
	}

	msgPackPDelayResp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			  header, time, ptpClock);

	rc = netSendPeerEvent(ptpClock->msgObuf, PTPD_PDELAY_RESP_LENGTH,
			      ptpClock, rtOpts);
	if (rc != 0) {
		/* If we failed for any reason other than failure to retrieve
		 * the timestamp then something is seriously wrong with the
		 * socket. Go to the faulty state and re-initialise. */
		handleSendFailure(rtOpts, ptpClock, "PdelayResp");
	} else {
		ticket = netExpectTimestamp(&ptpClock->interface->ts_cache,
					    &ts_user,
					    ptpClock->msgObuf,
					    PTPD_PDELAY_RESP_LENGTH,
					    getTrailerLength(ptpClock));
		if (sfptpd_ts_is_ticket_valid(ticket)) {
			ptpClock->pdelayresp_ticket = ticket;
		} else {
			WARNING("ptp %s: did not get tx timestamp ticket for Peer_Delay_Response msg\n", rtOpts->name);
			SYNC_MODULE_ALARM_SET(ptpClock->portAlarms, NO_TX_TIMESTAMPS);
			ptpClock->counters.txPktNoTimestamp++;
		}

		ptpClock->counters.pdelayRespMessagesSent++;
		DBGV("PDelayResp MSG sent ! \n");

		/* Check error queue immediately before falling back to epoll. */
		doHandleSockets(&ptpClock->interface->ifOpts,
				ptpClock->interface,
				TRUE, FALSE, FALSE);
	}
}


/*Pack and send on event multicast ip address a DelayResp message*/
static void
issueDelayResp(struct sfptpd_timespec *time,MsgHeader *header,RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	Integer64 correction;

	/* Test Function: Suppress Delay Response messsages */
	if (rtOpts->test.no_delay_resps)
		return;

	/* Test Function: Packet timestamp  - bad timestamp */
	if ((rtOpts->test.bad_timestamp.type != BAD_TIMESTAMP_TYPE_OFF) && 
	    ((header->sequenceId % rtOpts->test.bad_timestamp.interval_pkts) == 0)) {
		Integer32 jitter = (Integer32)((getRand() - 0.5) * 2.0 *
					       rtOpts->test.bad_timestamp.max_jitter);
		time->nsec += jitter;
		sfptpd_time_normalise(time);
		INFO("ptp %s: added jitter %d to delay req RX timestamp\n",
		     rtOpts->name, jitter);
	}

	/* Test mode: emulate transparent clock */
	if (rtOpts->test.xparent_clock.enable) {
		correction = (Integer64)(getRand() *
				       rtOpts->test.xparent_clock.max_correction);
		time->nsec += (Integer32)correction;
		sfptpd_time_normalise(time);
		INFO("ptp %s: set correction field of delay resp to %"PRIi64" ns\n",
		     rtOpts->name, correction);
	} else {
		/* Correction not used otherwise but keep the compiler happy. */
		correction = 0;
	}

	/* If the delay request sent in unicast and we are configured in hybrid
	 * mode then respond with unicast. Otherwise, send a multicast response */
	const struct sockaddr_storage *dst = NULL;
	socklen_t dstLen = 0;

	if (((header->flagField0 & PTPD_FLAG_UNICAST) != 0) &&
	    (ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_UNICAST_CAPABLE)) {
		dst = &ptpClock->interface->transport.lastRecvAddr;
		dstLen = ptpClock->interface->transport.lastRecvAddrLen;
	} else if (!(ptpClock->effective_comm_caps.delayRespCapabilities & PTPD_COMM_MULTICAST_CAPABLE)) {
		/* Silently ignore unicast delay requests if they are not in the
		   effective capabilities set.
		*/
		return;
	}

	msgPackDelayResp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			 header, time, ptpClock);

	/* Test mode: emulate transparent clock */
	if (rtOpts->test.xparent_clock.enable) {
		/* We have to set the correction field after preparing the
		 * delay resp message as this zeros the field. The correction
		 * field units are nanoseconds shifted by 16. */
		correction <<= 16;
		*(UInteger32 *)(ptpClock->msgObuf + 8) = flip32((UInteger32)(correction >> 32));
		*(UInteger32 *)(ptpClock->msgObuf + 12) = flip32((UInteger32)correction);
	}

	if (netSendGeneral(ptpClock->msgObuf, PTPD_PDELAY_RESP_LENGTH,
			   ptpClock, rtOpts, dst, dstLen) != 0) {
		handleSendFailure(rtOpts, ptpClock, "DelayResp");
	} else {
		DBGV("DelayResp MSG sent!\n");
		ptpClock->counters.pdelayRespMessagesSent++;
	}
}


/* @task70154: Pack and send a unicast DelayResp message with monitoring TLV */
static void
issueDelayRespWithMonitoring(struct sfptpd_timespec *time, MsgHeader *header,
			     RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	PTPMonRespTLV ptp_mon_resp_tlv;
	TimeInterval correction;
	ssize_t length;

	/* The last Sync timestamp is provided in the TLV; this is not part
	 * of the timing mechanism itself, it is to associate the timing
	 * in time. */
	fromInternalTime(&ptpClock->sync_send_time,
			 &ptp_mon_resp_tlv.lastSyncTimestamp,
			 &correction);

	/* Populate the TLV */
	ptp_mon_resp_tlv.tlvType = PTPD_TLV_PTPMON_RESP_OLD;
	ptp_mon_resp_tlv.portState = ptpClock->portState;
	writeProtocolAddress(&ptp_mon_resp_tlv.parentPortAddress,
			     &ptpClock->parentAddress,
			     ptpClock->parentAddressLen);
	populateParentDataSet(&ptp_mon_resp_tlv.parentDataSet, ptpClock);
	populateCurrentDataSet(&ptp_mon_resp_tlv.currentDataSet, ptpClock);
	populateTimePropertiesDataSet(&ptp_mon_resp_tlv.timePropertiesDataSet, ptpClock);

	/* Pack the message */
	msgPackDelayResp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			 header, time, ptpClock);
	length = appendPTPMonRespTLV(&ptp_mon_resp_tlv, ptpClock->msgObuf, sizeof ptpClock->msgObuf);

	if (ptpClock->transient_packet_state.mtie_tlv_requested) {
		MTIERespTLV mtie_resp_tlv;

		memset(&mtie_resp_tlv, '\0', sizeof mtie_resp_tlv);
		mtie_resp_tlv.tlvType = PTPD_TLV_MTIE_RESP_OLD;
		mtie_resp_tlv.mtieValid = ptpClock->mtie_window.mtie_valid;
		mtie_resp_tlv.mtieWindowNumber = ptpClock->mtie_window.mtie_window_number;
		mtie_resp_tlv.mtieWindowDuration = ptpClock->mtie_window.mtie_window_duration;
		mtie_resp_tlv.minOffsFromMaster = ptpClock->mtie_window.min_offs_from_master;
		mtie_resp_tlv.maxOffsFromMaster = ptpClock->mtie_window.max_offs_from_master;
		mtie_resp_tlv.minOffsFromMasterAt = ptpClock->mtie_window.min_offs_from_master_at;
		mtie_resp_tlv.maxOffsFromMasterAt = ptpClock->mtie_window.max_offs_from_master_at;

		length = appendMTIERespTLV(&mtie_resp_tlv, ptpClock->msgObuf, sizeof ptpClock->msgObuf);
	}

	if (!UNPACK_OK(length) ||
	    netSendGeneral(ptpClock->msgObuf, UNPACK_GET_SIZE(length),
			   ptpClock, rtOpts,
			   &ptpClock->nsmMonitorAddr,
			   ptpClock->nsmMonitorAddrLen) != 0) {
		ptpClock->counters.messageSendErrors++;
		DBGV("DelayRes+PTPMON_RESP_TLV message can't be sent\n");
	} else {
		DBGV("DelayResp+PTPMON_RESP_TLV MSG sent!\n");
		ptpClock->counters.monitoringTLVsSent++;
	}
}


static void
issuePDelayRespFollowUp(struct sfptpd_timespec *responseOriginTimestamp,
			MsgHeader *header,
			RunTimeOpts *rtOpts, PtpClock *ptpClock,
			const UInteger16 sequenceId)
{
	/* Test Function: Packet timestamp - bad timestamp */
	if ((rtOpts->test.bad_timestamp.type != BAD_TIMESTAMP_TYPE_OFF) && 
	    ((header->sequenceId % rtOpts->test.bad_timestamp.interval_pkts) == 0)) {
		Integer32 jitter = (Integer32)((getRand() - 0.5) * 2.0 *
					       rtOpts->test.bad_timestamp.max_jitter);
		responseOriginTimestamp->nsec += jitter;
		sfptpd_time_normalise(responseOriginTimestamp);
		INFO("ptp %s: added jitter %d to pdelay resp TX timestamp\n",
		     rtOpts->name, jitter);
	}

	msgPackPDelayRespFollowUp(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
				  header, responseOriginTimestamp,
				  ptpClock, sequenceId);

	if (netSendPeerGeneral(ptpClock->msgObuf, PTPD_PDELAY_RESP_FOLLOW_UP_LENGTH,
			       ptpClock) != 0) {
		handleSendFailure(rtOpts, ptpClock, "PDelayResp");
	} else {
		DBGV("PDelayRespFollowUp MSG sent ! \n");
		ptpClock->counters.pdelayRespFollowUpMessagesSent++;
	}
}


static void 
issueManagementRespOrAck(MsgManagement *outgoing, RunTimeOpts *rtOpts,
			 PtpClock *ptpClock, const struct sockaddr_storage *destAddress,
			 socklen_t destAddressLen)
{
	/* pack ManagementTLV */
	msgPackManagementTLV(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			     outgoing, ptpClock);

	/* set header messageLength, the outgoing->tlv->lengthField is now valid */
	outgoing->header.messageLength = PTPD_MANAGEMENT_LENGTH +
					 PTPD_TLV_LENGTH -
					 PTPD_TLV_MANAGEMENT_ID_LENGTH +
					 outgoing->tlv->lengthField;

	msgPackManagement(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			  outgoing, ptpClock);

	if (netSendGeneral(ptpClock->msgObuf, outgoing->header.messageLength,
			   ptpClock, rtOpts, destAddress, destAddressLen) != 0) {
		handleSendFailure(rtOpts, ptpClock, "Management response/acknowledge");
	} else {
		DBGV("Management response/acknowledge msg sent \n");
		ptpClock->counters.managementMessagesSent++;
	}
}

static void
issueManagementErrorStatus(MsgManagement *outgoing, RunTimeOpts *rtOpts,
			   PtpClock *ptpClock, const struct sockaddr_storage *destAddress,
			   socklen_t destAddressLen)
{
	/* pack ManagementErrorStatusTLV */
	msgPackManagementErrorStatusTLV(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
					outgoing, ptpClock);

	/* set header messageLength, the outgoing->tlv->lengthField is now valid */
	outgoing->header.messageLength = PTPD_MANAGEMENT_LENGTH +
					 PTPD_TLV_LENGTH -
					 PTPD_TLV_MANAGEMENT_ID_LENGTH +
					 outgoing->tlv->lengthField;

	msgPackManagement(ptpClock->msgObuf, sizeof ptpClock->msgObuf,
			  outgoing, ptpClock);

	if (netSendGeneral(ptpClock->msgObuf, outgoing->header.messageLength,
			   ptpClock, rtOpts, destAddress, destAddressLen) != 0) {
		handleSendFailure(rtOpts, ptpClock, "Management error status");
	} else {
		DBGV("Management error status msg sent \n");
		ptpClock->counters.managementMessagesSent++;
	}
}


static void statsAddNode(Octet *buf, MsgHeader *header, PtpInterface *ptpInterface)
{
	unsigned char *clock_id;
	Boolean master;
	uint16_t portNumber, domainNumber;

	DBGV("Updating foreign node table\n");

	/* Populate foreign node struct */
	portNumber = header->sourcePortIdentity.portNumber;
	domainNumber = header->domainNumber;

	master = (header->messageType == PTPD_MSG_ANNOUNCE) ? true : false;

	/* Add node to set or update entry if already have it */
	clock_id = (unsigned char *)header->sourcePortIdentity.clockIdentity;
	sfptpd_stats_add_node(ptpInterface->nodeSet, clock_id, master,
			      portNumber, domainNumber,
			      ptpInterface->transport.lastRecvHost);
}


/* The foreign master dataset handler uses an inverse of this function
 * to undo this action.
 *
 * Local timestamps are all UTC because our local clocks need appropriate time
 * for application purposes. This function converts that time to TAI time.
 *
 * This function is needed:
 *   - to enter TAI timescale timestamps into time calculations
 *   - when operating as a TAI master
 */
static void
applyUtcOffset(struct sfptpd_timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	if ((ptpClock->portState != PTPD_MASTER &&
	     (ptpClock->timePropertiesDS.currentUtcOffsetValid ||
	      rtOpts->alwaysRespectUtcOffset)) ||
	    (ptpClock->portState == PTPD_MASTER &&
	     ptpClock->timePropertiesDS.currentUtcOffsetValid &&
	     ptpClock->timePropertiesDS.ptpTimescale)) {

		/* Convert timestamp to TAI */
		time->sec += ptpClock->timePropertiesDS.currentUtcOffset;
		time->sec -= ptpClock->fakeUtcAdjustment;
	}
}


/* fin */
