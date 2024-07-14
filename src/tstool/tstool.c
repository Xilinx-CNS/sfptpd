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
#include <linux/sockios.h>
#include <linux/net_tstamp.h>

#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_netlink.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_logging.h"


/****************************************************************************
 * Constant
 ****************************************************************************/

#define OPT_PERSISTENT 0x10000
#define OPT_INITIAL 0x10001

static const char *opts_short = "hv";
static const struct option opts_long[] = {
	{ "help", 0, NULL, (int) 'h' },
	{ "verbose", 0, NULL, (int) 'v' },
	{ "persistent", 0, NULL, OPT_PERSISTENT },
	{ "initial", 0, NULL, OPT_INITIAL },
	{ NULL, 0, NULL, 0 }
};

const char *tx_types[] = {
	"off",
	"on",
	"onestep-sync",
	"onestep-p2p",
};
#define NUM_TX_TYPES (sizeof tx_types / sizeof *tx_types)

const char *rx_filters[] = {
	"none",
	"all",
	"some",
	"ptp-v1-l4-event",
	"ptp-v1-l4-sync",
	"ptp-v1-l4-delay-req",
	"ptp-v2-l4-event",
	"ptp-v2-l4-sync",
	"ptp-v2-l4-delay-req",
	"ptp-v2-l2-event",
	"ptp-v2-l2-sync",
	"ptp-v2-l2-delay-req",
	"ptp-v2-event",
	"ptp-v2-sync",
	"ptp-v2-delay-req",
	"ptp-ntp-all",
};
#define NUM_RX_FILTERS (sizeof rx_filters / sizeof *rx_filters)

const char *sof[] = {
	"tx_hardware",
	"tx_software",
	"rx_hardware",
	"rx_software",
	"software",
	"sys_hardware",
	"raw_hardware",
	"opt_id",
	"tx_sched",
	"tx_ack",
	"opt_cmsg",
	"opt_tsonly",
	"opt_stats",
	"opt_pktinfo",
	"opt_tx_swhw",
	"bind_phc",
	"opt_id_tcp",
	"17",
	"18",
	"19",
	"20",
	"21",
	"22",
	"onload_stream",
};
#define NUM_SOF (sizeof sof / sizeof *sof)


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
	CLOCK_CMD_DEDUP,
	CLOCK_CMD_INVALID
};

struct clock_command {
	enum clock_command_e tag;
	const char *name;
	int clock_args;
};

enum intf_command_e {
	INTF_CMD_LIST,
	INTF_CMD_INFO,
	INTF_CMD_SET_TS,
	INTF_CMD_INVALID
};

struct intf_command {
	enum intf_command_e tag;
	const char *name;
	int intf_args;
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
	{ CLOCK_CMD_DEDUP,   "dedup",   0 },
	{ CLOCK_CMD_INVALID, "INVALID", 0 },
};

static const struct intf_command intf_cmds[] = {
	{ INTF_CMD_LIST,    "list",    0 },
	{ INTF_CMD_INFO,    "info",    1 },
	{ INTF_CMD_SET_TS,  "set_ts",  1 },
	{ INTF_CMD_INVALID, "INVALID", 0 },
};


/****************************************************************************
 * Local functions
 ****************************************************************************/

static void format_flags(char *buf, ssize_t space,
			 const char **names, size_t num_known,
			 unsigned long flags)
{
	int i;
	int len = 0;
	const char ellipsis[] = u8"\u2026";
	ssize_t rewind = 1 + strlen(ellipsis);

	assert(space >= rewind);
	*buf = '\0';

	for (i = 0; len >= 0 && flags != 0 && space >= rewind; i++, flags >>= 1) {
		if (flags & 1) {
			if (i < num_known)
				len = snprintf(buf, space, " %s", names[i]);
			else
				len = snprintf(buf, space, " %d", i);
			if (len >= space)
				break;
			else
				space -= len, buf += len;
		}
	}

	if (flags != 0)
		snprintf(buf + space - rewind, rewind, "%s", ellipsis);
}

static int decode_option(const char **names, size_t num_known,
			 unsigned long *option, const char *text)
{
	int i;

	for (i = 0; i < num_known && strcmp(text, names[i]); i++);
	if (i == num_known) {
		ERROR("option %s invalid\n", text);
		return 1;
	}

	*option = i;
	return 0;
}


static int do_init(void)
{
	int rc;

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
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"syntax: %s [OPTIONS] SUBSYSTEM COMMAND..\n"
		"\n"
		"  OPTIONS\n"
		"        --persistent            Use sfptpd persistent frequency adjustment\n"
		"        --initial               Perform sfptpd initial clock correction\n"
		"    -h, --help                  Show usage\n"
		"    -v, --verbose               Be verbose\n\n"
		"  CLOCK SUBSYSTEM\n"
		"    clock list                  List clocks\n"
		"    clock info CLOCK            Show clock information\n"
		"    clock get CLOCK             Read clock\n"
		"    clock step CLOCK OFFSET     Step clock\n"
		"    clock slew CLOCK PPB        Adjust clock frequency\n"
		"    clock set_to CLOCK1 CLOCK2  CLOCK1 := CLOCK2\n"
		"    clock diff CLOCK1 CLOCK2    CLOCK1 - CLOCK2\n"
		"    clock dedup                 Deduplicate shared phc devices\n\n"
		"      CLOCK := <phcN> | <ethN> | system\n\n"
		"  INTERFACE SUBSYSTEM\n"
		"    interface list              List physical interfaces\n"
		"    interface info INTF         Show interface information\n"
		"    interface set_ts TX RX      Set timestamp modes\n\n"
		"      INTF := <ethN>\n"
                "      See 'info' response for available TX and RX modes\n",
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
	sfptpd_time_t freq_adj;
	sfptpd_time_t tick_len;
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
		all_clocks = sfptpd_clock_get_active_snapshot(&num_clocks);
		for (i = 0; i < num_clocks; i++) {
			printf("%s\n", sfptpd_clock_get_long_name(all_clocks[i]));
		}
		sfptpd_clock_free_active_snapshot(all_clocks);
		break;
	case CLOCK_CMD_INFO:
		sfptpd_clock_get_frequency(clocks[0], &freq_adj, &tick_len);
		printf("short-name: %s\n"
		       "long-name: %s\n"
		       "hw-id: %s\n"
		       "persistent-freq-correction: %.3Lf ppb\n"
		       "max-freq-adj: %.3Lf ppb\n"
		       "freq-adj: %.3Lf ppb\n"
		       "tick-len: %.3Lf s\n"
		       "diff-method: %s\n",
		       sfptpd_clock_get_short_name(clocks[0]),
		       sfptpd_clock_get_long_name(clocks[0]),
		       sfptpd_clock_get_hw_id_string(clocks[0]),
		       sfptpd_clock_get_freq_correction(clocks[0]),
		       sfptpd_clock_get_max_frequency_adjustment(clocks[0]),
		       freq_adj,
		       tick_len / 1000000000.0L,
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
	case CLOCK_CMD_DEDUP:
		rc = sfptpd_clock_deduplicate();
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


static int intf_command(int argc, char *argv[])
{
#define MAX_INTFS 2
	struct sfptpd_interface *interfaces[MAX_INTFS];
	struct sfptpd_db_query_result query_result;
	struct hwtstamp_config so_ts_req = { 0 };
	struct ethtool_ts_info ts_info;
	struct sfptpd_interface *intf;
	struct sfptpd_clock *clock;
	const struct intf_command *cmd;
	const char *command;
	char tx_types_str[128];
	char rx_filters_str[128];
	char sof_str[128];
	char tx_type_str[20];
	char rx_filter_str[20];
	bool supports_efx;
	bool supports_phc;
	unsigned long rx;
	unsigned long tx;
	int device_idx;
	int rc = 0;
	int i;

	if (argc < 1) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	command = argv[0];

	for (cmd = intf_cmds;
	     cmd->tag != INTF_CMD_INVALID &&
	     strcmp(command, cmd->name);
	     cmd++);

	assert(cmd->intf_args <= MAX_INTFS);
	if (argc - 1 < cmd->intf_args) {
		ERROR("insufficient number of interfaces specified\n");
		usage(stderr);
		return EXIT_FAILURE;
	}

	for (i = 0; i < cmd->intf_args; i++) {
		const char *intf_ref = argv[1 + i];
		interfaces[i] = sfptpd_interface_find_by_name(intf_ref);
		if (interfaces[i] == NULL) {
			fprintf(stderr, "unknown interfface: %s\n", intf_ref);
			return EXIT_FAILURE;
		}
	}

	switch(cmd->tag) {
	case INTF_CMD_LIST:
		query_result = sfptpd_interface_get_active_ptp_snapshot();
		for (i = 0; i < query_result.num_records; i++) {
			struct sfptpd_interface **intfp = query_result.record_ptrs[i];

			intf = *intfp;
			printf("%s\n", sfptpd_interface_get_name(intf));
		}
		query_result.free(&query_result);
		break;
	case INTF_CMD_INFO:
		intf = interfaces[0];
		clock = sfptpd_interface_get_clock(intf);
		sfptpd_interface_get_clock_device_idx(intf, &supports_phc, &device_idx, &supports_efx);
		sfptpd_interface_get_ts_info(intf, &ts_info);
		sfptpd_interface_ioctl(intf, SIOCGHWTSTAMP, &so_ts_req);
		format_flags(tx_types_str, sizeof tx_types_str, tx_types, NUM_TX_TYPES, ts_info.tx_types);
		format_flags(rx_filters_str, sizeof rx_filters_str, rx_filters, NUM_RX_FILTERS, ts_info.rx_filters);
		format_flags(sof_str, sizeof sof_str, sof, NUM_SOF, ts_info.so_timestamping);
		format_flags(tx_type_str, sizeof tx_types_str, tx_types, NUM_TX_TYPES, 1 << so_ts_req.tx_type);
		format_flags(rx_filter_str, sizeof rx_filters_str, rx_filters, NUM_RX_FILTERS, 1 << so_ts_req.rx_filter);
		printf("interface: %s\n"
		       "clock: %s\n"
		       "mac-address: %s\n"
		       "fw-version: %s\n"
		       "supported-apis:%s%s\n"
		       "supported-tx-modes:%s\n"
		       "supported-rx-filters:%s\n"
		       "supported-sof-flags:%s\n"
		       "tx-mode:%s\n"
		       "rx-filter:%s\n",
		       sfptpd_interface_get_name(intf),
		       clock == NULL ? "none" : sfptpd_clock_get_short_name(clock),
		       sfptpd_interface_get_mac_string(intf),
		       sfptpd_interface_get_fw_version(intf),
		       supports_phc ? " phc" : "",
		       supports_efx ? " efx" : "",
		       tx_types_str,
		       rx_filters_str,
		       sof_str,
		       tx_type_str,
		       rx_filter_str);
		break;
	case INTF_CMD_SET_TS:
		intf = interfaces[0];
		if (argc < 3 + cmd->intf_args) {
			ERROR("insufficient arguments for timestamp configuration\n");
			return EXIT_FAILURE;
		}
		if (decode_option(tx_types, NUM_TX_TYPES, &tx, argv[1 + cmd->intf_args]))
			return EXIT_FAILURE;
		if (decode_option(rx_filters, NUM_RX_FILTERS, &rx, argv[2 + cmd->intf_args]))
			return EXIT_FAILURE;
		so_ts_req.tx_type = tx;
		so_ts_req.rx_filter = rx;
		rc = sfptpd_interface_ioctl(intf, SIOCSHWTSTAMP, &so_ts_req);
		break;
	default:
		fprintf(stderr, "unknown interface command: %s\n", command);
		usage(stderr);
		return EXIT_FAILURE;
	}

	if (rc != 0) {
		ERROR("tstool: interface: %s: %s\n", command, strerror(rc));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


/****************************************************************************
 * Global functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
	struct sfptpd_config_general *gconf;
	const char *subsystem;
	int index;
	int opt;
	int rc = EXIT_FAILURE;
	int ret;

	/* Initialise config */
	ret = sfptpd_config_create(&config);
	if (ret != 0)
		return EXIT_FAILURE;

	/* Tweak config */
	gconf = sfptpd_general_config_get(config);
	gconf->non_sfc_nics = true;
	gconf->timestamping.disable_on_exit = false;
	gconf->clocks.persistent_correction = false;
	gconf->clocks.no_initial_correction = true;

	/* Handle command line arguments */
	while ((opt = getopt_long(argc, argv, opts_short, opts_long, &index)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			goto fail;
		case 'v':
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_NETLINK, 3);
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_SFPTPD, 6);
			sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_CLOCKS, 3);
			break;
		case OPT_PERSISTENT:
			gconf->clocks.persistent_correction = true;
			break;
		case OPT_INITIAL:
			gconf->clocks.no_initial_correction = false;
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

	if (do_init() != 0)
		goto fail;

	subsystem = argv[optind++];

	if (!strcmp(subsystem, "clock")) {
		rc = clock_command(argc - optind, argv + optind);
	} else if (!strcmp(subsystem, "interface") ||
		   !strcmp(subsystem, "intf")) {
		rc = intf_command(argc - optind, argv + optind);
	} else {
		fprintf(stderr, "unknown subsystem: %s\n", subsystem);
		usage(stderr);
	}

	do_finit();

fail:
	sfptpd_config_destroy(config);

	return rc;
}
