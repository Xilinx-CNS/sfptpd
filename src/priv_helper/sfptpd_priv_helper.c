/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Xilinx, Inc. */

/* Privileged helper for sfptpd */

#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sfptpd_priv_ops.h"
#include "sfptpd_crny_proto.h"
#include "sfptpd_crny_helper.h"


/****************************************************************************
 * Constant
 ****************************************************************************/

static const char *opts_short = "h";
static const struct option opts_long[] = {
	{ "help", 0, NULL, (int) 'h' },
	{ NULL, 0, NULL, 0 }
};


/****************************************************************************
 * Types
 ****************************************************************************/


/****************************************************************************
 * Local Data
 ****************************************************************************/

static bool verbose = false;


/****************************************************************************
 * Local functions
 ****************************************************************************/

static void usage(FILE *stream)
{
	fprintf(stream,
		"syntax: %s [OPTIONS] FD\n"
		"\n"
		"  OPTIONS\n"
		"    -h, --help           Show usage\n",
		program_invocation_short_name);
}

static int op_open_chrony(struct sfptpd_priv_resp_msg *resp_msg)
{
	char client_path[108];
	const char *failing_step;
	int sock;
	int rc;

	assert(resp_msg);

	snprintf(client_path, sizeof client_path,
		 CRNY_CONTROL_CLIENT_FMT, (int)getpid());

	rc = sfptpd_crny_helper_connect(client_path,
					CRNY_CONTROL_SOCKET_PATH,
					&sock,
					&failing_step);

	resp_msg->resp = SFPTPD_PRIV_RESP_OPEN_CHRONY;
	resp_msg->open_chrony = (struct sfptpd_priv_resp_open_chrony) {
		.rc = rc,
	};
	snprintf(resp_msg->open_chrony.failing_step,
		 sizeof resp_msg->open_chrony.failing_step, "%s", failing_step);

	return rc == 0 ? sock : -1;
}

static int server(int unix_fd)
{
	bool running = true;
	int rc = 0;

	while (running) {
		struct sfptpd_priv_req_msg req_msg;
		struct sfptpd_priv_resp_msg resp_msg = { .resp = SFPTPD_PRIV_RESP_OK };
		struct msghdr send_hdr = { 0 };
		struct iovec send_iov[1] = { 0 };
		int fds[1];
		int num_fds = 0;
		union {
				/* For alignment. See cmsg(3) */
			char buf[CMSG_SPACE(sizeof fds)];
			struct cmsghdr align;
		} send_cmsg_data = { .buf = { 0 } };
			struct cmsghdr *send_cmsg;
		ssize_t req_len;
		int fd = -1;

		rc = 0;

		/* Receive a command */

		req_len = recv(unix_fd, &req_msg, sizeof req_msg, 0);
		if (req_len == -1) {
			rc = errno;
			if (rc != EAGAIN && rc != EINTR) {
				perror("sfptpd_priv_helper:recv");
				running = false;
			} else {
				continue;
			}
		}

		/* Service commmand */
		switch (req_msg.req) {
		case SFPTPD_PRIV_REQ_SYNC:
			break;
		case SFPTPD_PRIV_REQ_CLOSE:
			running = false;
			break;
		case SFPTPD_PRIV_REQ_OPEN_CHRONY:
			fd = op_open_chrony(&resp_msg);
			break;
		}

		/* Set up for a plain send of the response data */
		send_hdr = (struct msghdr) {
			.msg_iov = send_iov,
			.msg_iovlen = 1,
		};
		send_iov[0] = (struct iovec) {
			.iov_base = &resp_msg,
			.iov_len = sizeof resp_msg
		};

		/* Add any fds to be returned via ancillary data */
		if (fd != -1) {
			fds[0] = fd;
			num_fds = 1;
		}
		if (num_fds != 0) {
			send_hdr.msg_control = send_cmsg_data.buf;
			send_hdr.msg_controllen = sizeof send_cmsg_data.buf;
			send_cmsg = CMSG_FIRSTHDR(&send_hdr);
			send_cmsg->cmsg_level = SOL_SOCKET;
			send_cmsg->cmsg_type = SCM_RIGHTS;
			send_cmsg->cmsg_len = CMSG_LEN(num_fds * sizeof *fds);
			memcpy(CMSG_DATA(send_cmsg), fds, num_fds * sizeof fds);
		}

		/* Send the response */
		sendmsg(unix_fd, &send_hdr, 0);

		while (num_fds)
			close(fds[--num_fds]);
	}

	return rc;
}


/****************************************************************************
 * Global functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
	static int unix_fd;
	const char *prog = "sfptpd-priv-helper";
	int index;
	int opt;

	/* Handle command line arguments */
	while ((opt = getopt_long(argc, argv, opts_short, opts_long, &index)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			return EXIT_SUCCESS;
		default:
			fprintf(stderr, "unexpected option: %s\n", argv[optind]);
			usage(stderr);
			return EXIT_FAILURE;
		}
	}

	if (argc - optind != 1) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	unix_fd = atoi(argv[optind++]);
	assert(optind == argc);

	if (unix_fd == -1)
		return EXIT_FAILURE;

	if (verbose)
		fprintf(stderr, "%s: started\n", prog);

	if (server(unix_fd) != 0)
		return EXIT_FAILURE;

	if (verbose)
		fprintf(stderr, "%s: stopped\n", prog);

	return EXIT_SUCCESS;
}
