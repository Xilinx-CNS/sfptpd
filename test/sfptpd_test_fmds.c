/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_fmds.c
 * @brief  Foreign Master Data Set unit tests
 */
#include <math.h>
#include "ptpd.h"
#include "ieee1588_types.h"

/****************************************************************************
 * External declarations
 ****************************************************************************/

/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof (a [0]))

/* A test sequence consists of an array of input events.
   Each event corresponds to an announce message, after
   which there is a defined delay. A boolean indicates
   that this message should be considered to correspond
   to a new or refreshed best master. */
struct fm_evt_in {
	MsgHeader *header;
	MsgAnnounce *announce;
	struct in_addr *address;
	int delay_after_ms;
	bool takeover_as_best;
};

/****************************************************************************
 * Local Data
 ****************************************************************************/

/* Tests required

1. Empty table
2. Single entry
3. Multiple entries
X4. Multiple entries with rogue domain - not a useful test as domain check
   is done at a higher level.
5. Multiple ports on same source
6. Full table
7. Full table with excess masters
8. Table of size 16
9. Changing best master
9. Changing best master, full table
9. Changing best master, full table, excess masters

 */

struct in_addr m1_address = { .s_addr = 0xc0a80001 };
struct in_addr m2_address = { .s_addr = 0xc0a80002 };
struct in_addr m3_address = { .s_addr = 0xc0a80003 };
struct in_addr m4_address = { .s_addr = 0xc0a80004 };

MsgHeader m1_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.messageLength = 0,
	.domainNumber = 0,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 1}, 0},
	.logMessageInterval = 0
};

MsgHeader m2_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 2}, 0},
};

MsgHeader m3_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 3}, 0},
};

MsgHeader m4_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 4}, 0},
};

MsgHeader m1_rogue_domain_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.domainNumber = 88,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 88, 1}, 0},
};

MsgHeader m1_port2_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.domainNumber = 88,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 1}, 1},
};

MsgHeader m2_port2_header = {
	.messageType = PTPD_MSG_ANNOUNCE,
	.versionPTP = 2,
	.domainNumber = 88,
	.sourcePortIdentity = { { 0, 0, 0, 0, 0, 0, 0, 2}, 1},
};

MsgAnnounce m1_announce = {
};

MsgAnnounce m2_announce = {
};

MsgAnnounce m3_announce = {
};

MsgAnnounce m4_announce = {
};

const struct fm_evt_in test_1[] = {
};

const struct fm_evt_in test_2[] = {
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
	{ &m1_header, &m1_announce, &m1_address, 1000, true},
};

const struct fm_evt_in test_3[] = {
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
};

const struct fm_evt_in test_4[] = {
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m1_rogue_domain_header, &m1_announce, &m1_address, 50, false},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
	{ &m1_rogue_domain_header, &m1_announce, &m1_address, 50, false},
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m1_rogue_domain_header, &m1_announce, &m1_address, 50, false},
	{ &m2_header, &m2_announce, &m2_address, 900, false},
};

const struct fm_evt_in test_5[] = {
	{ &m1_port2_header, &m2_announce, &m2_address, 50, false},
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m2_header, &m1_announce, &m1_address, 50, false},
	{ &m2_port2_header, &m2_announce, &m2_address, 850, false},
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m1_port2_header, &m2_announce, &m2_address, 50, false},
	{ &m2_port2_header, &m2_announce, &m2_address, 50, false},
	{ &m2_header, &m1_announce, &m1_address, 850, false},
	{ &m1_port2_header, &m2_announce, &m2_address, 50, false},
	{ &m1_header, &m1_announce, &m1_address, 50, true},
	{ &m2_port2_header, &m2_announce, &m2_address, 50, false},
	{ &m2_header, &m1_announce, &m1_address, 850, false},
};

const struct fm_evt_in test_6[] = {
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
};

const struct fm_evt_in test_9[] = {
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, false},
	{ &m2_header, &m2_announce, &m2_address, 100, true},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, false},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, true},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, false},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, true},
	{ &m2_header, &m2_announce, &m2_address, 100, true},
	{ &m3_header, &m3_announce, &m3_address, 100, true},
	{ &m4_header, &m4_announce, &m4_address, 700, true},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, false},
	{ &m2_header, &m2_announce, &m2_address, 100, false},
	{ &m3_header, &m3_announce, &m3_address, 100, false},
	{ &m4_header, &m4_announce, &m4_address, 700, false},
	{ &m1_header, &m1_announce, &m1_address, 100, false},
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/

int sfnanosleep(const struct sfptpd_timespec *sfrequest,
		struct sfptpd_timespec *sfremain)
{
	struct timespec request = { .tv_sec = sfrequest->sec,
				    .tv_nsec = sfrequest->nsec };
	struct timespec remain;
	int rc = nanosleep(&request, &remain);
	sfremain->sec = remain.tv_sec;
	sfremain->nsec = remain.tv_nsec;
	sfremain->nsec_frac = 0;
	return rc;
}

static int check_integrity(ForeignMasterDS *ds, const struct fm_evt_in *best, int slots)
{
	int errors = 0;

	if (ds->records == NULL) {
		printf("ERROR: records not allocated\n");
		errors++;
	}

	if (ds->max_records != slots) {
		printf("ERROR: wrong number of foreign record slots (%d)\n", ds->max_records);
		errors++;
	}

	if (ds->number_records > ds->max_records) {
		printf("ERROR: number_records out of range (%d)\n", ds->number_records);
		errors++;
	}

	if (ds->write_index < 0 || ds->write_index >= ds->max_records) {
		printf("ERROR: write_index out of range (%d)\n", ds->write_index);
		errors++;
	}

	if (ds->best_index < 0 || ds->best_index >= ds->max_records) {
		printf("ERROR: best_index out of range (%d)\n", ds->best_index);
		errors++;
	}

	/* Ensure best is present if defined */
	if (best != NULL) {
		ForeignMasterRecord *record = &ds->records[ds->best_index];

		if (0 != memcmp(&record->header.sourcePortIdentity,
				&best->header->sourcePortIdentity,
				sizeof record->header.sourcePortIdentity)) {
			printf("ERROR: best master not identified in table\n");
			errors++;
		}
	}

	return errors;
}

static int test_fmds(const char *name,
		     const struct fm_evt_in *sequence,
		     size_t size,
		     int slots)
{
	ForeignMasterDS ds;
	struct sfptpd_timespec now = { 0 };
	struct sfptpd_timespec next_announce = { 0 };
	struct sfptpd_timespec next_event = { 0 };
	struct sfptpd_timespec delay = { 0 };
	struct sfptpd_timespec remaining = { 0 };
	int i;
	int rc;
	int errors = 0;
	const struct fm_evt_in *last_best_event = NULL;
	struct sfptpd_timespec announce_interval = {
		.sec = 2,
		.nsec = 0
	};

	/* Start initial announce timer */
	sfclock_gettime(CLOCK_MONOTONIC, &now);
	sfptpd_time_add(&next_announce, &now, &announce_interval);
	
	initForeignMasterDS(&ds, slots);

	printf("Running foreign master data set test \"%s\" with %d slots\n", name, slots);

	errors += check_integrity(&ds, last_best_event, slots);

	for (i = 0; i < size; i++) {
		const struct fm_evt_in *event = sequence + i;
		int idx;
		struct sockaddr_in sockaddr;
		PortCommunicationCapabilities commcaps = { 0 };

		//printf("  step %d\n", i);

		sfclock_gettime(CLOCK_MONOTONIC, &now);

		sockaddr.sin_family = AF_INET;
		sockaddr.sin_addr = *event->address;

		/* Insert into the foreign master dataset as if we had
		   just seen an announce message. */
		idx = insertIntoForeignMasterDS(event->header,
						event->announce,
						&commcaps,
						&ds,
						(struct sockaddr_storage *) &sockaddr,
						sizeof sockaddr);

		/* WARNING: the next block modifies the data structures under test. */
		if (event->takeover_as_best) {

			/* Updated in dataset */
			ds.best_index = idx;

			/* Update local record */
			last_best_event = event;

			/* Update announce interval */
			sfptpd_time_float_s_to_timespec(powl(2.0, event->header->logMessageInterval),
							&announce_interval);

			/* Restart announce timer */
			sfptpd_time_add(&next_announce, &now, &announce_interval);
		}

		/* Pace the test as specified in the sequence */
		delay.sec = event->delay_after_ms / 1000;
		delay.nsec = (event->delay_after_ms % 1000) * 1000000;

		/* Record time for next event */
		sfptpd_time_add(&next_event, &now, &delay);
		
		displayForeignMasterRecords(&ds, NULL);

		do {
			sfclock_gettime(CLOCK_MONOTONIC, &now);

			if (sfptpd_time_is_greater_or_equal(&next_event, &next_announce)) {

				/* Next event is an announce interval expiry */
				if (sfptpd_time_is_greater_or_equal(&now, &next_announce)) {

					//printf("announce interval expired\n");

					/* And it has now expired */
					struct sfptpd_timespec threshold;

					/* Calculate the threshold for expiring foreign master entries */
					sfptpd_time_float_s_to_timespec(4.0 * sfptpd_time_timespec_to_float_s(&announce_interval),
									&threshold);
					sfptpd_time_subtract(&threshold, &now, &threshold);
					//printf("expiring foreign master records\n");
					expireForeignMasterRecords(&ds, &threshold);
					displayForeignMasterRecords(&ds, NULL);
					sfptpd_time_add(&next_announce, &now, &announce_interval);
					errors += check_integrity(&ds, last_best_event, slots);
				} else {
					//printf("waiting for announce interval to expire\n");

					/* Wait for the announce interval to expire */
					sfptpd_time_subtract(&delay, &next_announce, &now);
					sfnanosleep(&delay, &remaining);
				}
			} else {

				/* Next event is to process the next event in the sequence */
				if (sfptpd_time_is_greater_or_equal(&now, &next_event)) {

					//printf("ready for next event\n");

					/* And it is due now */
					errors += check_integrity(&ds, last_best_event, slots);
					break;
				} else {
					//printf("waiting for next event\n");

					/* Wait for the next event */
					sfptpd_time_subtract(&delay, &next_event, &now);
					rc = sfnanosleep(&delay, &remaining);
					if (rc != 0 && rc != EINTR) {
						perror("nanosleep");
						return 1;
					}
				}
			}
		} while (true);
	}

	freeForeignMasterDS(&ds);

	printf("%d errors for \"%s\n", errors, name);
	return errors;
}

/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_fmds (void)
{
	int errors = 0;

	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_PTPD2, 0);
	
	errors += test_fmds("empty table", test_1, ARRAY_SIZE(test_1), 4);
	errors += test_fmds("single entry", test_2, ARRAY_SIZE(test_2), 3);
        errors += test_fmds("multiple entries", test_3, ARRAY_SIZE(test_3), 3);
	errors += test_fmds("multiple entries, multiple ports", test_5, ARRAY_SIZE(test_5), 5);
	errors += test_fmds("full table", test_6, ARRAY_SIZE(test_6), 4);
	errors += test_fmds("full table, excess masters", test_6, ARRAY_SIZE(test_6), 3);
	errors += test_fmds("table of size 16", test_6, ARRAY_SIZE(test_6), 16);
	errors += test_fmds("changing master", test_9, ARRAY_SIZE(test_9), 8);
	errors += test_fmds("full table, changing master", test_9, ARRAY_SIZE(test_9), 4);
	errors += test_fmds("full table, changing master, excess masters", test_9, ARRAY_SIZE(test_9), 3);

	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_PTPD2, 0);

	printf("%d errors\n", errors);

	return errors == 0 ? 0 : EPROTO;
}


/* fin */
