/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test.c
 * @brief  Entry point fot the sfptpd tests
 */

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sfptpd_test.h"
#include "sfptpd_config.h"
#include "sfptpd_logging.h"
#include "sfptpd_engine.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_constants.h"


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define UNIT_TESTS_MAX (16)

typedef int (*sfptpd_unit_test_fn_t)(void);

struct sfptpd_unit_test {
	const char *name;
	sfptpd_unit_test_fn_t fn;
	bool run;
	int result;
};


/****************************************************************************
 * Local Data
 ****************************************************************************/

static struct sfptpd_unit_test unit_tests[UNIT_TESTS_MAX];
static unsigned int num_unit_tests = 0;


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static void register_unit_test(const char *name, sfptpd_unit_test_fn_t fn)
{
	assert(num_unit_tests < UNIT_TESTS_MAX);
	assert(name != NULL);
	assert(fn != NULL);

	unit_tests[num_unit_tests].name = name;
	unit_tests[num_unit_tests].fn = fn;
	unit_tests[num_unit_tests].run = false;
	num_unit_tests++;
}

static int find_unit_test(const char *name) {
	int i;

	for (i = 0; i < num_unit_tests && strcmp(name, unit_tests[i].name); i++);
	return (i == num_unit_tests ? -1 : i);
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int main(int argc, char **argv)
{
	unsigned int i;
	int result = 0;
	int not_found = 0;

	printf("sfptpd unit tests\n");

	/* Register test cases here... */
	register_unit_test("config", sfptpd_test_config);
	register_unit_test("hash", sfptpd_test_ht);
	register_unit_test("stats", sfptpd_test_stats);
	register_unit_test("filters", sfptpd_test_filters);
	register_unit_test("threading", sfptpd_test_threading);
	register_unit_test("bic", sfptpd_test_bic);
	register_unit_test("fmds", sfptpd_test_fmds);
	register_unit_test("link", sfptpd_test_link);

	/* If no arguments or just "all" are specified, run all tests */
	if (argc < 2 || (strcmp(argv[1], "all") == 0))
		for (i = 0; i < num_unit_tests; i++)
			unit_tests[i].run = true;
	else
		for (i = 1; i < argc; i++) {
			int test = find_unit_test(argv[i]);
			if (test == -1) {
				argv[++not_found] = argv[i];
				printf("unit test %s not found\n", argv[i]);
				result = ENOENT;
			} else {
				unit_tests[test].run = true;
			}
		}

	for (i = 0; i < num_unit_tests; i++) {
		struct sfptpd_unit_test *ut = unit_tests + i;
		int rc;

		if (ut->run) {
			printf("running %s unit test...\n", ut->name);
			rc = ut->fn();
			if (rc == 0) {
				printf("%s unit test passed\n", ut->name);
			} else {
				printf("%s unit test failed, %s\n",
				       ut->name, strerror(rc));
				result = rc;
			}
			ut->result = rc;
		} else {
			ut->result = 0;
		}
	}

	if (result != 0) {
		printf("unit tests failed, %s\n", strerror(result));
	} else {
		printf("unit tests passed\n");
	}

	printf("\nUNIT TEST RESULTS SUMMARY\n\n");
	printf("|    | Unit test   | Run     | Result       |\n");
	printf("| -- | ----------- | ------- | ------------ |\n");
	for (i = 0; i < num_unit_tests; i++) {
		struct sfptpd_unit_test *ut = unit_tests + i;

		printf("| %2d | %-11s | %-7s | %-12s |\n",
		       i, ut->name,
		       ut->run ? "Run" : "Not run",
		       ut->run ? (ut->result == 0 ? "Pass" :
				  strerrorname_np(ut->result)) :
		       "");
	}

	if (not_found != 0) {
		printf("\nunit tests not found:");
		for (i = 0; i < not_found; i++)
			printf(" %s", argv[1 + i]);
		printf("\n");
	}

	return result;
}


/* fin */
