/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

#ifndef _SFPTPD_CONTROL_H
#define _SFPTPD_CONTROL_H

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>

#include "sfptpd_test.h"

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

#include "sfptpd_test.h"

enum sfptpd_control_action {
	CONTROL_NOP,
	CONTROL_ERROR,
	CONTROL_EXIT,
	CONTROL_LOGROTATE,
	CONTROL_STEPCLOCKS,
	CONTROL_SELECTINSTANCE,
	CONTROL_TESTMODE,
	CONTROL_DUMPTABLES,
	CONTROL_PID_ADJUST,
};

union sfptpd_control_action_parameters {
	char selected_instance[64];
	struct {
		enum sfptpd_test_id id;
		int params[3];
	} test_mode;
	struct {
		int servo_type_mask;
		double kp;
		double ki;
		double kd;
		bool reset;
	} pid_adjust;
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create the unix domain socket for external control of the daemon
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_control_socket_open(struct sfptpd_config *config);

/** Get the fd to use in polling
 * @return The fd on success or -1 otherwise.
 */
int sfptpd_control_socket_get_fd(void);

/** Process activity on the socket
 * @return The action to perform in response to the client
 */
enum sfptpd_control_action sfptpd_control_socket_get_action(union sfptpd_control_action_parameters *param);

/** Close the socket
 */
void sfptpd_control_socket_close(void);


#endif /* _CONTROL_H */
