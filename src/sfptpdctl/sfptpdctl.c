/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2016-2024 Xilinx, Inc. */

/**
 * @file   sfptpdctl.c
 * @brief  Client to control an sfptpd daemon.
 */


#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdlib.h>


/****************************************************************************
 * Constant
 ****************************************************************************/

/** sfptpd control socket path */
#define SFPTPD_CONTROL_SOCKET_PATH  "/var/run/sfptpd/control-v1.sock"

static const char *opts_short = "hs:";
static const struct option opts_long[] = {
	{ "help", 0, NULL, (int) 'h' },
	{ "socket", 1, NULL, (int) 's' },
	{ NULL, 0, NULL, 0 }
};


/****************************************************************************
 * Local Data
 ****************************************************************************/


/****************************************************************************
 * Local functions
 ****************************************************************************/

static void usage(FILE *stream)
{
	fprintf(stream,
		"syntax: %s [OPTIONS] COMMAND-STRING*\n"
		"\n"
		"  COMMAND-STRING\n"
		"    exit                 cause sfptpd to exit\n"
		"    logrotate            cause the log files to be closed and reopened\n"
		"    stepclocks           cause the clocks to be stepped\n"
		"    testmode=MODE[,ARG]* select test mode (see sfptpd source)\n"
		"    selectinstance=NAME  select specific sync instance\n"
		"    dumptables           dump some internal state to message log\n"
		"    pid_adjust=[KP[,[KI][,[KD][,local|ptp|pps|reset]*]]]\n"
		"                         set PID coefficients with optional reset per servo type, or all by default\n"
		"\n"
		"  OPTIONS\n"
		"    -h, --help           Show usage\n"
		"    -s, --socket         Set control socket (default: %s)\n",
		program_invocation_short_name, SFPTPD_CONTROL_SOCKET_PATH);
}


/****************************************************************************
 * Global functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
	const char *control_addr = SFPTPD_CONTROL_SOCKET_PATH;
	struct sockaddr_un addr;
	int control_fd;
	int index;
	int opt;
	int rc;
	int i;

	/* Handle command line arguments */
	while ((opt = getopt_long(argc, argv, opts_short, opts_long, &index)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			return EXIT_SUCCESS;
		case 's':
			control_addr = optarg;
			break;
		default:
			fprintf(stderr, "unexpected option: %s\n", argv[optind]);
			usage(stderr);
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	/* Write the destination address */
	addr.sun_family = AF_UNIX;
	if (strlen(control_addr) >= sizeof addr.sun_path) {
		fprintf(stderr, "address too long: %s\n", control_addr);
		return EXIT_FAILURE;
	}
	strncpy(addr.sun_path, control_addr, sizeof addr.sun_path);

	/* Create a Unix domain socket for sending control packets */
	control_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (control_fd == -1) {
		perror("socket");
		return EXIT_FAILURE;
	}

	/* Connect to the path in the filesystem. */
	rc = connect(control_fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		perror("connect");
		return EXIT_FAILURE;
	}

	/* Send each positional argument as a separate command */
	for (i = optind; i < argc; i++) {
		rc = write(control_fd, argv[i], strlen(argv[i]));
		if (rc != strlen(argv[i])) {
			perror("write");
			return EXIT_FAILURE;
		}
	}

	rc = close(control_fd);
	if (rc == -1) {
		perror("close");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
