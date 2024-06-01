/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

/* Client for privileged helper */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>

#include "sfptpd_logging.h"
#include "sfptpd_priv.h"
#include "sfptpd_priv_ops.h"
#include "sfptpd_general_config.h"
#include "sfptpd_thread.h"
#include "sfptpd_crny_proto.h"
#include "sfptpd_crny_helper.h"


/* Simplified lifecycle of a request through the privileged helper client
 * ======================================================================
 *
 * +-----------------------------------------+
 * | sfptpd_priv_start_helper()              |
 * |                                         |   +----------------------+
 * |  Privileged helper process launched-------->| server()             |
 * |                                         |   |                      |
 * +-----------------------------------------+   |                      |
 *                                               |                      |
 *    +-----------------------------------+      |                      |
 *    | sfptpd_priv_open_chrony()         |      |                      |
 *    |                                   |      |                      |
 *    |  +-----------------------------+  |      |                      |
 *    |  | sfptpd_priv_rpc()           |  |      |                      |
 *    |  |                             |  |      |                      |
 *    |  |   send() -------PRIV_REQ_OPEN_CHRONY----> recvmsg()          |
 *    |  |                             |  |      |   op_open_chrony()   |
 *    |  |   recvmsg() {<--[rc=0|errno],failtext---} sendmsg()          |
 *    |  |             {<============*fd*==========}                    |
 *    |  |                             |  |      |                      |
 *    |  | RETURNS number of fds       |  |      |                      |
 *    |  | provided by the server or   |  |      |                      |
 *    |  | -errno on comms error       |  |      |                      |
 *    |  |                             |  |      |                      |
 *    |  | RESPONSE PAYLOAD is passed  |  |      |                      |
 *    |  | in caller-supplied argument |  |      |                      |
 *    |  +-----------------------------+  |      |                      |
 *    |                                   |      |                      |
 *    | Falls back to attempting          |      |                      |
 *    | op_open_chrony() directly.        |      |                      |
 *    |                                   |      |                      |
 *    | RETURNS number of fds provided    |      |                      |
 *    | by the server or -errno on        |      |                      |
 *    | comms error, notably ENOTCONN     |      |                      |
 *    | if no helper.                     |      |                      |
 *    |                                   |      |                      |
 *    | RESPONSE PAYLOAD is passed        |      |                      |
 *    | in caller-supplied argument,      |      |                      |
 *    | including the return code of      |      |                      |
 *    | server's chrony opening function  |      |                      |
 *    +-----------------------------------+      |                      |
 *                                               |                      |
 * +-----------------------------------------+   |                      |
 * | sfptpd_priv_stop_helper()               |   |                      |
 * |                                         |   |                      |
 * |  Privileged helper process stopped--------->|                      |
 * |                                         |   +----------------------+
 * +-----------------------------------------+
 *
 *
 * NOTES
 * =====
 *
 * - All requests and responses are serialised through a mutex.
 *
 * - On failure of the helper process:
 *   - The whole sfptpd process will immediately quit
 *     (recent kernels, e.g. EL9)
 *   - The whole sfptpd process will quit when a requested operation fails
 *     (older kernels)
 *
 * - On failure of sfptpd the helper process will be cleaned up automatically.
 */

/****************************************************************************
 * Types
 ****************************************************************************/

struct sfptpd_priv_state {
	pthread_mutex_t lock;
	int helper_fd;
	int helper_pid;
};


/****************************************************************************
 * Globals
 ****************************************************************************/

static struct sfptpd_priv_state priv_state = {
	.helper_fd = -1,
};


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Private functions
 ****************************************************************************/

static int sfptpd_priv_fail(struct sfptpd_priv_state *state, bool report)
{
	if (state->helper_fd != -1) {
		close(state->helper_fd);
		state->helper_fd = -1;
	}
	if (report) {
		CRITICAL("priv: helper connection failed, %s\n",
			 strerror(errno));

		/* On new kernels, helper failure is detected by
		 * pidfd poll. This is the fallback for older kernels. */
		sfptpd_thread_error(ECHILD);
	}
	return -ENOTCONN;
}

/* Perform an RPC call to the privileged helper.
 * Returns the number of fds created by the helper (or zero) on success and
 * -errno on failure. */
static int sfptpd_priv_rpc(struct sfptpd_priv_state *state,
			   const struct sfptpd_priv_req_msg *req,
			   struct sfptpd_priv_resp_msg *resp,
			   int returned_fds[1])
{
	const int max_fds = 1;
	int num_fds;
	struct iovec recv_iov[1];
	struct msghdr recv_hdr;
	union {
		char buf[CMSG_SPACE(1 * sizeof(int))];
		struct cmsghdr align;
	} recv_cmsg_data;
	struct cmsghdr *recv_cmsg;
	int rc;

	assert(state);
	assert(req);
	assert(resp);

	if (state->helper_fd == -1)
		return -ENOTCONN;

	pthread_mutex_lock(&state->lock);
	do {
		rc = send(state->helper_fd, req, sizeof *req, 0);
	} while (rc == -1 && errno == EINTR);

	if (rc < 0)
		return sfptpd_priv_fail(state, req->req != SFPTPD_PRIV_REQ_CLOSE);

	recv_hdr = (struct msghdr) {
		.msg_iov = recv_iov,
		.msg_iovlen = 1,
		.msg_control = returned_fds ? recv_cmsg_data.buf : NULL,
		.msg_controllen = returned_fds ? sizeof recv_cmsg_data.buf : 0
	};
	recv_iov[0] = (struct iovec) {
		.iov_base = resp,
		.iov_len = sizeof *resp
	};

	do {
		rc = recvmsg(state->helper_fd, &recv_hdr, 0);
	} while (rc == -1 && (errno  == EAGAIN || errno == EINTR));
	pthread_mutex_unlock(&state->lock);

	if (rc < 0)
		return sfptpd_priv_fail(state, req->req != SFPTPD_PRIV_REQ_CLOSE);

	if (returned_fds == NULL)
		return 0;

	recv_cmsg = CMSG_FIRSTHDR(&recv_hdr);

	if (recv_cmsg != NULL &&
	    ((num_fds = (recv_cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int)) > max_fds ||
	     recv_cmsg->cmsg_len != CMSG_LEN(num_fds * sizeof(int)) ||
	     recv_cmsg->cmsg_level != SOL_SOCKET ||
	     recv_cmsg->cmsg_type != SCM_RIGHTS)) {
		ERROR("priv: invalid or unexpected ancillary data received from helper\n");
		return -EINVAL;
	}

	if (recv_cmsg)
		memcpy(returned_fds, CMSG_DATA(recv_cmsg), num_fds * sizeof(int));
	else
		num_fds = 0;

	return num_fds;
}

static bool priv_sync(struct sfptpd_priv_state *state)
{
	struct sfptpd_priv_req_msg req = { .req = SFPTPD_PRIV_REQ_SYNC };
	struct sfptpd_priv_resp_msg resp = { 0 };

	return sfptpd_priv_rpc(state, &req, &resp, NULL) == 0;
}


/****************************************************************************
 * Public functions
 ****************************************************************************/

void sfptpd_priv_stop_helper(void)
{
	struct sfptpd_priv_req_msg req = { .req = SFPTPD_PRIV_REQ_CLOSE };
	struct sfptpd_priv_resp_msg resp = { 0 };
	struct sfptpd_priv_state *state = &priv_state;

	if (state->helper_fd != -1) {
		sfptpd_priv_rpc(state, &req, &resp, NULL);
		sfptpd_priv_fail(state, false);
	}

	pthread_mutex_destroy(&state->lock);
}

int sfptpd_priv_start_helper(struct sfptpd_config *config, int *pid)
{
	const char *helper_path = sfptpd_general_config_get(config)->priv_helper_path;
	struct sfptpd_priv_state *state = &priv_state;
	pid_t child;
	char fd_str[10] = "";
	char *child_args[] = {
		NULL,
		fd_str,
		NULL
	};
	char *child_env[] = {
		NULL
	};

	int sv[2];
	int rc;

	state->helper_fd = -1;
	state->helper_pid = -1;

	/* Do nothing if no helper defined */
	if (helper_path[0] == '\0')
		return 0;

	rc = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
	if (rc == -1) {
		rc = errno;
		CRITICAL("priv: could not create socket pair for helper, %s\n", strerror(rc));
		return -rc;
	}
	state->helper_fd = sv[0];
	rc = snprintf(fd_str, sizeof fd_str, "%d", sv[1]);
	if (rc <= 0 || rc == sizeof fd_str) {
		CRITICAL("priv: formatting socket pair fd\n");
		return -EINVAL;
	}

	child = fork();

	if (child == -1) {
		rc = errno;
		CRITICAL("priv: could not fork helper, %s\n", strerror(rc));
		return -rc;
	}

	if (child == 0) {
		/* Ideally do this for tidiness but it is only available on
		 * recent kernels and actually we have not opened anything else
		 * yet anyway so the attack surface is unaffected.
		 *
		 * Close all fds apart from stdio and child end of sock pair
		 *   close_range(3, sv[1] - 1, 0);
		 *   closefrom(sv[1] + 1);
		 */
		close(sv[0]);

		/* Launch the helper */
		child_args[0] = strdup(helper_path);
		rc = execve(helper_path, child_args, child_env);
		if (rc == -1)
			perror("priv: could not exec helper");
		close(sv[1]);
		_exit(EXIT_FAILURE);
	} else {
		close(sv[1]);
		state->helper_pid = child;
		rc = pthread_mutex_init(&state->lock, NULL);
		if (rc != 0) {
			rc = errno;
			CRITICAL("could not create helper lock, %s\n", strerror(rc));
			return -rc;
		}

		if (!priv_sync(state)) {
			sfptpd_priv_stop_helper();
			CRITICAL("could not start privileged helper\n");
			return -ESHUTDOWN;
		} else {
			TRACE_L3("priv: started helper\n");
		}
	}

	if (pid)
		*pid = state->helper_pid;

	return 0;
}

int sfptpd_priv_open_chrony(sfptpd_short_text_t failing_step,
			    const char *client_path)
{
	struct sfptpd_priv_req_msg req = { .req = SFPTPD_PRIV_REQ_OPEN_CHRONY };
	struct sfptpd_priv_resp_msg resp = { 0 };
	int rc_in;
	int rc_out;
	int fds[1];

	rc_in = sfptpd_priv_rpc(&priv_state, &req, &resp, fds);
	if (rc_in > 0) {
		TRACE_L5("priv: open-chrony: got fd %d from helper\n", fds[0]);
		rc_out = fds[0];
	} else if (rc_in == 0) {
		rc_out = -resp.open_chrony.rc;
		failing_step = resp.open_chrony.failing_step;
	} else if (rc_in == -ENOTCONN) {
		const char *step;
		rc_in = sfptpd_crny_helper_connect(client_path,
						   CRNY_CONTROL_SOCKET_PATH,
						   fds, &step);
		sfptpd_strncpy(failing_step, step, sizeof(sfptpd_short_text_t));
		if (rc_in == 0)
			rc_out = fds[0];
		else
			rc_out = -rc_in;
	} else {
		ERROR("priv: open_chrony: error calling helper, %s\n", strerror(-rc_in));
		rc_out = rc_in;
	}

	return rc_out;
}

int sfptpd_priv_open_dev(const char *path)
{
	struct sfptpd_priv_req_msg req = { .req = SFPTPD_PRIV_REQ_OPEN_DEV };
	struct sfptpd_priv_resp_msg resp = { 0 };
	const size_t max_path = sizeof req.open_dev.path;
	int rc;
	int fds[1];

	if (strnlen(path, max_path) == max_path)
		return -ENAMETOOLONG;
	sfptpd_strncpy(req.open_dev.path, path, max_path);

	rc = sfptpd_priv_rpc(&priv_state, &req, &resp, fds);
	if (rc > 0) {
		TRACE_L5("priv: open-dev: got fd %d from helper\n", fds[0]);
		rc = fds[0];
	} else if (rc == 0) {
		rc = -resp.open_dev.rc;
	} else if (rc == -ENOTCONN) {
		rc = open(path, O_RDWR);
		if (rc == -1)
			rc = -errno;
	} else {
		ERROR("priv: open_chrony: error calling helper, %s\n", strerror(rc));
	}
	return rc;
}

int sfptpd_priv_chrony_control(enum chrony_clock_control_op op)
{
	struct sfptpd_priv_req_msg req = { .req = SFPTPD_PRIV_REQ_CHRONY_CONTROL };
	struct sfptpd_priv_resp_msg resp = { 0 };
	int fds[1];
	int rc;

	req.chrony_control.op = op;
	rc = sfptpd_priv_rpc(&priv_state, &req, &resp, fds);
	if (rc >= 0) {
		rc = -resp.open_dev.rc;
	} else if (rc == -ENOTCONN) {
		rc = -sfptpd_crny_helper_control(op);
	} else {
		ERROR("priv: chrony_control: error calling helper, %s\n", strerror(rc));
	}
	return rc;
}
