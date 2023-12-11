/*-
 * Copyright (c) 2019-2021 Xilinx, Inc.
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
 * @file   management.c
 * @date   Wed Jul 27 13:07:30 CDT 2011
 *
 * @brief  Routines to handle incoming management messages
 *
 *
 */

#include "ptpd.h"
#include <sfptpd_constants.h>

void managementInit(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	int len;
	struct sfptpd_interface *interface;

	/* Construct the product description. This is the tuple of manufacturer,
	 * model number and serial number. If we are operating in software
	 * timestamping mode, we don't report a model or serial number. */
	len = snprintf(ptpClock->product_desc, sizeof(ptpClock->product_desc),
		       "%s", SFPTPD_MANUFACTURER);
	if (len > sizeof(ptpClock->product_desc)) len = sizeof(ptpClock->product_desc);

	interface = ptpClock->interface->interface;
	if (sfptpd_interface_get_class(interface) == SFPTPD_INTERFACE_SFC) {
		snprintf(ptpClock->product_desc + len,
			 sizeof(ptpClock->product_desc) - len, ";%s;%s",
			 SFPTPD_MODEL,
			 sfptpd_interface_get_mac_string(interface));

		/* Construct the revision data. This is the tuple of hardware
		 * version, firmware version and software version. We don't
		 * have a hardware version */
		snprintf(ptpClock->revision_data,
			 sizeof(ptpClock->revision_data), ";%s;%s",
			 sfptpd_interface_get_fw_version(interface),
			 SFPTPD_VERSION_TEXT);
	} else {
		/* Construct the revision data. We're doing software
		 * timestamping so only report the daemon version */
		snprintf(ptpClock->revision_data,
			 sizeof(ptpClock->revision_data), ";;%s",
			 SFPTPD_VERSION_TEXT);
	}

	/* Copy the user description from the run-time options */
	snprintf(ptpClock->user_description, sizeof(ptpClock->user_description),
		 "%s", rtOpts->ifOpts->user_description);
}

void managementShutdown(PtpClock *ptpClock)
{
	/* Nothing to do here */
}

/**\brief Initialize outgoing management message fields*/
void managementInitOutgoingMsg(MsgManagement *incoming,
			       MsgManagement *outgoing,
			       PtpClock *ptpClock)
{
	/* set header fields */
	// TODO @management use standard message packing fn
	outgoing->header.majorSdoId = 0x0;
	outgoing->header.messageType = PTPD_MSG_MANAGEMENT;
	outgoing->header.minorVersionPTP = ptpClock->rtOpts.ptp_version_minor;
	outgoing->header.versionPTP = PTPD_PROTOCOL_VERSION;
	outgoing->header.messageLength = PTPD_MANAGEMENT_LENGTH +
					 PTPD_TLV_LENGTH -
					 PTPD_TLV_MANAGEMENT_ID_LENGTH;
	outgoing->header.domainNumber = ptpClock->domainNumber;
	outgoing->header.minorSdoId = 0x00;
	/* set header flagField to zero for management messages, Spec 13.3.2.6 */
	outgoing->header.flagField0 = 0x00;
	outgoing->header.flagField1 = 0x00;
	outgoing->header.correctionField = 0;
	outgoing->header.messageTypeSpecific = 0x00000000;
	copyPortIdentity(&outgoing->header.sourcePortIdentity, &ptpClock->portIdentity);
	outgoing->header.sequenceId = incoming->header.sequenceId;
	outgoing->header.controlField = PTPD_CONTROL_FIELD_MANAGEMENT;
	outgoing->header.logMessageInterval = PTPD_MESSAGE_INTERVAL_UNDEFINED;

	/* set management message fields */
	copyPortIdentity(&outgoing->targetPortIdentity, &incoming->header.sourcePortIdentity);
	outgoing->startingBoundaryHops = incoming->startingBoundaryHops - incoming->boundaryHops;
	outgoing->boundaryHops = outgoing->startingBoundaryHops;
	/* set default action to avoid uninitialized value */
	outgoing->actionField = PTPD_MGMT_ACTION_GET;

	/* init managementTLV */
	XMALLOC(outgoing->tlv, sizeof(ManagementTLV));
	outgoing->tlv->tlvType = PTPD_TLV_MANAGEMENT;
	outgoing->tlv->managementId = incoming->tlv->managementId;
	outgoing->tlv->dataField = NULL;
}

/**\brief Handle incoming NULL_MANAGEMENT message*/
ptpd_mgmt_error_e handleMMNullManagement(MsgManagement* incoming,
					 MsgManagement* outgoing,
					 PtpClock* ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;

	DBGV("received NULL_MANAGEMENT message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
	case PTPD_MGMT_ACTION_SET:
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		DBGV(" GET/SET action\n");
		break;
	case PTPD_MGMT_ACTION_COMMAND:
		outgoing->actionField = PTPD_MGMT_ACTION_ACKNOWLEDGE;
		DBGV(" COMMAND action\n");
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming CLOCK_DESCRIPTION management message*/
ptpd_mgmt_error_e handleMMClockDescription(MsgManagement *incoming,
					   MsgManagement *outgoing,
					   PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMClockDescription *data = NULL;

	DBGV("received CLOCK_DESCRIPTION message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		/* Table 38 */
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMClockDescription));
		data = (MMClockDescription*)outgoing->tlv->dataField;
		memset(data, 0, sizeof(MMClockDescription));
		/* this is an ordnary node, clockType bit-array entry 0 is one */
		data->clockType0 = PTPD_CLOCK_TYPE_ORDINARY;
		data->clockType1 = 0;
		/* physical layer protocol */
		data->physicalLayerProtocol.lengthField =
			sizeof(PTPD2_PHYSICAL_LAYER_PROTOCOL) - 1;
		XMALLOC(data->physicalLayerProtocol.textField,
			data->physicalLayerProtocol.lengthField);
		memcpy(data->physicalLayerProtocol.textField,
		       &PTPD2_PHYSICAL_LAYER_PROTOCOL,
		       data->physicalLayerProtocol.lengthField);
		/* physical address i.e. the MAC address */
		data->physicalAddress.addressLength = PTP_UUID_LENGTH;
		XMALLOC(data->physicalAddress.addressField, PTP_UUID_LENGTH);
		memcpy(data->physicalAddress.addressField,
		       ptpClock->interface->transport.interfaceID,
		       PTP_UUID_LENGTH);
		/* protocol address e.g. IP address */
		writeProtocolAddress(&data->protocolAddress,
				     &ptpClock->interface->transport.interfaceAddr,
				     ptpClock->interface->transport.interfaceAddrLen);
		/* manufacturerIdentity OUI */
		data->manufacturerIdentity0 = SFPTPD_OUI0;
		data->manufacturerIdentity1 = SFPTPD_OUI1;
		data->manufacturerIdentity2 = SFPTPD_OUI2;
		/* reserved */
		data->reserved = 0;
		/* product description */
		data->productDescription.lengthField = strlen(ptpClock->product_desc);
		XMALLOC(data->productDescription.textField,
			data->productDescription.lengthField);
		memcpy(data->productDescription.textField,
		       ptpClock->product_desc,
		       data->productDescription.lengthField);
		/* revision data */
		data->revisionData.lengthField = strlen(ptpClock->revision_data);
		XMALLOC(data->revisionData.textField,
			data->revisionData.lengthField);
		memcpy(data->revisionData.textField,
		       ptpClock->revision_data,
		       data->revisionData.lengthField);
		/* user description */
		data->userDescription.lengthField = strlen(ptpClock->user_description);
		XMALLOC(data->userDescription.textField,
			data->userDescription.lengthField);
		memcpy(data->userDescription.textField,
		       ptpClock->user_description,
		       data->userDescription.lengthField);
		/* the profile identity for the default profile in use */
		data->profileIdentity0 = ptpClock->rtOpts.profile->id[0];
		data->profileIdentity1 = ptpClock->rtOpts.profile->id[1];
		data->profileIdentity2 = ptpClock->rtOpts.profile->id[2];
		data->profileIdentity3 = ptpClock->rtOpts.profile->id[3];
		data->profileIdentity4 = ptpClock->rtOpts.profile->id[4];
		data->profileIdentity5 = ptpClock->rtOpts.profile->id[5];
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming SLAVE_ONLY management message type*/
ptpd_mgmt_error_e handleMMSlaveOnly(MsgManagement *incoming,
				    MsgManagement *outgoing,
				    PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMSlaveOnly *data = NULL;

	DBGV("received SLAVE_ONLY message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMSlaveOnly*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->slaveOnly = (data->so != 0)? TRUE: FALSE;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMSlaveOnly));
		data = (MMSlaveOnly*)outgoing->tlv->dataField;
		/* GET actions */
		data->so = ptpClock->slaveOnly? 1: 0;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming USER_DESCRIPTION management message type*/
ptpd_mgmt_error_e handleMMUserDescription(MsgManagement *incoming,
					  MsgManagement *outgoing,
					  PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMUserDescription *data = NULL;

	DBGV("received USER_DESCRIPTION message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMUserDescription*)incoming->tlv->dataField;
		UInteger8 userDescriptionLength = data->userDescription.lengthField;
		if (userDescriptionLength <= PTPD_MGMT_USER_DESCRIPTION_MAX) {
			memset(ptpClock->user_description, 0, sizeof(ptpClock->user_description));
			memcpy(ptpClock->user_description, data->userDescription.textField,
					userDescriptionLength);
			/* add null-terminator to make use of C string function strlen later */
			ptpClock->user_description[userDescriptionLength] = '\0';
		} else {
			WARNING("management user description exceeds specification length \n");
			return PTPD_MGMT_ERROR_WRONG_LENGTH;
		}
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof( MMUserDescription));
		data = (MMUserDescription*)outgoing->tlv->dataField;
		memset(data, 0, sizeof(MMUserDescription));
		/* GET actions */
		data->userDescription.lengthField = strlen(ptpClock->user_description);
		XMALLOC(data->userDescription.textField,
			data->userDescription.lengthField);
		memcpy(data->userDescription.textField,
		       ptpClock->user_description,
		       data->userDescription.lengthField);
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming INITIALIZE management message type*/
ptpd_mgmt_error_e handleMMInitialize(MsgManagement *incoming,
				     MsgManagement *outgoing,
				     PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMInitialize *incomingData = NULL;
	MMInitialize *outgoingData = NULL;

	DBGV("received INITIALIZE message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_COMMAND:
		DBGV(" COMMAND action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_ACKNOWLEDGE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMInitialize));
		incomingData = (MMInitialize*)incoming->tlv->dataField;
		outgoingData = (MMInitialize*)outgoing->tlv->dataField;
		/* Table 45 - INITIALIZATION_KEY enumeration */
		switch (incomingData->initializeKey) {
		case PTPD_MGMT_INITIALIZE_EVENT:
			/* cause INITIALIZE event */
			ptpClock->portState = PTPD_INITIALIZING;
			break;
		default:
			/* do nothing, implementation specific */
			DBGV("initializeKey != 0, do nothing\n");
		}
		outgoingData->initializeKey = incomingData->initializeKey;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming DEFAULT_DATA_SET management message type*/
ptpd_mgmt_error_e handleMMDefaultDataSet(MsgManagement *incoming,
					 MsgManagement *outgoing,
					 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMDefaultDataSet *data = NULL;

	DBGV("received DEFAULT_DATA_SET message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMDefaultDataSet));
		data = (MMDefaultDataSet*)outgoing->tlv->dataField;
		/* GET actions */
		/* get bit and align for slave only */
		Octet so = ptpClock->slaveOnly? 2: 0;
		/* get bit and align by shifting right 1 since TWO_STEP_FLAG is either 0b00 or 0b10 */
		Octet tsc = ptpClock->twoStepFlag >> 1;
		data->so_tsc = so | tsc;
		data->reserved0 = 0x0;
		data->numberPorts = ptpClock->interface->global->ports_created;
		data->priority1 = ptpClock->priority1;
		data->clockQuality.clockAccuracy = ptpClock->clockQuality.clockAccuracy;
		data->clockQuality.clockClass = ptpClock->clockQuality.clockClass;
		data->clockQuality.offsetScaledLogVariance =
				ptpClock->clockQuality.offsetScaledLogVariance;
		data->priority2 = ptpClock->priority2;
		/* copy clockIdentity */
		copyClockIdentity(data->clockIdentity, ptpClock->clockIdentity);
		data->domainNumber = ptpClock->domainNumber;
		data->reserved1 = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

void populateCurrentDataSet(MMCurrentDataSet *data, PtpClock *ptpClock)
{
	sfptpd_time_t ofm, mpd;

	data->stepsRemoved = ptpClock->stepsRemoved;
	ofm = servo_get_offset_from_master(&ptpClock->servo);
	data->offsetFromMaster.scaledNanoseconds = sfptpd_time_float_ns_to_scaled_ns(ofm);
	mpd = servo_get_mean_path_delay(&ptpClock->servo);
	data->meanPathDelay.scaledNanoseconds = sfptpd_time_float_ns_to_scaled_ns(mpd);
}

/**\brief Handle incoming CURRENT_DATA_SET management message type*/
ptpd_mgmt_error_e handleMMCurrentDataSet(MsgManagement *incoming,
					 MsgManagement *outgoing,
					 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMCurrentDataSet *data = NULL;

	DBGV("received CURRENT_DATA_SET message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof( MMCurrentDataSet));
		data = (MMCurrentDataSet*)outgoing->tlv->dataField;
		/* GET actions */
		populateCurrentDataSet(data, ptpClock);
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

void populateParentDataSet(MMParentDataSet *data, PtpClock *ptpClock)
{
	copyPortIdentity(&data->parentPortIdentity, &ptpClock->parentPortIdentity);
	data->PS = ptpClock->parentStats;
	data->reserved = 0;
	data->observedParentOffsetScaledLogVariance =
		ptpClock->observedParentOffsetScaledLogVariance;
	data->observedParentClockPhaseChangeRate =
		ptpClock->observedParentClockPhaseChangeRate;
	data->grandmasterPriority1 = ptpClock->grandmasterPriority1;
	data->grandmasterClockQuality.clockAccuracy =
		ptpClock->grandmasterClockQuality.clockAccuracy;
	data->grandmasterClockQuality.clockClass =
		ptpClock->grandmasterClockQuality.clockClass;
	data->grandmasterClockQuality.offsetScaledLogVariance =
		ptpClock->grandmasterClockQuality.offsetScaledLogVariance;
	data->grandmasterPriority2 = ptpClock->grandmasterPriority2;
	copyClockIdentity(data->grandmasterIdentity, ptpClock->grandmasterIdentity);
}

/**\brief Handle incoming PARENT_DATA_SET management message type*/
ptpd_mgmt_error_e handleMMParentDataSet(MsgManagement *incoming,
					MsgManagement *outgoing,
					PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMParentDataSet *data = NULL;

	DBGV("received PARENT_DATA_SET message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMParentDataSet));
		data = (MMParentDataSet*)outgoing->tlv->dataField;
		/* GET actions */
		populateParentDataSet(data, ptpClock);
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

void populateTimePropertiesDataSet(MMTimePropertiesDataSet *data, PtpClock *ptpClock)
{
	data->currentUtcOffset = ptpClock->timePropertiesDS.currentUtcOffset;
	Octet ftra = SET_FIELD(ptpClock->timePropertiesDS.frequencyTraceable, PTPD_FTRA);
	Octet ttra = SET_FIELD(ptpClock->timePropertiesDS.timeTraceable, PTPD_TTRA);
	Octet ptp = SET_FIELD(ptpClock->timePropertiesDS.ptpTimescale, PTPD_PTPT);
	Octet utcv = SET_FIELD(ptpClock->timePropertiesDS.currentUtcOffsetValid, PTPD_UTCV);
	Octet li59 = SET_FIELD(ptpClock->timePropertiesDS.leap59, PTPD_LI59);
	Octet li61 = SET_FIELD(ptpClock->timePropertiesDS.leap61, PTPD_LI61);
	data->ftra_ttra_ptp_utcv_li59_li61 = ftra | ttra | ptp | utcv | li59 | li61;
	data->timeSource = ptpClock->timePropertiesDS.timeSource;
}

/**\brief Handle incoming PROPERTIES_DATA_SET management message type*/
ptpd_mgmt_error_e handleMMTimePropertiesDataSet(MsgManagement *incoming,
						MsgManagement *outgoing,
						PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMTimePropertiesDataSet *data = NULL;

	DBGV("received TIME_PROPERTIES message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMTimePropertiesDataSet));
		data = (MMTimePropertiesDataSet*)outgoing->tlv->dataField;
		/* GET actions */
		populateTimePropertiesDataSet(data, ptpClock);
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming PORT_DATA_SET management message type*/
ptpd_mgmt_error_e handleMMPortDataSet(MsgManagement *incoming,
				      MsgManagement *outgoing,
				      PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMPortDataSet *data = NULL;
	sfptpd_time_t mpd;

	DBGV("received PORT_DATA_SET message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMPortDataSet));
		data = (MMPortDataSet*)outgoing->tlv->dataField;
		copyPortIdentity(&data->portIdentity, &ptpClock->portIdentity);
		data->portState = ptpClock->portState;
		data->logMinDelayReqInterval = ptpClock->logMinDelayReqInterval;
		mpd = servo_get_mean_path_delay(&ptpClock->servo);
		/* TODO The IEEE spec specifies the peer delay but this looks
		 * like it might be a bug. */
		data->peerMeanPathDelay.scaledNanoseconds = sfptpd_time_float_ns_to_scaled_ns(mpd);
		data->logAnnounceInterval = ptpClock->logAnnounceInterval;
		data->announceReceiptTimeout = ptpClock->announceReceiptTimeout;
		data->logSyncInterval = ptpClock->logSyncInterval;
		data->delayMechanism  = ptpClock->delayMechanism;
		data->logMinPdelayReqInterval = ptpClock->logMinPdelayReqInterval;
		data->reserved = 0;
		data->versionNumber = PTPD_PROTOCOL_VERSION;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming PRIORITY1 management message type*/
ptpd_mgmt_error_e handleMMPriority1(MsgManagement *incoming,
				    MsgManagement *outgoing,
				    PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMPriority1 *data = NULL;

	DBGV("received PRIORITY1 message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMPriority1*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->priority1 = data->priority1;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMPriority1));
		data = (MMPriority1*)outgoing->tlv->dataField;
		/* GET actions */
		data->priority1 = ptpClock->priority1;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming PRIORITY2 management message type*/
ptpd_mgmt_error_e handleMMPriority2(MsgManagement *incoming,
				    MsgManagement *outgoing,
				    PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMPriority2 *data = NULL;

	DBGV("received PRIORITY2 message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMPriority2*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->priority2 = data->priority2;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMPriority2));
		data = (MMPriority2*)outgoing->tlv->dataField;
		/* GET actions */
		data->priority2 = ptpClock->priority2;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming DOMAIN management message type*/
ptpd_mgmt_error_e handleMMDomain(MsgManagement *incoming,
				 MsgManagement *outgoing,
				 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMDomain *data = NULL;

	DBGV("received DOMAIN message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMDomain*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->domainNumber = data->domainNumber;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMDomain));
		data = (MMDomain*)outgoing->tlv->dataField;
		/* GET actions */
		data->domainNumber = ptpClock->domainNumber;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming LOG_ANNOUNCE_INTERVAL management message type*/
ptpd_mgmt_error_e handleMMLogAnnounceInterval(MsgManagement *incoming,
					      MsgManagement *outgoing,
					      PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received LOG_ANNOUNCE_INTERVAL message\n");

	MMLogAnnounceInterval* data = NULL;

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMLogAnnounceInterval*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->logAnnounceInterval = data->logAnnounceInterval;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMLogAnnounceInterval));
		data = (MMLogAnnounceInterval*)outgoing->tlv->dataField;
		/* GET actions */
		data->logAnnounceInterval = ptpClock->logAnnounceInterval;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming ANNOUNCE_RECEIPT_TIMEOUT management message type*/
ptpd_mgmt_error_e handleMMAnnounceReceiptTimeout(MsgManagement *incoming,
						 MsgManagement *outgoing,
						 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received ANNOUNCE_RECEIPT_TIMEOUT message\n");

	MMAnnounceReceiptTimeout* data = NULL;

	switch(incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMAnnounceReceiptTimeout*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->announceReceiptTimeout = data->announceReceiptTimeout;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMAnnounceReceiptTimeout));
		data = (MMAnnounceReceiptTimeout*)outgoing->tlv->dataField;
		/* GET actions */
		data->announceReceiptTimeout = ptpClock->announceReceiptTimeout;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming LOG_SYNC_INTERVAL management message type*/
ptpd_mgmt_error_e handleMMLogSyncInterval(MsgManagement *incoming,
					  MsgManagement *outgoing,
					  PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received LOG_SYNC_INTERVAL message\n");

	MMLogSyncInterval* data = NULL;

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMLogSyncInterval*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->logSyncInterval = data->logSyncInterval;

		/* Update the configured interval in the servo */
		servo_set_interval(&ptpClock->servo,
				   powl(2, ptpClock->logSyncInterval));

		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMLogSyncInterval));
		data = (MMLogSyncInterval*)outgoing->tlv->dataField;
		/* GET actions */
		data->logSyncInterval = ptpClock->logSyncInterval;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming VERSION_NUMBER management message type*/
ptpd_mgmt_error_e handleMMVersionNumber(MsgManagement *incoming,
					MsgManagement *outgoing,
					PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received VERSION_NUMBER message\n");

	MMVersionNumber* data = NULL;

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMVersionNumber));
		data = (MMVersionNumber*)outgoing->tlv->dataField;
		/* GET actions */
		data->reserved0 = 0x0;
		data->versionNumber = PTPD_PROTOCOL_VERSION;
		data->reserved1 = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming ENABLE_PORT management message type*/
ptpd_mgmt_error_e handleMMEnablePort(MsgManagement *incoming,
				     MsgManagement *outgoing,
				     PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received ENABLE_PORT message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_COMMAND:
		DBGV(" COMMAND action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_ACKNOWLEDGE;
		/* check if port is disabled, if so then initialize */
		if(ptpClock->portState == PTPD_DISABLED) {
			ptpClock->portState = PTPD_INITIALIZING;
		}
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming DISABLE_PORT management message type*/
ptpd_mgmt_error_e handleMMDisablePort(MsgManagement *incoming,
				      MsgManagement *outgoing,
				      PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	DBGV("received DISABLE_PORT message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_COMMAND:
		DBGV(" COMMAND action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_ACKNOWLEDGE;
		/* disable port */
		ptpClock->portState = PTPD_DISABLED;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming TIME management message type*/
ptpd_mgmt_error_e handleMMTime(MsgManagement *incoming,
			       MsgManagement *outgoing,
			       PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMTime *data = NULL;

	DBGV("received TIME message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		if (ptpClock->slaveOnly ||
		    ptpClock->portState == PTPD_SLAVE ||
		    ptpClock->portState == PTPD_UNCALIBRATED) {
			return PTPD_MGMT_ERROR_NOT_SUPPORTED;
		} else {
			DBGV(" SET action\n");
			data = (MMTime*)incoming->tlv->dataField;
			/* SET actions */
			/* TODO: add currentTime */
		}
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMTime));
		data = (MMTime*)outgoing->tlv->dataField;
		/* GET actions */
		TimeInternal internalTime;
		getTime(&internalTime);
		if ((ptpClock->portState != PTPD_MASTER) &&
		    (ptpClock->timePropertiesDS.currentUtcOffsetValid || rtOpts->alwaysRespectUtcOffset)) {
			internalTime.seconds -= ptpClock->timePropertiesDS.currentUtcOffset;
		}
		fromInternalTime(&internalTime, &data->currentTime);
		timestamp_display(&data->currentTime);
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming CLOCK_ACCURACY management message type*/
ptpd_mgmt_error_e handleMMClockAccuracy(MsgManagement *incoming,
					MsgManagement *outgoing,
					PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMClockAccuracy *data = NULL;

	DBGV("received CLOCK_ACCURACY message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		if (ptpClock->slaveOnly ||
		    ptpClock->portState == PTPD_SLAVE ||
		    ptpClock->portState == PTPD_UNCALIBRATED) {
			return PTPD_MGMT_ERROR_NOT_SUPPORTED;
		} else {
			DBGV(" SET action\n");
			data = (MMClockAccuracy*)incoming->tlv->dataField;
			/* SET actions */
			ptpClock->clockQuality.clockAccuracy = data->clockAccuracy;
			ptpClock->record_update = TRUE;
		}
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMClockAccuracy));
		data = (MMClockAccuracy*)outgoing->tlv->dataField;
		/* GET actions */
		data->clockAccuracy = ptpClock->clockQuality.clockAccuracy;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming UTC_PROPERTIES management message type*/
ptpd_mgmt_error_e handleMMUtcProperties(MsgManagement *incoming,
					MsgManagement *outgoing,
					PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMUtcProperties *data = NULL;

	DBGV("received UTC_PROPERTIES message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		if (ptpClock->slaveOnly ||
		    ptpClock->portState == PTPD_SLAVE ||
		    ptpClock->portState == PTPD_UNCALIBRATED) {
			return PTPD_MGMT_ERROR_NOT_SUPPORTED;
		} else {
			DBGV(" SET action\n");
			data = (MMUtcProperties*)incoming->tlv->dataField;
			/* SET actions */
			ptpClock->timePropertiesDS.currentUtcOffset = data->currentUtcOffset;
			/* set bit */
			ptpClock->timePropertiesDS.currentUtcOffsetValid = IS_SET(data->utcv_li59_li61, PTPD_UTCV);
			ptpClock->timePropertiesDS.leap59 = IS_SET(data->utcv_li59_li61, PTPD_LI59);
			ptpClock->timePropertiesDS.leap61 = IS_SET(data->utcv_li59_li61, PTPD_LI61);
			ptpClock->record_update = TRUE;
		}
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMUtcProperties));
		data = (MMUtcProperties*)outgoing->tlv->dataField;
		/* GET actions */
		data->currentUtcOffset = ptpClock->timePropertiesDS.currentUtcOffset;
		Octet utcv = SET_FIELD(ptpClock->timePropertiesDS.currentUtcOffsetValid, PTPD_UTCV);
		Octet li59 = SET_FIELD(ptpClock->timePropertiesDS.leap59, PTPD_LI59);
		Octet li61 = SET_FIELD(ptpClock->timePropertiesDS.leap61, PTPD_LI61);
		data->utcv_li59_li61 = utcv | li59 | li61;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming TRACEABILITY_PROPERTIES management message type*/
ptpd_mgmt_error_e handleMMTraceabilityProperties(MsgManagement *incoming,
						 MsgManagement *outgoing,
						 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMTraceabilityProperties *data = NULL;

	DBGV("received TRACEABILITY_PROPERTIES message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		if (ptpClock->slaveOnly ||
		    ptpClock->portState == PTPD_SLAVE ||
		    ptpClock->portState == PTPD_UNCALIBRATED) {
			return PTPD_MGMT_ERROR_NOT_SUPPORTED;
		} else {
			DBGV(" SET action\n");
			data = (MMTraceabilityProperties*)incoming->tlv->dataField;
			/* SET actions */
			ptpClock->timePropertiesDS.frequencyTraceable = IS_SET(data->ftra_ttra, PTPD_FTRA);
			ptpClock->timePropertiesDS.timeTraceable = IS_SET(data->ftra_ttra, PTPD_TTRA);
			ptpClock->record_update = TRUE;
		}
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMTraceabilityProperties));
		data = (MMTraceabilityProperties*)outgoing->tlv->dataField;
		/* GET actions */
		Octet ftra = SET_FIELD(ptpClock->timePropertiesDS.frequencyTraceable, PTPD_FTRA);
		Octet ttra = SET_FIELD(ptpClock->timePropertiesDS.timeTraceable, PTPD_TTRA);
		data->ftra_ttra = ftra | ttra;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming DELAY_MECHANISM management message type*/
ptpd_mgmt_error_e handleMMDelayMechanism(MsgManagement *incoming,
					 MsgManagement *outgoing,
					 PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMDelayMechanism *data = NULL;

	DBGV("received DELAY_MECHANISM message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMDelayMechanism*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->delayMechanism = data->delayMechanism;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMDelayMechanism));
		data = (MMDelayMechanism*)outgoing->tlv->dataField;
		/* GET actions */
		data->delayMechanism = ptpClock->delayMechanism;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming LOG_MIN_PDELAY_REQ_INTERVAL management message type*/
ptpd_mgmt_error_e handleMMLogMinPdelayReqInterval(MsgManagement *incoming,
						  MsgManagement *outgoing,
						  PtpClock *ptpClock)
{
	ptpd_mgmt_error_e rc = PTPD_MGMT_OK;
	MMLogMinPdelayReqInterval *data = NULL;

	DBGV("received LOG_MIN_PDELAY_REQ_INTERVAL message\n");

	switch (incoming->actionField) {
	case PTPD_MGMT_ACTION_SET:
		DBGV(" SET action\n");
		data = (MMLogMinPdelayReqInterval*)incoming->tlv->dataField;
		/* SET actions */
		ptpClock->logMinPdelayReqInterval = data->logMinPdelayReqInterval;
		ptpClock->record_update = TRUE;
		/* intentionally fall through to GET case */
	case PTPD_MGMT_ACTION_GET:
		DBGV(" GET action\n");
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		XMALLOC(outgoing->tlv->dataField, sizeof(MMLogMinPdelayReqInterval));
		data = (MMLogMinPdelayReqInterval*)outgoing->tlv->dataField;
		/* GET actions */
		data->logMinPdelayReqInterval = ptpClock->logMinPdelayReqInterval;
		data->reserved = 0x0;
		break;
	default:
		DBGV(" unhandled action %d\n", incoming->actionField);
		rc = PTPD_MGMT_ERROR_NOT_SUPPORTED;
		break;
	}

	return rc;
}

/**\brief Handle incoming ERROR_STATUS management message type*/
void handleMMErrorStatus(MsgManagement *incoming)
{
	(void)incoming;
	DBGV("received MANAGEMENT_ERROR_STATUS message \n");
	/* implementation specific */
}

/**\brief Handle issuing ERROR_STATUS management message type*/
void handleErrorManagementMessage(MsgManagement *incoming,
				  MsgManagement *outgoing, PtpClock *ptpClock,
				  ptpd_mgmt_error_e errorId)
{
	/* init management error status tlv fields */
	outgoing->tlv->tlvType = PTPD_TLV_MANAGEMENT_ERROR_STATUS;
	/* management error status managementId field is the errorId */
	outgoing->tlv->managementId = (Enumeration16)errorId;
	switch(incoming->actionField)
	{
	case PTPD_MGMT_ACTION_GET:
	case PTPD_MGMT_ACTION_SET:
		outgoing->actionField = PTPD_MGMT_ACTION_RESPONSE;
		break;
	case PTPD_MGMT_ACTION_COMMAND:
		outgoing->actionField = PTPD_MGMT_ACTION_ACKNOWLEDGE;
		break;
	default:
		outgoing->actionField = 0;
	}

	XMALLOC(outgoing->tlv->dataField, sizeof(MMErrorStatus));
	MMErrorStatus *data = (MMErrorStatus*)outgoing->tlv->dataField;
	/* set managementId */
	data->managementId = incoming->tlv->managementId;
	data->reserved = 0x00;
	data->displayData.lengthField = 0;
	data->displayData.textField = NULL;
}


/* fin */
