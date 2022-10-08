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
 * @file   foreign.c
 *
 * @brief  Code to manage the foreign master dataset
 *
 */

#include "ptpd.h"


/* Function definitions */

static void
foreignMasterDataSetDiagnostics(ForeignMasterDS *ds, const char *text)
{
	DBGV("%s: number=%d, max=%d, write=%d, best=%d\n", text,
	     ds->number_records, ds->max_records, ds->write_index, ds->best_index);
}


static ForeignMasterRecord *
findForeignMasterRecord(const MsgHeader *header, ForeignMasterDS *ds)
{
	ForeignMasterRecord *record;
	int i,j;

	j = ds->best_index;

	/* Check if Foreign master is already known.
	   'i' is the loop variable but 'j' is the subscript/index variable.
	   The relationship is that j = (i + current_best) % length
	   so that we look at the entries most likely to be relevant first.
	 */
	for (i = 0; i < ds->number_records; i++) {
		record = &ds->records[j];

		if (!memcmp(header->sourcePortIdentity.clockIdentity,
			    record->foreignMasterPortIdentity.clockIdentity,
			    CLOCK_IDENTITY_LENGTH) &&
		    (header->sourcePortIdentity.portNumber ==
		     record->foreignMasterPortIdentity.portNumber)) {

			/* Found Foreign Master in Foreignmaster data set */
			return record;
		}
		j = (j+1)%ds->number_records;
	}
	return NULL;
}


static void recordForeignMasterAnnounce(ForeignMasterRecord *record)
{
	clock_gettime(CLOCK_MONOTONIC,
		      &record->announceTimes[record->announceTimesWriteIdx]);
	record->announceTimesWriteIdx++;
	if (record->announceTimesWriteIdx == FOREIGN_MASTER_THRESHOLD)
		record->announceTimesWriteIdx = 0;
	if (record->announceTimesCount < FOREIGN_MASTER_THRESHOLD)
		record->announceTimesCount++;
}


Boolean doesForeignMasterLatestAnnounceQualify(ForeignMasterRecord *record,
					       const struct timespec *threshold)
{
	if (record->announceTimesCount != 0) {
		int index = record->announceTimesWriteIdx - 1;
		if (index < 0)
			index += FOREIGN_MASTER_THRESHOLD;
		assert(index >= 0);

		if (sfptpd_time_is_greater_or_equal(&record->announceTimes[index],
						    threshold)) {
			return TRUE;
		}
	}
	return FALSE;
}


Boolean doesForeignMasterEarliestAnnounceQualify(ForeignMasterRecord *record,
						 const struct timespec *threshold)
{
	if (record->announceTimesCount != 0) {
		int index = record->announceTimesWriteIdx - record->announceTimesCount;
		if (index < 0)
			index += FOREIGN_MASTER_THRESHOLD;
		assert(index >= 0);

		if (sfptpd_time_is_greater_or_equal(&record->announceTimes[index],
						    threshold)) {
			return TRUE;
		}
	}
	return FALSE;
}


int
initForeignMasterDS(ForeignMasterDS *ds, int max_records)
{
	ds->max_records = max_records;
	ds->write_index = 0;
	ds->best_index = 0;
	ds->number_records = 0;
	ds->records = (ForeignMasterRecord *)
		calloc(max_records, sizeof(ForeignMasterRecord));
	if (ds->records == NULL) {
		CRITICAL("failed to allocate PTP module foreign master memory\n");
		free(ds->records);
		return ENOMEM;
	}

	return 0;
}


void
resetForeignMasterDS(ForeignMasterDS *ds)
{
	foreignMasterDataSetDiagnostics(ds, "foreign.reset.entry");

	ds->number_records = 0;
	ds->write_index = 0;
	ds->best_index = 0;

	foreignMasterDataSetDiagnostics(ds, "foreign.reset.exit");
}


void
freeForeignMasterDS(ForeignMasterDS *ds)
{
	resetForeignMasterDS(ds);
	if (ds->records != NULL) {
		free(ds->records);
		ds->records = NULL;
	}
}


/* Insert or update the record for a foreign master into the
   foreign master dataset, returning the index of the relevant
   entry. */
int
insertIntoForeignMasterDS(MsgHeader *header,
			  MsgAnnounce *announce,
			  PortCommunicationCapabilities *comm_caps,
			  ForeignMasterDS *foreignMasterDS,
			  const struct sockaddr_storage *senderAddr,
			  socklen_t senderAddrLen)
{
	ForeignMasterRecord *record;
	int j;

	foreignMasterDataSetDiagnostics(foreignMasterDS, "foreign.insert.entry");

	record = findForeignMasterRecord(header, foreignMasterDS);

	if (record != NULL) {

		/* Foreign Master is already in Foreignmaster data set */
		DBGV("addForeign: foreign master announce times updated\n");

	} else {

		/* Don't override best master */
		if (foreignMasterDS->write_index < foreignMasterDS->number_records &&
		    foreignMasterDS->write_index == foreignMasterDS->best_index) {
			foreignMasterDS->write_index =
				(foreignMasterDS->write_index + 1) % foreignMasterDS->max_records;
		}
		j = foreignMasterDS->write_index;

		/* New Foreign Master */
		if (foreignMasterDS->number_records < foreignMasterDS->max_records) {
			foreignMasterDS->number_records++;
		}

		record = &foreignMasterDS->records[j];
		record->announceTimesWriteIdx = 0;
		record->announceTimesCount = 0;

		/* Copy new foreign master data set from Announce message */
		copyClockIdentity(record->foreignMasterPortIdentity.clockIdentity,
				  header->sourcePortIdentity.clockIdentity);
		record->foreignMasterPortIdentity.portNumber =
			header->sourcePortIdentity.portNumber;

		foreignMasterDS->write_index =
			(foreignMasterDS->write_index + 1) % foreignMasterDS->max_records;

		DBGV("addForeign: new foreign Master added\n");
	}

	/*
	 * header and announce field of each Foreign Master are
	 * useful to run Best Master Clock Algorithm
	 */
	record->header = *header;
	record->announce = *announce;
	record->comm_caps = *comm_caps;

	/* Store the IP address of the master to facilitate hybrid mode */
	copyAddress(&record->address,
		    &record->addressLen,
		    senderAddr,
		    senderAddrLen);

	/* Set the last refresh time for ageing */
	recordForeignMasterAnnounce(record);

	foreignMasterDataSetDiagnostics(foreignMasterDS, "foreign.insert.exit");

	/* Subtract the record pointer from the base of the table
	   to give the index of the new record. */
	return record - foreignMasterDS->records;
}


void
addForeign(Octet *buf, size_t length, MsgHeader *header, PtpClock *ptpClock)
{
	MsgAnnounce announce;
	PortCommunicationCapabilities comm_caps = { 0 };

	msgUnpackAnnounce(buf, length, &announce);

	if (ptpClock->transient_packet_state.port_comm_caps_provided) {
		/* Copy announced capabilities */
		comm_caps = ptpClock->transient_packet_state.port_comm_caps;
	} else {
		/* Default is hybrid mode-capable */
		comm_caps.syncCapabilities = PTPD_COMM_MULTICAST_CAPABLE;
		comm_caps.delayRespCapabilities = PTPD_COMM_MULTICAST_CAPABLE | PTPD_COMM_UNICAST_CAPABLE;
	}
	insertIntoForeignMasterDS(header, &announce,
				  &comm_caps,
				  &ptpClock->foreign,
				  &ptpClock->interface->transport.lastRecvAddr,
				  ptpClock->interface->transport.lastRecvAddrLen);
}

static void
removeUtcOffset(struct timespec *time, RunTimeOpts *rtOpts, PtpClock *ptpClock) {
	if ((ptpClock->portState != PTPD_MASTER) &&
	    (ptpClock->timePropertiesDS.currentUtcOffsetValid || rtOpts->alwaysRespectUtcOffset)) {
		time->tv_sec -= ptpClock->timePropertiesDS.currentUtcOffset;
	}
}

static void
applyForeignUtcOffset(struct timespec *time, int UtcOffset, RunTimeOpts *rtOpts, ForeignMasterRecord *record, PtpClock *ptpClock) {
	/* Check if the foreign master has announced its own UTC offset as invalid. 
		record->header is the header of the announce message, which was stored 
		into record->header by insertIntoForeignMasterDS */
	bool currentUtcOffsetValid = IS_SET(record->header.flagField1, PTPD_UTCV);
	/* Check the UTC offset override setting. 
		If the setting is enabled then we ignore the UTC offset advertised by
		the GM and instead apply the override value.
	*/
	if (rtOpts->overrideUtcOffset) {
		time->tv_sec -= rtOpts->overrideUtcOffsetSeconds;
	} else if ((currentUtcOffsetValid || rtOpts->alwaysRespectUtcOffset)) {
		time->tv_sec -= UtcOffset;
	}
}

void
calculateForeignOffset(ForeignSyncSnapshot *syncSnapshot, const Timestamp *syncOriginTimestamp,
		       ForeignMasterRecord *record, PtpClock *ptpClock)
{
	struct timespec sync_time;
/* sync_time will store the sync origin timestamp, which is from the foreign
	master under scrutiny and has NOT had the UTC offset applied to it. 

	This timestamp could be in UTC or it could be in TAI. 

	In order to ensure that the timestamp is in UTC time we need to 
	subtract from it the UTC offset advertised by the GM under scrutiny. 

	To clarify: The GM sends us both a sync message and an announce message. 

	The sync origin timestamp is from the sync message. 

	The UTC offset comes from the announce message. 

	We need to apply the UTC offset from the GM's announce message to the
	sync origin timestamp in order to ensure that the timestamp is in UTC. 

	It is important that we apply the UTC offset from the GM whose timestamp
	is being scrutinized, since, for example, if the GM's timestamp is correct
	(in TAI) but its UTC offset is wrong then we would want to disqualify it.

	TAI is ahead of UTC so we need to subtract the UTC offset from the 
	timestamp to get it in UTC.

	The sync message may arrive before the announce message. In that case, 
	we must wait until the announce message arrives, which is why we check
	for record->announceTimesCount in order to decide whether to return a 
	result or not. 
*/
	struct timespec foreign_offset; /* foreign_offset holds the result. */
	struct timespec local_time;
/* local_time will hold the local timestamp from the NIC that has already 
	had the	applyUtcOffset function called on it. 

	This means it may or may not have had a UTC offset added to it. If a 
	master had not yet been selected, then a UTC offset would not be added.

	The UTC offset that applyUtcOffset adds to the timestamp (if it does
	indeed decide to add a UTC offset) might come from any foreign master.

	The local timestamp was originally in UTC. After the applyUtcOffset 
	function was called on it, it may or may not have had a UTC offset
	added to it. If it did, then it would be in TAI. Otherwise it would 
	still be in UTC. 

	In order to guarantee that the timestamp is in UTC, we call the reverse
	of the applyUtcOffset function, removeUtcOffset, which does the opposite
	of whatever applyUtcOffset did. That means, if a UTC offset was added, 
	then removeUtcOffset will subtract it from the timestamp, otherwise it
	doesn't do anything. 

	After the removeUtcOffset function is called on the local timestamp, the 
	timestamp is guaranteed to be in UTC. 

	An alternative approach would be to store a copy of the local timestamp 
	before it has had the applyUtcOffset function called on it. 

	This approach was considered cleaner as it keeps the special code for this
	feature in as few places as possible.
*/

	assert(syncSnapshot != NULL);
	assert(syncOriginTimestamp != NULL);
	assert(record != NULL);
	assert(ptpClock != NULL);

	if (syncSnapshot->have_timestamp && record->announceTimesCount) {
		toInternalTime(&sync_time, syncOriginTimestamp);

		local_time.tv_sec = syncSnapshot->timestamp.tv_sec;
		local_time.tv_nsec = syncSnapshot->timestamp.tv_nsec;

		/* Undo the applyUtcOffset function. 
		   This ensures that local_time is now in UTC. 
		*/
		removeUtcOffset(&local_time, &ptpClock->rtOpts, ptpClock);

		/* Subtract the UTC offset advertised by the GM under scrutiny
		   from the sync origin timestamp from the GM under scrutiny.
		   This ensures that sync_time is now in UTC. 
		*/
		applyForeignUtcOffset(&sync_time, record->announce.currentUtcOffset, &ptpClock->rtOpts, record, ptpClock);

		sfptpd_time_subtract(&foreign_offset, &sync_time, &local_time);
		syncSnapshot->offset = sfptpd_time_timespec_to_float_ns(&foreign_offset);
		syncSnapshot->have_offset = true;
	}
}


void
recordForeignSync(const MsgHeader *header, PtpClock *ptpClock, TimeInternal *timestamp)
{
	if (ptpClock->discriminator_valid) {
		ForeignMasterRecord *record = findForeignMasterRecord(header, &ptpClock->foreign);
		if (record) {
			ForeignSyncSnapshot *snapshot = &record->syncSnapshot;

			snapshot->have_timestamp = true;
			snapshot->seq = header->sequenceId;
			internalTime_to_ts(timestamp, &snapshot->timestamp);

			if ((header->flagField0 & PTPD_FLAG_TWO_STEP) == 0) {
				calculateForeignOffset(snapshot,
						       &ptpClock->interface->msgTmp.sync.originTimestamp,
						       record,
						       ptpClock);

			} else {
				snapshot->have_offset = false;
			}
		}
	}
}


void
recordForeignFollowUp(const MsgHeader *header, PtpClock *ptpClock, const MsgFollowUp *payload)
{
	if (ptpClock->discriminator_valid) {
		ForeignMasterRecord *record = findForeignMasterRecord(header, &ptpClock->foreign);
		if (record) {
			ForeignSyncSnapshot *snapshot = &record->syncSnapshot;

			if (header->sequenceId == snapshot->seq) {
				calculateForeignOffset(snapshot,
						       &payload->preciseOriginTimestamp,
						       record,
						       ptpClock);
			} else {
				/* Invalidate snapshot if sequence ID of FollowUp does not match Sync */
				snapshot->have_timestamp = false;
				snapshot->have_offset = false;
			}
		}
	}
}


void
getForeignMasterExpiryTime(PtpClock *ptpClock, struct timespec *threshold)
{
	struct timespec window;

	clock_gettime(CLOCK_MONOTONIC, threshold);
	sfptpd_time_float_s_to_timespec(FOREIGN_MASTER_TIME_WINDOW *
					powl(2, ptpClock->logAnnounceInterval), &window);
	sfptpd_time_subtract(threshold, threshold, &window);
}


void
expireForeignMasterRecords(ForeignMasterDS *ds, const struct timespec *threshold)
{
	ForeignMasterRecord *record;
	int i;

	foreignMasterDataSetDiagnostics(ds, "foreign.expiry.entry");

	for (i = 0; i < ds->number_records; ) {
		record = &ds->records[i];

		if(!doesForeignMasterLatestAnnounceQualify(record, threshold)) {

			/* Remove old record */
			memmove(&ds->records[i],
				&ds->records[i + 1],
				(sizeof *record) * ds->number_records - i - 1);

			DBGV("Expired foreign master record %d/%d\n",
			     i + 1,
			     ds->number_records);

			if (ds->write_index > i)
				ds->write_index--;

			if (ds->best_index > i)
				ds->best_index--;

			ds->number_records--;
		} else {
			i++;
		}
	}

	foreignMasterDataSetDiagnostics(ds, "foreign.expiry.exit");
}

/* fin */
