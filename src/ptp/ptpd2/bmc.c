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
 * @file   bmc.c
 * @date   Wed Jun 23 09:36:09 2010
 *
 * @brief  Best master clock selection code.
 *
 * The functions in this file are used by the daemon to select the
 * best master clock from any number of possibilities.
 */

#include "ptpd.h"

enum qualification {
	QUALIFIED,
	UNQUALIFIED_BY_ANNOUNCE_EXPIRY,
	UNQUALIFIED_BY_DISCRIMINATOR,
	UNQUALIFIED_BY_STEPS_REMOVED,
};


/* Init ptpClock with run time values (initialization constants are in constants.h)*/
void initData(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	DBGV("initData\n");

	/* Default data set */
	ptpClock->twoStepFlag = PTPD_TWO_STEP_FLAG;

	/*
	 * init clockIdentity with MAC address and 0xFF and 0xFF. see
	 * spec 7.5.2.2.2 (2019 version).
	 *
	 * This is a fallback assignment in case of operating with system LRC
	 * that does not have its own hw_id. Otherwise this should get
	 * overwritten by the hw_id already assigned to the clock by sfptpd.
	 */
	ptpClock->clockIdentity[0] = ptpClock->interface->transport.interfaceID[0];
	ptpClock->clockIdentity[1] = ptpClock->interface->transport.interfaceID[1];
	ptpClock->clockIdentity[2] = ptpClock->interface->transport.interfaceID[2];
	ptpClock->clockIdentity[3] = ptpClock->interface->transport.interfaceID[3];
	ptpClock->clockIdentity[4] = ptpClock->interface->transport.interfaceID[4];
	ptpClock->clockIdentity[5] = ptpClock->interface->transport.interfaceID[5];
	ptpClock->clockIdentity[6] = 0xFF;
	ptpClock->clockIdentity[7] = 0xFF;

	if (memcmp(&rtOpts->ifOpts->clock_id, &SFPTPD_CLOCK_ID_UNINITIALISED, sizeof(sfptpd_clock_id_t)))
		memcpy(ptpClock->clockIdentity, &rtOpts->ifOpts->clock_id, 8);

	if (rtOpts->slaveOnly)
		rtOpts->clockQuality.clockClass = SLAVE_ONLY_CLOCK_CLASS;

	ptpClock->clockQuality = rtOpts->clockQuality;
	ptpClock->priority1 = rtOpts->priority1;
	ptpClock->priority2 = rtOpts->priority2;

	ptpClock->domainNumber = rtOpts->domainNumber;
	ptpClock->slaveOnly = rtOpts->slaveOnly;

	/* Port configuration data set */

	/*
	 * PortIdentity Init
	 * portNumber (spec 7.5.2.3) is set to 1 here to represent a single port
	 * if uninitialised (0 represents uninitialised) otherwise set by caller
	 * to ordinal of this port.
	 */
	copyClockIdentity(ptpClock->portIdentity.clockIdentity,
			  ptpClock->clockIdentity);
	if (ptpClock->portIdentity.portNumber == 0) {
		ptpClock->portIdentity.portNumber = 1;
	}

	/* select the initial rate of delayreqs until we receive the first announce message */
	ptpClock->logMinDelayReqInterval = rtOpts->minDelayReqInterval;
	ptpClock->logDelayRespReceiptTimeout = rtOpts->delayRespReceiptTimeout;
	ptpClock->logAnnounceInterval = rtOpts->announceInterval;
	ptpClock->announceReceiptTimeout = rtOpts->announceReceiptTimeout;
	ptpClock->logSyncInterval = rtOpts->syncInterval;
	ptpClock->syncReceiptTimeout = rtOpts->syncReceiptTimeout;
	ptpClock->delayMechanism = rtOpts->delayMechanism;
	ptpClock->logMinPdelayReqInterval = rtOpts->minPdelayReqInterval;

	/*
	 * Initialize random number generator using same method as ptpv1:
	 * seed is now initialized from the last bytes of our mac addres
	 * (collected in net.c:findIface())
	 */
	srand((ptpClock->interface->transport.interfaceID[ETHER_ADDR_LEN - 1] << 8) +
	    ptpClock->interface->transport.interfaceID[ETHER_ADDR_LEN - 2]);

	/*Init other stuff*/
	resetForeignMasterDS(&ptpClock->foreign);
}


/*Local clock is becoming Master. Table 13 (9.3.5) of the spec.*/
void m1(const RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	/* Default data set */
	ptpClock->twoStepFlag = PTPD_TWO_STEP_FLAG;

	/* Current data set update */
	ptpClock->stepsRemoved = rtOpts->stepsRemoved;

	/* Reset the servo */
	if (rtOpts->node_type == PTPD_NODE_CLOCK) {
		servo_reset(&ptpClock->servo);
	}

	/* Parent data set */
	copyClockIdentity(ptpClock->parentPortIdentity.clockIdentity,
	       ptpClock->clockIdentity);

	ptpClock->parentPortIdentity.portNumber = 0;
	ptpClock->parentStats = FALSE;
	ptpClock->observedParentClockPhaseChangeRate = 0;
	ptpClock->observedParentOffsetScaledLogVariance = 0;
	copyClockIdentity(ptpClock->grandmasterIdentity,
			  (ptpClock->boundaryGrandmasterDefined ?
			   ptpClock->boundaryGrandmasterIdentity :
			   ptpClock->clockIdentity));
	ptpClock->grandmasterClockQuality.clockAccuracy =
		ptpClock->clockQuality.clockAccuracy;
	ptpClock->grandmasterClockQuality.clockClass =
		ptpClock->clockQuality.clockClass;
	ptpClock->grandmasterClockQuality.offsetScaledLogVariance =
		ptpClock->clockQuality.offsetScaledLogVariance;
	ptpClock->grandmasterPriority1 = ptpClock->priority1;
	ptpClock->grandmasterPriority2 = ptpClock->priority2;
	copyAddress(&ptpClock->parentAddress, &ptpClock->parentAddressLen,
		    &ptpClock->interface->transport.interfaceAddr,
		    ptpClock->interface->transport.interfaceAddrLen);

	/* Time Properties data set */
	ptpClock->timePropertiesDS.currentUtcOffsetValid = rtOpts->timeProperties.currentUtcOffsetValid;
	ptpClock->timePropertiesDS.currentUtcOffset = rtOpts->timeProperties.currentUtcOffset;
	ptpClock->timePropertiesDS.timeTraceable = rtOpts->timeProperties.timeTraceable;
	ptpClock->timePropertiesDS.frequencyTraceable = rtOpts->timeProperties.frequencyTraceable;
	ptpClock->timePropertiesDS.ptpTimescale = rtOpts->timeProperties.ptpTimescale;
	ptpClock->timePropertiesDS.timeSource = rtOpts->timeProperties.timeSource;
}


/* first cut on a passive mode specific BMC actions */
void p1(ForeignMasterRecord *master, PtpClock *ptpClock, const RunTimeOpts *rtOpts)
{
	MsgHeader *header = &master->header;
	MsgAnnounce *announce = &master->announce;

	/* Default data set */
	ptpClock->twoStepFlag = PTPD_TWO_STEP_FLAG;

	/* Current data set update */
	ptpClock->stepsRemoved = rtOpts->stepsRemoved;

	/* Parent DS. Take a copy of the other master we can see. We need this
	 * in order to correctly continue to process announce messages from this
	 * master. */
	copyClockIdentity(ptpClock->parentPortIdentity.clockIdentity,
	       header->sourcePortIdentity.clockIdentity);
	ptpClock->parentPortIdentity.portNumber =
		header->sourcePortIdentity.portNumber;
	copyClockIdentity(ptpClock->grandmasterIdentity,
			announce->grandmasterIdentity);
	ptpClock->grandmasterClockQuality.clockAccuracy =
		announce->grandmasterClockQuality.clockAccuracy;
	ptpClock->grandmasterClockQuality.clockClass =
		announce->grandmasterClockQuality.clockClass;
	ptpClock->grandmasterClockQuality.offsetScaledLogVariance =
		announce->grandmasterClockQuality.offsetScaledLogVariance;
	ptpClock->grandmasterPriority1 = announce->grandmasterPriority1;
	ptpClock->grandmasterPriority2 = announce->grandmasterPriority2;
	copyAddress(&ptpClock->parentAddress, &ptpClock->parentAddressLen,
		    &master->address, master->addressLen);

	/* Time Properties data set */
	ptpClock->timePropertiesDS.currentUtcOffsetValid = rtOpts->timeProperties.currentUtcOffsetValid;
	ptpClock->timePropertiesDS.currentUtcOffset = rtOpts->timeProperties.currentUtcOffset;
	ptpClock->timePropertiesDS.timeTraceable = rtOpts->timeProperties.timeTraceable;
	ptpClock->timePropertiesDS.frequencyTraceable = rtOpts->timeProperties.frequencyTraceable;
	ptpClock->timePropertiesDS.ptpTimescale = rtOpts->timeProperties.ptpTimescale;
	ptpClock->timePropertiesDS.timeSource = rtOpts->timeProperties.timeSource;
}

void ptpd_update_announce_interval(PtpClock *ptpClock, const RunTimeOpts *rtOpts)
{
	ForeignMasterDS *ds = &ptpClock->foreign;
	ForeignMasterRecord *record;
	Integer8 longestInterval = PTPD_MESSAGE_INTERVAL_UNDEFINED;
	Integer8 msgInterval;
	int i;

	for (i = 0; i < ds->number_records; i++) {
		record = &ds->records[i];
		Integer8 interval = record->header.logMessageInterval;

		if (interval != PTPD_MESSAGE_INTERVAL_UNDEFINED &&
		    (longestInterval == PTPD_MESSAGE_INTERVAL_UNDEFINED ||
		     interval > longestInterval)) {

			longestInterval = interval;
		}
	}

	if (longestInterval != PTPD_MESSAGE_INTERVAL_UNDEFINED) {

		msgInterval = longestInterval;

		/* Saturate the interval such that it is within the range
		 * of values we can support */
		if (msgInterval < PTPD_ANNOUNCE_INTERVAL_MIN)
			msgInterval = PTPD_ANNOUNCE_INTERVAL_MIN;
		else if (msgInterval > PTPD_ANNOUNCE_INTERVAL_MAX)
			msgInterval = PTPD_ANNOUNCE_INTERVAL_MAX;

		if (ptpClock->logAnnounceInterval != msgInterval) {

			/* The interval has changed so log a message */
			if (msgInterval != longestInterval) {
				WARNING("ptp %s: longest announce interval (%d) from currently-announcing"
					"masters is out of range (was %d, using %d)\n",
					rtOpts->name,
					longestInterval,
					ptpClock->logAnnounceInterval, msgInterval);
			} else {
				INFO("ptp %s: received new longest announce interval %d from "
				     "currently-announcing masters (was %d)\n",
				     rtOpts->name,
				     msgInterval,
				     ptpClock->logAnnounceInterval);
			}
		}
	} else {
		msgInterval = rtOpts->announceInterval;

		if (ptpClock->logAnnounceInterval != msgInterval) {
			INFO("ptp %s: no current announce intervals, "
			     "reverting to default %d (was %d)\n",
			     rtOpts->name,
			     msgInterval,
			     ptpClock->logAnnounceInterval);
		}
	}

	ptpClock->logAnnounceInterval = msgInterval;
}

/*Local clock is synchronized to Ebest Table 16 (9.3.5) of the spec*/
void s1(ForeignMasterRecord *master, PtpClock *ptpClock, const RunTimeOpts *rtOpts)
{
	Boolean previousLeap59 = FALSE, previousLeap61 = FALSE;
	int previousUtcOffset = 0;
	MsgHeader *header = &master->header;
	MsgAnnounce *announce = &master->announce;
	Integer8 msgInterval;

	/* TODO This is almost certainly wrong. Surely we should never get here
	 * unless in the slave state - but note that s1 is called before setting
	 * the state ?! */
	if (ptpClock->portState == PTPD_SLAVE ||
	    ptpClock->portState == PTPD_UNCALIBRATED ||
	    ptpClock->portState == PTPD_PASSIVE) {
		previousLeap59 = ptpClock->timePropertiesDS.leap59;
		previousLeap61 = ptpClock->timePropertiesDS.leap61;
		previousUtcOffset = ptpClock->timePropertiesDS.currentUtcOffset;
	}

	/* Current DS */
	ptpClock->stepsRemoved = announce->stepsRemoved + 1;

	/* Parent DS */
	copyClockIdentity(ptpClock->parentPortIdentity.clockIdentity,
	       header->sourcePortIdentity.clockIdentity);
	ptpClock->parentPortIdentity.portNumber =
		header->sourcePortIdentity.portNumber;
	copyClockIdentity(ptpClock->grandmasterIdentity,
			announce->grandmasterIdentity);
	ptpClock->grandmasterClockQuality.clockAccuracy =
		announce->grandmasterClockQuality.clockAccuracy;
	ptpClock->grandmasterClockQuality.clockClass =
		announce->grandmasterClockQuality.clockClass;
	ptpClock->grandmasterClockQuality.offsetScaledLogVariance =
		announce->grandmasterClockQuality.offsetScaledLogVariance;
	ptpClock->grandmasterPriority1 = announce->grandmasterPriority1;
	ptpClock->grandmasterPriority2 = announce->grandmasterPriority2;
	copyAddress(&ptpClock->parentAddress, &ptpClock->parentAddressLen,
		    &master->address, master->addressLen);

	/* If the announce message interval is defined then update our copy */
	msgInterval = header->logMessageInterval;
	if (msgInterval != PTPD_MESSAGE_INTERVAL_UNDEFINED) {
		/* Saturate the interval such that it is within the range
		 * of values we can support */
		if (msgInterval < PTPD_ANNOUNCE_INTERVAL_MIN)
			msgInterval = PTPD_ANNOUNCE_INTERVAL_MIN;
		else if (msgInterval > PTPD_ANNOUNCE_INTERVAL_MAX)
			msgInterval = PTPD_ANNOUNCE_INTERVAL_MAX;

		if (ptpClock->logAnnounceInterval != msgInterval) {
			/* The interval has changed so log a message */
			if (msgInterval != header->logMessageInterval) {
				WARNING("ptp %s: received out-of-range Announce interval %d "
					"from master (was %d, using %d)\n",
					rtOpts->name,
					header->logMessageInterval,
					ptpClock->logAnnounceInterval, msgInterval);
			} else {
				INFO("ptp %s: received new Announce interval %d from "
				     "master (was %d)\n",
				     rtOpts->name,
				     msgInterval, ptpClock->logAnnounceInterval);
			}

			ptpClock->logAnnounceInterval = msgInterval;
		}
	}

	/* Timeproperties DS */
	ptpClock->timePropertiesDS.currentUtcOffset = announce->currentUtcOffset;
	if ((ptpClock->portState != PTPD_PASSIVE) &&
	    ptpClock->timePropertiesDS.currentUtcOffsetValid && 
	    !IS_SET(header->flagField1, PTPD_UTCV)) {
		if(rtOpts->alwaysRespectUtcOffset)
			WARNING("ptp %s: UTC Offset no longer valid and ptpengine:"
				"always_respect_utc_offset is set: continuing as normal\n",
				rtOpts->name);
		else
			WARNING("ptp %s: UTC Offset no longer valid - clock jump expected\n",
				rtOpts->name);
	}
	ptpClock->timePropertiesDS.currentUtcOffsetValid = IS_SET(header->flagField1, PTPD_UTCV);
	ptpClock->timePropertiesDS.timeTraceable = IS_SET(header->flagField1, PTPD_TTRA);
	ptpClock->timePropertiesDS.frequencyTraceable = IS_SET(header->flagField1, PTPD_FTRA);
	ptpClock->timePropertiesDS.ptpTimescale = IS_SET(header->flagField1, PTPD_PTPT);
	ptpClock->timePropertiesDS.timeSource = announce->timeSource;

	/* Handle UTC override option */
	if (rtOpts->overrideUtcOffset) {
		int old_offset = ptpClock->timePropertiesDS.currentUtcOffset;
		int new_offset = rtOpts->overrideUtcOffsetSeconds;
		if (old_offset != new_offset)
			WARNING("ptp %s: overriding UTC offset of %d with configured offset of %d\n",
				rtOpts->name, old_offset, new_offset);
		ptpClock->timePropertiesDS.currentUtcOffsetValid = TRUE;
		ptpClock->timePropertiesDS.currentUtcOffset = new_offset;
	}

#if defined(MOD_TAI) &&  NTP_API == 4
	/*
	 * update kernel TAI offset, but only if timescale is
	 * PTP not ARB - spec section 7.2
	 */
	if (ptpClock->timePropertiesDS.ptpTimescale &&
	    (ptpClock->timePropertiesDS.currentUtcOffsetValid || rtOpts->alwaysRespectUtcOffset) &&
	    ((int)ptpClock->timePropertiesDS.currentUtcOffset != previousUtcOffset)) {
		setKernelUtcOffset(ptpClock->timePropertiesDS.currentUtcOffset);
		INFO("ptp %s: Set kernel UTC offset to %d\n",
		     rtOpts->name,
		     ptpClock->timePropertiesDS.currentUtcOffset);
	}
#endif /* MOD_TAI */

	/* Leap second handling */

	/* TODO This is almost certainly wrong. Surely we should never get here
	 * unless in the slave state - but note that s1 is called before setting
	 * the state ?! */
        if (ptpClock->portState == PTPD_SLAVE ||
	    ptpClock->portState == PTPD_UNCALIBRATED) {
		/* We must not take leap second updates while a leap second
		 * is in progress. This shouldn't happen but we should not
		 * rely on the master behaving. We'll take the update when
		 * the first announce arrives after the leap second has
		 * completed.
		 */
		if (!ptpClock->leapSecondInProgress) {
			ptpClock->timePropertiesDS.leap59 = IS_SET(header->flagField1, PTPD_LI59);
			ptpClock->timePropertiesDS.leap61 = IS_SET(header->flagField1, PTPD_LI61);
			
			if(ptpClock->timePropertiesDS.leap59 && ptpClock->timePropertiesDS.leap61) {
				WARNING("ptp %s: both Leap59 and Leap61 flags set!\n", rtOpts->name);
				ptpClock->counters.protocolErrors++;
			}

			if ((previousLeap59 && !ptpClock->timePropertiesDS.leap59) ||
		            (previousLeap61 && !ptpClock->timePropertiesDS.leap61)) {
				WARNING(INFO_PREFIX "ptp %s: leap second event aborted by GM!\n",
					rtOpts->name);
			}
		}

		if (previousUtcOffset != (int)ptpClock->timePropertiesDS.currentUtcOffset) {
			if (!ptpClock->leapSecondInProgress)
				WARNING(INFO_PREFIX "ptp %s: UTC offset changed from "
					"%d to %d with no leap second pending!\n",
					rtOpts->name,
					previousUtcOffset,
					ptpClock->timePropertiesDS.currentUtcOffset);
			else
				NOTICE(INFO_PREFIX "ptp %s: UTC offset changed from "
				       "%d to %d\n",
				       rtOpts->name,
				       previousUtcOffset,
				       ptpClock->timePropertiesDS.currentUtcOffset);
		}
	} else if (previousUtcOffset != (int)ptpClock->timePropertiesDS.currentUtcOffset)
		INFO("ptp %s: UTC offset changed from %d to %d on entering SLAVE|UNCALIBRATED state\n",
		     rtOpts->name, previousUtcOffset, ptpClock->timePropertiesDS.currentUtcOffset);
}


/*Copy local data set into header and announce message. 9.3.4 table 12*/
static void
copyD0(MsgHeader *header, MsgAnnounce *announce, PtpClock *ptpClock)
{
	announce->grandmasterPriority1 = ptpClock->priority1;
	copyClockIdentity(announce->grandmasterIdentity,
			ptpClock->clockIdentity);
	announce->grandmasterClockQuality.clockClass =
		ptpClock->clockQuality.clockClass;
	announce->grandmasterClockQuality.clockAccuracy =
		ptpClock->clockQuality.clockAccuracy;
	announce->grandmasterClockQuality.offsetScaledLogVariance =
		ptpClock->clockQuality.offsetScaledLogVariance;
	announce->grandmasterPriority2 = ptpClock->priority2;
	announce->stepsRemoved = 0;
	copyClockIdentity(header->sourcePortIdentity.clockIdentity,
	       ptpClock->clockIdentity);

	/* Copy TimePropertiesDS into FlagField1 */
	header->flagField1 = ptpClock->timePropertiesDS.leap61			<< 0;
	header->flagField1 |= ptpClock->timePropertiesDS.leap59			<< 1;
	header->flagField1 |= ptpClock->timePropertiesDS.currentUtcOffsetValid	<< 2;
	header->flagField1 |= ptpClock->timePropertiesDS.ptpTimescale		<< 3;
	header->flagField1 |= ptpClock->timePropertiesDS.timeTraceable		<< 4;
	header->flagField1 |= ptpClock->timePropertiesDS.frequencyTraceable	<< 5;
}


/*Data set comparison bewteen two foreign masters (9.3.4 fig 27)
 * return similar to memcmp() */

static Integer8
bmcDataSetComparison(const MsgHeader *headerA, const MsgAnnounce *announceA,
		     const MsgHeader *headerB, const MsgAnnounce *announceB,
		     const PtpClock *ptpClock, const RunTimeOpts * rtOpts)
{
	DBGV("Data set comparison \n");
	short comp = 0;

	/*Identity comparison*/
	comp = memcmp(announceA->grandmasterIdentity,announceB->grandmasterIdentity,CLOCK_IDENTITY_LENGTH);

	if (comp!=0)
		goto dataset_comp_part_1;

	  /* Algorithm part2 Fig 28 */
	if (announceA->stepsRemoved > announceB->stepsRemoved+1)
		return 1;
	if (announceA->stepsRemoved+1 < announceB->stepsRemoved)
		return -1;

	/* A within 1 of B */

	if (announceA->stepsRemoved > announceB->stepsRemoved) {
		comp = memcmp(headerA->sourcePortIdentity.clockIdentity,ptpClock->parentPortIdentity.clockIdentity,CLOCK_IDENTITY_LENGTH);
		if(comp < 0)
			return -1;
		if(comp > 0)
			return 1;
		DBG("Sender=Receiver : Error -1");
		return 0;
	}

	if (announceA->stepsRemoved < announceB->stepsRemoved) {
		comp = memcmp(headerB->sourcePortIdentity.clockIdentity,ptpClock->parentPortIdentity.clockIdentity,CLOCK_IDENTITY_LENGTH);

		if(comp < 0)
			return -1;
		if(comp > 0)
			return 1;
		DBG("Sender=Receiver : Error -1");
		return 0;
	}

	/*  steps removed A = steps removed B */
	comp = memcmp(headerA->sourcePortIdentity.clockIdentity,headerB->sourcePortIdentity.clockIdentity,CLOCK_IDENTITY_LENGTH);

	if (comp<0) {
		return -1;
	}

	if (comp>0) {
		return 1;
	}

	/* identity A = identity B */

	if (headerA->sourcePortIdentity.portNumber < headerB->sourcePortIdentity.portNumber)
		return -1;
	if (headerA->sourcePortIdentity.portNumber > headerB->sourcePortIdentity.portNumber)
		return 1;

	DBG("Sender=Receiver : Error -2");
	return 0;

	  /* Algorithm part 1 Fig 27 */
dataset_comp_part_1:

	/* Compare GM priority1 */
	if (announceA->grandmasterPriority1 < announceB->grandmasterPriority1)
		return -1;
	if (announceA->grandmasterPriority1 > announceB->grandmasterPriority1)
		return 1;

	/* non-standard BMC extension to prioritise GMs with UTC valid */
	if (rtOpts->preferUtcValid) {
		Boolean utcA = IS_SET(headerA->flagField1, PTPD_UTCV);
		Boolean utcB = IS_SET(headerB->flagField1, PTPD_UTCV);
		if (utcA > utcB)
			return -1;
		if (utcA < utcB)
			return 1;
	}

	/* Compare GM class */
	if (announceA->grandmasterClockQuality.clockClass <
			announceB->grandmasterClockQuality.clockClass)
		return -1;
	if (announceA->grandmasterClockQuality.clockClass >
			announceB->grandmasterClockQuality.clockClass)
		return 1;

	/* Compare GM accuracy */
	if (announceA->grandmasterClockQuality.clockAccuracy <
			announceB->grandmasterClockQuality.clockAccuracy)
		return -1;
	if (announceA->grandmasterClockQuality.clockAccuracy >
			announceB->grandmasterClockQuality.clockAccuracy)
		return 1;

	/* Compare GM offsetScaledLogVariance */
	if (announceA->grandmasterClockQuality.offsetScaledLogVariance <
			announceB->grandmasterClockQuality.offsetScaledLogVariance)
		return -1;
	if (announceA->grandmasterClockQuality.offsetScaledLogVariance >
			announceB->grandmasterClockQuality.offsetScaledLogVariance)
		return 1;

	/* Compare GM priority2 */
	if (announceA->grandmasterPriority2 < announceB->grandmasterPriority2)
		return -1;
	if (announceA->grandmasterPriority2 > announceB->grandmasterPriority2)
		return 1;

	/* Compare GM identity */
	if (comp < 0)
		return -1;
	else if (comp > 0)
		return 1;
	return 0;
}

/*State decision algorithm 9.3.3 Fig 26*/
static UInteger8
bmcStateDecision(ForeignMasterRecord *bestForeignMaster,
		 const RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	Integer8 comp;
	MsgHeader tmp_header;
	MsgAnnounce tmp_announce;
	Boolean newBM;

	newBM = ((memcmp(bestForeignMaster->header.sourcePortIdentity.clockIdentity,
			 ptpClock->parentPortIdentity.clockIdentity,CLOCK_IDENTITY_LENGTH)) ||
		 (bestForeignMaster->header.sourcePortIdentity.portNumber != ptpClock->parentPortIdentity.portNumber));

	if (ptpClock->slaveOnly) {
		s1(bestForeignMaster, ptpClock, rtOpts);
		if (newBM) {
			displayPortIdentity(ptpClock,
					    &bestForeignMaster->header.sourcePortIdentity,
					    "new best master selected:");
			ptpClock->counters.masterChanges++;

			/* If hybrid mode was previously successful number
			 * of failures will be negative so need to reset
			 * to zero when master changes */
			ptpClock->unicast_delay_resp_failures = 0;

			if (ptpClock->portState == PTPD_SLAVE ||
			    ptpClock->portState == PTPD_UNCALIBRATED)
				displayStatus(ptpClock, "state: ");
		}
		return (sfptpd_clock_get_been_locked(ptpClock->servo.clock) ?
			PTPD_SLAVE : PTPD_UNCALIBRATED);
	}

	if ((!ptpClock->foreign.number_records) &&
	    (ptpClock->portState == PTPD_LISTENING))
		return PTPD_LISTENING;

	copyD0(&tmp_header, &tmp_announce, ptpClock);

	DBGV("local clockQuality.clockClass: %d \n", ptpClock->clockQuality.clockClass);

	comp = bmcDataSetComparison(&tmp_header, &tmp_announce,
				    &bestForeignMaster->header,
				    &bestForeignMaster->announce,
				    ptpClock, rtOpts);
	if (ptpClock->clockQuality.clockClass < 128) {
		if (comp < 0) {
			m1(rtOpts, ptpClock);
			return PTPD_MASTER;
		} else if (comp > 0) {
			p1(bestForeignMaster, ptpClock, rtOpts);
			if (newBM) {
				displayPortIdentity(ptpClock,
						    &bestForeignMaster->header.sourcePortIdentity,
						    "new best master selected:");
				ptpClock->counters.masterChanges++;

				/* If hybrid mode was previously successful number
				 * of failures will be negative so need to reset
				 * to zero when master changes */
				ptpClock->unicast_delay_resp_failures = 0;

				if(ptpClock->portState == PTPD_PASSIVE)
					displayStatus(ptpClock, "state: ");
			}
			return PTPD_PASSIVE;
		} else {
			DBG("Error in bmcDataSetComparison..\n");
		}
	} else {
		if (comp < 0) {
			m1(rtOpts, ptpClock);
			return PTPD_MASTER;
		} else if (comp > 0) {
			s1(bestForeignMaster, ptpClock, rtOpts);
			if (newBM) {
				displayPortIdentity(ptpClock,
						    &bestForeignMaster->header.sourcePortIdentity,
						    "new best master selected:");
				ptpClock->counters.masterChanges++;

				/* If hybrid mode was previously successful number
				 * of failures will be negative so need to reset
				 * to zero when master changes */
				ptpClock->unicast_delay_resp_failures = 0;

				if(ptpClock->portState == PTPD_SLAVE ||
				   ptpClock->portState == PTPD_UNCALIBRATED)
					displayStatus(ptpClock, "state: ");
			}
			return (sfptpd_clock_get_been_locked(ptpClock->servo.clock) ?
				PTPD_SLAVE : PTPD_UNCALIBRATED);
		} else {
			DBG("Error in bmcDataSetComparison..\n");
		}
	}

	ptpClock->counters.protocolErrors++;
	/*  MB: Is this the return code below correct? */
	/*  Anyway, it's a valid return code. */

	return PTPD_FAULTY;
}


static enum qualification
getQualification (ForeignMasterRecord *foreign,
		  const struct sfptpd_timespec *expiry_threshold,
		  const sfptpd_time_t discriminator_offset,
		  const sfptpd_time_t discriminator_threshold)
{
	/* Disqualify masters from which we have not heard within the threhold period. */
	if (foreign->announceTimesCount < FOREIGN_MASTER_THRESHOLD ||
	    !doesForeignMasterEarliestAnnounceQualify(foreign, expiry_threshold))
		return UNQUALIFIED_BY_ANNOUNCE_EXPIRY;

	/* Disqualify masters whose Sync times are too far out from the BMC
	 * discriminator time source, if enabled. */
	if (discriminator_offset) {
		ForeignSyncSnapshot *syncSnapshot = &foreign->syncSnapshot;

		/* If we have not yet seen a sync then wait until we do */
		if (!syncSnapshot->have_offset)
			return UNQUALIFIED_BY_DISCRIMINATOR;

		/* Take the absolute difference between the foreign master offset and
		 * discriminator offset */
		sfptpd_time_t diff = syncSnapshot->offset - discriminator_offset;
		diff = sfptpd_time_abs(diff);

		portIdentity_display(&foreign->header.sourcePortIdentity);
		DBGV("bmc: offset from discriminator=" SFPTPD_FORMAT_FLOAT "ns, in threshold=%d\n",
		     diff, diff < discriminator_threshold);

		/* Compare the difference with the defined discriminator threshold */
		if (diff >= discriminator_threshold) {
			return UNQUALIFIED_BY_DISCRIMINATOR;
		}
	}

	/* Disqualify masters who are 255 or more steps removed. */
	if (foreign->announce.stepsRemoved >= 255)
		return UNQUALIFIED_BY_STEPS_REMOVED;

	return QUALIFIED;
}


/*
 * Delete all unselected but qualified foreign masters from the data set.
 * Masters that are not yet qualified are skipped.
 */
static void
deleteUnselectedMasters(ForeignMasterDS *foreignMasterDS, PtpClock *ptpClock)
{
	Integer16 readIndex;
	Integer16 writeIndex;
	struct sfptpd_timespec threshold;
	ForeignMasterRecord *records = foreignMasterDS->records;

	DBGV("deleteUnselectedMasters\n");

	getForeignMasterExpiryTime(ptpClock, &threshold);

	for (readIndex = writeIndex = 0;
	     readIndex < foreignMasterDS->number_records;
	     readIndex++) {
		if (getQualification(&records[readIndex], &threshold,
					ptpClock->discriminator_offset,
					ptpClock->rtOpts.discriminator_threshold) == QUALIFIED) {
			if (readIndex == foreignMasterDS->best_index) {
				foreignMasterDS->best_index = writeIndex;
				records[writeIndex++] = records[readIndex];
			}
		} else {
			records[writeIndex++] = records[readIndex];
		}
	}
	foreignMasterDS->number_records = writeIndex;
	foreignMasterDS->write_index = writeIndex;
}

/*
 * Perform the Best Master Clock Algorithm (BMCA) on the foreign master dataset.
 */
UInteger8
bmc(ForeignMasterDS *foreignMasterDS,
    const RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
	/* Index of the 'other' record when comparing */
	Integer16 i;

	/* Index of the best record found so far */
	Integer16 besti;

	/* The number of qualified records */
	Integer16 numberRecords = 0;
	struct sfptpd_timespec threshold;

	/* Flag indicating if there has been a competitive choice between qualified records */
	Boolean hadResult = FALSE;

	/* Flag indicating there was disqualification due to discriminator */
	Boolean discriminator_disqual = FALSE;

	DBGV("ptp %s: bmc: number_records=%d \n",
	     rtOpts->name,
	     foreignMasterDS->number_records);

	/* If no records, maintain existing state */
	if (foreignMasterDS->number_records == 0) {
		if (ptpClock->portState == PTPD_MASTER) {
			m1(rtOpts, ptpClock);
		}
		return ptpClock->portState;
	}

	getForeignMasterExpiryTime(ptpClock, &threshold);

	/* Find the first qualified record and make it our initial candidate for 'best' */
	for (besti = 0; (besti < foreignMasterDS->number_records) && (numberRecords == 0);) {
		enum qualification qual = getQualification(&foreignMasterDS->records[besti], &threshold,
							   ptpClock->discriminator_offset,
							   ptpClock->rtOpts.discriminator_threshold);
		if (qual == QUALIFIED)
			numberRecords = 1;
		else if (qual == UNQUALIFIED_BY_DISCRIMINATOR) {
			discriminator_disqual = TRUE;
			besti++;
		} else
			besti++;
	}
	DBGV("ptp %s: bmc: initial: best index=%d, qualified records=%d\n",
	     rtOpts->name, besti, numberRecords);

	/* Do pairwise comparison between the latest candidate for 'best' and
	   the remaining foreign master records */
	for (i = besti + 1; i<foreignMasterDS->number_records;i++) {
		ForeignMasterRecord *record = &foreignMasterDS->records[i];
		ForeignMasterRecord *best = &foreignMasterDS->records[besti];
		enum qualification qual = getQualification(&foreignMasterDS->records[i], &threshold,
							   ptpClock->discriminator_offset,
							   ptpClock->rtOpts.discriminator_threshold);
		if (qual == QUALIFIED) {
			int comp;

			/* Increment the count of qualified records */
			numberRecords++;

			/* Compare a pair of records */
			comp = bmcDataSetComparison(&record->header,
						    &record->announce,
						    &best->header,
						    &best->announce,
						    ptpClock, rtOpts);

			/* If the new record was better, promote it to the
			   latest candidate for 'best' */
			if (comp < 0)
				besti = i;

			/* Set the 'hadResult' flag if one of the candidates
			   was better than the other */
			hadResult |= (comp != 0);

			DBGV("ptp %s: bmc: so far: best index=%d, qualified records=%d, index=%d\n",
			     rtOpts->name, best, numberRecords, i);
		} else if (qual == UNQUALIFIED_BY_DISCRIMINATOR)
			discriminator_disqual = TRUE;
		DBGV("ptp %s: bmc: candidate %d not qualified\n",
		     rtOpts->name, i);
	}

	DBGV("ptp %s: bmc: done: competitive=%s, qualified records=%d\n",
	     rtOpts->name, hadResult ? "yes" : "no", numberRecords);
	if (hadResult || (numberRecords == 1)) {
		DBGV("Best record : %d \n",besti);
		foreignMasterDS->best_index = besti;
		deleteUnselectedMasters(foreignMasterDS, ptpClock);

		return bmcStateDecision(&foreignMasterDS->records[foreignMasterDS->best_index],
					rtOpts, ptpClock);
	} else if (discriminator_disqual &&
		   (ptpClock->portState == PTPD_SLAVE ||
		    ptpClock->portState == PTPD_UNCALIBRATED)) {
		WARNING("ptp %s: bmc: only remaining master is outside discriminator threshold\n", rtOpts->name);
		return PTPD_LISTENING;
	} else {
		return ptpClock->portState;
	}
}



/*

13.3.2.6, page 126

PTPv2 valid flags per packet type:

ALL:
   .... .0.. .... .... = PTP_UNICAST
SYNC+Pdelay Resp:
   .... ..0. .... .... = PTP_TWO_STEP

Announce only:
   .... .... ..0. .... = FREQUENCY_TRACEABLE
   .... .... ...0 .... = TIME_TRACEABLE
   .... .... .... 0... = PTP_TIMESCALE
   .... .... .... .0.. = PTP_UTC_REASONABLE
   .... .... .... ..0. = PTP_LI_59
   .... .... .... ...0 = PTP_LI_61

*/
