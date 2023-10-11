/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2016-2023 Xilinx, Inc. */

/**
 * @file   sfptpdctl.c
 * @brief  Client to control an sfptpd daemon.
 */


#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


/** sfptpd control socket path */
#define SFPTPD_CONTROL_SOCKET_PATH  "/var/run/sfptpd-control-v1.sock"


int main(int argc, char *argv[])
{
	int control_fd;
	int rc;

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = SFPTPD_CONTROL_SOCKET_PATH
	};

	if (argc != 2) {
		fprintf(stderr,
			"syntax: %s COMMAND\n"
			"\n"
			"  COMMAND (examples)\n"
			"    exit                 cause sfptpd to exit\n"
			"    logrotate            cause the log files to be closed and reopened\n"
			"    stepclocks           cause the clocks to be stepped\n"
			"    selectinstance=name  select specific sync instance\n"
			"    dumptables           dump some internal state to message log\n"
			"    pid_adjust=[KP[,[KI][,[KD][,local|ptp|pps|reset]*]]]\n"
			"                         set PID coefficients with optional reset per servo type, or all by default\n",
			argv[0]);
		return 1;
	}

	/* Create a Unix domain socket for sending control packets */
	control_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (control_fd == -1) {
		perror("socket");
		return 1;
	}

	/* Connect to the path in the filesystem. */
	rc = connect(control_fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		perror("connect");
		return 1;
	}

	rc = write(control_fd, argv[1], strlen(argv[1]));
	if (rc != strlen(argv[1])) {
		perror("write");
		return 1;
	}

	rc = close(control_fd);
	if (rc == -1) {
		perror("close");
		return 1;
	}

	return 0;
}
