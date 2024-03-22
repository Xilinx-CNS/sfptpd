/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CRNY_HELPER_H
#define _SFPTPD_CRNY_HELPER_H


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

extern int sfptpd_crny_helper_connect(const char *client_path,
				      const char *server_path,
				      int *sock_ret,
				      const char **failing_step);


#endif /* _SFPTPD_CRNY_HELPER_H */
