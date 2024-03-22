/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_PRIV_H
#define _SFPTPD_PRIV_H

#include "sfptpd_priv_ops.h"

/****************************************************************************
 * Structures and Types
 ****************************************************************************/


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/* Start the privileged helper, if configured, and connect to it.
 * Returns -errno on error. */
extern int sfptpd_priv_start_helper(struct sfptpd_config *config,
				    int *pid);

/* Stop the privileged helper, if started. */
extern void sfptpd_priv_stop_helper(void);

/* Connect to the chronyd control channel, first attempting the the
 * privileged helper, falling back to a direct attempt if not present.
 * Returns fd or -errno on error. */
extern int sfptpd_priv_open_chrony(sfptpd_short_text_t failing_step,
				   const char *client_path);

#endif /* _SFPTPD_PRIV_H */
