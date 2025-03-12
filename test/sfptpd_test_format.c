/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2025 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_test_format.c
 * @brief  Format interpolation unit test
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfptpd_misc.h"

/****************************************************************************
 * External declarations
 ****************************************************************************/

/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof (a [0]))

enum format1 {
	FMT_A,
	FMT_B,
};

struct object {
	int number;
	char *name;
};

static ssize_t interp1(char *buffer, size_t space, int id, void *context, char opt);

struct test_case {
	char *format;
	struct object *object;
	char *expected_str;
	int error;
};

/****************************************************************************
 * Local Data
 ****************************************************************************/

static struct object object1 = {
	123456,
	"object1"
};

static struct object object2 = {
	-123456,
	"object_2"
};

static struct test_case test_data[] = {
	{ "", NULL, "" },
	{ "No interpolation", NULL, "No interpolation" },
	{ "%%", NULL, "%" },
	{ "%", &object1, "", EINVAL },
	{ "%a", &object1, "123456", EINVAL },
	{ "%a+", &object1, "+123456" },
	{ "%a_", &object2, "-123456" },
	{ "%a%", &object2, "-123456" },
	{ "%a_%", &object1, "", EINVAL },
	{ "%_", &object1, "", EINVAL },
	{ ">%a+", &object2, ">-123456" },
	{ "%b", &object1, "object1" },
	{ "%b%b.", &object1, "object1object1." },
	{ "%b%a_", &object1, "object1123456" },
	{ "%b%a_.", &object1, "object1123456." },
	{ ">%b<%a_\n", &object2, ">object_2<-123456\n" },
};

static struct sfptpd_interpolation specifiers1[] = {
	{ FMT_A, 'a', true, interp1 },
	{ FMT_B, 'b', false, interp1 },
	{ SFPTPD_INTERPOLATORS_END }
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static ssize_t interp1(char *buffer, size_t space, int id, void *context, char opt)
{
	const struct object *object = (const struct object *) context;

	switch (id) {
	case FMT_A:
		return snprintf(buffer, space, "%s%d", opt == '+' && object->number >= 0 ? "+" : "", object->number);
	case FMT_B:
		return snprintf(buffer, space, "%s", object->name);
	default:
		return 0;
	}
}

static int test_format (const char *title,
			struct sfptpd_interpolation *interpolators,
			struct test_case *test_cases,
			int num_test_cases) {
	int failures = 0;
	int i;
	int e;

	for (i = 0; i < num_test_cases; i++) {
		ssize_t expected_len = strlen(test_cases[i].expected_str);
		ssize_t len;
		ssize_t len2;
		char *buf;
		bool failed = false;

		len = sfptpd_format(interpolators, test_cases[i].object, NULL, 0, test_cases[i].format);
		if (len < 0 && (e = errno) != test_cases[i].error) {
			int e = errno;
			printf("%s: test %d: got error sizing string:\n rc = %s\n errno = %s\n expected = %s\n",
			       title, i, strerror(-len), strerror(e), strerror(test_cases[i].error));
			failures++;
			continue;
		}

		if (len >= 0 && len != expected_len) {
			printf("%s: test %d: expected length %zd, got %zd\n",
			       title, i, expected_len, len);
			failed = true;
		}

		if (len < 0) {
			if (failed)
				failures++;
			continue;
		}

		buf = malloc(len + 1);
		assert(buf);
		*buf ='\0';

		len2 = sfptpd_format(interpolators, test_cases[i].object, buf, len + 1, test_cases[i].format);
		if (len2 < 0 && (e = errno) != test_cases[i].error) {
			printf("%s: test %d: got error formatting string:\n rc = %s\n errno = %s\n expected = %s\n",
			       title, i, strerror(-len), strerror(e), strerror(test_cases[i].error));
			failures++;
			free(buf);
			continue;
		}

		if (len2 != len) {
			printf("%s: test %d: actual length %zd differs from predicted length %zd\n",
			       title, i, len2, len);
			failed = true;
		}

		if (len2 >= 0 && strcmp(buf, test_cases[i].expected_str)) {
			printf("%s: test %d: result '%s' differs from expected '%s'\n",
			       title, i, buf, test_cases[i].expected_str);
			failed = true;
		}

		/* Now try with bufer that is one byte too short. */
		memset(buf, '\377', len + 1);
		if (len >= 1)
			len2 = sfptpd_format(interpolators, test_cases[i].object, buf, len, test_cases[i].format);
		else
			len2 = -1;
		if (len >= 1 && len2 != len) {
			printf("%s: test %d: actual length %zd differs from "
			       "predicted length %zd when undersized buffer "
			       "provided\n",
			       title, i, len2, len);
			failed = true;
		}

		if (len2 >= 0 && strncmp(buf, test_cases[i].expected_str, len - 1)) {
			printf("%s: test %d: truncated result '%s' differs from "
			       "truncated portion of  expected '%s'\n",
			       title, i, buf, test_cases[i].expected_str);
			failed = true;
		}

		if (len2 >= 0 && buf[len2 - 1] != '\0') {
			printf("%s: test %d: truncated result is not "
			       "NUL-terminated\n", title, i);
			failed = true;
		}

		if (failed) {
			printf("%s: test %d: failed format string was '%s'\n",
			       title, i, test_cases[i].format);
			failures++;
		}

		free(buf);
	}

	printf("%s: %d failures out of %d tests\n",
	       title, failures, i);

	return failures;
}


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_format (void)
{
	int failures = 0;

	failures += test_format("format", specifiers1, test_data, ARRAY_SIZE(test_data));

	return failures == 0 ? 0 : ERANGE;
}


/* fin */
