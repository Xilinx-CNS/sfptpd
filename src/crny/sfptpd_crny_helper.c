/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2017-2024 Advanced Micro Devices, Inc. */

/* Chrony connection helper */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <regex.h>

#include "sfptpd_crny_helper.h"


/****************************************************************************
 * Macros
 ****************************************************************************/


/****************************************************************************
 * Types
 ****************************************************************************/


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Function prototypes
 ****************************************************************************/


/****************************************************************************
 * Configuration
 ****************************************************************************/

int sfptpd_crny_helper_connect(const char *client_path,
			       const char *server_path,
			       int *sock_ret,
			       const char **failing_step)
{
	struct sockaddr_un client_addr = {
		.sun_family = AF_UNIX
	};
	struct sockaddr_un server_addr = {
		.sun_family = AF_UNIX
	};
	int sock;
	int flags;
	int rc = EINVAL;

	assert(client_path);
	assert(server_path);
	assert(sock_ret);
	assert(failing_step);

	*sock_ret = -1;
	*failing_step = "unknown";

	if (snprintf(client_addr.sun_path,
		     sizeof client_addr.sun_path,
		     "%s", client_path) >=
	    sizeof client_addr.sun_path) {
		*failing_step = "snprintf1";
		return ENOMEM;
	}

	if (snprintf(server_addr.sun_path,
		     sizeof server_addr.sun_path,
		     "%s", server_path) >=
	    sizeof server_addr.sun_path) {
		*failing_step = "snprintf2";
		return ENOMEM;
	}

	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		*failing_step = "socket";
		return errno;
	}

	flags = fcntl(sock, F_GETFD);
	if (flags == -1) {
		*failing_step = "fcntl(F_GETFD)";
		rc = errno;
		goto cleanup;
	}

	flags |= FD_CLOEXEC;

	if (fcntl(sock, F_SETFD, flags) < 0) {
		*failing_step = "fcntl(F_SETFD)";
		rc = errno;
		goto cleanup;
	}

	if (fcntl(sock, F_SETFL, O_NONBLOCK)) {
		*failing_step = "fcntl(F_SETFL)";
		rc = errno;
		goto cleanup;
	}

	/* Bind the local socket to the path we just specified */
	/* Note: we need to unlink before bind, in case the socket wasn't cleaned up last time */
	unlink(client_path);
	if (bind(sock, &client_addr, sizeof client_addr) < 0) {
		*failing_step = "bind";
		rc = errno;
		goto cleanup2;
	}

	/* You need to chmod 0666 the socket otherwise pselect will time out. */
	if (chmod(client_path, 0666) < 0) {
		*failing_step = "chmod";
		rc = errno;
		goto cleanup2;
	}

	/* Connect the socket */
	if (connect(sock, &server_addr, sizeof server_addr) < 0) {
		*failing_step = "connect";
		rc = errno;
		if (rc != EINPROGRESS)
			goto cleanup2;
	} else {
		rc = 0;
	}

	*sock_ret = sock;
	*failing_step = "success";

	return rc;

cleanup2:
	unlink(client_path);
cleanup:
	close(sock);
	return rc;
}

