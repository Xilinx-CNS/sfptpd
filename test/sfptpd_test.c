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
	num_unit_tests++;
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int main(int argc, char **argv)
{
	unsigned int i;
	int rc;
	int result = 0;
	bool found = false;

	printf("sfptpd unit tests\n");

	/* Register test cases here... */
	register_unit_test("config", sfptpd_test_config);
	register_unit_test("hash", sfptpd_test_ht);
	register_unit_test("stats", sfptpd_test_stats);
	register_unit_test("filters", sfptpd_test_filters);
	register_unit_test("threading", sfptpd_test_threading);
	register_unit_test("bic", sfptpd_test_bic);
	register_unit_test("fmds", sfptpd_test_fmds);

	/* If no arguments are specified, run all tests */
	for (i = 0; i < num_unit_tests; i++) {
		if ((argc == 1) || (strcmp(argv[1], "all") == 0) ||
		    (strcmp(argv[1], unit_tests[i].name) == 0)) {
			printf("running %s unit test...\n", unit_tests[i].name);
			found = true;
			rc = unit_tests[i].fn();
			if (rc == 0) {
				printf("%s unit test passed\n", unit_tests[i].name);
			} else {
				printf("%s unit test failed, %s\n",
				       unit_tests[i].name, strerror(rc));
				result = rc;
			}
		}
	}

	if (!found)
		printf("unit test %s not found\n", argv[1]);
	else if (result != 0)
		printf("unit tests failed, %s\n", strerror(result));
	else
		printf("unit tests passed\n");

	return result;
}


/* fin */
