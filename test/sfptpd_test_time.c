/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_test_time.c
 * @brief  Time manipulation functions unit test
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
#include <ctype.h>
#include <signal.h>

#include "sfptpd_misc.h"
#include "sfptpd_statistics.h"
#include "sfptpd_logging.h"
#include "sfptpd_test.h"

#define MAX_INSTRS 40
#define MAX_STACK 20
#define MEM_MIN 'a'
#define MEM_MAX 'z'
#define MEM_SZ ((MEM_MAX) - (MEM_MIN) + 1)

enum result {
	R_ROK,
	R_INTERR,
	R_FAIL,
};

enum test_op {
	/* Push sfptpd_timespec using API */
	OP_INIT,
	OP_INIT_S,
	OP_INIT_NS,
	OP_INIT_NS16,
	OP_INIT_NULL1,
	OP_INIT_NULL2,
	/* API conversion functions */
	OP_FROM_F_S,
	OP_FROM_F_NS,
	OP_TO_F_S,
	OP_TO_F_NS,
	OP_NS16_TO_NS,
	OP_NS_TO_NS16,
	OP_FROM_NS16,
	OP_TO_NS16,
	/* Push test literal */
	OP_LIT_T,
	OP_LIT_F,
	OP_LIT_I,
	OP_LIT_B,
	OP_SET_S,
	OP_SET_NS,
	OP_SET_FRAC,
	OP_DIRECT_ADD,
	/* Non-API comparison functions */
	OP_EQ,
	OP_NE,
	OP_GT,
	OP_LT,
	/* Non-API arithmetic */
	OP_MUL,
	OP_DIV,
	OP_TO_F,
	/* API operations to test */
	OP_ADD,
	OP_SUB,
	OP_ZERO_OUT,
	OP_NEG,
	OP_CMP,
	OP_IS_ZERO,
	OP_IS_GE,
	OP_NORMALISE,
	OP_THRESHOLD,
	/* Stack management */
	OP_POP,
	OP_DUP,
	OP_DUP2,
	OP_SWAP,
	/* Tests */
	OP_TST_EQ,
	OP_TST_FALSE,
	OP_TST_TRUE,
	OP_TST_ZERO,
	OP_TST_NEG,
	OP_TST_POS_NZ,
	OP_TST_IS_NRM,
	/* Debug */
	OP_PRINT,
	OP_PRINTX,
	/* Memory */
	OP_STO,
	OP_RCL,
	/* Control */
	OP_BRK,
	OP_END,
	_OP_MAX
};

enum op_family {
	OPF_API,
	OPF_AUX,
	OPF_STK,
	OPF_TST,
	OPF_DBG,
	OPF_MEM,
	OPF_CTL,
};

enum test_type {
	TYPE_INVAL,
	TYPE_I,
	TYPE_F,
	TYPE_T,
	TYPE_III,
	TYPE_B,
};

struct oper {
	enum op_family family;
	int args_in;
	int stack_change;
	const char *mnemonic;
};

const struct oper operations[] = {
	/* API operations to test */
	[OP_INIT]       = { OPF_API, 0, +1, "INIT" },
	[OP_INIT_S]	= { OPF_API, 0, +1, "INIT_S" },
	[OP_INIT_NS]	= { OPF_API, 0, +1, "INIT_NS" },
	[OP_INIT_NS16]	= { OPF_API, 0, +1, "INIT_NS16" },
	[OP_INIT_NULL1]	= { OPF_API, 0, +1, "INIT_NULL1" },
	[OP_INIT_NULL2]	= { OPF_API, 0, +1, "INIT_NULL2" },
	[OP_FROM_F_S]	= { OPF_API, 1,  0, "FROM_F_S" },
	[OP_FROM_F_NS]	= { OPF_API, 1,  0, "FROM_F_NS" },
	[OP_TO_F_S]	= { OPF_API, 1,  0, "TO_F_S" },
	[OP_TO_F_NS]	= { OPF_API, 1,  0, "TO_F_NS" },
	[OP_NS16_TO_NS]	= { OPF_API, 1,  0, "NS16_TO_NS" },
	[OP_NS_TO_NS16]	= { OPF_API, 1,  0, "NS_TO_NS16" },
	[OP_FROM_NS16]	= { OPF_API, 1,  0, "FROM_NS16" },
	[OP_TO_NS16]	= { OPF_API, 1,  0, "TO_NS16" },
	[OP_ADD]	= { OPF_API, 2, -1, "ADD" },
	[OP_SUB]	= { OPF_API, 2, -1, "SUB" },
	[OP_ZERO_OUT]	= { OPF_API, 1,  0, "ZERO_OUT" },
	[OP_NEG]	= { OPF_API, 1,  0, "NEG" },
	[OP_CMP]	= { OPF_API, 2, -1, "CMP" },
	[OP_THRESHOLD]	= { OPF_API, 3, -2, "THRESHOLD" },
	[OP_IS_ZERO]	= { OPF_API, 1,  0, "IS_ZERO" },
	[OP_IS_GE]	= { OPF_API, 2, -1, "IS_GE" },
	[OP_NORMALISE]	= { OPF_API, 1,  0, "NORMALISE" },
	/* Push test literal */
	[OP_LIT_T]	= { OPF_AUX, 0, +1, "LIT_T" },
	[OP_LIT_F]	= { OPF_AUX, 0, +1, "LIT_F" },
	[OP_LIT_I]	= { OPF_AUX, 0, +1, "LIT_I" },
	[OP_LIT_B]	= { OPF_AUX, 0, +1, "LIT_B" },
	/* Set values in timespec */
	[OP_SET_S]	= { OPF_AUX, 1,  0, "SET_S" },
	[OP_SET_NS]	= { OPF_AUX, 1,  0, "SET_NS" },
	[OP_SET_FRAC]	= { OPF_AUX, 1,  0, "SET_FRAC" },
	[OP_DIRECT_ADD]	= { OPF_AUX, 1,  0, "DIRECT_ADD" },
	/* Comparisons yielding a boolean on stack */
	[OP_EQ]		= { OPF_AUX, 0,  0, "EQ" },
	[OP_NE]		= { OPF_AUX, 0,  0, "NE" },
	[OP_GT]		= { OPF_AUX, 0,  0, "GT" },
	[OP_LT]		= { OPF_AUX, 0,  0, "LT" },
	/* Non-API arithmetic */
	[OP_MUL]	= { OPF_AUX, 2, -1, "MUL" },
	[OP_DIV]	= { OPF_AUX, 2, -1, "DIV" },
	[OP_TO_F]	= { OPF_AUX, 1,  0, "TO_F" },
	/* Stack management */
	[OP_POP]	= { OPF_STK, 1, -1, "POP" },
	[OP_DUP]	= { OPF_STK, 1, +1, "DUP" },
	[OP_DUP2]	= { OPF_STK, 2, +1, "DUP2" },
	[OP_SWAP]	= { OPF_STK, 2,  0, "SWAP" },
	/* Tests */
	[OP_TST_EQ]	= { OPF_TST, 2, -2, "TST_EQ" },
	[OP_TST_FALSE]	= { OPF_TST, 1, -1, "TST_FALSE" },
	[OP_TST_TRUE]	= { OPF_TST, 1, -1, "TST_TRUE" },
	[OP_TST_ZERO]	= { OPF_TST, 1, -1, "TST_ZERO" },
	[OP_TST_NEG]	= { OPF_TST, 1, -1, "TST_NEG" },
	[OP_TST_POS_NZ]	= { OPF_TST, 1, -1, "TST_POS_NZ" },
	[OP_TST_IS_NRM] = { OPF_TST, 1, -1, "TST_IS_NRM" },
	/* Debug */
	[OP_PRINT]	= { OPF_DBG, 1, -1, "PRINT" },
	[OP_PRINTX]	= { OPF_DBG, 1, -1, "PRINTX" },
	/* Memory */
	[OP_STO]	= { OPF_MEM, 1, -1, "STO" },
	[OP_RCL]	= { OPF_MEM, 0, +1, "RCL" },
	/* Control */
	[OP_BRK]	= { OPF_CTL, 0,  0, "BRK" },
	[OP_END]	= { OPF_CTL, 0,  0, "END" },
};

struct test_val {
	enum test_type type;
	union {
		int64_t i;
		sfptpd_time_t f;
		struct sfptpd_timespec t;
		int64_t iii[3];
		bool b;
	};
};

typedef struct test_val mem_t[MEM_SZ];

struct test_instr {
	enum test_op op;
	struct test_val operand;
};

struct test_details {
	int test_num;
	struct test_instr instrs[MAX_INSTRS];
	char *test_desc;
};

static struct test_details tests[] =
{
	{ 1, {
		{ OP_INIT_NS, { .i = 100000000UL }},
		{ OP_INIT_S, { .i = 3 }},
		{ OP_ADD },
		{ OP_TO_F_S },
		{ OP_LIT_F, { .f = 3.1L }},
		{ OP_TST_EQ },
		{ OP_END }}},
	{ 2, {
		{ OP_INIT_NS, { .i = 100000000UL }},
		{ OP_INIT_S, { .i = 3 }},
		{ OP_SUB },
		{ OP_TO_F_S },
		{ OP_LIT_F, { .f = -2.9L }},
		{ OP_TST_EQ },
		{ OP_END }}},
	{ 3, {
		{ OP_INIT, { .iii = { 2, 3, 0xC << 24 }}},
		{ OP_DUP },
		{ OP_TO_F_NS },
		{ OP_LIT_F, { .f = 0.5L }},
		{ OP_MUL },
		{ OP_DUP },
		{ OP_FROM_F_S },
		{ OP_IS_ZERO },
		{ OP_TST_FALSE },
		{ OP_DUP },
		{ OP_TST_POS_NZ },
		{ OP_FROM_F_NS },
		{ OP_DUP },
		{ OP_ADD },
		{ OP_SUB },
		{ OP_INIT_S, { .i =  123 }},
		{ OP_ZERO_OUT },
		{ OP_CMP },
		{ OP_TST_ZERO },
		{ OP_END }}},

	{ 4, {
		{ OP_LIT_F, { .f = 2.000000001L }},
		{ OP_FROM_F_S },
		{ OP_INIT_NS16, { .i = 60000}},
		{ OP_INIT_NS, { .i = 2000000000UL }},
		{ OP_DUP },
		{ OP_TST_IS_NRM },
		{ OP_ADD },
		{ OP_SUB },
		{ OP_TO_F_NS },
		{ OP_DUP },
		{ OP_LIT_F, { .f = 1.0L }},
		{ OP_CMP },
		{ OP_TST_NEG },
		{ OP_LIT_F, { .f = 0.01L }},
		{ OP_CMP },
		{ OP_TST_POS_NZ },
		{ OP_END }}},

	{ 5, {
		{ OP_INIT_NS, { .i = 500000000UL }},
		{ OP_DUP },
		{ OP_STO, { .i = 'a' }},
		{ OP_DUP },
		{ OP_ADD },
		{ OP_TST_IS_NRM },
		{ OP_END }}},

	{ 6, {
		{ OP_INIT, { .iii = { 0, 1000000000ULL, 1 }}}, /* Unnormalised timespecs will not display correctly. */
		{ OP_NORMALISE },
		{ OP_DUP },
		{ OP_TST_IS_NRM },
		{ OP_LIT_T, { .iii = { 1, 0, 1 }}},
		{ OP_TST_EQ },
		{ OP_END }}},
	{ 7, {
		{ OP_INIT, { .iii = { 0, 1000000000ULL, 1 }}},
		{ OP_NORMALISE },
		{ OP_DUP },
		{ OP_TST_IS_NRM },
		{ OP_DUP },
		{ OP_LIT_T, { .iii = { 1, 0, 2 }}},
		{ OP_DUP },
		{ OP_STO, { .i = 'b' }},
		{ OP_CMP },
		{ OP_LT },
		{ OP_TST_TRUE },
		{ OP_LIT_T, { .iii = { 0, 0, 1}}},
		{ OP_ADD },
		{ OP_DUP },
		{ OP_RCL, { .i = 'b' }},
		{ OP_CMP },
		{ OP_EQ },
		{ OP_TST_TRUE },
		{ OP_RCL, { .i = 'b' }},
		{ OP_TST_EQ },
		{ OP_END }}},

	{ 8, {
		{ OP_RCL, { .i = 'a' }},
		{ OP_DUP },
		{ OP_SUB },
		{ OP_TST_ZERO },
		{ OP_END }}},

	{ 9, {
		{ OP_INIT, { .iii = { 0, 1000000000ULL, 1 }}},
		{ OP_NORMALISE },
		{ OP_DUP },
		{ OP_TST_IS_NRM },
		{ OP_LIT_T, { .iii = { 1, 0, 1 }}},
		{ OP_TST_EQ },
		{ OP_END }}},

	{ 10, {
		{ OP_INIT_NULL1 },
		{ OP_TST_ZERO },
		{ OP_INIT_NULL2 },
		{ OP_TST_ZERO },
		{ OP_INIT_S, { .i = 0 }},
		{ OP_TST_ZERO },
		{ OP_INIT_NS, { .i = 0 }},
		{ OP_TST_ZERO },
		{ OP_INIT_NS16, { .i = 0 }},
		{ OP_TST_ZERO },
		{ OP_INIT, { .iii = {0, 0, 0 }}},
		{ OP_TST_ZERO },
		{ OP_LIT_F, { .f = 0.0L }},
		{ OP_DUP },
		{ OP_FROM_F_S },
		{ OP_TST_ZERO },
		{ OP_FROM_F_NS },
		{ OP_TST_ZERO },
		{ OP_LIT_I, { .i = 0 }},
		{ OP_FROM_NS16 },
		{ OP_TST_ZERO },
		{ OP_INIT, { .iii = { INT64_MAX, 999999999, UINT32_MAX }}},
		{ OP_ZERO_OUT },
		{ OP_TST_ZERO },
		{ OP_INIT, { .iii = { INT64_MAX, UINT32_MAX, UINT32_MAX }}}, /* Unnormalised, will print wrong */
		{ OP_ZERO_OUT },
		{ OP_TST_ZERO },
		{ OP_END }}},

	{ 11, {
		{ OP_INIT_NULL1 },
		{ OP_INIT_NULL1 },
		{ OP_IS_GE },
		{ OP_TST_TRUE },
		{ OP_INIT_S, { .i = 2 }},
		{ OP_DUP },
		{ OP_INIT_NS16, { .i = 100 }},
		{ OP_SWAP },
		{ OP_DUP2 },
		{ OP_IS_GE },
		{ OP_TST_TRUE },
		{ OP_SWAP },
		{ OP_IS_GE },
		{ OP_TST_FALSE },
		{ OP_END }}},

	{ 12, {
		{ OP_LIT_F, { .f = 0.0L }},
		{ OP_DUP },
		{ OP_NEG },
		{ OP_TST_ZERO },
		{ OP_FROM_F_S },
		{ OP_NEG },
		{ OP_TST_ZERO },
		{ OP_INIT_S, { .i = 1 }},
		{ OP_DUP },
		{ OP_NEG },
		{ OP_DUP },
		{ OP_TST_NEG },
		{ OP_ADD },
		{ OP_TST_ZERO },
		{ OP_END }}},

	{ 13, {
		{ OP_LIT_F, { .f = -1.1L }},
		{ OP_DUP },
		{ OP_FROM_F_S },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_NEG },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_TO_F_S },
		{ OP_NEG },
		{ OP_TST_EQ },
		{ OP_END }}},

	{ 1300, {
		{ OP_LIT_F, { .f = -1.1L }},
		{ OP_DUP },
		{ OP_FROM_F_S },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_INIT_NULL2 },
		{ OP_SWAP},
		{ OP_SUB },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_TO_F_S },
		{ OP_NEG },
		{ OP_TST_EQ },
		{ OP_END }}, "debug NEG failure" },

	{ 14, {
		{ OP_LIT_I, { .i = 40000 }},
		{ OP_DUP },
		{ OP_TO_F },
		{ OP_LIT_I, { .i = 65536 }},
		{ OP_TO_F },
		{ OP_DIV },
		{ OP_INIT_NS16, { .i = 40000 }},
		{ OP_TO_F_NS },
		{ OP_TST_EQ },
		{ OP_NS16_TO_NS },
		{ OP_INIT_NS16, { .i = 40000 }},
		{ OP_TO_F_NS },
		{ OP_TST_EQ },
		{ OP_INIT_NS16, { .i = 40000 }},
		{ OP_TO_F_NS },
		{ OP_LIT_F, { .f = 2.0L }},
		{ OP_DIV },
		{ OP_DUP },
		{ OP_LIT_F, { .f = 40000.0L/(2*65536) }},
		{ OP_TST_EQ },
		{ OP_NS_TO_NS16 },
		{ OP_LIT_I, { .i = 20000 }},
		{ OP_TST_EQ },
		{ OP_END }}},

	{ 15, {
		{ OP_LIT_I, { .i = 0x0700000000000ULL }},
		{ OP_TO_F },
		{ OP_FROM_F_NS },
		{ OP_TO_NS16 },
		{ OP_LIT_I, { .i = 0x7FFFFFFFFFFFFFFFULL }},
		{ OP_CMP },
		{ OP_LT },
		{ OP_TST_TRUE },
		{ OP_LIT_I, { .i = 0x120000000000ULL }},
		{ OP_DUP }, { OP_PRINTX },
		{ OP_TO_F },
		{ OP_LIT_I, { .i = 0x10 }},
		{ OP_TO_F },
		{ OP_MUL },
		{ OP_FROM_F_NS },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_TO_NS16 },
		{ OP_DUP }, { OP_PRINTX },
		{ OP_LIT_I, { .i = 0x7FFFFFFFFFFFFFFFULL }},
		{ OP_DUP }, { OP_PRINTX },
		{ OP_TST_EQ },
		{ OP_END }}},

	{ 16, {
		/* PPS sample timestamp */
		{ OP_INIT_NS, { .i = 999971107 }},
		{ OP_DUP },
		{ OP_LIT_T , { .iii = {0, 999971107, 0 }}},
		{ OP_TST_EQ },
		{ OP_DUP },
		/* Check if leading or trailing */
		{ OP_INIT_NS, { .i = 500000000 }},
		{ OP_DUP },
		{ OP_LIT_T, { .iii = {0, 500000000, 0 }}},
		{ OP_TST_EQ },
		{ OP_IS_GE },
		{ OP_TST_TRUE },
		/* Subtract one second with API */
		{ OP_DUP },
		{ OP_INIT_S, { .i = 1 }},
		{ OP_SUB },
		{ OP_LIT_T, { .iii = {-1, 999971107, 0 }}},
		{ OP_TST_EQ },
		/*Force s to -1 */
		{ OP_DUP },
		{ OP_SET_S, { .i = -1 }},
		{ OP_SET_FRAC , { .i = 0 }},
		{ OP_LIT_T, { .iii = {-1, 999971107, 0 }}},
		{ OP_TST_EQ },
		/* Subtract one second */
		{ OP_DIRECT_ADD, { .iii = {-1, 0, 0 }}},
		{ OP_SET_FRAC , { .i = 0 }},
		{ OP_DUP },
		{ OP_LIT_T, { .iii = {-1, 999971107, 0 }}},
		{ OP_TST_EQ },
		/* Convert to float */
		{ OP_TO_F_NS },
		{ OP_LIT_F, { .f = 999971107L - 1000000000L }},
		{ OP_TST_EQ },
		{ OP_END }}, "SWPTP-1448: directed test for PPS regression"},
	{ 17, {
		{ OP_INIT, { .iii = { 1L, 0, 0 }}},
		{ OP_INIT, { .iii = { 2L, 0, 0 }}},
		{ OP_INIT, { .iii = { 0L, 500000000U, 0 }}},
		{ OP_THRESHOLD },
		{ OP_TST_FALSE },
		{ OP_INIT, { .iii = { 1L, 0, 0 }}},
		{ OP_INIT, { .iii = { 1L, 500000000U, 0 }}},
		{ OP_INIT, { .iii = { 0L, 500000000U, 0 }}},
		{ OP_THRESHOLD },
		{ OP_TST_TRUE },
		{ OP_INIT, { .iii = { -1L, 0, 0 }}},
		{ OP_INIT, { .iii = { -2L, 0, 0 }}},
		{ OP_INIT, { .iii = { 0L, 500000000U, 0 }}},
		{ OP_THRESHOLD },
		{ OP_TST_FALSE },
		{ OP_INIT, { .iii = { -1L, 0, 0 }}},
		{ OP_INIT, { .iii = { -1L, 500000000U, 0 }}},
		{ OP_INIT, { .iii = { 0L, 500000000U, 0 }}},
		{ OP_THRESHOLD },
		{ OP_TST_TRUE },
		{ OP_INIT, { .iii = { -1L, 0, 0 }}},
		{ OP_INIT, { .iii = { -2L, 999999999U, 0xFFFFFFFFU }}},
		{ OP_INIT, { .iii = { 0L, 500000000U, 0 }}},
		{ OP_THRESHOLD },
		{ OP_TST_TRUE },
		{ OP_END }}, "Threshold equality test" },
};

static void print_stack(const struct test_val *stack, int sp)
{
	int i = 0;

	printf("stack:");
	for (i = 0; i < sp; i++) {
		const struct test_val *sv = &stack[i];
		switch(stack[i].type) {
		case TYPE_I:
			printf(" %" PRId64, sv->i);
			break;
		case TYPE_F:
			printf(" " SFPTPD_FORMAT_FLOAT "L", sv->f);
			break;
		case TYPE_T:
			printf(" " SFPTPD_FMT_SSFTIMESPEC "(frac=0x%08X)" ,
			       SFPTPD_ARGS_SSFTIMESPEC(sv->t), sv->t.nsec_frac);
			break;
		case TYPE_B:
			printf(" %s", sv->b ? "true" : "false");
			break;
		default:
			assert(!!"unexpected type on stack");
		}
	}
	printf("\n");
}

static bool check_stack(int sp, int req) {
	if (sp == MAX_STACK) {
		printf("stack oveflow\n");
		return true;
	}
	if (req > sp) {
		printf("stack underflow\n");
		return true;
	}
	return false;
}

static bool check_type(const struct test_val *val, enum test_type type) {
	if (val->type != type || val->type == TYPE_INVAL) {
		printf("type error\n");
		return true;
	}
	return false;
}

static bool check_var(uint64_t key) {
	if (key < MEM_MIN || key > MEM_MAX) {
		if (isprint(key) && !isspace(key))
			printf("invalid variable '%c'\n", (char) key);
		else
			printf("invalid variable 0x%" PRIx64 "\n", key);
		return true;
	}
	return false;
}

static enum result bad_type(void) {
	printf("type error\n");
	return R_INTERR;
}

static bool check_result(bool condition) {
	if (!condition) {
		printf("result check failed\n");
		return true;
	}
	return false;
}

static enum result run_test(const struct test_details *test, mem_t *mem,
			    bool *tested_ops)
{
	struct test_val stack[MAX_STACK];
	const struct test_instr *instr;
	const struct oper *op;
	int pc;
	int sp;
	bool check;

	sp = 0;
	for (pc = 0;; pc++) {
		/* Convenience accessors for next and top two stack elements */
		struct test_val *s0 = &stack[sp];
		struct test_val *s1 = &stack[sp-1];
		struct test_val *s2 = &stack[sp-2];
		struct test_val *s3 = &stack[sp-3];

		/* Integrity checks */
		assert(pc < MAX_INSTRS);
		if (check_stack(sp, 0))
			return R_INTERR;

		instr = &test->instrs[pc];
		assert(instr->op >= 0 && instr->op < _OP_MAX);
		op = operations + instr->op;
		tested_ops[instr->op] = true;

		printf("  %-10s ", op->mnemonic);

		if (check_stack(sp, op->args_in))
			return R_INTERR;

		switch (instr->op) {
		case OP_INIT:
			s0->type = TYPE_T;
			sfptpd_time_init(&s0->t,
					 instr->operand.iii[0],
					 instr->operand.iii[1],
					 instr->operand.iii[2]);
			break;
		case OP_INIT_S:
			s0->type = TYPE_T;
			sfptpd_time_from_s(&s0->t,
					   instr->operand.i);
			break;
		case OP_INIT_NS:
			s0->type = TYPE_T;
			sfptpd_time_from_ns(&s0->t,
					    instr->operand.i);
			break;
		case OP_INIT_NS16:
			s0->type = TYPE_T;
			sfptpd_time_from_ns16(&s0->t,
					      instr->operand.i);
			break;
		case OP_INIT_NULL1:
			s0->type = TYPE_T;
			s0->t = SFPTPD_NULL_TIME;
			break;
		case OP_INIT_NULL2:
			s0->type = TYPE_T;
			s0->t = sfptpd_time_null();
			break;
		case OP_ZERO_OUT:
			switch (s1->type) {
			case TYPE_B:
				s1->b = false;
				break;
			case TYPE_I:
				s1->i = 0;
				break;
			case TYPE_F:
				s1->f = 0.0L;
				break;
			case TYPE_T:
				sfptpd_time_zero(&s1->t);
				break;
			default:
				return bad_type();
			}
			break;
		case OP_ADD:
			if (check_type(s1, s2->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_B:
				s2->b = s2->b || s1->b;
				break;
			case TYPE_I:
				s2->i = s1->i + s2->i;
				break;
			case TYPE_F:
				s2->f = s1->f + s2->f;
				break;
			case TYPE_T:
				sfptpd_time_add(&s2->t, &s2->t, &s1->t);
				break;
			default:
				return bad_type();
			}
			break;
		case OP_SUB:
			if (check_type(s1, s2->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_I:
				s2->i = s2->i - s1->i;
				break;
			case TYPE_F:
				s2->f = s2->f - s1->f;
				break;
			case TYPE_T:
				sfptpd_time_subtract(&s2->t, &s2->t, &s1->t);
				break;
			default:
				return bad_type();
			}
			break;
		case OP_MUL:
			if (check_type(s1, s2->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_B:
				s2->b = s2->b && s1->b;
				break;
			case TYPE_I:
				s2->i = s2->i * s1->i;
				break;
			case TYPE_F:
				s2->f = s2->f * s1->f;
				break;
			default:
				return bad_type();
			}
			break;
		case OP_DIV:
			if (check_type(s1, s2->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_I:
				s2->i = s2->i / s1->i;
				break;
			case TYPE_F:
				s2->f = s2->f / s1->f;
				break;
			default:
				return bad_type();
			}
			break;
		case OP_TO_F:
			switch (s1->type) {
			case TYPE_I:
				s1->f = (sfptpd_time_t) s1->i;
				break;
			case TYPE_F:
				break;
			default:
				return bad_type();
			}
			s1->type = TYPE_F;
			break;
		case OP_NEG:
			switch (s1->type) {
			case TYPE_B:
				s1->b = !s1->b;
				break;
			case TYPE_I:
				s1->i = -s1->i;
				break;
			case TYPE_F:
				s1->f = -s1->f;
				break;
			case TYPE_T:
				sfptpd_time_negate(&s1->t, &s1->t);
				break;
			default:
				return bad_type();
			}
			break;
		case OP_NORMALISE:
			if (check_type( s1, TYPE_T))
				return R_INTERR;
			sfptpd_time_normalise(&s1->t);
			break;
		case OP_CMP:
			if (check_type(s1, s2->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_B:
				s2->i = (s2->b && !s2->b) ? 1 : ((s1->b && !s2->b) ? -1 : 0);
				break;
			case TYPE_I:
				s2->i = (s2->i > s1->i) ? 1 : ((s2->i < s1->i) ? -1 : 0);
				break;
			case TYPE_F:
				s2->i = (s2->f > s1->f) ? 1 : ((s2->f < s1->f) ? -1 : 0);
				break;
			case TYPE_T:
				s2->i = sfptpd_time_cmp(&s2->t, &s1->t);
				break;
			default:
				return bad_type();
			}
			s2->type = TYPE_I;
			break;
		case OP_THRESHOLD:
			if (check_type(s2, s1->type))
				return R_INTERR;
			if (check_type(s3, s1->type))
				return R_INTERR;
			switch (s1->type) {
			case TYPE_T:
				s3->b = sfptpd_time_equal_within(&s3->t, &s2->t, &s1->t);
				break;
			default:
				return bad_type();
			}
			s3->type = TYPE_B;
			break;
		case OP_IS_ZERO:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->b = sfptpd_time_is_zero(&s1->t);
			s1->type = TYPE_B;
			break;
		case OP_IS_GE:
			if (check_type(s1, TYPE_T) ||
			    check_type(s2, TYPE_T))
				return R_INTERR;
			s2->b = sfptpd_time_is_greater_or_equal(&s2->t, &s1->t);
			s2->type = TYPE_B;
			break;
		case OP_TO_F_S:
		case OP_TO_F_NS:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			if (instr->op == OP_TO_F_S)
				s1->f = sfptpd_time_timespec_to_float_s(&s1->t);
			else
				s1->f = sfptpd_time_timespec_to_float_ns(&s1->t);
			s1->type = TYPE_F;
			break;
		case OP_FROM_F_S:
		case OP_FROM_F_NS:
			if (check_type(s1, TYPE_F))
				return R_INTERR;
			if (instr->op == OP_FROM_F_S)
				sfptpd_time_float_s_to_timespec(s1->f, &s1->t);
			else
				sfptpd_time_float_ns_to_timespec(s1->f, &s1->t);
			s1->type = TYPE_T;
			break;
		case OP_FROM_NS16:
			if (check_type(s1, TYPE_I))
				return R_INTERR;
			sfptpd_time_from_ns16(&s1->t, s1->i);
			s1->type = TYPE_T;
			break;
		case OP_TO_NS16:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->i = sfptpd_time_to_ns16(s1->t);
			s1->type = TYPE_I;
			break;
		case OP_NS16_TO_NS:
			if (check_type(s1, TYPE_I))
				return R_INTERR;
			s1->f = sfptpd_time_scaled_ns_to_float_ns(s1->i);
			s1->type = TYPE_F;
			break;
		case OP_NS_TO_NS16:
			if (check_type(s1, TYPE_F))
				return R_INTERR;
			s1->i = sfptpd_time_float_ns_to_scaled_ns(s1->f);
			s1->type = TYPE_I;
			break;
		case OP_LIT_F:
			s0->type = TYPE_F;
			s0->f = instr->operand.f;
			break;
		case OP_LIT_I:
			s0->type = TYPE_I;
			s0->i = instr->operand.i;
			break;
		case OP_LIT_B:
			s0->type = TYPE_B;
			s0->b = instr->operand.b;
			break;
		case OP_LIT_T:
			s0->type = TYPE_T;
			s0->t.sec = instr->operand.iii[0];
			s0->t.nsec = instr->operand.iii[1];
			s0->t.nsec_frac = instr->operand.iii[2];
			break;
		case OP_SET_S:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->t.sec = instr->operand.i;
			break;
		case OP_SET_NS:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->t.nsec = instr->operand.i;
			break;
		case OP_SET_FRAC:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->t.nsec_frac = instr->operand.i;
			break;
		case OP_DIRECT_ADD:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			s1->t.sec += instr->operand.iii[0];
			s1->t.nsec += instr->operand.iii[1];
			s1->t.nsec_frac += instr->operand.iii[2];
			break;
		case OP_EQ:
			switch (s1->type) {
			case TYPE_I:
				s1->b = s1->i == 0;
				break;
			case TYPE_F:
				s1->b = s1->f == 0.0L;
				break;
			default:
				return bad_type();
			}
			s1->type = TYPE_B;
			break;
		case OP_NE:
			switch (s1->type) {
			case TYPE_I:
				s1->b = s1->i != 0;
				break;
			case TYPE_F:
				s1->b = s1->f != 0.0L;
				break;
			default:
				return bad_type();
			}
			s1->type = TYPE_B;
			break;
		case OP_GT:
			switch (s1->type) {
			case TYPE_I:
				s1->b = s1->i > 0;
				break;
			case TYPE_F:
				s1->b = s1->f > 0.0L;
				break;
			default:
				return bad_type();
			}
			s1->type = TYPE_B;
			break;
		case OP_LT:
			switch (s1->type) {
			case TYPE_I:
				s1->b = s1->i < 0;
				break;
			case TYPE_F:
				s1->b = s1->f < 0.0L;
				break;
			default:
				return bad_type();
			}
			s1->type = TYPE_B;
			break;
		case OP_DUP:
			*s0 = *s1;
			break;
		case OP_DUP2:
			*s0 = *s2;
			break;
		case OP_POP:
			break;
		case OP_SWAP:
			{
				struct test_val tmp = *s2;

				*s2 = *s1;
				*s1 = tmp;
			}
			break;
		case OP_TST_TRUE:
			if (check_type(s1, TYPE_B))
				return R_INTERR;
			if (check_result(s1->b))
				return R_FAIL;
			break;
		case OP_TST_FALSE:
			if (check_type(s1, TYPE_B))
				return R_INTERR;
			if (check_result(!s1->b))
				return R_FAIL;
			break;
		case OP_TST_EQ:
			if (check_type(s1, s2->type))
				return R_INTERR;
			check = true;
			switch (s1->type) {
			case TYPE_B:
				check = (s1->b && s2->b) || (!s1->b && !s2->b);
				break;
			case TYPE_I:
				check = s1->i == s2->i;
				break;
			case TYPE_F:
				check = s1->f == s2->f;
				break;
			case TYPE_T:
				check = s1->t.sec == s2->t.sec &&
					s1->t.nsec == s2->t.nsec &&
					s1->t.nsec_frac == s2->t.nsec_frac;
				break;
			default:
				return bad_type();
			}
			if (check_result(check))
				return R_FAIL;
			break;
		case OP_TST_ZERO:
			switch (s1->type) {
			case TYPE_I:
				check = s1->i == 0;
				break;
			case TYPE_F:
				check = s1->f == 0.0L;
				break;
			case TYPE_T:
				check = (s1->t.sec == 0 &&
					 s1->t.nsec == 0 &&
					 s1->t.nsec_frac == 0);
				break;
			default:
				return bad_type();
			}
			if (check_result(check))
				return R_FAIL;
			break;
		case OP_TST_NEG:
			switch (s1->type) {
			case TYPE_I:
				check = s1->i < 0;
				break;
			case TYPE_F:
				check = s1->f < 0.0L;
				break;
			case TYPE_T:
				check = s1->t.sec < 0;
				break;
			default:
				return bad_type();
			}
			if (check_result(check))
				return R_FAIL;
			break;
		case OP_TST_POS_NZ:
			switch (s1->type) {
			case TYPE_I:
				check = s1->i > 0;
				break;
			case TYPE_F:
				check = s1->f > 0.0L;
				break;
			case TYPE_T:
				check = (s1->t.sec > 0  ||
					 s1->t.nsec > 0 ||
					 s1->t.nsec_frac > 0 );
				break;
			default:
				return bad_type();
			}
			if (check_result(check))
				return R_FAIL;
			break;
		case OP_TST_IS_NRM:
			if (check_type(s1, TYPE_T))
				return R_INTERR;
			if (s1->t.nsec >= 1000000000UL)
				return R_FAIL;
			break;
		case OP_PRINT:
			switch (s1->type) {
			case TYPE_B:
				printf("   B(%s)\n", s1->b ? "true" : "false");
				break;
			case TYPE_I:
				printf("   I(%" PRId64 ")\n", s1->i);
				break;
			case TYPE_F:
				printf("   F(" SFPTPD_FORMAT_FLOAT ")\n", s1->f);
				break;
			case TYPE_T:
				printf("   T(" SFPTPD_FMT_SSFTIMESPEC ")\n",
				       SFPTPD_ARGS_SSFTIMESPEC(s1->t));
				break;
			default:
				return bad_type();
			}
			break;
		case OP_PRINTX:
			switch (s1->type) {
			case TYPE_I:
				printf("   I(%" PRIx64 ")\n", s1->i);
				break;
			case TYPE_T:
				printf("   T(%016" PRIX64 ".%08X%08X\n",
				       s1->t.sec, s1->t.nsec, s1->t.nsec_frac);
				break;
			default:
				return bad_type();
			}
			break;
		case OP_STO:
			if (check_var(instr->operand.i))
				return R_INTERR;
			(*mem)[instr->operand.i - MEM_MIN] = *s1;
			break;
		case OP_RCL:
			if (check_var(instr->operand.i))
				return R_INTERR;
			*s0 = (*mem)[instr->operand.i - MEM_MIN];
			break;
		case OP_BRK:
			printf("\n<BRK: pc=%d sp=%d>\n", pc, sp);
			raise(SIGTRAP);
			break;
		case OP_END:
			if (sp != 0) {
				printf("stack not empty (sp=%d)\n", sp);
				return R_INTERR;
			}
			return R_ROK;
		default:
			printf("unhandled op\n");
			return R_INTERR;
		}

		sp += op->stack_change;
		print_stack(stack, sp);
	}

	/* Should always exit via OP_END */
	return R_INTERR;
}

static bool output_test_result(enum result result) {
	if (result == R_FAIL) {
		printf(" failed\n");
	} else if (result == R_ROK) {
		printf("passed\n");
	} else {
		printf(" failed due to internal error\n");
	}
	return result == R_ROK;
}

int sfptpd_test_time(void)
{
	const int n_tests = (sizeof(tests) / sizeof(tests[0]));
	bool ops_tested[_OP_MAX] = { 0 };
	mem_t mem = {{ 0 }};
	int failures = 0;
	int rc = 0;
	int ii;
	int i;

	for (ii = 0; ii < n_tests; ii++) {
		bool ops_tested_now[_OP_MAX] = { 0 };

		printf("Test %d: %s\n",
		       tests[ii].test_num,
		       tests[ii].test_desc ? tests[ii].test_desc : "");
		if (!output_test_result(run_test(&tests[ii], &mem, ops_tested_now))) {
			printf("  API operations executed in failed test:");
			for (i = 0; i < _OP_MAX; i++)
				if (ops_tested_now[i] && operations[i].family == OPF_API)
					printf(" %s", operations[i].mnemonic);
			printf("\n");
			failures++;
		}
		for (i = 0; i < _OP_MAX; i++)
			ops_tested[i] = ops_tested[i] || ops_tested_now[i];
	}

	for (i = 0; i < _OP_MAX; i++)
		if (!ops_tested[i] && operations[i].family == OPF_API)
			printf("Untested API operation: %s\n", operations[i].mnemonic);

	if (failures != 0) {
		printf("time manipulation functions: %d out of %d unit tests failed\n",
		       failures, n_tests);
		rc = ERANGE;
	}
	return rc;
}

