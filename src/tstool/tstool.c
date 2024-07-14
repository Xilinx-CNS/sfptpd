/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

/* Timestamping control utility */

#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <regex.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>

#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_netlink.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_logging.h"


/****************************************************************************
 * Constant
 ****************************************************************************/

static const char *opts_short = "hv";
static const struct option opts_long[] = {
	{ "help", 0, NULL, (int) 'h' },
	{ "verbose", 0, NULL, (int) 'v' },
	{ NULL, 0, NULL, 0 }
};


/****************************************************************************
 * Types
 ****************************************************************************/

enum clock_command_e {
	CLOCK_CMD_LIST,
	CLOCK_CMD_INFO,
	CLOCK_CMD_GET,
	CLOCK_CMD_STEP,
	CLOCK_CMD_SLEW,
	CLOCK_CMD_SET_TO,
	CLOCK_CMD_DIFF,
	CLOCK_CMD_INVALID
};

struct clock_command {
	enum clock_command_e tag;
	const char *name;
	int clock_args;
};


/****************************************************************************
 * Local Data
 ****************************************************************************/

static struct sfptpd_config *config = NULL;
static struct sfptpd_nl_state *netlink = NULL;
static const struct sfptpd_link_table *initial_link_table = NULL;

static const struct clock_command clock_cmds[] = {
	{ CLOCK_CMD_LIST,    "list",    0 },
	{ CLOCK_CMD_INFO,    "info",    1 },
	{ CLOCK_CMD_GET,     "get",     1 },
	{ CLOCK_CMD_STEP,    "step",    1 },
	{ CLOCK_CMD_SLEW,    "slew",    1 },
	{ CLOCK_CMD_SET_TO,  "set_to",  2 },
	{ CLOCK_CMD_DIFF,    "diff",    2 },
	{ CLOCK_CMD_INVALID, "INVALID", 0 },
};


/****************************************************************************
 * Local functions
 ****************************************************************************/

static int do_init(void)
{
        struct sfptpd_config_general *gconf;
	int rc;

	/* Initialise config */
	rc = sfptpd_config_create(&config);
	if (rc != 0)
		return EXIT_FAILURE;

	/* Tweak config */
	gconf = sfptpd_general_config_get(config);
	gconf->non_sfc_nics = true;

	/* Start netlink service */
	netlink = sfptpd_netlink_init();
	if (netlink == NULL) {
		CRITICAL("could not start netlink\n");
		return EXIT_FAILURE;
	}

	rc = sfptpd_netlink_scan(netlink);
	if (rc != 0) {
		CRITICAL("scanning with netlink, %s\n",
			 strerror(rc));
		return rc;
	}

	/* Wait 5 seconds for initial link table */
	initial_link_table = sfptpd_netlink_table_wait(netlink, 1, 5000);
	if (initial_link_table == NULL) {
		CRITICAL("could not get initial link table, %s\n",
			 strerror(errno));
		return EXIT_FAILURE;
	}

	/* Start clock management */
	rc = sfptpd_clock_initialise(config, NULL);
	if (rc != 0)
		return EXIT_FAILURE;

	/* Start interface management */
	rc = sfptpd_interface_initialise(config, NULL,
					 initial_link_table);
	if (rc != 0)
		return EXIT_FAILURE;

	return 0;
}

static void do_finit(void)
{
	sfptpd_clock_shutdown();
	sfptpd_interface_shutdown(config);
	if (netlink != NULL)
		sfptpd_netlink_finish(netlink);
	sfptpd_config_destroy(config);
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"syntax: %s [OPTIONS] SUBSYSTEM COMMAND..\n"
		"\n"
		"  OPTIONS\n"
		"    -h, --help                  Show usage\n"
		"    -v, --verbose               Be verbose\n\n"
		"  CLOCK SUBSYSTEM\n"
		"    clock list                  List clocks\n"
		"    clock info                  Show clock information\n"
		"    clock get CLOCK             Read clock\n"
		"    clock step CLOCK OFFSET     Step clock\n"
		"    clock slew CLOCK PPB        Adjust clock frequency\n"
		"    clock set_to CLOCK1 CLOCK2  CLOCK1 := CLOCK2\n"
		"    clock diff CLOCK1 CLOCK2    CLOCK1 - CLOCK2\n\n"
		"      CLOCK := <phcN> | <ethN> | system\n",
		program_invocation_short_name);
}


static int clock_command(int argc, char *argv[])
{
#define MAX_CLOCKS 2
	struct sfptpd_timespec times[MAX_CLOCKS];
	struct sfptpd_clock *clocks[MAX_CLOCKS];
	struct sfptpd_interface *interface;
	struct sfptpd_clock **all_clocks;
	const struct clock_command *cmd;
	const char *command;
	size_t num_clocks;
	sfptpd_time_t f;
	int tokens;
	int rc = 0;
	int i;

	if (argc < 1) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	command = argv[0];

	for (cmd = clock_cmds;
	     cmd->tag != CLOCK_CMD_INVALID &&
	     strcmp(command, cmd->name);
	     cmd++);

	assert(cmd->clock_args <= MAX_CLOCKS);
	if (argc - 1 < cmd->clock_args) {
		ERROR("insufficient number of clocks specified\n");
		usage(stderr);
		return EXIT_FAILURE;
	}

	for (i = 0; i < cmd->clock_args; i++) {
		const char *clock_ref = argv[1 + i];
		clocks[i] = sfptpd_clock_find_by_name(clock_ref);
		if (clocks[i] == NULL &&
		    (interface = sfptpd_interface_find_by_name(clock_ref))) {
			clocks[i] = sfptpd_interface_get_clock(interface);
		}
		if (clocks[i] == NULL) {
			fprintf(stderr, "unknown clock: %s\n", clock_ref);
			return EXIT_FAILURE;
		}
	}

	switch(cmd->tag) {
	case CLOCK_CMD_LIST:
		sfptpd_clock_diagnostics(3);
		all_clocks = sfptpd_clock_get_active_snapshot(&num_clocks);
		for (i = 0; i < num_clocks; i++) {
			printf("%s\n", sfptpd_clock_get_long_name(all_clocks[i]));
		}
		sfptpd_clock_free_active_snapshot(all_clocks);
		break;
	case CLOCK_CMD_INFO:
		printf("short-name: %s\n"
		       "long-name: %s\n"
		       "hw-id: %s\n"
		       "persistent-freq-correction: %.3Lf ppb\n"
		       "max-freq-adj: %.3Lf ppb\n"
		       "diff-method: %s\n",
		       sfptpd_clock_get_short_name(clocks[0]),
		       sfptpd_clock_get_long_name(clocks[0]),
		       sfptpd_clock_get_hw_id_string(clocks[0]),
		       sfptpd_clock_get_freq_correction(clocks[0]),
		       sfptpd_clock_get_max_frequency_adjustment(clocks[0]),
		       sfptpd_clock_get_diff_method(clocks[0]));
		break;
	case CLOCK_CMD_GET:
		rc = sfptpd_clock_get_time(clocks[0], times + 0);
		printf("%s: " SFPTPD_FMT_SFTIMESPEC "\n",
		       sfptpd_clock_get_short_name(clocks[0]),
		       SFPTPD_ARGS_SFTIMESPEC(times[0]));
		break;
	case CLOCK_CMD_STEP:
		tokens = sscanf(argv[1 + cmd->clock_args], "%Lf", &f);
		if (tokens != 1) {
			ERROR("invalid offset specified\n");
			return EXIT_FAILURE;
		}
		sfptpd_time_float_s_to_timespec(f, times + 0);
		rc = sfptpd_clock_adjust_time(clocks[0], times + 0);
		break;
	case CLOCK_CMD_SLEW:
		tokens = sscanf(argv[1 + cmd->clock_args], "%Lf", &f);
		if (tokens != 1) {
			ERROR("invalid frequency adjustment specified\n");
			return EXIT_FAILURE;
		}
		rc = sfptpd_clock_adjust_frequency(clocks[0], f);
		break;
	case CLOCK_CMD_DIFF:
		rc = sfptpd_clock_compare(clocks[0], clocks[1], times + 0);
		printf("%s-%s: " SFPTPD_FMT_SSFTIMESPEC "\n",
		       sfptpd_clock_get_short_name(clocks[0]),
		       sfptpd_clock_get_short_name(clocks[1]),
		       SFPTPD_ARGS_SSFTIMESPEC(times[0]));
		break;
	case CLOCK_CMD_SET_TO:
		rc = sfptpd_clock_set_time(clocks[0], clocks[1], NULL, false);
		break;
	default:
		fprintf(stderr, "unknown clock command: %s\n", command);
		usage(stderr);
		return EXIT_FAILURE;
	}

	if (rc != 0) {
		ERROR("tstool: clock: %s: %s\n", command, strerror(rc));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


/****************************************************************************
 * Global functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
	const char *subsystem;
	int index;
	int opt;
	int rc = EXIT_FAILURE;

	if (do_init() != 0)
		goto fail;

	/* Handle command line arguments */
	while ((opt = getopt_long(argc, argv, opts_short, opts_long, &index)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			goto fail;
		case 'v':
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_NETLINK, 3);
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_SFPTPD, 3);
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_CLOCKS, 3);
			break;
		default:
			fprintf(stderr, "unexpected option: %s\n", argv[optind]);
			usage(stderr);
			goto fail;
		}
	}

	if (argc - optind < 1) {
		usage(stderr);
		goto fail;
	}

	subsystem = argv[optind++];

	if (!strcmp(subsystem, "clock")) {
		rc = clock_command(argc - optind, argv + optind);
	} else {
		fprintf(stderr, "unknown subsystem: %s\n", subsystem);
		usage(stderr);
	}

fail:
	do_finit();

	return rc;
}
