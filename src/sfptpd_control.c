/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

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
#include "sfptpd_servo.h"


/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

#define COMMAND_BUFFER_SIZE 128
#define MODULE "control"
#define PREFIX MODULE ": "
#define COMMAND_DELIM "="
#define PARAM_DELIM ","

static const char *COMMAND_EXIT = "exit";
static const char *COMMAND_LOGROTATE = "logrotate";
static const char *COMMAND_STEPCLOCKS = "stepclocks";
static const char *COMMAND_SELECTINSTANCE = "selectinstance";
static const char *COMMAND_TESTMODE = "testmode";
static const char *COMMAND_DUMPTABLES = "dumptables";
static const char *COMMAND_PID_ADJUST = "pid_adjust";

static const struct sfptpd_test_mode_descriptor test_modes[] = SFPTPD_TESTS_ARRAY;


/****************************************************************************
 * Local Variables
 ****************************************************************************/

static int control_fd = -1;
static char *control_path;


/****************************************************************************
 * Local Functions
 ****************************************************************************/



/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_control_socket_open(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general_config;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX
	};
	size_t sz;
	int rc;

	general_config = sfptpd_general_config_get(config);
	sz = sfptpd_format(sfptpd_log_get_format_specifiers(), NULL,
			   NULL, 0,
			   general_config->control_path);
	if (sz < 0)
		return errno;

	control_path = malloc(++sz);
	if (control_path == NULL)
		return errno;

	rc = sfptpd_format(sfptpd_log_get_format_specifiers(), NULL,
			   control_path, sz,
			   general_config->control_path);
	if (rc < 0)
		goto fail;

	if (strlen(control_path) >= sizeof addr.sun_path) {
		errno = ENAMETOOLONG;
		goto fail;
	}

	sfptpd_strncpy(addr.sun_path, control_path, sizeof addr.sun_path);

	/* Remove any existing socket, ignoring errors */
	unlink(control_path);

	/* Create a Unix domain socket for receiving control packets */
	control_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (control_fd == -1) {
		ERROR(PREFIX "couldn't create socket\n");
	        goto fail;
	}

	/* Bind to the path in the filesystem. */
	rc = bind(control_fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		ERROR(PREFIX "couldn't bind socket to %s\n",
		      control_path);
	        goto fail;
	}

	/* Set ownership of socket. Defer error to any consequent failure. */
	if (chown(control_path, general_config->uid, general_config->gid))
		TRACE_L4(PREFIX "could not set socket ownership, %s\n",
			 strerror(errno));

	return 0;
fail:
	free(control_path);
	return errno;
}


int sfptpd_control_socket_get_fd(void)
{
	return control_fd;
}


enum sfptpd_control_action sfptpd_control_socket_get_action(union sfptpd_control_action_parameters *param)
{
	char buf[COMMAND_BUFFER_SIZE];
	const char *command;
	const char *opt;
	char *opts;
	int rc;

	assert(control_fd != -1);

	rc = read(control_fd, buf, sizeof buf - 1);
	if (rc == -1) {
		ERROR(PREFIX "couldn't read from socket, %s\n",
		      strerror(rc));
		return CONTROL_ERROR;
	} else {
		assert(rc < sizeof buf);
		buf[rc] = '\0';
	}

	opts = buf;
	command = strsep(&opts, COMMAND_DELIM);

	if (command == NULL) {
		NOTICE(PREFIX "no command given\n");
		return CONTROL_NOP;
	} else if (!strcmp(command, COMMAND_EXIT)) {
		return CONTROL_EXIT;
	} else if (!strcmp(command, COMMAND_LOGROTATE)) {
		return CONTROL_LOGROTATE;
	} else if (!strcmp(command, COMMAND_STEPCLOCKS)) {
		return CONTROL_STEPCLOCKS;
	} else if (!strcmp(command, COMMAND_DUMPTABLES)) {
		return CONTROL_DUMPTABLES;
	} else if (!strcmp(command, COMMAND_SELECTINSTANCE)) {
		opt = strsep(&opts, COMMAND_DELIM);
		if (opt == NULL) {
			ERROR(PREFIX "%s: no instance provided\n", command);
			return CONTROL_ERROR;
		}
		sfptpd_strncpy(param->selected_instance, opt,
			       sizeof(param->selected_instance));
		return CONTROL_SELECTINSTANCE;
	} else if (!strcmp(command, COMMAND_TESTMODE)) {
		const int max_params = sizeof param->test_mode.params / sizeof *param->test_mode.params;
		const struct sfptpd_test_mode_descriptor *mode;
		char *token;
		int i;

		memset(param, '\0', sizeof *param);
		token = strsep(&opts, PARAM_DELIM);
		if (token != NULL) {
			/* Search for test mode in array; no loop body. */
			for (mode = test_modes; mode->name != NULL && strcmp(token, mode->name) != 0; mode++);
		} else {
			ERROR(PREFIX "no test mode specified\n");
			return CONTROL_ERROR;
		}
		if (mode->name == NULL) {
			ERROR(PREFIX "test mode %s unknown\n", token);
			return CONTROL_ERROR;
		}
		param->test_mode.id = mode->id;
		for (i = 0; i < max_params; i++) {
			token = strsep(&opts, PARAM_DELIM);
			if (token != NULL) {
				errno = 0;
				param->test_mode.params[i] = strtol(token, NULL, 0);
				if (errno != 0) {
					ERROR(PREFIX "%s has invalid mode parameter: %s\n", command, token);
					return CONTROL_ERROR;
				}
			} else {
				break;
			}
		}
		return CONTROL_TESTMODE;
	} else if (!strcmp(command, COMMAND_PID_ADJUST)) {
		char *token;
		int type = 0;

		token = strsep(&opts, PARAM_DELIM);
		param->pid_adjust.servo_type_mask = 0;
		param->pid_adjust.kp = NAN;
		param->pid_adjust.ki = NAN;
		param->pid_adjust.kd = NAN;
		param->pid_adjust.reset = false;

		/* Positional parameters */
		if (token != NULL && *token)
			param->pid_adjust.kp = strtod(token, NULL);
		token = strsep(&opts, PARAM_DELIM);
		if (token != NULL && *token)
			param->pid_adjust.ki = strtod(token, NULL);
		token = strsep(&opts, PARAM_DELIM);
		if (token != NULL && *token)
			param->pid_adjust.kd = strtod(token, NULL);

		/* Keyword parameters */
		while ((token = strsep(&opts, PARAM_DELIM))) {
			if ((type = sfptpd_servo_get_type_flag(token))) {
				param->pid_adjust.servo_type_mask |= type;
			} else if (!strcmp(token, "reset")) {
				param->pid_adjust.reset = true;
			} else {
				ERROR(PREFIX "%s has unexpected token: %s\n", command, token);
				return CONTROL_ERROR;
			}
		}

		/* Default to modifying all servo types */
		if (param->pid_adjust.servo_type_mask == 0)
			param->pid_adjust.servo_type_mask = SFPTPD_SERVO_TYPE_ALL;
		return CONTROL_PID_ADJUST;
	} else {
		NOTICE(PREFIX "unknown command %s received\n",
		       command);
		return CONTROL_NOP;
	}
}


void sfptpd_control_socket_close(void)
{
	if (control_fd != -1) {
		close(control_fd);
		control_fd = -1;
	}
	if (control_path != NULL) {
		unlink(control_path);
		free(control_path);
	}
}



/* fin */
