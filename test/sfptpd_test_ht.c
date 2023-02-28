/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_ht.c
 * @brief  Hash table unit test
 */

#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>

#include "sfptpd_misc.h"
#include "sfptpd_statistics.h"
#include "sfptpd_test.h"

#define SFPTPD_HT_CLOCK_ID_MAX		(15)
#define SFPTPD_HT_PORT_NUM_MAX		(200)
#define SFPTPD_HT_DOMAIN_NUM_MAX	(10)
#define SFPTPD_HT_BOOL_MAX		(1)

#define TEST_HOST_ADDR_LEN (60)

struct test_details {
	int test_num;
	int num_nodes;
	int repeat;
	char *test_desc;
	bool clear_table;
	bool replay_entries;
};

static struct test_details tests[] =
{
	{1, SFPTPD_HT_STATS_SET_MAX - 5, 1, "Test 1: Adding entries to hash table\n", true, false},
	{2, 0, 1, "Test 2: Clearing entries from hash table\n", true, false},
	{3, SFPTPD_HT_STATS_SET_MAX + 5, 1, "Test 3: Adding more entries than maximum to hash table\n", false, false},
	{4, 0, 1, "Test 4: Clearing entries from full hash table\n", true, false},
	{5, SFPTPD_HT_STATS_SET_MAX / 2, 1, "Test 5: Adding already present entries\n", false, true},
	{6, SFPTPD_HT_STATS_SET_MAX + 5, 15, "Test 6: Adding and clearing entries to check for memory leaks\n", true, false}
};

int random_num(int limit) {
	int divisor = RAND_MAX / (limit + 1);
	int random_val;

	do {
		random_val = rand() / divisor;
	} while (random_val > limit);

	return random_val;
}

void random_addr(char addr[TEST_HOST_ADDR_LEN])
{
	enum {
		IPv4,
		IPv6,
		IPv6_LINK_SCOPE,
		TYPES
	} type = random_num(TYPES - 1);
	char intf[18] = "";

	switch (type) {
	case IPv4:
		snprintf(addr, TEST_HOST_ADDR_LEN,
			 "%d.%d.%d.%d",
			 random_num(255), random_num(255),
			 random_num(255), random_num(255));
		break;
	case IPv6_LINK_SCOPE:
		snprintf(intf, sizeof intf,
			 "%%enp%ds%df%d",
			 random_num(15), random_num(15), random_num(15));
	case IPv6:
		snprintf(addr, TEST_HOST_ADDR_LEN,
			 "%x:%x:%x:%x:%x:%x:%x:%x%s",
			 random_num(0xffff), random_num(0xffff),
			 random_num(0xffff), random_num(0xffff),
			 random_num(0xffff), random_num(0xffff),
			 random_num(0xffff), random_num(0xffff),
			 intf);
	case TYPES:
		break;
	}
}

void assign_clock_id(unsigned char *clock_id) {
	int index;

	for (index = 0; index < SFPTPD_CLOCK_HW_ID_SIZE; index++) {
		clock_id[index] = random_num(SFPTPD_HT_CLOCK_ID_MAX);
	}
}

bool add_repeat_entries(struct sfptpd_hash_table *table, struct sfptpd_stats_ptp_node reference[],
			int num_nodes)
{
	int ii, rc;
	bool success = true;

	for (ii = 0; ii < num_nodes; ii++) {
		rc = sfptpd_ht_add(table, (void *)&reference[ii], false);
		if (rc != EEXIST) {
			printf("entry %s was unexpectedly not found in table\n",
				reference[ii].clock_id_string);
			success = false;
		}
	}
	return success;
}

bool add_and_check_nodes(struct sfptpd_hash_table *table, struct test_details test) {
	int ii, rc, max_entries, entries, nodes_present = 0;
	uint16_t port_no, domain_no;
	unsigned char clock_id[SFPTPD_CLOCK_HW_ID_SIZE];
	char transport_addr[TEST_HOST_ADDR_LEN];
	bool master, overflow = false, local_success = false, overall_success = true;
	struct sfptpd_stats_ptp_node *node, *reference_nodes;
	struct sfptpd_ht_iter iter;

	reference_nodes = calloc(1, sizeof(*reference_nodes) * test.num_nodes);
	if (reference_nodes == NULL) {
		printf("Insufficient memory to allocate reference_nodes\n");
		return false;
	}

	max_entries = sfptpd_ht_get_max_num_entries(table);
	if (test.num_nodes > max_entries) {
		overflow = true;
	}
	if (max_entries != SFPTPD_HT_STATS_SET_MAX) {
		printf("max_entries does not equal SFPTPD_HT_STATS_SET_MAX: entry numbers may be wrong\n");
	}

	if (test.clear_table) {
		sfptpd_ht_clear_entries(table);
	}

	for (ii = 0; ii < test.num_nodes; ii++) {
		/* Create random values */
		assign_clock_id(clock_id);
		port_no = random_num(SFPTPD_HT_PORT_NUM_MAX);
		domain_no = random_num(SFPTPD_HT_DOMAIN_NUM_MAX);
		master = random_num(SFPTPD_HT_BOOL_MAX) ? true : false;
		random_addr(transport_addr);

		/* Add nodes into table */
		rc = sfptpd_stats_add_node(table, clock_id, master,
					   port_no, domain_no, transport_addr);
		if ((ii >= max_entries) && (rc != ENOSPC)) {
			printf("Incorrect return code on table overflow %s\n", strerror(rc));
			overall_success = false;
		} else if ((rc != 0) && (ii < max_entries)) {
			printf("Incorrect return code for node addition %s\n", strerror(rc));
			overall_success = false;
		}

		/* Save nodes for reference */
		memcpy(&reference_nodes[ii].clock_id, clock_id,
		       sizeof(reference_nodes[ii].clock_id));
		sfptpd_clock_init_hw_id_string(reference_nodes[ii].clock_id_string,
					       reference_nodes[ii].clock_id,
					       SFPTPD_CLOCK_HW_ID_STRING_SIZE);
		reference_nodes[ii].state = (master) ? "Master" : "Slave";
		reference_nodes[ii].domain_number = domain_no;
		reference_nodes[ii].port_number = port_no;
		snprintf(reference_nodes[ii].transport_address,
			 sizeof(reference_nodes[ii].transport_address),
			 "%-39.39s", transport_addr);
	}

	/* Re-add already added entries */
	if (test.replay_entries) {
		overall_success &= add_repeat_entries(table, reference_nodes, test.num_nodes);
	}

	entries = sfptpd_ht_get_num_entries(table);

	/* Iterate through nodes, checking them against reference */
	node = sfptpd_stats_node_ht_get_first(table, &iter);
	while (node != NULL) {
		for (ii = 0; ii < test.num_nodes; ii++) {
			if ((memcmp(&reference_nodes[ii].clock_id, &node->clock_id,
			    sizeof(node->clock_id)) == 0)
			    && (reference_nodes[ii].port_number == node->port_number)
			    && (strncmp(reference_nodes[ii].transport_address,
					node->transport_address, TEST_HOST_ADDR_LEN) == 0)) {
				local_success = true;
				break;
			}
		}
		if (!local_success) {
			printf("node with clock ID %s was not found in reference\n",
				node->clock_id_string);
			overall_success = false;
		}
		local_success = false;
		node = sfptpd_stats_node_ht_get_next(&iter);
		nodes_present++;
	}

	/* Check the table contains the right number of entries */
	if (overflow) {
		if (nodes_present != max_entries) {
			printf("%d nodes found, table was expected to have reached capacity of %d\n",
				nodes_present, max_entries);
				overall_success = false;
		}
		if (entries != max_entries) {
			printf("table recorded %d nodes, maximum of %d expected\n",
				entries, max_entries);
			overall_success = false;
		}
	} else {
		if (nodes_present != test.num_nodes) {
			printf("%d nodes found, %d nodes were expected\n",
			       nodes_present, test.num_nodes);
			overall_success = false;
		}
		if (entries != test.num_nodes) {
			printf("table recorded %d nodes, %d nodes were expected\n",
				entries, test.num_nodes);
			overall_success = false;
		}
	}

	free(reference_nodes);

	return overall_success;
}

bool add_and_check_wrapper(struct sfptpd_hash_table *table, struct test_details test)
{
	int ii;
	bool success = true;

	for (ii = 0; ii < test.repeat; ii++) {
		success &= add_and_check_nodes(table, test);
	}

	return success;
}

bool output_test_result(bool test_success) {
	if (!test_success) {
		printf("failed\n");
	} else {
		printf("passed\n");
	}
	return test_success;
}

int sfptpd_test_ht(void)
{
	int ii, rc = 0;
	bool success = true;
	struct sfptpd_hash_table *table;

	srand(time(NULL));
	table = sfptpd_stats_create_set();

	for (ii = 0; ii < (sizeof(tests) / sizeof(tests[0])); ii++) {
		printf("%s", tests[ii].test_desc);
		success &= output_test_result(add_and_check_wrapper(table, tests[ii]));
	}

	sfptpd_ht_free(table);

	if (!success) {
		rc = 1;
	}
	return rc;
}

