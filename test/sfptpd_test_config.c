/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_config.c
 * @brief  Config parsing unit test
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

#include "sfptpd_config.h"
#include "sfptpd_misc.h"
#include "sfptpd_test.h"


/****************************************************************************
 * External declarations
 ****************************************************************************/

extern unsigned int tokenize(char *input, unsigned int max_tokens, const char *tokens[]);


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

struct tokenize_test {
	const char *input;
	unsigned int expected_num_tokens;
	const char *expected_tokens[SFPTPD_CONFIG_TOKENS_MAX];
};


/****************************************************************************
 * Local Data
 ****************************************************************************/

static struct tokenize_test tokenize_tests[] =
{
	/* Test comments */
	{"# comment", 0, {""}},
	{"one two three four five-tokens", 5, {"one", "two", "three", "four", "five-tokens"}},
	{"one two-tokens # no no no not this", 2, {"one", "two-tokens"}},
	{"one two-tokens # \" no no not this", 2, {"one", "two-tokens"}},
	{"one two-tokens # \' no no not this", 2, {"one", "two-tokens"}},
	{"one two-tokens # \t no no not this", 2, {"one", "two-tokens"}},
	{"one two# three four five-tokens", 2, {"one", "two"}},
	{"o#ne; two# three four ;five-tokens", 1, {"o"}},

	/* Test white space */
	{"\t\tone\t two three     \t   four five-tokens", 5, {"one", "two", "three", "four", "five-tokens"}},
	{"one \n two", 1, {"one"}},
	{"one\n two", 1, {"one"}},
	{"o\ne two", 1, {"o"}},

	/* Tests of null termination */
	{"one two three\0", 3, {"one", "two", "three"}},
	{"\0one two three\0", 0, {}},
	{"one t\0wo three\0", 2, {"one", "t"}},

	/* Escapes */
	{"\\#one two three", 3, {"#one", "two", "three"}},
	{"one \\; two three", 4, {"one", ";", "two", "three"}},
	{"\\\none two three", 3, {"\none", "two", "three"}},
	{"one\\\n two three", 3, {"one\n", "two", "three"}},
	{"one tw\\\\o \\\\ three", 4, {"one", "tw\\o", "\\", "three"}},
	{"a\\bc\\de\\fg # the rest is a comment", 1, {"abcdefg"}},
	{"\\\0one two three", 0, {}},
	{"one two t\\\0hree", 3, {"one", "two", "t"}},
	{"one\\\t two\\  three\\ \\ ", 3, {"one\t", "two ", "three  "}},
	{"one\\' \\'two\\' th\\'ree \\'", 4, {"one'", "'two'", "th\'ree", "'"}},
	{"one\\' \\'two\\' th\\'ree '", 3, {"one'", "'two'", "th'ree"}},
	{"one\\\" \\\"two\\\" th\\\"ree \\\"", 4, {"one\"", "\"two\"", "th\"ree", "\""}},
	{"one\\\" \\\"two\\\" th\\\"ree \"", 3, {"one\"", "\"two\"", "th\"ree"}},

	/* Quotes */
	{"\"one\" 'two' \"three\"", 3, {"one", "two", "three"}},
	{"\"one' \"two' \"three'", 3, {"one' ", "two'", "three'"}},
	{"\"one two three\"", 1, {"one two three"}},
	{"\"one' 'two'\" three", 2, {"one' 'two'", "three"}},
	{"\"\" \"a\" \"'\" '' '\"'", 5, {"", "a", "'", "", "\""}},
	{"\"o\\\\ne\" '\\two' \"th\\#ree\"", 3, {"o\\ne", "two", "th#ree"}},
	{"\"o#;ne\" '#two' \";three\"", 3, {"o#;ne", "#two", ";three"}},
	{"\"\tone\t\" 'two \t   ' \"    thr\tee  \"", 3, {"\tone\t", "two \t   ", "    thr\tee  "}},
	{"\"\none\" 'two\n' \"t\nh\nree\"", 3, {"\none", "two\n", "t\nh\nree"}},
	{"\"one two three", 1, {"one two three"}},
	{"one two three \"", 3, {"one", "two", "three"}},
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_config(void)
{
	int rc = 0;
	int test;
	unsigned int i, j, num_tokens;
	char line[SFPTPD_CONFIG_LINE_LENGTH_MAX];
	const char *tokens[SFPTPD_CONFIG_TOKENS_MAX];

	for (i = 0; i < sizeof(tokenize_tests)/sizeof(tokenize_tests[0]); i++) {
		sfptpd_strncpy(line, tokenize_tests[i].input, sizeof(line)); 
	
		num_tokens = tokenize(line, SFPTPD_CONFIG_TOKENS_MAX, tokens);

		printf("test %d", i);
		//printf("\n  input: \"%s\"\n", tokenize_tests[i].input);
		test = 0;

		if (num_tokens != tokenize_tests[i].expected_num_tokens) {
			printf("  unexpected num tokens: got %d, expected %d\n",
			       num_tokens, tokenize_tests[i].expected_num_tokens);
			test = 1;
		}

		for (j = 0; (j < num_tokens) && (j < tokenize_tests[i].expected_num_tokens); j++) {
			if (strcmp(tokens[j], tokenize_tests[i].expected_tokens[j]) != 0) {
				printf("  unexpected token[%d]: got %s, expected %s\n",
				       j, tokens[j], tokenize_tests[i].expected_tokens[j]);
				test = 1;
			}
		}

		if (test == 0)
			printf("  passed\n");
		else
			rc = test;
	}

	return rc;
}


/* fin */
