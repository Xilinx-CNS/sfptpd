/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CRNY_HELPER_H
#define _SFPTPD_CRNY_HELPER_H


/****************************************************************************
 * Types
 ****************************************************************************/

/* operation for clock control script */
enum chrony_clock_control_op {
	CRNY_CTRL_OP_NOP,
	CRNY_CTRL_OP_ENABLE,
	CRNY_CTRL_OP_DISABLE,
	CRNY_CTRL_OP_SAVE,
	CRNY_CTRL_OP_RESTORE,
	CRNY_CTRL_OP_RESTORENORESTART,
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

extern int sfptpd_crny_helper_connect(const char *client_path,
				      const char *server_path,
				      int *sock_ret,
				      const char **failing_step);

extern int sfptpd_crny_helper_control(enum chrony_clock_control_op op);

#endif /* _SFPTPD_CRNY_HELPER_H */
