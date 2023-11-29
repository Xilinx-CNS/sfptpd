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
 * @file   display.c
 * @date   Thu Aug 12 09:06:21 2010
 *
 * @brief  General routines for displaying internal data.
 *
 *
 */

#include "ptpd.h"
#include <sys/socket.h>
#include <netdb.h>

/**\brief Display an Integer64 type*/
void
integer64_display(const char *fieldName, const Integer64 * bigint)
{
  DBGV("%s : %"PRIi64"\n", fieldName, *bigint);
}

/**\brief Display an UInteger48 type*/
void
uInteger48_display(const char *fieldName, const UInteger48 * bigint)
{
  DBGV("%s : %"PRIu64"\n", fieldName, (*bigint) & 0xFFFFFFFFFFFFULL);
}

/** \brief Display a struct sfptpd_timespec Structure*/
void
timespec_display(const struct timespec *time)
{
	DBGV("seconds : %ld \n", time->tv_sec);
	DBGV("nanoseconds : %ld \n", time->tv_nsec);
}

/** \brief Display a struct sfptpd_timespec Structure*/
void
sftimespec_display(const struct sfptpd_timespec *time)
{
	DBGV("seconds : %ld \n", time->sec);
	DBGV("nanoseconds : %ld.%03d \n", time->nsec,
	     (uint32_t) (((uint64_t) time->nsec_frac) * 1000 >> 32));
}

/** \brief Display a Timestamp Structure*/
void
timestamp_display(const Timestamp * timestamp)
{
	uInteger48_display("seconds", &timestamp->secondsField);
	DBGV("nanoseconds : %u \n", timestamp->nanosecondsField);
}

/**\brief Display a Clockidentity Structure*/
void
clockIdentity_display(const ClockIdentity clockIdentity)
{

	DBGV(
	    "ClockIdentity : %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
	    clockIdentity[0], clockIdentity[1], clockIdentity[2],
	    clockIdentity[3], clockIdentity[4], clockIdentity[5],
	    clockIdentity[6], clockIdentity[7]
	);

}

/**\brief Display MAC address*/
void
clockUUID_display(const Octet * sourceUuid)
{

	DBGV(
	    "sourceUuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
	    sourceUuid[0], sourceUuid[1], sourceUuid[2], sourceUuid[3],
	    sourceUuid[4], sourceUuid[5]
	);

}

/**\brief Display a network address*/
void address_display(const char *key,
		     const struct sockaddr_storage *address,
		     socklen_t length,
		     Boolean verbose)
{
	int rc;
	char host_buf[NI_MAXHOST] = "";

	rc = getnameinfo((struct sockaddr *) address, length,
			 host_buf, sizeof host_buf,
			 NULL, 0,
			 NI_NUMERICHOST);

	if (rc != 0) {
		snprintf(host_buf, sizeof host_buf,
			 "(%s)",
			 gai_strerror(rc));
	}

	if (verbose) {
		DBGV("%s : %s \n", key, host_buf);
	} else {
		DBG("%s : %s \n", key, host_buf);
	}
}


/**\brief Display Network info*/
void
netPath_display(const struct ptpd_transport *net,
		const PtpClock *ptpClock)
{
	DBGV("eventSock : %d \n", net->eventSock);
	DBGV("generalSock : %d \n", net->generalSock);
	address_display("multicastAddress", &net->multicastAddr, net->multicastAddrLen, TRUE);
	address_display("peerAddress", &net->peerMulticastAddr, net->peerMulticastAddrLen, TRUE);
	address_display("unicastAddress", &net->peerMulticastAddr, net->peerMulticastAddrLen, TRUE);
}

/**\brief Display a IntervalTimer Structure*/
void
intervalTimer_display(const IntervalTimer * ptimer)
{
	DBGV("interval : %d \n", ptimer->interval);
	DBGV("left : %d \n", ptimer->left);
	DBGV("expire : %d \n", ptimer->expire);
}




/**\brief Display a Portidentity Structure*/
void
portIdentity_display(const PortIdentity * portIdentity)
{
	clockIdentity_display(portIdentity->clockIdentity);
	DBGV("port number : %d \n", portIdentity->portNumber);

}

/**\brief Display a Clockquality Structure*/
void
clockQuality_display(const ClockQuality * clockQuality)
{
	DBGV("clockClass : %d \n", clockQuality->clockClass);
	DBGV("clockAccuracy : %d \n", clockQuality->clockAccuracy);
	DBGV("offsetScaledLogVariance : %d \n", clockQuality->offsetScaledLogVariance);
}

/**\brief Display PTPText Structure*/
void
PTPText_display(const PTPText *p, const PtpClock *ptpClock)
{
	DBGV("    lengthField : %d \n", p->lengthField);
	DBGV("    textField : %.*s \n", (int)p->lengthField,  p->textField);
}

/**\brief Display the Network Interface Name*/
void
iFaceName_display(const Octet * iFaceName)
{

	int i;

	DBGV("iFaceName : ");

	for (i = 0; i < IFACE_NAME_LENGTH; i++) {
		DBGV("%c", iFaceName[i]);
	}
	DBGV("\n");

}

/**\brief Display an Unicast Adress*/
void
unicast_display(const Octet * unicast)
{

	int i;

	DBGV("Unicast adress : ");

	for (i = 0; i < NET_ADDRESS_LENGTH; i++) {
		DBGV("%c", unicast[i]);
	}
	DBGV("\n");

}


/**\brief Display Sync message*/
void
msgSync_display(const MsgSync * sync)
{
	DBGV("Message Sync : \n");
	timestamp_display(&sync->originTimestamp);
	DBGV("\n");
}

/**\brief Display Header message*/
void
msgHeader_display(const MsgHeader * header)
{
	DBGV("Message header : \n");
	DBGV("\n");
	DBGV("majorSdoId : 0x%01x\n", header->majorSdoId);
	DBGV("messageType : %d\n", header->messageType);
	DBGV("minorVersionPTP : %d\n", header->minorVersionPTP);
	DBGV("versionPTP : %d\n", header->versionPTP);
	DBGV("messageLength : %d\n", header->messageLength);
	DBGV("domainNumber : %d\n", header->domainNumber);
	DBGV("minorSdoId: 0x%02x\n", header->minorSdoId);
	DBGV("FlagField %02hhx:%02hhx\n", header->flagField0, header->flagField1);
	integer64_display("CorrectionField", &header->correctionField);
	DBGV("SourcePortIdentity : \n");
	portIdentity_display(&header->sourcePortIdentity);
	DBGV("sequenceId : %d\n", header->sequenceId);
	DBGV("controlField : %d\n", header->controlField);
	DBGV("logMessageInterval : %d\n", header->logMessageInterval);
	DBGV("\n");
}

/**\brief Display Announce message*/
void
msgAnnounce_display(const MsgAnnounce * announce)
{
	DBGV("Announce Message : \n");
	DBGV("\n");
	DBGV("originTimestamp : \n");
	DBGV("secondField  : \n");
	timestamp_display(&announce->originTimestamp);
	DBGV("currentUtcOffset : %d \n", announce->currentUtcOffset);
	DBGV("grandMasterPriority1 : %d \n", announce->grandmasterPriority1);
	DBGV("grandMasterClockQuality : \n");
	clockQuality_display(&announce->grandmasterClockQuality);
	DBGV("grandMasterPriority2 : %d \n", announce->grandmasterPriority2);
	DBGV("grandMasterIdentity : \n");
	clockIdentity_display(announce->grandmasterIdentity);
	DBGV("stepsRemoved : %d \n", announce->stepsRemoved);
	DBGV("timeSource : %d \n", announce->timeSource);
	DBGV("\n");
}

/**\brief Display Follow_UP message*/
void
msgFollowUp_display(const MsgFollowUp * follow)
{
	timestamp_display(&follow->preciseOriginTimestamp);
}

/**\brief Display DelayReq message*/
void
msgDelayReq_display(const MsgDelayReq * req)
{
	timestamp_display(&req->originTimestamp);
}

/**\brief Display DelayResp message*/
void
msgDelayResp_display(const MsgDelayResp * resp)
{
	timestamp_display(&resp->receiveTimestamp);
	portIdentity_display(&resp->requestingPortIdentity);
}

/**\brief Display Pdelay_Req message*/
void
msgPDelayReq_display(const MsgPDelayReq * preq)
{
	timestamp_display(&preq->originTimestamp);
}

/**\brief Display Pdelay_Resp message*/
void
msgPDelayResp_display(const MsgPDelayResp * presp)
{

	timestamp_display(&presp->requestReceiptTimestamp);
	portIdentity_display(&presp->requestingPortIdentity);
}

/**\brief Display Pdelay_Resp Follow Up message*/
void
msgPDelayRespFollowUp_display(const MsgPDelayRespFollowUp * prespfollow)
{

	timestamp_display(&prespfollow->responseOriginTimestamp);
	portIdentity_display(&prespfollow->requestingPortIdentity);
}

/**\brief Display Signaling message*/
void
msgSignaling_display(const MsgSignaling * signaling)
{
        DBGV("Signaling Message : \n");
        DBGV("\n");
        DBGV("targetPortIdentity : \n");
	portIdentity_display(&signaling->targetPortIdentity);
}

/**\brief Display Management message*/
void
msgManagement_display(const MsgManagement * manage)
{
        DBGV("Management Message : \n");
        DBGV("\n");
        DBGV("targetPortIdentity : \n");
	portIdentity_display(&manage->targetPortIdentity);
	DBGV("startingBoundaryHops : %d \n", manage->startingBoundaryHops);
	DBGV("boundaryHops : %d \n", manage->boundaryHops);
	DBGV("actionField : %d\n", manage->actionField);
}

/**\brief Display ManagementTLV Slave Only message*/
void
mMSlaveOnly_display(const MMSlaveOnly *slaveOnly, const PtpClock *ptpClock)
{
	DBGV("Slave Only ManagementTLV message \n");
	DBGV("SO : %d \n", slaveOnly->so);
}

/**\brief Display ManagementTLV Clock Description message*/
void
mMClockDescription_display(const MMClockDescription *clockDescription, const PtpClock *ptpClock)
{
	DBGV("Clock Description ManagementTLV message \n");
	DBGV("clockType0 : %d \n", clockDescription->clockType0);
	DBGV("clockType1 : %d \n", clockDescription->clockType1);
	DBGV("physicalLayerProtocol : \n");
	PTPText_display(&clockDescription->physicalLayerProtocol, ptpClock);
	DBGV("physicalAddressLength : %d \n", clockDescription->physicalAddress.addressLength);
	if(clockDescription->physicalAddress.addressField) {
		DBGV("physicalAddressField : \n");
		clockUUID_display(clockDescription->physicalAddress.addressField);
	}
	DBGV("protocolAddressNetworkProtocol : %d \n", clockDescription->protocolAddress.networkProtocol);
	DBGV("protocolAddressLength : %d \n", clockDescription->protocolAddress.addressLength);
	if(clockDescription->protocolAddress.addressField) {
		DBGV("protocolAddressField : %d.%d.%d.%d \n",
			(UInteger8)clockDescription->protocolAddress.addressField[0],
			(UInteger8)clockDescription->protocolAddress.addressField[1],
			(UInteger8)clockDescription->protocolAddress.addressField[2],
			(UInteger8)clockDescription->protocolAddress.addressField[3]);
	}
	DBGV("manufacturerIdentity0 : %d \n", clockDescription->manufacturerIdentity0);
	DBGV("manufacturerIdentity1 : %d \n", clockDescription->manufacturerIdentity1);
	DBGV("manufacturerIdentity2 : %d \n", clockDescription->manufacturerIdentity2);
	DBGV("productDescription : \n");
	PTPText_display(&clockDescription->productDescription, ptpClock);
	DBGV("revisionData : \n");
	PTPText_display(&clockDescription->revisionData, ptpClock);
	DBGV("userDescription : \n");
	PTPText_display(&clockDescription->userDescription, ptpClock);
	DBGV("profileIdentity0 : %d \n", clockDescription->profileIdentity0);
	DBGV("profileIdentity1 : %d \n", clockDescription->profileIdentity1);
	DBGV("profileIdentity2 : %d \n", clockDescription->profileIdentity2);
	DBGV("profileIdentity3 : %d \n", clockDescription->profileIdentity3);
	DBGV("profileIdentity4 : %d \n", clockDescription->profileIdentity4);
	DBGV("profileIdentity5 : %d \n", clockDescription->profileIdentity5);
}

void
mMUserDescription_display(const MMUserDescription* userDescription, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMInitialize_display(const MMInitialize* initialize, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMDefaultDataSet_display(const MMDefaultDataSet* defaultDataSet, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMCurrentDataSet_display(const MMCurrentDataSet* currentDataSet, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMParentDataSet_display(const MMParentDataSet* parentDataSet, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMTimePropertiesDataSet_display(const MMTimePropertiesDataSet* timePropertiesDataSet, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMPortDataSet_display(const MMPortDataSet* portDataSet, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMPriority1_display(const MMPriority1* priority1, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMPriority2_display(const MMPriority2* priority2, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMDomain_display(const MMDomain* domain, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMLogAnnounceInterval_display(const MMLogAnnounceInterval* logAnnounceInterval, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMAnnounceReceiptTimeout_display(const MMAnnounceReceiptTimeout* announceReceiptTimeout, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMLogSyncInterval_display(const MMLogSyncInterval* logSyncInterval, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMVersionNumber_display(const MMVersionNumber* versionNumber, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMTime_display(const MMTime* time, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMClockAccuracy_display(const MMClockAccuracy* clockAccuracy, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMUtcProperties_display(const MMUtcProperties* utcProperties, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMTraceabilityProperties_display(const MMTraceabilityProperties* traceabilityProperties, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMDelayMechanism_display(const MMDelayMechanism* delayMechanism, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMLogMinPdelayReqInterval_display(const MMLogMinPdelayReqInterval* logMinPdelayReqInterval, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

void
mMErrorStatus_display(const MMErrorStatus* errorStatus, const PtpClock *ptpClock)
{
	/* TODO: implement me */
}

/**\brief Display Default data set of a PtpClock*/
void
displayDefault(PtpClock * ptpClock)
{

	DBGV("---Ptp Clock Default Data Set-- \n");
	DBGV("Port: %s\n", ptpClock->rtOpts.name);
	DBGV("\n");
	DBGV("twoStepFlag : %d \n", ptpClock->twoStepFlag);
	clockIdentity_display(ptpClock->clockIdentity);
	DBGV("numberPorts : %d \n", ptpClock->interface->global->ports_created);
	clockQuality_display(&(ptpClock->clockQuality));
	DBGV("priority1 : %d \n", ptpClock->priority1);
	DBGV("priority2 : %d \n", ptpClock->priority2);
	DBGV("domainNumber : %d \n", ptpClock->domainNumber);
	DBGV("slaveOnly : %d \n", ptpClock->slaveOnly);
	DBGV("\n");
}


/**\brief Display Current data set of a PtpClock*/
void
displayCurrent(PtpClock * ptpClock)
{
	sfptpd_time_t offset = servo_get_offset_from_master(&ptpClock->servo);
	sfptpd_time_t delay = servo_get_mean_path_delay(&ptpClock->servo);
	
	DBGV("---Ptp Clock Current Data Set-- \n");
	DBGV("Port: %s\n", ptpClock->rtOpts.name);
	DBGV("\n");

	DBGV("stepsremoved : %d \n", ptpClock->stepsRemoved);
	DBGV("Offset from master : %0.3Lf\n", offset);
	DBGV("Mean path delay : %0.3Lf\n", delay);
	DBGV("\n");
}



/**\brief Display Parent data set of a PtpClock*/
void
displayParent(PtpClock * ptpClock)
{

	DBGV("---Ptp Clock Parent Data Set-- \n");
	DBGV("Port: %s\n", ptpClock->rtOpts.name);
	DBGV("\n");
	portIdentity_display(&(ptpClock->parentPortIdentity));
	DBGV("parentStats : %d \n", ptpClock->parentStats);
	DBGV("observedParentOffsetScaledLogVariance : %d \n", ptpClock->observedParentOffsetScaledLogVariance);
	DBGV("observedParentClockPhaseChangeRate : %d \n", ptpClock->observedParentClockPhaseChangeRate);
	DBGV("--GrandMaster--\n");
	clockIdentity_display(ptpClock->grandmasterIdentity);
	clockQuality_display(&ptpClock->grandmasterClockQuality);
	DBGV("grandmasterpriority1 : %d \n", ptpClock->grandmasterPriority1);
	DBGV("grandmasterpriority2 : %d \n", ptpClock->grandmasterPriority2);
	DBGV("\n");
}

/**\brief Display Global data set of a PtpClock*/
void
displayGlobal(PtpClock * ptpClock)
{

	DBGV("---Ptp Clock Global Time Data Set-- \n");
	DBGV("Port: %s\n", ptpClock->rtOpts.name);
	DBGV("\n");

	DBGV("currentUtcOffset : %d \n", ptpClock->timePropertiesDS.currentUtcOffset);
	DBGV("currentUtcOffsetValid : %d \n", ptpClock->timePropertiesDS.currentUtcOffsetValid);
	DBGV("leap59 : %d \n", ptpClock->timePropertiesDS.leap59);
	DBGV("leap61 : %d \n", ptpClock->timePropertiesDS.leap61);
	DBGV("timeTraceable : %d \n", ptpClock->timePropertiesDS.timeTraceable);
	DBGV("frequencyTraceable : %d \n", ptpClock->timePropertiesDS.frequencyTraceable);
	DBGV("ptpTimescale : %d \n", ptpClock->timePropertiesDS.ptpTimescale);
	DBGV("timeSource : %d \n", ptpClock->timePropertiesDS.timeSource);
	DBGV("\n");
}

/**\brief Display Port data set of a PtpClock*/
void
displayPort(PtpClock * ptpClock)
{
	DBGV("---Ptp Clock Port Data Set-- \n");
	DBGV("Port: %s\n", ptpClock->rtOpts.name);
	DBGV("\n");

	portIdentity_display(&ptpClock->portIdentity);
	DBGV("port state : %d \n", ptpClock->portState);
	DBGV("port alarms : 0x%x \n", ptpClock->portAlarms);
	DBGV("logMinDelayReqInterval : %d \n", ptpClock->logMinDelayReqInterval);
	if (ptpClock->delayMechanism == PTPD_DELAY_MECHANISM_P2P) {
		sfptpd_time_t delay = servo_get_mean_path_delay(&ptpClock->servo);
		DBGV("delayRespReceiptTimeout : %d \n", ptpClock->logDelayRespReceiptTimeout);
		DBGV("peerMeanPathDelay : %0.3Lf\n", delay);
	}
	DBGV("logAnnounceInterval : %d \n", ptpClock->logAnnounceInterval);
	DBGV("announceReceiptTimeout : %d \n", ptpClock->announceReceiptTimeout);
	DBGV("logSyncInterval : %d \n", ptpClock->logSyncInterval);
	DBGV("syncReceiptTimeout : %d \n", ptpClock->syncReceiptTimeout);
	DBGV("delayMechanism : %d \n", ptpClock->delayMechanism);
	DBGV("logMinPdelayReqInterval : %d \n", ptpClock->logMinPdelayReqInterval);
	DBGV("\n");
}

/**\brief Display ForeignMaster data set of a PtpClock*/
void
displayForeignMasterRecords(ForeignMasterDS *ds, const struct sfptpd_timespec *threshold)
{

	ForeignMasterRecord *record;
	int i;

	record = ds->records;

	for (i = 0; i < ds->number_records; i++) {
		int j;

		portIdentity_display(&record->foreignMasterPortIdentity);

		for (j = 0; j < record->announceTimesCount; j++) {
			int read_ptr;
			struct sfptpd_timespec *tn;

			read_ptr = record->announceTimesWriteIdx - j - 1;
			if (read_ptr < 0)
				read_ptr += FOREIGN_MASTER_THRESHOLD;
			assert (read_ptr >= 0);

			tn = &record->announceTimes[read_ptr];

			DBGV("announce time t%d: " SFPTPD_FMT_SFTIMESPEC "\n",
			     -j, SFPTPD_ARGS_SFTIMESPEC(*tn));
		}
		if (threshold) {
			if (doesForeignMasterEarliestAnnounceQualify(record, threshold) &&
			    record->announceTimesCount >= FOREIGN_MASTER_THRESHOLD) {
				DBGV("qualifies\n");
			}
			if (!doesForeignMasterLatestAnnounceQualify(record, threshold)) {
				DBGV("expiring\n");
			}
		}

		//msgHeader_display(&record->header);
		//msgAnnounce_display(&record->announce);

		record++;
	}
}

/**\brief Display ForeignMaster data set of a PtpClock*/
void
displayForeignMaster(PtpClock *ptpClock)
{
	ForeignMasterDS *dataset = &ptpClock->foreign;
	struct sfptpd_timespec threshold;

	if (dataset->number_records > 0) {

		DBGV("---Ptp Clock Foreign Data Set-- \n");
		DBGV("Port: %s\n", ptpClock->rtOpts.name);
		DBGV("\n");
		DBGV("There is %d Foreign master Recorded \n", dataset->number_records);


		getForeignMasterExpiryTime(ptpClock, &threshold);

		displayForeignMasterRecords(&ptpClock->foreign,
					    &threshold);

	} else {
		DBGV("No Foreign masters recorded \n");
	}

	DBGV("\n");
}

/**\brief Display other data set of a PtpClock*/

void
displayOthers(PtpClock * ptpClock)
{
	int i;

	/* Usefull to display name of timers */
	    char timer[][26] = {
		"PDELAYREQ_INTERVAL_TIMER",
		"SYNC_INTERVAL_TIMER",
		"ANNOUNCE_RECEIPT_TIMER",
		"ANNOUNCE_INTERVAL_TIMER"
	};

	DBGV("---Ptp Others Data Set--\n");
	DBGV("\n");
	DBGV("\n");
	DBGV("delay_req_receive_time :\n");
	sftimespec_display(&ptpClock->pdelay_req_receive_time);
	DBGV("\n");
	DBGV("delay_req_send_time :\n");
	sftimespec_display(&ptpClock->pdelay_req_send_time);
	DBGV("\n");
	DBGV("delay_resp_receive_time :\n");
	sftimespec_display(&ptpClock->pdelay_resp_receive_time);
	DBGV("\n");
	DBGV("delay_resp_send_time :\n");
	sftimespec_display(&ptpClock->pdelay_resp_send_time);
	DBGV("\n");
	DBGV("sync_receive_time :\n");
	sftimespec_display(&ptpClock->sync_receive_time);
	DBGV("\n");
	DBGV("sentPdelayReq : %d\n", ptpClock->sentPDelayReq);
	DBGV("sentPDelayReqSequenceId : %d\n", ptpClock->sentPDelayReqSequenceId);
	DBGV("waitingForFollow : %d\n", ptpClock->waitingForFollow);
	DBGV("\n");

	for (i = 0; i < TIMER_ARRAY_SIZE; i++) {
		DBGV("%s :\n", timer[i]);
		intervalTimer_display(&ptpClock->itimer[i]);
		DBGV("\n");
	}

	netPath_display(&ptpClock->interface->transport,
			ptpClock);
	clockUUID_display(ptpClock->interface->transport.interfaceID);
	DBGV("\n");
}


/**\brief Display Buffer in & out of a PtpClock*/
void
displayBuffer(PtpClock * ptpClock)
{

	int i;
	int j;

	j = 0;

	DBGV("PtpClock Buffer Out  \n");
	DBGV("\n");

	for (i = 0; i < PACKET_SIZE; i++) {
		DBGV(":%02hhx", ptpClock->msgObuf[i]);
		j++;

		if (j == 8) {
			DBGV(" ");

		}
		if (j == 16) {
			DBGV("\n");
			j = 0;
		}
	}
	DBGV("\n");
	j = 0;
	DBGV("\n");

	DBGV("PtpClock Buffer In  \n");
	DBGV("\n");
	for (i = 0; i < PACKET_SIZE; i++) {
		DBGV(":%02hhx", ptpClock->interface->msgIbuf[i]);
		j++;

		if (j == 8) {
			DBGV(" ");

		}
		if (j == 16) {
			DBGV("\n");
			j = 0;
		}
	}
	DBGV("\n");
	DBGV("\n");
}

/**\convert port state to string*/
const char
*portState_getName(Enumeration8 portState)
{
    static const char *ptpStates[] = {
        [PTPD_UNINITIALIZED] = "PTP_UNINITIALIZED",
        [PTPD_INITIALIZING] = "PTP_INITIALIZING",
        [PTPD_FAULTY] = "PTP_FAULTY",
        [PTPD_DISABLED] = "PTP_DISABLED",
        [PTPD_LISTENING] = "PTP_LISTENING",
        [PTPD_PRE_MASTER] = "PTP_PRE_MASTER",
        [PTPD_MASTER] = "PTP_MASTER",
        [PTPD_PASSIVE] = "PTP_PASSIVE",
        [PTPD_UNCALIBRATED] = "PTP_UNCALIBRATED",
        [PTPD_SLAVE] = "PTP_SLAVE"
    };

    /* converting to int to avoid compiler warnings when comparing enum*/
    static const int max = PTPD_SLAVE;
    int intstate = portState;

    if( intstate < 0 || intstate > max ) {
        return("PTP_UNKNOWN");
    }

    return(ptpStates[portState]);
}

/**\brief Display all PTP clock (port) counters*/
void
displayCounters(PtpClock * ptpClock)
{
	INFO("============= PTP port counters =============\n");
	INFO("Port: %s\n", ptpClock->rtOpts.name);
	INFO("Message counters:\n");
	INFO("              announceMessagesSent : %d\n",
		ptpClock->counters.announceMessagesSent);
	INFO("          announceMessagesReceived : %d\n",
		ptpClock->counters.announceMessagesReceived);
	INFO("                  syncMessagesSent : %d\n",
		ptpClock->counters.syncMessagesSent);
	INFO("              syncMessagesReceived : %d\n",
		ptpClock->counters.syncMessagesReceived);
	INFO("              followUpMessagesSent : %d\n",
		ptpClock->counters.followUpMessagesSent);
	INFO("          followUpMessagesReceived : %d\n",
		ptpClock->counters.followUpMessagesReceived);
	INFO("              delayReqMessagesSent : %d\n",
		 ptpClock->counters.delayReqMessagesSent);
	INFO("          delayReqMessagesReceived : %d\n",
		ptpClock->counters.delayReqMessagesReceived);
	INFO("             delayRespMessagesSent : %d\n",
		ptpClock->counters.delayRespMessagesSent);
	INFO("         delayRespMessagesReceived : %d\n",
		ptpClock->counters.delayRespMessagesReceived);
	INFO("             pdelayReqMessagesSent : %d\n",
		ptpClock->counters.pdelayReqMessagesSent);
	INFO("         pdelayReqMessagesReceived : %d\n",
		ptpClock->counters.pdelayReqMessagesReceived);
	INFO("            pdelayRespMessagesSent : %d\n",
		ptpClock->counters.pdelayRespMessagesSent);
	INFO("        pdelayRespMessagesReceived : %d\n",
		ptpClock->counters.pdelayRespMessagesReceived);
	INFO("    pdelayRespFollowUpMessagesSent : %d\n",
		ptpClock->counters.pdelayRespFollowUpMessagesSent);
	INFO("pdelayRespFollowUpMessagesReceived : %d\n",
		ptpClock->counters.pdelayRespFollowUpMessagesReceived);
	INFO("             signalingMessagesSent : %d\n",
		ptpClock->counters.signalingMessagesSent);
	INFO("         signalingMessagesReceived : %d\n",
		ptpClock->counters.signalingMessagesReceived);
	INFO("            managementMessagesSent : %d\n",
		ptpClock->counters.managementMessagesSent);
	INFO("        managementMessagesReceived : %d\n",
		ptpClock->counters.managementMessagesReceived);

/* not implemented yet */
#if 0
	INFO("FMR counters:\n");
	INFO("                      foreignAdded : %d\n",
		ptpClock->counters.foreignAdded);
	INFO("                        foreignMax : %d\n",
		ptpClock->counters.foreignMax);
	INFO("                    foreignRemoved : %d\n",
		ptpClock->counters.foreignRemoved);
	INFO("                   foreignOverflow : %d\n",
		ptpClock->counters.foreignOverflow);
#endif /* 0 */

	INFO("Protocol engine counters:\n");
	INFO("                  stateTransitions : %d\n",
		ptpClock->counters.stateTransitions);
	INFO("                     masterChanges : %d\n",
		ptpClock->counters.masterChanges);
	INFO("                  announceTimeouts : %d\n",
		ptpClock->counters.announceTimeouts);
	INFO("                      syncTimeouts : %d\n",
		ptpClock->counters.syncTimeouts);
	INFO("                  followUpTimeouts : %d\n",
		ptpClock->counters.followUpTimeouts);
	INFO("               outOfOrderFollowUps : %d\n",
		ptpClock->counters.outOfOrderFollowUps);
	INFO("                 delayRespTimeouts : %d\n",
		ptpClock->counters.delayRespTimeouts);
	INFO("                        clockSteps : %d\n",
		ptpClock->counters.clockSteps);
	INFO("  adaptive-outlier-filter-discards : %d / %d\n",
		ptpClock->counters.outliers, ptpClock->counters.outliersNumSamples);

	INFO("Discarded / unknown message counters:\n");
	INFO("                 discardedMessages : %d\n",
		ptpClock->counters.discardedMessages);
	INFO("                   unknownMessages : %d\n",
		ptpClock->counters.unknownMessages);
	INFO("                   ignoredAnnounce : %d\n",
		ptpClock->counters.ignoredAnnounce);
	INFO("    aclManagementDiscardedMessages : %d\n",
		ptpClock->counters.aclManagementDiscardedMessages);
	INFO("        aclTimingDiscardedMessages : %d\n",
		ptpClock->counters.aclTimingDiscardedMessages);

	INFO("Error counters:\n");
	INFO("                 messageSendErrors : %d\n",
		ptpClock->counters.messageSendErrors);
	INFO("                 messageRecvErrors : %d\n",
		ptpClock->counters.messageRecvErrors);
	INFO("               messageFormatErrors : %d\n",
		ptpClock->counters.messageFormatErrors);
	INFO("                    protocolErrors : %d\n",
		ptpClock->counters.protocolErrors);
	INFO("             versionMismatchErrors : %d\n",
		ptpClock->counters.versionMismatchErrors);
	INFO("              domainMismatchErrors : %d\n",
		ptpClock->counters.domainMismatchErrors);
	INFO("            sequenceMismatchErrors : %d\n",
		ptpClock->counters.sequenceMismatchErrors);
	INFO("           delayModeMismatchErrors : %d\n",
		ptpClock->counters.delayModeMismatchErrors);
	INFO("       sendPacketsMissingTimestamp : %d\n",
		ptpClock->counters.txPktNoTimestamp);
	INFO("   receivedPacketsMissingTimestamp : %d\n",
		ptpClock->counters.rxPktNoTimestamp);
}

/**\brief Display all PTP clock (port) statistics*/
void
displayStatistics(PtpClock* ptpClock)
{

/*
	INFO("Clock stats: ofm mean: %d, ofm median: %d,"
	     "ofm std dev: %d, observed drift std dev: %d\n",
	     ptpClock->stats.ofmMean, ptpClock->stats.ofmMedian,
	     ptpClock->stats.ofmStdDev, ptpClock->stats.driftStdDev);
*/

}

/**\brief Display All data sets and counters of a PtpClock*/
void
displayPtpClock(PtpClock * ptpClock)
{
	displayDefault(ptpClock);
	displayCurrent(ptpClock);
	displayParent(ptpClock);
	displayGlobal(ptpClock);
	displayPort(ptpClock);
	displayForeignMaster(ptpClock);
	displayBuffer(ptpClock);
	displayOthers(ptpClock);
	displayCounters(ptpClock);
	displayStatistics(ptpClock);
}
