/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_control.c
 * @brief  Provides capability to control the daemon.
 */

#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>

#include "sfptpd_constants.h"
#include "sfptpd_logging.h"
#include "sfptpd_control.h"
#include "sfptpd_general_config.h"


/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

#define COMMAND_BUFFER_SIZE 128

static const char *COMMAND_EXIT = "exit";
static const char *COMMAND_LOGROTATE = "logrotate";
static const char *COMMAND_STEPCLOCKS = "stepclocks";
static const char *COMMAND_SELECTINSTANCE = "selectinstance=";
static const char *COMMAND_TESTMODE = "testmode=";

static const struct sfptpd_test_mode_descriptor test_modes[] = SFPTPD_TESTS_ARRAY;


/****************************************************************************
 * Local Variables
 ****************************************************************************/

static int control_fd = -1;
static const char *control_path;


/****************************************************************************
 * Local Functions
 ****************************************************************************/



/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_control_socket_open(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general_config;
	int rc;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX
	};

	general_config = sfptpd_general_config_get(config);
	control_path = general_config->control_path;
	sfptpd_strncpy(addr.sun_path, control_path, sizeof addr.sun_path);

	/* Remove any existing socket, ignoring errors */
	unlink(control_path);

	/* Create a Unix domain socket for receiving control packets */
	control_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (control_fd == -1) {
		ERROR("couldn't create control socket\n");
	        return errno;
	}

	/* Bind to the path in the filesystem. */
	rc = bind(control_fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		ERROR("couldn't bind control socket to %s\n",
		      control_path);
	        return errno;
	}

	/* Set ownership of socket. Defer error to any consequent failure. */
	if (chown(control_path, general_config->uid, general_config->gid))
		TRACE_L4("could not set control socket ownership, %s\n",
			 strerror(errno));

	return 0;
}


int sfptpd_control_socket_get_fd(void)
{
	return control_fd;
}


enum sfptpd_control_action sfptpd_control_socket_get_action(union sfptpd_control_action_parameters *param)
{
	char buf[COMMAND_BUFFER_SIZE];
	int rc;

	assert(control_fd != -1);

	rc = read(control_fd, buf, sizeof buf - 1);
	if (rc == -1) {
		ERROR("couldn't read from control socket, %s\n",
		      strerror(rc));
		return CONTROL_ERROR;
	} else {
		assert(rc < sizeof buf);
		buf[rc] = '\0';
	}

	if (!strcmp(buf, COMMAND_EXIT)) {
		return CONTROL_EXIT;
	} else if (!strcmp(buf, COMMAND_LOGROTATE)) {
		return CONTROL_LOGROTATE;
	} else if (!strcmp(buf, COMMAND_STEPCLOCKS)) {
		return CONTROL_STEPCLOCKS;
	} else if (!strncmp(buf, COMMAND_SELECTINSTANCE, strlen(COMMAND_SELECTINSTANCE))) {
		sfptpd_strncpy(param->selected_instance,
			       &buf[strlen(COMMAND_SELECTINSTANCE)],
			       sizeof(param->selected_instance));
		return CONTROL_SELECTINSTANCE;
	} else if (!strncmp(buf, COMMAND_TESTMODE, strlen(COMMAND_TESTMODE))) {
		char *save;
		char *token;
		const struct sfptpd_test_mode_descriptor *mode;
		int i;
		const int max_params = sizeof param->test_mode.params / sizeof *param->test_mode.params;

		memset(param, '\0', sizeof *param);
		token = strtok_r(buf + strlen(COMMAND_TESTMODE), ",", &save);
		if (token != NULL) {
			/* Search for test mode in array; no loop body. */
			for (mode = test_modes; mode->name != NULL && strcmp(token, mode->name) != 0; mode++);
		} else {
			ERROR("no test mode specified\n");
			return CONTROL_ERROR;
		}
		if (mode->name == NULL) {
			ERROR("test mode %s unknown\n", token);
			return CONTROL_ERROR;
		}
		param->test_mode.id = mode->id;
		for (i = 0; i < max_params; i++) {
			token = strtok_r(NULL, ",", &save);
			if (token != NULL) {
				errno = 0;
				param->test_mode.params[i] = strtol(token, NULL, 0);
				if (errno != 0) {
					ERROR("Invalid test mode parameter specified: %s\n", token);
					return CONTROL_ERROR;
				}
			} else {
				break;
			}
		}
		return CONTROL_TESTMODE;
	} else {
		NOTICE("unknown command %s received on control socket\n",
		       buf);
		return CONTROL_NOP;
	}
}


void sfptpd_control_socket_close(void)
{
	if (control_fd != -1) {
		close(control_fd);
		control_fd = -1;
	}
	unlink(control_path);
}



/* fin */
