/*-
 * Copyright (c) 2019      Xilinx, Inc.
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
 * @file   evtmon.c
 * @date   Wed Jun 23 09:40:39 2010
 *
 * @brief  The code that implements Slave Event Monitoring
 *
 *
 */

#include "ptpd.h"
#include "ptpd_lib.h"


static ptpd_slave_tx_ts_msg_e ptpd_msg_type_to_tx_ts_type(ptpd_msg_id_e msg_type)
{
	switch (msg_type) {
	case PTPD_MSG_DELAY_REQ:
		return PTPD_SLAVE_TX_TS_DELAY_REQ;
	case PTPD_MSG_PDELAY_REQ:
		return PTPD_SLAVE_TX_TS_PDELAY_REQ;
	case PTPD_MSG_PDELAY_RESP:
		return PTPD_SLAVE_TX_TS_PDELAY_RESP;
	default:
		return PTPD_SLAVE_TX_TS_NUM;
	}
}


static ptpd_msg_id_e ptpd_tx_ts_type_to_msg_type(ptpd_slave_tx_ts_msg_e tx_ts_type)
{
	switch (tx_ts_type) {
	case PTPD_SLAVE_TX_TS_DELAY_REQ:
		return PTPD_MSG_DELAY_REQ;
	case PTPD_SLAVE_TX_TS_PDELAY_REQ:
		return PTPD_MSG_PDELAY_REQ;
	case PTPD_SLAVE_TX_TS_PDELAY_RESP:
		return PTPD_MSG_PDELAY_RESP;
	default:
		return -1;
	}
}


/* Send an event monitoring message that has already been prepared */
static void sendMonitoringMessage(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	int i, num_dests;

	/* Correct for implicit destination when using PTP multicast address */
	num_dests = rtOpts->num_monitor_dests;
	if (num_dests == 0)
		num_dests = 1;

	for (i = 0; i < num_dests; i++) {
		if (netSendMonitoring(ptpClock->msgObuf, getHeaderLength(ptpClock->msgObuf),
				      ptpClock, rtOpts, rtOpts->monitor_address + i,
				      rtOpts->monitor_address_len[i]) != 0) {
			handleSendFailure(rtOpts, ptpClock, "Signaling");
		} else {
			DBGV("Signaling MSG sent!\n");
			ptpClock->counters.signalingMessagesSent++;
		}
	}
	ptpClock->sentSignalingSequenceId++;
}


/* @task71778: Slave Event Monitoring (timing data) (IEEE1588-Rev 2017 draft 16.11.4.1) */
static void
flushSlaveRxSyncTimingData(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	SlaveEventMonitoringConfig *config = &rtOpts->rx_sync_timing_data_config;
	SlaveEventMonitoringState *state = &ptpClock->slave_rx_sync_timing_data_state;
	SlaveRxSyncTimingDataElement *records = ptpClock->slave_rx_sync_timing_data_records;

	if (config->tlv_enable) {
		MsgSignaling msgSignaling = { { 0 } };
		SlaveRxSyncTimingDataTLV data;
		ssize_t pack_result;

		assert(state->num_events <= config->events_per_tlv);

		signalingInitOutgoingMsg(&msgSignaling, ptpClock);

		/* pack SignalingTLV */
		pack_result = packMsgSignaling(&msgSignaling, ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		assert(PACK_OK(pack_result));

		data.preamble.sourcePortIdentity = state->source_port;
		data.num_elements = state->num_events;
		data.elements = records;

		pack_result = appendSlaveRxSyncTimingDataTLV(&data,
							     ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		state->num_events = 0;
		sendMonitoringMessage(ptpClock, rtOpts);
	}
}


/* @task71778: Slave Event Monitoring (computed data) (IEEE1588-Rev draft 16.11.4.2) */
static void
flushSlaveRxSyncComputedData(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	SlaveEventMonitoringConfig *config = &rtOpts->rx_sync_computed_data_config;
	SlaveEventMonitoringState *state = &ptpClock->slave_rx_sync_computed_data_state;
	SlaveRxSyncComputedDataElement *records = ptpClock->slave_rx_sync_computed_data_records;

	if (config->tlv_enable) {
		MsgSignaling msgSignaling = { { 0 } };
		SlaveRxSyncComputedData data;
		ssize_t pack_result;

		assert(state->num_events <= config->events_per_tlv);

		/* (IEEE1588-Rev 16.11.4.2.4) Indicate that we are supplying
		   valid data for offset (bit 2) and mpd (bit 1) only. */
		data.computedFlags = 6;

		signalingInitOutgoingMsg(&msgSignaling, ptpClock);

		/* pack SignalingTLV */
		pack_result = packMsgSignaling(&msgSignaling, ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		assert(PACK_OK(pack_result));

		data.sourcePortIdentity = state->source_port;
		pack_result = appendSlaveRxSyncComputedDataTLV(&data,
							     records,
							     state->num_events,
							     ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		state->num_events = 0;
		sendMonitoringMessage(ptpClock, rtOpts);
	}
}


/* @task71778: Slave Event Monitoring (timing data) (IEEE1588-Rev draft 16.11.4.1) */
static void
rxSyncTimingDataMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	SlaveEventMonitoringConfig *timing_data_config = &rtOpts->rx_sync_timing_data_config;
	SlaveEventMonitoringState *timing_data_state = &ptpClock->slave_rx_sync_timing_data_state;
	SlaveRxSyncTimingDataElement *timing_data_records = ptpClock->slave_rx_sync_timing_data_records;

	if (timing_data_config->logging_enable) {
		if (timing_data_state->skip_count == 0) {
			SlaveRxSyncTimingDataElement *record =
				&timing_data_records[timing_data_state->num_events];

			/* If the source port has changed, flush the old entries out. */
			if (memcmp(ptpClock->parentPortIdentity.clockIdentity,
				   timing_data_state->source_port.clockIdentity,
				   CLOCK_IDENTITY_LENGTH) ||
			    (ptpClock->parentPortIdentity.portNumber !=
			     timing_data_state->source_port.portNumber)) {
				if (timing_data_state->num_events != 0) {
					flushSlaveRxSyncTimingData(ptpClock, rtOpts);
					timing_data_state->num_events = 0;
				}
				timing_data_state->source_port = ptpClock->parentPortIdentity;
			}

			/* Populate a new record */
			record->sequenceId = ptpClock->recvSyncSequenceId;
			fromInternalTime(&ptpClock->sync_send_time, &record->syncOriginTimestamp);
			fromInternalTime(&ptpClock->sync_receive_time, &record->syncEventIngressTimestamp);
			record->totalCorrectionField = sfptpd_time_to_ns16(ptpClock->sync_correction_field);
			record->cumulativeScaledRateOffset = 0;

			/* When we have filled a set of records, flush them. */
			if (++timing_data_state->num_events == timing_data_config->events_per_tlv) {
				flushSlaveRxSyncTimingData(ptpClock, rtOpts);
				timing_data_state->num_events = 0;
			}
		}

		if (timing_data_state->skip_count == timing_data_config->logging_skip) {
			timing_data_state->skip_count = 0;
		} else {
			timing_data_state->skip_count++;
		}
	}
}


/* @task71778: Slave Event Monitoring (computed data) (IEEE1588-Rev draft 16.11.4.2) */
static void
rxSyncComputedDataMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	SlaveEventMonitoringConfig *computed_data_config = &rtOpts->rx_sync_computed_data_config;
	SlaveEventMonitoringState *computed_data_state = &ptpClock->slave_rx_sync_computed_data_state;
	SlaveRxSyncComputedDataElement *computed_data_records = ptpClock->slave_rx_sync_computed_data_records;

	if (computed_data_config->logging_enable) {
		if (computed_data_state->skip_count == 0) {
			SlaveRxSyncComputedDataElement *record =
				&computed_data_records[computed_data_state->num_events];
			sfptpd_time_t offset, mpd;

			/* If the source port has changed, flush the old entries out. */
			if (memcmp(ptpClock->parentPortIdentity.clockIdentity,
				   computed_data_state->source_port.clockIdentity,
				   CLOCK_IDENTITY_LENGTH) ||
			    (ptpClock->parentPortIdentity.portNumber !=
			     computed_data_state->source_port.portNumber)) {
				if (computed_data_state->num_events != 0) {
					flushSlaveRxSyncComputedData(ptpClock, rtOpts);
					computed_data_state->num_events = 0;
				}
				computed_data_state->source_port = ptpClock->parentPortIdentity;
			}

			/* Populate a new record */
			record->sequenceId = ptpClock->recvSyncSequenceId;
			offset = servo_get_offset_from_master(&ptpClock->servo);
			mpd = servo_get_mean_path_delay(&ptpClock->servo);
			record->offsetFromMaster = sfptpd_time_float_ns_to_scaled_ns(offset);
			record->meanPathDelay = sfptpd_time_float_ns_to_scaled_ns(mpd);
			record->scaledNeighbourRateRatio = 0;

			/* When we have filled a set of records, flush them. */
			if (++computed_data_state->num_events == computed_data_config->events_per_tlv) {
				flushSlaveRxSyncComputedData(ptpClock, rtOpts);
				computed_data_state->num_events = 0;
			}
		}

		if (computed_data_state->skip_count == computed_data_config->logging_skip) {
			computed_data_state->skip_count = 0;
		} else {
			computed_data_state->skip_count++;
		}
	}
}


/* @task71778: Slave Event Monitoring (ingress) (IEEE1588-Rev draft 16.11.4) */
void
ingressEventMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	rxSyncTimingDataMonitor(ptpClock, rtOpts);
	rxSyncComputedDataMonitor(ptpClock, rtOpts);
}


/* @task71778: Slave Event Monitoring (tx timestamps) (IEEE1588-Rev draft 16.11.5.1) */
static void
flushSlaveTxEventTimestamps(PtpClock *ptpClock, RunTimeOpts *rtOpts, ptpd_slave_tx_ts_msg_e type)
{
	SlaveEventMonitoringConfig *config = &rtOpts->tx_event_timestamps_config;
	SlaveEventMonitoringState *state = &ptpClock->slave_tx_event_timestamps_state[type];
	SlaveTxEventTimestampsElement *records = &ptpClock->slave_tx_event_timestamps_records[type][0];

	if (config->tlv_enable) {
		MsgSignaling msgSignaling = { { 0 } };
		SlaveTxEventTimestamps data;
		ssize_t pack_result;

		assert(state->num_events <= config->events_per_tlv);

		data.eventMessageType = ptpd_tx_ts_type_to_msg_type(type);

		signalingInitOutgoingMsg(&msgSignaling, ptpClock);

		/* pack SignalingTLV */
		pack_result = packMsgSignaling(&msgSignaling, ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		assert(PACK_OK(pack_result));

		copyPortIdentity(&data.sourcePortIdentity, &ptpClock->portIdentity);
		pack_result = appendSlaveTxEventTimestampsTLV(&data,
							      records,
							      state->num_events,
							      ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		state->num_events = 0;
		sendMonitoringMessage(ptpClock, rtOpts);
	}
}


/* @task71778: Slave Event Monitoring (egress) (IEEE1588-Rev draft 16.11.5) */
void
egressEventMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts,
		   ptpd_msg_id_e msg_type, const struct sfptpd_timespec *time)
{
	ptpd_slave_tx_ts_msg_e type = ptpd_msg_type_to_tx_ts_type(msg_type);
	SlaveEventMonitoringConfig *config = &rtOpts->tx_event_timestamps_config;
	SlaveEventMonitoringState *state = &ptpClock->slave_tx_event_timestamps_state[type];
	SlaveTxEventTimestampsElement *records = &ptpClock->slave_tx_event_timestamps_records[type][0];

	assert(type != PTPD_SLAVE_TX_TS_NUM);

	if (config->logging_enable) {
		if (state->skip_count == 0) {
			SlaveTxEventTimestampsElement *record =
				&records[state->num_events];

			/* If the source port has changed, flush the old entries out. */
			if (memcmp(ptpClock->parentPortIdentity.clockIdentity,
				   state->source_port.clockIdentity,
				   CLOCK_IDENTITY_LENGTH) ||
			    (ptpClock->parentPortIdentity.portNumber !=
			     state->source_port.portNumber)) {
				if (state->num_events != 0) {
					flushSlaveTxEventTimestamps(ptpClock, rtOpts, type);
					state->num_events = 0;
				}
				state->source_port = ptpClock->parentPortIdentity;
			}

			/* Populate a new record */
			switch (type) {
			case PTPD_SLAVE_TX_TS_DELAY_REQ:
				record->sequenceId = ptpClock->sentDelayReqSequenceId;
				break;
			case PTPD_SLAVE_TX_TS_PDELAY_REQ:
				record->sequenceId = ptpClock->sentPDelayReqSequenceId;
				break;
			case PTPD_SLAVE_TX_TS_PDELAY_RESP:
				record->sequenceId = ptpClock->recvPDelayReqSequenceId;
				break;
			default:
				assert(!"Invalid tx message type for egress monitor");
			}
			fromInternalTime(time, &record->eventEgressTimestamp);

			/* When we have filled a set of records, flush them. */
			if (++state->num_events == config->events_per_tlv) {
				flushSlaveTxEventTimestamps(ptpClock, rtOpts, type);
				state->num_events = 0;
			}
		}

		if (state->skip_count == config->logging_skip) {
			state->skip_count = 0;
		} else {
			state->skip_count++;
		}
	}
}


/* @task65531: Slave Status Monitoring (Solarflare extension) */
void
slaveStatusMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts,
		   int missingMessageAlarms, int otherAlarms, int events, int flags)
{
	MsgSignaling msgSignaling;
	ssize_t pack_result;
	SlaveStatus data;
	struct sfptpd_timespec report_time;

	if (rtOpts->slave_status_monitoring_enable) {

		sfclock_gettime(CLOCK_REALTIME, &report_time);

		signalingInitOutgoingMsg(&msgSignaling, ptpClock);

		/* pack SignalingTLV */
		pack_result = packMsgSignaling(&msgSignaling, ptpClock->msgObuf, sizeof ptpClock->msgObuf);
		assert(PACK_OK(pack_result));

		copyClockIdentity(data.grandmasterIdentity, ptpClock->grandmasterIdentity);
		data.portState = ptpClock->portState;
		data.missingMessageAlarms = missingMessageAlarms;
		data.otherAlarms = otherAlarms;
		data.events = events;
		data.flags = flags;

		fromInternalTime(&report_time, &data.reportTimestamp);

		pack_result = appendSlaveStatusTLV(&data,
						   ptpClock->msgObuf,
						   sizeof ptpClock->msgObuf);
		sendMonitoringMessage(ptpClock, rtOpts);
	}
}


/* fin */
