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
 * @file   monitor.c
 * @date   Wed Jun 23 09:40:39 2010
 *
 * @brief  The code that implements monitoring mode
 *
 *
 */

#include "ptpd.h"
#include "ptpd_lib.h"


enum ptpd_tlv_result
slave_rx_sync_timing_data_handler(const MsgHeader *header, ssize_t length,
				  struct sfptpd_timespec *time, Boolean timestampValid,
				  RunTimeOpts *rtOpts, PtpClock *ptpClock,
				  TLV *tlv, size_t tlv_offset)
{
	SlaveRxSyncTimingDataTLV data;
	PtpRemoteStats stats;
	ssize_t result;

	DBGV("Signaling+SLAVE_RX_SYNC_TIMING_DATA TLV received : \n");

	if (length < PTPD_SIGNALING_LENGTH) {
		DBG("Error: Signaling message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return PTPD_TLV_RESULT_ERROR;
	}

	if (rtOpts->remoteStatsLogger.log_rx_sync_timing_data_fn == NULL) {
		DBG2("HandleSignaling : disregarding monitoring TLV when not a remote monitor\n");
		ptpClock->counters.monitoringTLVsDiscarded++;
		return PTPD_TLV_RESULT_CONTINUE;
	}

	result = unpackSlaveRxSyncTimingDataTLV(tlv->valueField, tlv->lengthField,
						&data, ptpClock);
	if (UNPACK_OK(result)) {
		stats.port_identity = &header->sourcePortIdentity;
		stats.address = &ptpClock->interface->transport.lastRecvAddr;
		stats.address_len = ptpClock->interface->transport.lastRecvAddrLen;
		stats.domain = header->domainNumber;
		stats.ref_port_identity = &data.preamble.sourcePortIdentity;

		rtOpts->remoteStatsLogger.log_rx_sync_timing_data_fn(&rtOpts->remoteStatsLogger,
								     stats,
								     data.num_elements,
								     data.elements);

		freeSlaveRxSyncTimingDataTLV(&data);
		ptpClock->counters.monitoringTLVsReceived++;
	} else {
		ptpClock->counters.messageFormatErrors++;
	}
	return PTPD_TLV_RESULT_CONTINUE;
}


enum ptpd_tlv_result
slave_rx_sync_computed_data_handler(const MsgHeader *header, ssize_t length,
				    struct sfptpd_timespec *time, Boolean timestampValid,
				    RunTimeOpts *rtOpts, PtpClock *ptpClock,
				    TLV *tlv, size_t tlv_offset)
{
	SlaveRxSyncComputedDataTLV data;
	PtpRemoteStats stats;
	ssize_t result;

	DBGV("Signaling+SLAVE_RX_SYNC_COMPUTED_DATA TLV received : \n");

	if (length < PTPD_SIGNALING_LENGTH) {
		DBG("Error: Signaling message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return PTPD_TLV_RESULT_ERROR;
	}

	if (rtOpts->remoteStatsLogger.log_rx_sync_computed_data_fn == NULL) {
		DBG2("HandleSignaling : disregarding monitoring TLV when not in monitoring mode\n");
		ptpClock->counters.monitoringTLVsDiscarded++;
		return PTPD_TLV_RESULT_CONTINUE;
	}

	result = unpackSlaveRxSyncComputedDataTLV(tlv->valueField, tlv->lengthField,
						&data, ptpClock);
	if (UNPACK_OK(result)) {
		stats.port_identity = &header->sourcePortIdentity;
		stats.address = &ptpClock->interface->transport.lastRecvAddr;
		stats.address_len = ptpClock->interface->transport.lastRecvAddrLen;
		stats.domain = header->domainNumber;
		stats.ref_port_identity = &data.preamble.sourcePortIdentity;

		rtOpts->remoteStatsLogger.log_rx_sync_computed_data_fn(&rtOpts->remoteStatsLogger,
								       stats,
								       data.num_elements,
								       data.elements);

		freeSlaveRxSyncComputedDataTLV(&data);
		ptpClock->counters.monitoringTLVsReceived++;
	} else {
		ptpClock->counters.messageFormatErrors++;
	}
	return PTPD_TLV_RESULT_CONTINUE;
}


enum ptpd_tlv_result
slave_tx_event_timestamps_handler(const MsgHeader *header, ssize_t length,
				  struct sfptpd_timespec *time, Boolean timestampValid,
				  RunTimeOpts *rtOpts, PtpClock *ptpClock,
				  TLV *tlv, size_t tlv_offset)
{
	SlaveTxEventTimestampsTLV data;
	PtpRemoteStats stats;
	ssize_t result;

	DBGV("Signaling+SLAVE_TX_EVENT_TIMESTAMPS TLV received : \n");

	if (length < PTPD_SIGNALING_LENGTH) {
		DBG("Error: Signaling message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return PTPD_TLV_RESULT_ERROR;
	}

	if (rtOpts->remoteStatsLogger.log_tx_event_timestamps_fn == NULL) {
		DBG2("HandleSignaling : disregarding monitoring TLV when not in monitoring mode\n");
		ptpClock->counters.monitoringTLVsDiscarded++;
		return PTPD_TLV_RESULT_CONTINUE;
	}

	result = unpackSlaveTxEventTimestampsTLV(tlv->valueField, tlv->lengthField,
						 &data, ptpClock);
	if (UNPACK_OK(result)) {
		stats.port_identity = &header->sourcePortIdentity;
		stats.address = &ptpClock->interface->transport.lastRecvAddr;
		stats.address_len = ptpClock->interface->transport.lastRecvAddrLen;
		stats.domain = header->domainNumber;
		stats.ref_port_identity = &data.preamble.sourcePortIdentity;

		rtOpts->remoteStatsLogger.log_tx_event_timestamps_fn(&rtOpts->remoteStatsLogger,
								     stats,
								     data.preamble.eventMessageType,
								     data.num_elements,
								     data.elements);

		freeSlaveTxEventTimestampsTLV(&data);
		ptpClock->counters.monitoringTLVsReceived++;
	} else {
		ptpClock->counters.messageFormatErrors++;
	}
	return PTPD_TLV_RESULT_CONTINUE;
}

enum ptpd_tlv_result
slave_status_handler(const MsgHeader *header, ssize_t length,
		     struct sfptpd_timespec *time, Boolean timestampValid,
		     RunTimeOpts *rtOpts, PtpClock *ptpClock,
		     TLV *tlv, size_t tlv_offset)
{
	SlaveStatus data;
	PtpRemoteStats stats;
	ssize_t result;

	DBGV("Signaling+SLAVE_STATUS TLV received : \n");

	if (length < PTPD_SIGNALING_LENGTH) {
		DBG("Error: Signaling message too short\n");
		ptpClock->counters.messageFormatErrors++;
		return PTPD_TLV_RESULT_ERROR;
	}

	if (rtOpts->remoteStatsLogger.log_slave_status_fn == NULL) {
		DBG2("HandleSignaling : disregarding monitoring TLV when not in monitoring mode\n");
		ptpClock->counters.monitoringTLVsDiscarded++;
		return PTPD_TLV_RESULT_CONTINUE;
	}

	result = unpackSlaveStatus(tlv->valueField, tlv->lengthField,
				   &data, ptpClock);
	if (UNPACK_OK(result)) {
		stats.port_identity = &header->sourcePortIdentity;
		stats.address = &ptpClock->interface->transport.lastRecvAddr;
		stats.address_len = ptpClock->interface->transport.lastRecvAddrLen;
		stats.domain = header->domainNumber;

		rtOpts->remoteStatsLogger.log_slave_status_fn(&rtOpts->remoteStatsLogger,
							      stats,
							      &data);

		ptpClock->counters.monitoringTLVsReceived++;
	} else {
		ptpClock->counters.messageFormatErrors++;
	}
	return PTPD_TLV_RESULT_CONTINUE;
}


/* fin */
